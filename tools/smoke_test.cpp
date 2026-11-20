// Plays every level of the patched game with random input under ASan/UBSan, with the Bit's
// settings, and checks the quicksave behaviour of mods/12-quicksave_store.diff. Run by smoke_test.sh.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "anarch_settings.h"

static int stores;
static void store(){ stores++; }
#define SFG_QUICKSAVE_STORE store
#include "game.h"

#define CHECK(cond) do { if(!(cond)){ fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while(0)

static uint32_t now;
static uint8_t keys[7];

void SFG_setPixel(uint16_t x, uint16_t y, uint8_t){ CHECK(x < SFG_SCREEN_RESOLUTION_X && y < SFG_SCREEN_RESOLUTION_Y); }
uint32_t SFG_getTimeMs(){ return now; }
void SFG_sleepMs(uint16_t){}
int8_t SFG_keyPressed(uint8_t k){ return k < 7 ? keys[k] : 0; }
void SFG_getMouseOffset(int16_t*, int16_t*){}
void SFG_setMusic(uint8_t){}
void SFG_processEvent(uint8_t, uint8_t){}
void SFG_playSound(uint8_t, uint8_t){}
void SFG_save(uint8_t*){}
uint8_t SFG_load(uint8_t*){ return 1; }

int main(){
	srand(1);
	SFG_init();
	for(uint8_t level = 0; level < SFG_NUMBER_OF_LEVELS; level++){
		SFG_setAndInitLevel(level);
		SFG_setGameState(SFG_GAME_STATE_PLAYING);
		RCL_Vector2D savedPosition{};
		for(int f = 0; f < 6000; f++){ // 100 s of game time per level
			if(f % 15 == 0) for(auto& k : keys) k = rand() % 3 == 0;
			keys[SFG_KEY_C] = 0; // stay out of the menu
			if(f == 2000){
				SFG_quickSave();
				savedPosition = SFG_player.camera.position;
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
				SFG_game.save[2] ^= 0x5a;
				SFG_game.settings ^= 0x02;
			}
			if(SFG_game.state != SFG_GAME_STATE_PLAYING) SFG_setGameState(SFG_GAME_STATE_PLAYING);
			now += 1000 / SFG_FPS + (f % 3 != 0);
			SFG_mainLoopBody();
		}
		printf("level %d: ok, health %d\n", level + 1, SFG_player.health);
	}
	CHECK(stores == SFG_NUMBER_OF_LEVELS); // every quicksave reached the front end's storage hook
	printf("smoke test passed\n");
}
