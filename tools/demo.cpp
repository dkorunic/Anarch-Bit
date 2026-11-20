// Plays a scripted demo of the patched game with the Bit's settings on the PC and writes the frames
// for make_demo_gif.py. Built and run by make_demo.sh; see README.
#include <cstdio>
#include <cstdint>
#include "anarch_settings.h"
#include "game.h"

// Bits: 0 up, 1 right, 2 down, 3 left, 4 A, 5 B, 6 C (= Bit's Menu button), as SFG_KEY_*.
enum : uint8_t { U = 1, R = 2, D = 4, L = 8, A = 16, B = 32, C = 64 };
struct Step { int frames; uint8_t keys; };
// Level 1: grab the shotgun west of the start, come back, shoot the two barrels up north.
// Tuned by watching the result; change a mod or setting that affects movement and it may need retuning.
static const Step script[] = { // durations in 1/60 s, whatever SFG_FPS is
	{ 90, 0 }, { 6, A },  // main menu, play
	{ 90, 0 }, { 6, A },  // intro text, skip
	{ 90, 0 }, { 6, A },  // level start
	{ 40, 0 },
	{ 23, R }, { 40, U }, // turn west, walk over the bullets: shotgun
	{ 61, L }, { 35, U }, // back east
	{ 29, R }, { 30, U }, // north, between the lamps
	{ 20, 0 }, { 80, A }, // two shots: the barrels explode
	{ 90, 0 },
};

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
			keys = s.keys;
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
