// Anarch's sound as 8 kHz 8-bit PCM for the Bit's buzzer: its sampled effects and bytebeat music,
// mixed as Anarch's SDL front end mixes them, plus the stock firmware's chirps as a square wave.
// main.cpp's timer interrupt plays `ring` as PWM duty. No ESP-IDF dependencies, so
// tools/smoke_test.cpp checks it too. Include game.h first.
#pragma once

#include <algorithm>
#include <cstdint>
#include "sounds.h" // Anarch's effects and music; needs game.h

// Chirps as frequency sweeps, the way Bit-Firmware's ChirpSystem plays them: { start Hz, end Hz, ms }.
// 0 Hz is a pause; a chirp ends at its first 0 ms entry.
struct Chirp { int16_t from, to, ms; };
constexpr int MaxChirps = 10;
enum { ChirpGood, ChirpWin, ChirpLose, ChirpBoot };
constexpr Chirp Chirps[][MaxChirps] = {
	{ { 400, 600, 50 } },  // ChirpGood: RoboCtrl::Buttons::playGood
	{ { 600, 400, 200 }, { 400, 1000, 200 } }, // ChirpWin: Buttons::playWin
	{ { 400, 300, 200 }, { 0, 0, 50 }, { 300, 200, 200 }, { 0, 0, 50 }, { 200, 50, 400 } }, // ChirpLose: Buttons::playLose
	{ { 294, 440, 100 }, { 0, 0, 50 }, { 587, 587, 100 }, { 0, 0, 50 }, { 440, 440, 100 }, // ChirpBoot: stock boot jingle
	  { 0, 0, 50 }, { 784, 392, 100 }, { 0, 0, 50 }, { 294, 440, 100 }, { 587, 587, 100 } },
};

// Music gain per track, in 1/64ths, after the bass cut (Mixer::bassCut): each bytebeat track at
// ~22 RMS of the 128 the output swings, level with what the buzzer plays of a full-volume effect
// (~21 RMS above 250 Hz). Measured on the 8-bit output itself over each track's first 30 s, clipping
// included: the cut leaves sharp edges, 0.5-1.7% of samples clip. The SDL front end's flat x16
// without the cut was barely audible on the buzzer: 70-99% of the music's energy is below 250 Hz.
constexpr int16_t MusicGain[] = { 380, 996, 395, 485, 921, 270 };
static_assert(sizeof(MusicGain) / sizeof(*MusicGain) == SFG_TRACK_COUNT, "one gain per music track");

struct Mixer {
	static constexpr uint32_t Rate = 8000, RingSize = 1024; // the ring holds 128 ms
	uint8_t ring[RingSize] = {}; // sample n is ring[n % RingSize]
	uint32_t written = 0; // samples generated so far
	bool music = false;

	// One of Anarch's sound effects, from the next generated sample on; volume as SFG_playSound's,
	// louder in powers of two as the SDL front end plays it.
	void sound(uint8_t index, uint8_t volume){
		const int32_t scale = 1 << (volume / 37);
		for(uint32_t i = 0; i < SFG_SFX_SAMPLE_COUNT; i++)
			sfx[(written + i) % SFG_SFX_SAMPLE_COUNT] += (128 - SFG_GET_SFX_SAMPLE(index, i)) * scale;
		sfxEnd = written + SFG_SFX_SAMPLE_COUNT;
	}

	void chirp(int index, uint8_t volume){
		chirpAt = Chirps[index];
		seg = segSample = 0;
		amp = volume * 32; // full volume: as loud as a full-volume sound effect
	}

	// Generates samples up to (not including) sample number upTo.
	void fill(uint32_t upTo){
		for(; int32_t(upTo - written) > 0; written++){
			int32_t mix = sfx[written % SFG_SFX_SAMPLE_COUNT];
			sfx[written % SFG_SFX_SAMPLE_COUNT] = 0;
			if(music){
				const int32_t sample = SFG_getNextMusicSample(); // may move on to the next track
				const uint8_t track = SFG_MusicState.track;
				mix += int32_t(MusicGain[track] * bassCut(sample - SFG_musicTrackAverages[track]));
			}
			mix += chirpSample();
			// The buzzer idles at duty 0 (no current, no hiss) and centres on 50% while anything
			// plays: the bias ramps between them over 16 ms, so starting and stopping doesn't click.
			const bool active = music || chirpAt || int32_t(sfxEnd - written) > 0;
			bias += active ? bias < 128 : -(bias > 0);
			ring[written % RingSize] = std::clamp<int32_t>(bias + (mix >> 6), 0, 255);
		}
	}

private:
	int32_t sfx[SFG_SFX_SAMPLE_COUNT] = {}; // effects ahead, summed (int32: overlapping ones can't wrap)
	uint32_t sfxEnd = 0; // the last effect plays until here
	const Chirp* chirpAt = nullptr;
	int seg = 0;
	uint32_t segSample = 0, phase = 0;
	int32_t amp = 0, bias = 0;
	float hpIn = 0, hp1 = 0, hp2 = 0;

	// Two one-pole high-passes at 400 Hz (12 dB per octave below it): the bass a buzzer can't play
	// would only take up the output's range, so the music keeps the rest, made louder by MusicGain.
	float bassCut(float x){
		constexpr float A = 1 / (1 + 2 * 3.14159265f * 400 / Rate);
		const float y1 = A * (hp1 + x - hpIn);
		hpIn = x;
		hp2 = A * (hp2 + y1 - hp1);
		hp1 = y1;
		return hp2;
	}

	int32_t chirpSample(){
		if(!chirpAt) return 0;
		const Chirp& c = chirpAt[seg];
		const uint32_t length = c.ms * Rate / 1000;
		int32_t v = 0;
		if(c.from){
			const uint32_t hz = c.from + (c.to - c.from) * int32_t(segSample) / int32_t(length);
			phase += uint32_t(hz * (0x100000000ull / Rate)); // 2^32 = one period
			v = phase >> 31 ? amp : -amp;
		}
		if(++segSample == length){
			segSample = 0;
			if(++seg == MaxChirps || chirpAt[seg].ms == 0) chirpAt = nullptr;
		}
		return v;
	}
};
