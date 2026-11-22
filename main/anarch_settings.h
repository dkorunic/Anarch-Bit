// Anarch settings for the Bit, shared by the firmware (main.cpp) and the demo GIF tool (tools/demo.cpp).
#pragma once

#define SFG_SCREEN_RESOLUTION_X 128
#define SFG_SCREEN_RESOLUTION_Y 128
// 16 ms frames, so 62.5 FPS (Anarch counts whole ms). Measured on a Bit 3 with all mods and both
// cores rendering (mod 13): core 0 busy 67-80% in play; the heaviest fights reach 88-97% and dip to
// 53-58 FPS for a second or two (frames up to ~18 ms; the game keeps its speed, running two logic
// steps per frame drawn). On one core 60 measured 93-131%, and 45 was the most it held.
#define SFG_FPS 60
#define SFG_RAYCASTING_MAX_STEPS 40
#define SFG_RAYCASTING_SUBSAMPLE 1 // full horizontal resolution; 2 halves the ray casting if load is still >100%
#define SFG_DIMINISH_SPRITES 1
#define SFG_DITHERED_SHADOW 1
#define SFG_CAN_EXIT 0
#define SFG_PARTICLE_SIZE 2 // particle_effects mod; its default 4 px is sized for bigger screens
