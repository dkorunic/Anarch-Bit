// Anarch settings for the Bit, shared by the firmware (main.cpp) and the demo GIF tool (tools/demo.cpp).
#pragma once

#define SFG_SCREEN_RESOLUTION_X 128
#define SFG_SCREEN_RESOLUTION_Y 128
#define SFG_FPS 60
#define SFG_RAYCASTING_MAX_STEPS 40
#define SFG_RAYCASTING_SUBSAMPLE 1 // full horizontal resolution; set 2 if the CPU load print shows >100%
#define SFG_DIMINISH_SPRITES 1
#define SFG_DITHERED_SHADOW 1
#define SFG_CAN_EXIT 0
#define SFG_PARTICLE_SIZE 2 // particle_effects mod; its default 4 px is sized for bigger screens
