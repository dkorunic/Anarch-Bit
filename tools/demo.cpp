// Plays a scripted demo of the patched game with the Bit's settings on the PC and writes the frames
// for make_demo_gif.py. Built and run by make_demo.sh; see README.
#include <cstdio>
#include <cstdint>
#include <cmath>
#include "anarch_settings.h"
#include "game.h"

// Bits: 0 up, 1 right, 2 down, 3 left, 4 A, 5 B, 6 C (= Bit's Menu button), as SFG_KEY_*; Auto: the
// keys autopilot() picks.
enum : uint8_t { U = 1, R = 2, D = 4, L = 8, A = 16, B = 32, C = 64, Auto = 128 };
struct Step { int frames; uint8_t keys; };
// Level 1, played by autopilot() once the level starts: west over the bullets (the shotgun), back and
// north, shooting the two barrels; then north and up the ramp, killing the warrior there, and through
// the door beyond. The autopilot steers by the player's position, so it holds at any SFG_FPS; checked
// by watching the result (contact sheets of the frames). Change a mod, setting or the level and it may
// need retuning. Deterministic; the run is checked to its end.
static const Step script[] = { // durations in 1/60 s, whatever SFG_FPS is
	{ 90, 0 }, { 6, A },  // main menu, play
	{ 90, 0 }, { 6, A },  // intro text, skip
	{ 90, 0 }, { 6, A },  // level start
	{ 40, 0 },
	{ 1052, Auto },       // 17.5 s: ends 1 s after the route, before a far monster comes in range
};

// Squares (x, y) the autopilot walks through, in order (level 1's map: tools need a dump of
// SFG_floorHeightAt to plan): the bullets west of the start, a spot south of the barrels, north
// between the lamps, east and up the ramp along row 41, the door at 46,37 (it opens as the player
// comes close), the corridor beyond. Straight lines between them stay clear of the walls, the
// raised blocks and the lamps.
struct Point { float x, y; };
static constexpr Point route[] = { { 48.6f, 55.5f }, { 52.5f, 53.5f }, { 52.5f, 46.5f }, { 52.5f, 44.5f }, { 58.5f, 44.5f },
	{ 58.5f, 41.5f }, { 50.5f, 41.5f }, { 46.5f, 38.5f }, { 46.5f, 35.5f }, { 46.5f, 30.5f } };
static size_t waypoint;

// Nothing between the two points higher than half a square above the player's floor (a wall, a
// raised block, a closed door), sampled every 1/8 square.
static bool clearLine(float x0, float y0, float x1, float y1){
	const RCL_Unit floor = SFG_floorHeightAt(x0, y0);
	const int n = std::hypot(x1 - x0, y1 - y0) * 8;
	for(int i = 1; i < n; i++){
		const float x = x0 + (x1 - x0) * i / n, y = y0 + (y1 - y0) * i / n;
		const RCL_Unit f = SFG_floorHeightAt(x, y), c = SFG_ceilingHeightAt(x, y);
		if(f - floor > RCL_UNITS_PER_SQUARE / 2 || c - f < RCL_UNITS_PER_SQUARE / 2) return false;
	}
	return true;
}

// How far to turn to face (dx, dy), in Anarch's angle units (1024 per turn, -512..511; Right adds).
// Its direction vector for angle a is (cos a, -sin a).
static int turnTo(float dx, float dy){
	const int want = std::lround(std::atan2(-dy, dx) * 1024 / (2 * M_PI));
	return ((want - (int) SFG_player.camera.direction) % 1024 + 1536) % 1024 - 512;
}

// The nearest monster that isn't dead or dying, or barrel, within 5 squares in a clear line: turn
// to it, fire once facing it. Otherwise follow the route, walking while roughly facing the next
// point. Only while playing (no keys on a level-start or death screen).
static uint8_t autopilot(){
	if(SFG_game.state != SFG_GAME_STATE_PLAYING) return 0;
	const float px = SFG_player.camera.position.x / float(RCL_UNITS_PER_SQUARE), py = SFG_player.camera.position.y / float(RCL_UNITS_PER_SQUARE);
	float nearest = 5, tx = 0, ty = 0;
	auto consider = [&](float x, float y){
		const float d = std::hypot(x - px, y - py);
		if(d < nearest && clearLine(px, py, x, y)){
			nearest = d;
			tx = x;
			ty = y;
		}
	};
	for(int i = 0; i < SFG_currentLevel.monsterRecordCount; i++){
		const SFG_MonsterRecord& m = SFG_currentLevel.monsterRecords[i];
		if(SFG_MR_STATE(m) != SFG_MONSTER_STATE_DEAD && SFG_MR_STATE(m) != SFG_MONSTER_STATE_DYING)
			consider(m.coords[0] / 4.0f, m.coords[1] / 4.0f); // coords: 1/4 squares
	}
	for(int i = 0; i < SFG_currentLevel.itemRecordCount; i++){ // items still in the level; a barrel goes when shot
		const SFG_LevelElement& e = SFG_currentLevel.levelPointer->elements[SFG_currentLevel.itemRecords[i] & ~SFG_ITEM_RECORD_ACTIVE_MASK];
		if(e.type == SFG_LEVEL_ELEMENT_BARREL) consider(e.coords[0] + 0.5f, e.coords[1] + 0.5f);
	}
	constexpr int step = SFG_PLAYER_TURN_UNITS_PER_FRAME; // one frame's turn
	if(nearest < 5){
		const int turn = turnTo(tx - px, ty - py);
		return turn > step ? R : turn < -step ? L : A;
	}
	constexpr size_t points = sizeof(route) / sizeof(*route);
	while(waypoint < points && std::hypot(route[waypoint].x - px, route[waypoint].y - py) < 0.4f) waypoint++;
	if(waypoint == points) return 0;
	const int turn = turnTo(route[waypoint].x - px, route[waypoint].y - py);
	return (turn > step ? R : turn < -step ? L : 0) | (std::abs(turn) < 96 ? U : 0);
}

static uint8_t screen[SFG_SCREEN_RESOLUTION_X * SFG_SCREEN_RESOLUTION_Y];
static uint32_t now;
static uint8_t keys;

void SFG_setPixel(uint16_t x, uint16_t y, uint8_t c){ screen[y * SFG_SCREEN_RESOLUTION_X + x] = c; }
uint32_t SFG_getTimeMs(){ return now; }
void SFG_sleepMs(uint16_t){}
int8_t SFG_keyPressed(uint8_t k){ return k < 7 && (keys >> k & 1); }
void SFG_getMouseOffset(int16_t*, int16_t*){}
void SFG_setMusic(uint8_t){}
void SFG_processEvent(uint8_t, uint8_t){}
void SFG_playSound(uint8_t, uint8_t){}
void SFG_save(uint8_t*){}
uint8_t SFG_load(uint8_t*){ return 0; }

int main(int argc, char** argv){
	if(argc != 3){
		fprintf(stderr, "usage: %s frames.raw palette.raw\n", argv[0]);
		return 1;
	}
	FILE* pal = fopen(argv[2], "wb");
	FILE* out = fopen(argv[1], "wb");
	if(!pal || !out){
		perror("fopen");
		return 1;
	}
	for(uint16_t c : paletteRGB565){ // RGB565 -> RGB888
		const uint8_t rgb[3] = { uint8_t((c >> 11) * 255 / 31), uint8_t((c >> 5 & 63) * 255 / 63), uint8_t((c & 31) * 255 / 31) };
		fwrite(rgb, 1, 3, pal);
	}

	SFG_init();
	int frame = 0;
	uint32_t nextCapture = 0, scriptTime = 0; // ms
	for(const Step& s : script){
		scriptTime += s.frames * 1000 / 60;
		for(; now < scriptTime; frame++){
			keys = s.keys == Auto ? autopilot() : s.keys;
			now = (frame + 1) * 1000 / SFG_FPS; // exact game time, no drift
			const bool init = SFG_game.state == SFG_GAME_STATE_INIT; // this call draws nothing
			SFG_mainLoopBody();
			if(now >= nextCapture && !init){ // a 20 FPS GIF at any SFG_FPS
				fwrite(screen, 1, sizeof(screen), out);
				nextCapture += 50;
			}
		}
	}
	fclose(out);
	fclose(pal);
	fprintf(stderr, "%d game frames\n", frame);
	return 0;
}
