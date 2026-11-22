/*
  Anarch front end for CircuitMess Bit (ESP32-S3, ST7735S 128x128, 7 buttons,
  buzzer). Hardware setup mirrors Bit-Firmware (Pins.cpp, Display.cpp).
*/

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/gptimer.h>
#include <hal/ledc_ll.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include <esp_efuse.h>
#include <nvs_flash.h>
#include <esp_sleep.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_partition.h>
#include <esp_app_desc.h>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <LovyanGFX.hpp>

#include "anarch_settings.h"

#define SFG_LOG(str) { printf("Anarch: %s\n", str); }

static void quickSaveStore();
#define SFG_QUICKSAVE_STORE quickSaveStore // mods/12-quicksave_store.diff: keep quicksaves across power-off
#define RCL_CAST_RAYS_PARALLEL renderColumns // mods/13-parallel_columns.diff: the columns on both cores

#include "game.h"
#include "audio.h"
#include "resume.h"

// Pin numbers from Bit-Firmware Pins.cpp: rev 0-2 = Bit / Bit 2, rev 3-4 = Bit 3.
struct BitPins { int up, down, left, right, a, b, menu, bl, buzz, sck, mosi, dc, rst, batt, vref; };
static constexpr BitPins PinsRev1 = { 5, 4, 3, 6, 7, 8, 9, 12, 11, 13, 14, 15, 16, 10, -1 };
static constexpr BitPins PinsRev3 = { 11, 13, 12, 9, 14, 15, 21, 8, 16, 4, 5, 6, 7, 3, 2 };
static BitPins pins;

// Same order as SFG_KEY_UP, RIGHT, DOWN, LEFT, A, B, C (Menu acts as C).
static gpio_num_t keyPins[7];

static uint16_t palette[256]; // byte-swapped RGB565, the order the panel expects
// Two frames: the game draws into one while DMA sends the other to the panel.
DMA_ATTR static uint16_t frames[2][SFG_SCREEN_RESOLUTION_X * SFG_SCREEN_RESOLUTION_Y];
static uint16_t* frame = frames[0];
static bool slept;

static lgfx::Bus_SPI bus;
static lgfx::Panel_ST7735S panel;
static LGFX_Device lcd;

// Sound: core 0 mixes Anarch's effects, music and the chirps into mixer.ring (audio.h), PcmLead
// samples ahead of what plays; a timer interrupt on core 1 sets each sample as the buzzer's PWM duty
// (8-bit at 156 kHz, far above hearing), 8000 times a second.
static Mixer mixer;
static constexpr uint32_t PcmLead = 400; // 50 ms: longer than the longest frame or sleep between fills
static std::atomic<uint32_t> pcmPlayed, pcmReady; // samples played (interrupt) and mixed (core 0)
static gptimer_handle_t pcmTimer;
static void audioFill();
static void audioOff();

// The ray casting's odd columns on core 1 (mods/13-parallel_columns.diff), while core 0 does the
// even ones; see renderColumns.
struct ColumnJob { RCL_Camera cam; RCL_ArrayFunction arrayFunc, typeFunc; RCL_ColumnFunction columnFunc; RCL_RayConstraints constraints; };
static ColumnJob columnJob;
static SemaphoreHandle_t columnsGo, columnsDone;
static std::atomic<uint32_t> core1BusyUs;

static constexpr uint32_t SleepMs = 2 * 60 * 1000; // Bit-Firmware default auto-sleep: 2 min without input
static constexpr uint32_t DimMs = SleepMs - 10 * 1000; // dim the screen 10 s before, as a warning

static constexpr uint8_t Brightness = 100; // 0-100, Bit-Firmware default screenBrightness
static uint32_t blDuty; // current backlight duty, 0 = off
static uint32_t blFull; // duty for Brightness

extern const uint8_t splashStart[] asm("_binary_Splash_bmp_start"); // Bit-Firmware spiffs_image/Splash.bmp
extern const uint8_t splashEnd[] asm("_binary_Splash_bmp_end");

// Quicksave partition: two snapshots, each [firmware ELF SHA-256][SFG_Game][SFG_Player][SFG_CurrentLevel]: the
// player's quicksave at 0, the game to resume (resume.h) at ResumeAt. A snapshot holds pointers into this build's
// flash, so only the exact same build loads it. The hash goes in last: a torn write never loads.
static constexpr size_t QsHash = 32, QsPlayer = QsHash + sizeof(SFG_Game), QsLevel = QsPlayer + sizeof(SFG_Player),
	QsEnd = QsLevel + sizeof(SFG_CurrentLevel), ResumeAt = 0x8000;
static_assert(QsEnd <= ResumeAt && ResumeAt + QsEnd <= 64 * 1024, "grow the quicksave partition in partitions.csv");
static bool running; // the game loop runs: shutdown() stores the game to resume
static bool snapshotStore(size_t at, const SFG_Game& game, const SFG_Player& player, const SFG_CurrentLevel& level);

static bool lowBattery; // stock BatteryElement blinks its empty icon below 15 %

static adc_oneshot_unit_handle_t adcUnit;
static adc_channel_t battChan;
static adc_cali_handle_t battCali; // rev 3+ only
static float battOffset; // mV: rev 3+ from the vref calibration, rev 1 from efuse
static float battEma = -1;
static bool battMonitor = true; // off when the chip has no ADC calibration to compute volts from

static uint8_t readRevision(){
	static const esp_efuse_desc_t revField = { EFUSE_BLK3, 32, 8 }; // EfuseMeta.h
	static const esp_efuse_desc_t* revBlob[] = { &revField, nullptr };
	uint8_t rev = 0;
	esp_efuse_read_field_blob(revBlob, &rev, 8);
	return rev;
}

static void initDisplay(uint8_t rev){
	auto b = bus.config();
	b.spi_host = SPI2_HOST;
	b.spi_mode = 0;
	b.freq_write = 40000000;
	b.pin_sclk = pins.sck;
	b.pin_mosi = pins.mosi;
	b.pin_miso = -1;
	b.pin_dc = pins.dc;
	b.dma_channel = SPI_DMA_CH_AUTO;
	bus.config(b);
	panel.setBus(&bus);

	auto p = panel.config();
	p.pin_cs = -1;
	p.pin_rst = pins.rst;
	p.pin_busy = -1;
	p.panel_width = 128;
	p.panel_height = 128;
	p.readable = false;
	p.invert = false;
	p.rgb_order = false;
	p.dlen_16bit = false;
	p.bus_shared = false;
	if(rev == 4){
		p.memory_width = p.memory_height = 128;
		p.offset_x = p.offset_y = 0;
		p.offset_rotation = 0;
	}else{
		p.memory_width = p.memory_height = 132;
		p.offset_x = 2;
		p.offset_y = 1;
		p.offset_rotation = rev == 3 ? 0 : 2;
	}
	panel.config(p);
	lcd.setPanel(&panel);
	lcd.init();
	lcd.fillScreen(0);
}

static void initBacklight(){
	ledc_timer_config_t t = {};
	t.speed_mode = LEDC_LOW_SPEED_MODE;
	t.duty_resolution = LEDC_TIMER_10_BIT;
	t.timer_num = LEDC_TIMER_1; // own timer: the buzzer keeps changing LEDC_TIMER_0's frequency
	t.freq_hz = 5000;
	t.clk_cfg = LEDC_USE_XTAL_CLK; // S3: one clock source for all LEDC timers, see initBuzzer
	ESP_ERROR_CHECK(ledc_timer_config(&t));

	ledc_channel_config_t c = {};
	c.gpio_num = pins.bl;
	c.speed_mode = LEDC_LOW_SPEED_MODE;
	c.channel = LEDC_CHANNEL_1;
	c.timer_sel = LEDC_TIMER_1;
	c.duty = 0;
	c.flags.output_invert = 1; // as Bit-Firmware's PWM(LedBl, invert): duty 0 = backlight off
	ESP_ERROR_CHECK(ledc_channel_config(&c));

	const float level = (10 + Brightness * 0.9f) / 100; // BacklightBrightness::mapDuty: min 10%, squared
	blFull = std::lround(level * level * 100) * 1024 / 100;
}

static void backlightSet(uint32_t duty){
	blDuty = duty;
	ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
	ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

// BacklightBrightness::fadeIn/fadeOut: 100 steps of 2 ms, here from the current duty to any other.
static void backlightFade(uint32_t to){
	const int from = blDuty;
	if(from == (int) to) return;
	for(int i = 1; i <= 100; i++){
		backlightSet(from + ((int) to - from) * i / 100);
		vTaskDelay(pdMS_TO_TICKS(2));
		audioFill(); // the fade takes 200 ms, longer than the sound buffered ahead
	}
}

// Same as Bit-Firmware shutdown(): park the pins so nothing leaks current, then sleep until power cycle.
[[noreturn]] static void shutdown(){
	audioOff();
	if(running && resumable()) printf("Resume %s\n", snapshotStore(ResumeAt, SFG_game, SFG_player, SFG_currentLevel) ? "stored" : "store failed");
	backlightFade(0);
	const struct { int pin, level; gpio_pull_mode_t pull; } park[] = {
		{ pins.bl, 1, GPIO_PULLUP_ONLY }, { pins.buzz, 0, GPIO_PULLDOWN_ONLY }, { pins.vref, 0, GPIO_PULLDOWN_ONLY } };
	for(const auto& p : park){
		if(p.pin < 0) continue;
		gpio_set_direction((gpio_num_t) p.pin, GPIO_MODE_OUTPUT);
		gpio_set_level((gpio_num_t) p.pin, p.level);
		gpio_set_pull_mode((gpio_num_t) p.pin, p.pull);
		gpio_hold_en((gpio_num_t) p.pin);
	}
	esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
	gpio_deep_sleep_hold_en();
	esp_deep_sleep_start();
}

static bool readBatt(float& mV){
	int v = 0;
	const esp_err_t err = battCali ? adc_oneshot_get_calibrated_result(adcUnit, battCali, battChan, &v) : adc_oneshot_read(adcUnit, battChan, &v);
	if(err == ESP_OK) mV = v;
	return err == ESP_OK;
}

static void sampleBatt(){
	if(!battMonitor) return;
	float v;
	if(readBatt(v)) battEma = battEma < 0 ? v : battEma * 0.95f + v * 0.05f;
}

// Battery percentage exactly as Bit-Firmware BatteryRev3 / BatteryRev1 (ADCReader) compute it.
static float battPercent(){
	if(!battMonitor || battEma < 0) return 100; // unknown (no calibration, or no reading yet): never shut down for it
	float mV, lo, hi;
	if(battCali){
		mV = (int) (-1.1316f * (377.595f - (battEma * 4 + battOffset)));
		lo = 3600; hi = 4150;
	}else{
		mV = battEma + battOffset;
		lo = 2870; hi = 3820;
	}
	return std::clamp((mV - lo) / (hi - lo), 0.0f, 1.0f) * 100.0f;
}

static void initBattery(){
	adc_unit_t unit;
	ESP_ERROR_CHECK(adc_oneshot_io_to_channel(pins.batt, &unit, &battChan));
	adc_oneshot_unit_init_cfg_t u = {};
	u.unit_id = unit;
	ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &adcUnit));
	adc_oneshot_chan_cfg_t c = {};
	c.atten = pins.vref >= 0 ? ADC_ATTEN_DB_2_5 : ADC_ATTEN_DB_12;
	c.bitwidth = ADC_BITWIDTH_12;
	ESP_ERROR_CHECK(adc_oneshot_config_channel(adcUnit, battChan, &c));

	if(pins.vref >= 0){
		adc_cali_curve_fitting_config_t cc = {};
		cc.unit_id = unit;
		cc.chan = battChan;
		cc.atten = c.atten;
		cc.bitwidth = c.bitwidth;
		// Bit-Firmware aborts here; raw readings would give nonsense volts and could trip the
		// low-battery shutdown at every boot, so run without battery monitoring instead.
		if(adc_cali_create_scheme_curve_fitting(&cc, &battCali) != ESP_OK){
			printf("No ADC calibration in eFuse: battery monitoring off\n");
			battMonitor = false;
			return;
		}

		// Switch the 2.5 V reference onto the battery pin and measure the ADC offset (BatteryRev3::calibrate).
		const auto vref = (gpio_num_t) pins.vref;
		gpio_set_direction(vref, GPIO_MODE_OUTPUT);
		gpio_set_level(vref, 1);
		vTaskDelay(pdMS_TO_TICKS(100));
		float v = 0, total = 0;
		for(int i = 0; i < 20; i++){
			readBatt(v);
			if(i >= 10) total += v * 4;
			vTaskDelay(pdMS_TO_TICKS(10));
		}
		battOffset = 2500 - total / 10;
		gpio_set_level(vref, 0);
		vTaskDelay(pdMS_TO_TICKS(100));
	}else{
		static const esp_efuse_desc_t offField = { EFUSE_BLK3, 0, 16 }; // BatteryRev1::getVoltOffset
		static const esp_efuse_desc_t* offBlob[] = { &offField, nullptr };
		int16_t off = 0;
		esp_efuse_read_field_blob(offBlob, &off, 16);
		battOffset = off;
	}
	sampleBatt();
}

static void buzzerDuty(uint32_t duty){
	ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
	ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// One sample per tick, from the ring core 0 fills; if it ever runs dry, the last level holds.
// Writes the duty registers directly: the three writes ledc_set_duty/ledc_update_duty make,
// without the driver's lock, cheap enough for 8000 interrupts a second.
static bool IRAM_ATTR pcmTick(gptimer_handle_t, const gptimer_alarm_event_data_t*, void*){
	const uint32_t n = pcmPlayed.load(std::memory_order_relaxed);
	if(n == pcmReady.load(std::memory_order_acquire)) return false;
	ledc_dev_t* hw = LEDC_LL_GET_HW();
	ledc_ll_set_duty_int_part(hw, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, mixer.ring[n % Mixer::RingSize]);
	ledc_ll_set_duty_start(hw, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, true);
	ledc_ll_ls_channel_update(hw, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
	pcmPlayed.store(n + 1, std::memory_order_release);
	return false;
}

// Core 0 only (the mixer runs Anarch's music and reads its state): mixes up to PcmLead ahead.
static void audioFill(){
	mixer.fill(pcmPlayed.load(std::memory_order_acquire) + PcmLead);
	pcmReady.store(mixer.written, std::memory_order_release);
}

static void audioOff(){
	if(!pcmTimer) return; // not started yet (an empty battery at boot): the buzzer was never driven
	gptimer_stop(pcmTimer);
	buzzerDuty(0);
}

// Pinned to core 1: starts the sound interrupt there (allocated on the core that enables it, away
// from the game), then casts the odd columns of each frame renderColumns hands over.
static void columnTask(void*){
	gptimer_config_t t = {};
	t.clk_src = GPTIMER_CLK_SRC_DEFAULT;
	t.direction = GPTIMER_COUNT_UP;
	t.resolution_hz = 1000000;
	ESP_ERROR_CHECK(gptimer_new_timer(&t, &pcmTimer));
	gptimer_event_callbacks_t cb = {};
	cb.on_alarm = pcmTick;
	ESP_ERROR_CHECK(gptimer_register_event_callbacks(pcmTimer, &cb, nullptr));
	ESP_ERROR_CHECK(gptimer_enable(pcmTimer));
	gptimer_alarm_config_t alarm = {};
	alarm.alarm_count = 1000000 / Mixer::Rate;
	alarm.flags.auto_reload_on_alarm = true;
	ESP_ERROR_CHECK(gptimer_set_alarm_action(pcmTimer, &alarm));
	ESP_ERROR_CHECK(gptimer_start(pcmTimer));

	while(true){
		xSemaphoreTake(columnsGo, portMAX_DELAY);
		const int64_t start = esp_timer_get_time();
		const ColumnJob& j = columnJob;
		RCL_castRaysMultiHitColumns(j.cam, j.arrayFunc, j.typeFunc, j.columnFunc, j.constraints, 1, 2);
		core1BusyUs += esp_timer_get_time() - start;
		xSemaphoreGive(columnsDone);
	}
}

// RCL_renderComplex's column loop, split: each column only reads the level and writes its own
// pixels and depth-buffer entries, so the two halves never touch the same data (the smoke test
// checks the picture is exactly the one-core picture). Sprites and the HUD follow on core 0.
void renderColumns(RCL_Camera cam, RCL_ArrayFunction arrayFunc, RCL_ArrayFunction typeFunc, RCL_ColumnFunction columnFunc,
	RCL_RayConstraints constraints){
	columnJob = { cam, arrayFunc, typeFunc, columnFunc, constraints };
	xSemaphoreGive(columnsGo);
	RCL_castRaysMultiHitColumns(cam, arrayFunc, typeFunc, columnFunc, constraints, 0, 2);
	xSemaphoreTake(columnsDone, portMAX_DELAY);
}

static void initBuzzer(){
	ledc_timer_config_t t = {};
	t.speed_mode = LEDC_LOW_SPEED_MODE;
	t.duty_resolution = LEDC_TIMER_10_BIT;
	t.timer_num = LEDC_TIMER_0;
	t.duty_resolution = LEDC_TIMER_8_BIT; // the PCM samples' 8 bits, 40 MHz / 256 = 156.25 kHz
	t.freq_hz = 156250;
	t.clk_cfg = LEDC_USE_XTAL_CLK; // S3: one clock source for all LEDC timers (the backlight's too)
	ESP_ERROR_CHECK(ledc_timer_config(&t));

	ledc_channel_config_t c = {};
	c.gpio_num = pins.buzz;
	c.speed_mode = LEDC_LOW_SPEED_MODE;
	c.channel = LEDC_CHANNEL_0;
	c.timer_sel = LEDC_TIMER_0;
	c.duty = 0;
	ESP_ERROR_CHECK(ledc_channel_config(&c));
	buzzerDuty(0); // also sets the duty step registers pcmTick leaves as they are
}

IRAM_ATTR void SFG_setPixel(uint16_t x, uint16_t y, uint8_t colorIndex){
	frame[y * SFG_SCREEN_RESOLUTION_X + x] = palette[colorIndex];
}

uint32_t SFG_getTimeMs(){
	return esp_timer_get_time() / 1000;
}

void SFG_sleepMs(uint16_t timeMs){
	slept = true;
	vTaskDelay(pdMS_TO_TICKS(timeMs));
}

int8_t SFG_keyPressed(uint8_t key){
	return key < 7 && gpio_get_level(keyPins[key]) == 0; // active low, pulled up
}

void SFG_getMouseOffset(int16_t* x, int16_t* y){}

void SFG_setMusic(uint8_t value){
	if(value == SFG_MUSIC_NEXT) SFG_nextMusicTrack();
	else mixer.music = value == SFG_MUSIC_TURN_ON;
}

void SFG_processEvent(uint8_t event, uint8_t data){
	if(!(SFG_game.settings & 0x01)) return; // Anarch's sound setting, checked like SFG_playGameSound does
	switch(event){
		case SFG_EVENT_PLAYER_TAKES_ITEM: mixer.chirp(ChirpGood, 255); break;
		case SFG_EVENT_LEVEL_WON: mixer.chirp(ChirpWin, 255); break;
		case SFG_EVENT_PLAYER_DIES: mixer.chirp(ChirpLose, 255); break;
		default: break;
	}
}

void SFG_playSound(uint8_t soundIndex, uint8_t volume){
	mixer.sound(soundIndex, volume);
}

static const esp_partition_t* quickSavePartition(){
	const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t) 0x40, "quicksave");
	return p && p->size >= ResumeAt + QsEnd ? p : nullptr;
}

static bool snapshotStore(size_t at, const SFG_Game& game, const SFG_Player& player, const SFG_CurrentLevel& level){
	const esp_partition_t* p = quickSavePartition();
	return p && esp_partition_erase_range(p, at, (QsEnd + 4095) & ~4095u) == ESP_OK
		&& esp_partition_write(p, at + QsHash, &game, sizeof(SFG_Game)) == ESP_OK
		&& esp_partition_write(p, at + QsPlayer, &player, sizeof(SFG_Player)) == ESP_OK
		&& esp_partition_write(p, at + QsLevel, &level, sizeof(SFG_CurrentLevel)) == ESP_OK
		&& esp_partition_write(p, at, esp_app_get_description()->app_elf_sha256, QsHash) == ESP_OK;
}

// False if there is none this build wrote, or it couldn't be read whole.
static bool snapshotLoad(size_t at, SFG_Game& game, SFG_Player& player, SFG_CurrentLevel& level){
	const esp_partition_t* p = quickSavePartition();
	uint8_t hash[QsHash];
	return p && esp_partition_read(p, at, hash, QsHash) == ESP_OK
		&& memcmp(hash, esp_app_get_description()->app_elf_sha256, QsHash) == 0
		&& esp_partition_read(p, at + QsHash, &game, sizeof(SFG_Game)) == ESP_OK
		&& esp_partition_read(p, at + QsPlayer, &player, sizeof(SFG_Player)) == ESP_OK
		&& esp_partition_read(p, at + QsLevel, &level, sizeof(SFG_CurrentLevel)) == ESP_OK;
}

static void quickSaveStore(){
	const bool ok = snapshotStore(0, SFG_quickSaveGame, SFG_quickSavePlayer, SFG_quickSaveCurrentLevel);
	printf("Quicksave %s\n", ok ? "stored" : "store failed, kept in RAM only");
}

// After SFG_init (which marks "no quicksave"): bring back the stored one, if this build wrote it.
static void quickSaveLoad(){
	if(!snapshotLoad(0, SFG_quickSaveGame, SFG_quickSavePlayer, SFG_quickSaveCurrentLevel)) SFG_quickSaveGame.frame = 0;
}

// The game stored at the last auto-sleep or low-battery shutdown, resumed once: its hash is erased
// before it resumes, so after a later power cut (the power switch skips shutdown) the game starts
// afresh, not back here. An erase that fails means no resume, never one over and over.
static void resume(){
	const bool ok = resumeGame([](SFG_Game& game, SFG_Player& player, SFG_CurrentLevel& level){
		return snapshotLoad(ResumeAt, game, player, level)
			&& esp_partition_erase_range(quickSavePartition(), ResumeAt, 0x1000) == ESP_OK;
	}, quickSaveLoad);
	if(ok) printf("Resumed\n");
}

// Small blinking red battery in the top-right corner of the frame about to be sent.
static void drawLowBattery(uint32_t now){
	if(!lowBattery || now / 500 % 2) return;
	const uint16_t red = __builtin_bswap16(0xF800), white = __builtin_bswap16(0xFFFF);
	constexpr int x0 = 114, y0 = 3, w = 10, h = 6; // body; the 1 px terminal nub sits right of it
	for(int y = 0; y < h; y++)
		for(int x = 0; x <= w; x++){
			const bool edge = x == 0 || x == w - 1 || y == 0 || y == h - 1;
			uint16_t c;
			if(x == w) { if(y < 2 || y > 3) continue; c = white; }
			else c = edge ? white : x <= 2 ? red : 0;
			frame[(y0 + y) * SFG_SCREEN_RESOLUTION_X + x0 + x] = c;
		}
}

void SFG_save(uint8_t data[SFG_SAVE_SIZE]){
	nvs_handle_t h;
	if(nvs_open("anarch", NVS_READWRITE, &h) != ESP_OK) return;
	nvs_set_blob(h, "save", data, SFG_SAVE_SIZE);
	nvs_commit(h);
	nvs_close(h);
}

uint8_t SFG_load(uint8_t data[SFG_SAVE_SIZE]){
	nvs_handle_t h;
	size_t len = SFG_SAVE_SIZE;
	if(nvs_open("anarch", NVS_READONLY, &h) == ESP_OK){ // fails until the first save: leave data as is
		nvs_get_blob(h, "save", data, &len);
		nvs_close(h);
	}
	return 1;
}

extern "C" void app_main(){
	esp_err_t err = nvs_flash_init();
	if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND){
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	gpio_force_unhold_all(); // pins stay latched from a previous low-battery shutdown otherwise

	const uint8_t rev = readRevision();
	pins = rev >= 3 ? PinsRev3 : PinsRev1;

	// Stock thresholds: Critical is below 5% at boot (no screen, straight off), below 1% while running.
	// shortcut: two fixed thresholds instead of stock's 8-level Hysteresis; same result for a slowly falling EMA.
	initBattery();
	if(battPercent() < 5) shutdown();

	const int keys[7] = { pins.up, pins.right, pins.down, pins.left, pins.a, pins.b, pins.menu };
	uint64_t mask = 0;
	for(int i = 0; i < 7; i++){
		keyPins[i] = (gpio_num_t) keys[i];
		mask |= 1ULL << keys[i];
	}
	gpio_config_t io = {};
	io.pin_bit_mask = mask;
	io.mode = GPIO_MODE_INPUT;
	io.pull_up_en = GPIO_PULLUP_ENABLE;
	ESP_ERROR_CHECK(gpio_config(&io));

	for(int i = 0; i < 256; i++) palette[i] = __builtin_bswap16(paletteRGB565[i]);

	initBacklight(); // dark while the panel initializes
	initDisplay(rev);
	initBuzzer();
	columnsGo = xSemaphoreCreateBinary();
	columnsDone = xSemaphoreCreateBinary();
	if(!columnsGo || !columnsDone || xTaskCreatePinnedToCore(columnTask, "columns", 8192, nullptr, 5, nullptr, 1) != pdPASS){
		printf("Not enough RAM to start core 1\n");
		shutdown();
	}

	// Stock boot: splash, fade in, jingle, splash stays at least 2 s, fade out, then the UI fades in.
	lcd.drawBmp(splashStart, splashEnd - splashStart, 36, 11);
	backlightFade(blFull);
	const uint32_t splashTime = SFG_getTimeMs();
	SFG_init();
	quickSaveLoad();
	if(SFG_game.settings & 0x01) mixer.chirp(ChirpBoot, 255);
	while(SFG_getTimeMs() - splashTime < 2000){
		audioFill();
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	backlightFade(0);

	SFG_game.frameTime = SFG_getTimeMs(); // SFG_init set 0: don't catch up on the splash time in the first frame
	resume(); // after frameTime: the level clock is rebased to it
	running = true;
	lcd.startWrite(); // keep the bus so pushImageDMA returns while the transfer runs
	bool lit = false, dimmed = false;
	uint32_t tickTime = SFG_getTimeMs(), activityTime = tickTime, statTime = tickTime, busyUs = 0, framesDrawn = 0;
	while(true){
		const uint32_t now = SFG_getTimeMs();
		// Shown by `idf.py monitor` every second: core 0's busy share (game, its columns, sending
		// frames; over ~100% and the game can't keep SFG_FPS), core 1's columns, frames drawn.
		if(now - statTime >= 1000){
			printf("CPU load: %d%% (columns on core 1: %d%%), %d FPS\n", (int) (busyUs / (now - statTime) / 10),
				(int) (core1BusyUs.exchange(0) / (now - statTime) / 10), (int) framesDrawn);
			statTime = now;
			busyUs = framesDrawn = 0;
		}
		if(now - tickTime >= 100){
			tickTime = now;

			for(uint8_t k = 0; k < 7; k++) if(SFG_keyPressed(k)) activityTime = now;
			if(now - activityTime >= SleepMs) shutdown();
			if(lit && (now - activityTime >= DimMs) != dimmed){
				dimmed = !dimmed;
				if(dimmed) backlightFade(blFull / 4);
				else backlightSet(blFull); // instantly, the player is back
			}

			sampleBatt();
			lowBattery = battPercent() < 15;
			if(battPercent() < 1){
				audioOff();
				backlightSet(blFull);
				lcd.fillScreen(TFT_BLACK);
				lcd.setTextColor(TFT_WHITE);
				lcd.setTextDatum(lgfx::middle_center);
				lcd.drawString("Battery critical,", 64, 56);
				lcd.drawString("shutting down", 64, 72);
				vTaskDelay(pdMS_TO_TICKS(6000));
				shutdown();
			}
		}

		slept = false;
		const bool init = SFG_game.state == SFG_GAME_STATE_INIT; // waits for A/B release, draws nothing
		const int64_t start = esp_timer_get_time();
		SFG_mainLoopBody();
		audioFill();
		if(init) vTaskDelay(1);
		else if(!slept){
			// LovyanGFX waits for the previous transfer itself; the game then draws into the other buffer.
			drawLowBattery(now);
			lcd.pushImageDMA(0, 0, SFG_SCREEN_RESOLUTION_X, SFG_SCREEN_RESOLUTION_Y, (const lgfx::swap565_t*) frame);
			frame = frame == frames[0] ? frames[1] : frames[0];
			busyUs += esp_timer_get_time() - start;
			framesDrawn++;
			if(!lit){
				lit = true;
				backlightFade(blFull);
			}
			// Behind schedule: the next call renders again without sleeping. Yield a tick so the
			// idle task (and the task watchdog it feeds) still runs on this core.
			if(SFG_getTimeMs() - SFG_game.frameTime >= SFG_MS_PER_FRAME) vTaskDelay(1);
		}
	}
}
