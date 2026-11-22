// Plays every level of the patched game with random input under ASan/UBSan, with the Bit's
// settings, letting deaths, wins and level transitions run their course, and checks the quicksave
// behaviour of mods/12-quicksave_store.diff, that rendering the columns on two threads (mod 13, as
// the firmware's two cores do) gives exactly the one-thread picture, and the PCM mixer (audio.h).
// Run by smoke_test.sh.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "anarch_settings.h"

static int stores;
static void store(){ stores++; }
#define SFG_QUICKSAVE_STORE store
#define RCL_CAST_RAYS_PARALLEL castColumns
#include "game.h"
#include "audio.h"

// As main.cpp's renderColumns: odd columns on another thread (core 1), even ones on this one.
static bool parallel = true;
void castColumns(RCL_Camera cam, RCL_ArrayFunction arrayFunc, RCL_ArrayFunction typeFunc, RCL_ColumnFunction columnFunc,
	RCL_RayConstraints constraints){
	if(!parallel) return RCL_castRaysMultiHit(cam, arrayFunc, typeFunc, columnFunc, constraints);
	std::thread odd(RCL_castRaysMultiHitColumns, cam, arrayFunc, typeFunc, columnFunc, constraints, 1, 2);
	RCL_castRaysMultiHitColumns(cam, arrayFunc, typeFunc, columnFunc, constraints, 0, 2);
	odd.join();
}

#define CHECK(cond) do { if(!(cond)){ fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while(0)

static uint32_t now;
static uint8_t keys[7];

static uint8_t screen[SFG_SCREEN_RESOLUTION_Y][SFG_SCREEN_RESOLUTION_X];
void SFG_setPixel(uint16_t x, uint16_t y, uint8_t c){
	CHECK(x < SFG_SCREEN_RESOLUTION_X && y < SFG_SCREEN_RESOLUTION_Y);
	screen[y][x] = c;
}
uint32_t SFG_getTimeMs(){ return now; }
void SFG_sleepMs(uint16_t){}
int8_t SFG_keyPressed(uint8_t k){ return k < 7 ? keys[k] : 0; }
void SFG_getMouseOffset(int16_t*, int16_t*){}
static Mixer mixer; // fed as main.cpp feeds it, so the sanitizers see it during play
void SFG_setMusic(uint8_t v){ if(v == SFG_MUSIC_NEXT) SFG_nextMusicTrack(); else mixer.music = v == SFG_MUSIC_TURN_ON; }
void SFG_processEvent(uint8_t, uint8_t){}
void SFG_playSound(uint8_t i, uint8_t volume){ mixer.sound(i, volume); }
void SFG_save(uint8_t*){}
uint8_t SFG_load(uint8_t*){ return 1; }

// Silence until something plays, then the bias ramp, Anarch's samples, a chirp's pitch, clipping.
static void testMixer(){
	static Mixer m; // static: 8 KB of effects
	m.fill(800);
	for(uint8_t v : m.ring) CHECK(v == 0); // idle: duty 0
	m.sound(0, 255);
	m.fill(m.written + 1);
	CHECK(m.ring[800 % Mixer::RingSize] == std::clamp(1 + (128 - SFG_GET_SFX_SAMPLE(0, 0)), 0, 255)); // bias starts ramping
	m.fill(m.written + 200);
	for(uint32_t n = 928; n < 1000; n++){ // ramped up: 128 + the sample at full volume (x64, then >> 6)
		const uint32_t i = n - 800; // a variable: SFG_GET_SFX_SAMPLE doesn't parenthesise its arguments
		CHECK(m.ring[n % Mixer::RingSize] == std::clamp(128 + (128 - SFG_GET_SFX_SAMPLE(0, i)), 0, 255));
	}
	m.fill(m.written + 2 * SFG_SFX_SAMPLE_COUNT); // the effect ends, the bias ramps back to 0
	CHECK(m.ring[(m.written - 1) % Mixer::RingSize] == 0);

	// ChirpGood: 400 -> 600 Hz in 50 ms, so about 25 periods, 50 level changes
	static Mixer c;
	c.chirp(ChirpGood, 255);
	c.fill(Mixer::Rate * 50 / 1000);
	int changes = 0;
	for(uint32_t n = 1; n < c.written; n++) changes += (c.ring[n] > c.ring[n - 1] + 2) || (c.ring[n] + 2 < c.ring[n - 1]);
	CHECK(changes >= 48 && changes <= 52);

	for(int i = 0; i < 8; i++) m.sound(2, 255); // eight explosions at once: clipped, never wrapped
	m.fill(m.written + 3000);
	m.music = true; // music runs Anarch's bytebeat
	const uint32_t t = SFG_MusicState.t;
	m.fill(m.written + 100);
	CHECK(SFG_MusicState.t == t + 100);
	m.music = false;
}

// Draws the current frame on one thread and on two and checks the pictures are the same.
static bool sameOnTwoThreads(){
	static uint8_t one[sizeof(screen)];
	parallel = false;
	SFG_draw();
	memcpy(one, screen, sizeof(one));
	parallel = true;
	SFG_draw();
	return memcmp(one, screen, sizeof(one)) == 0;
}

int main(){
	testMixer();
	printf("mixer: ok\n");
	srand(1);
	int threadChecks = 0;
	SFG_init();
	int statesSeen = 0;
	for(uint8_t level = 0; level < SFG_NUMBER_OF_LEVELS; level++){
		SFG_setAndInitLevel(level);
		SFG_setGameState(SFG_GAME_STATE_PLAYING);
		RCL_Vector2D savedPosition{};
		uint32_t savedLevelTime = 0;
		int stuck = 0, deaths = 0, wins = 0;
		for(int f = 0; f < 6000; f++){ // 100 s of game time per level
			if(f % 15 == 0) for(auto& k : keys) k = rand() % 3 == 0;
			keys[SFG_KEY_C] = 0; // stay out of the menu
			if(f == 2000){
				SFG_quickSave();
				savedPosition = SFG_player.camera.position;
				savedLevelTime = SFG_game.frameTime - SFG_currentLevel.timeStart;
			}
			if(f == 4000){
				// quickload restores the level but keeps current progress and settings (mod 12)
				SFG_game.save[2] ^= 0x5a;
				SFG_game.settings ^= 0x02;
				uint8_t save[SFG_SAVE_SIZE];
				memcpy(save, SFG_game.save, sizeof(save));
				const uint8_t settings = SFG_game.settings;
				SFG_quickLoad();
				CHECK(memcmp(save, SFG_game.save, sizeof(save)) == 0 && settings == SFG_game.settings);
				CHECK(SFG_player.camera.position.x == savedPosition.x && SFG_player.camera.position.y == savedPosition.y);
				CHECK(SFG_game.frameTime - SFG_currentLevel.timeStart == savedLevelTime); // level clock rebased
				SFG_game.save[2] ^= 0x5a;
				SFG_game.settings ^= 0x02;
			}
			const uint8_t before = SFG_game.state;
			if(f == 5000) SFG_winLevel(); // random play never finishes a level: run the win, save and next-level path
			now += 1000 / SFG_FPS + (f % 3 != 0);
			SFG_mainLoopBody();
			mixer.fill(now * 8); // 8 samples per ms
			if(f % 25 == 0 && SFG_game.state == SFG_GAME_STATE_PLAYING){
				CHECK(sameOnTwoThreads());
				threadChecks++;
			}
			statesSeen |= 1 << SFG_game.state;
			if(SFG_game.state != before){
				deaths += SFG_game.state == SFG_GAME_STATE_LOSE;
				wins += SFG_game.state == SFG_GAME_STATE_WIN;
			}
			// Deaths, wins and level starts play out (random A presses move them on, as a player
			// would); only a game stuck outside play for 5 s gets its level restarted.
			stuck = SFG_game.state == SFG_GAME_STATE_PLAYING ? 0 : stuck + 1;
			if(stuck == 5 * SFG_FPS){
				SFG_setAndInitLevel(level);
				SFG_setGameState(SFG_GAME_STATE_PLAYING);
				stuck = 0;
			}
		}
		printf("level %d: ok, %d deaths, %d wins\n", level + 1, deaths, wins);
	}
	printf("game states reached:");
	for(int st = 0; st < 9; st++) if(statesSeen >> st & 1) printf(" %d", st);
	printf("\n");
	CHECK(statesSeen >> SFG_GAME_STATE_LOSE & 1 && statesSeen >> SFG_GAME_STATE_WIN & 1); // both paths ran
	CHECK(stores == SFG_NUMBER_OF_LEVELS); // every quicksave reached the front end's storage hook
	CHECK(threadChecks > 1000);
	printf("%d frames drawn identically on one and two threads\n", threadChecks);
	printf("smoke test passed\n");
}
