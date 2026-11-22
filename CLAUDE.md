# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Anarch-Bit: ESP-IDF firmware (ESP32-S3, no PSRAM, 8 MB flash) that runs only Anarch, a raycasting
shooter, on a CircuitMess Bit (128x128 ST7735S, 7 buttons, buzzer). Anarch is the git submodule
`anarch/` (szymor/anarch), never edited in place: it is patched by `mods/*.diff` at build time.
README.md covers controls, flashing and user-facing behaviour.

## Commands

Firmware (ESP-IDF 5.x; tested with v5.4.1 in `~/esp/esp-idf`; needs `patch`):

```shell
git submodule update --init
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor     # prints "CPU load: N% (columns on core 1: M%), F FPS" every second
idf.py merge-bin -o anarch-bit-merged.bin   # -> build/anarch-bit-merged.bin (idf.py build doesn't update it)
```

PC tools (no ESP-IDF; run from a plain shell, the ESP-IDF shell's Python lacks Pillow):

```shell
tools/smoke_test.sh                  # the only test: all of it, ~1 min under ASan/UBSan (also in CI)
tools/patch_anarch.sh <dir>          # the patched Anarch tree the firmware is built from
tools/make_demo.sh                   # regenerates demo.gif (Python + Pillow)
web/build.sh <out dir> build/anarch-bit-merged.bin <version>   # browser installer site
```

`tools/smoke_test.cpp` is one program with no per-check selection: `testMixer()`, then every level
played with random input (deaths, a forced win per level, quicksave/quickload of mod 12, a
simulated shutdown and resume via `rebootAndResume()`), drawing the columns on two threads and
every 25th playing frame checked pixel-identical to a one-thread draw. To run part of it, edit
`main()` temporarily.

## Architecture

- `main/main.cpp`: the Anarch front end (`SFG_setPixel`, `SFG_getTimeMs`, `SFG_keyPressed`,
  `SFG_playSound`, `SFG_setMusic`, `SFG_save`/`SFG_load`, ...) plus all Bit hardware: pins (two
  board revisions, chosen by the efuse revision), ST7735S via LovyanGFX with double-buffered DMA,
  backlight and buzzer on LEDC, battery ADC, auto-sleep and deep-sleep shutdown, quicksave
  storage. The hardware code is shared in spirit with the sibling CGB-Bit project (same Bit, same
  pins, display, backlight, battery and shutdown code); keep the two consistent when fixing it.
- `main/anarch_settings.h`: Anarch's compile-time settings (`SFG_FPS 45`, resolution,
  `SFG_RAYCASTING_SUBSAMPLE`, ...), shared by the firmware, the smoke test and the demo tool.
- `main/audio.h`: the PCM `Mixer` (no ESP-IDF dependency, tested by the smoke test). Include
  `game.h` before it; it includes Anarch's `sounds.h` itself.
- `main/resume.h`: when shutdown stores the game (`resumable()`) and how boot resumes it
  (`resumeGame()`), shared with the smoke test; `main.cpp` does the flash I/O.
- `mods/NN-name.diff`: applied in name order by `tools/patch_anarch.sh` with `patch -F0` (no
  fuzz: a mod that no longer fits fails the build). 01-11 are Anarch's own mods rebased to stack;
  12 (quicksave store) and 13 (parallel columns) are ours. A new or changed mod is made against
  the output of `tools/patch_anarch.sh` with the lower-numbered mods applied, then
  `tools/smoke_test.sh`. The firmware build patches into `build/esp-idf/main/anarch/` (read the
  patched sources there, not in `anarch/`).
- Mods hook the front end through macros defined in `main.cpp` before `#include "game.h"`:
  `SFG_QUICKSAVE_STORE` (mod 12) and `RCL_CAST_RAYS_PARALLEL` (mod 13).

Game loop: `SFG_mainLoopBody()` runs fixed-rate logic steps (as many as time requires, so it already
skips drawing when behind) and draws once, or sleeps via `SFG_sleepMs`. `app_main` pushes the
drawn frame by DMA and swaps buffers only when it didn't sleep.

Two cores (mod 13): `RCL_renderComplex` calls `renderColumns`, which hands the odd ray-casting
columns to `columnTask` (pinned to core 1) and casts the even ones itself, then waits. It's safe
because each column only reads the level and writes its own pixels and depth-buffer entries;
the mod computes raycastlib's lazily initialised FOV factors before splitting. Anything added to
the per-column path (`RCL_castRayMultiHit`, `_RCL_columnFunctionComplex`, `SFG_pixelFunc`, the
`SFG_*At` map functions) must keep that property; the smoke test's pixel comparison catches a
violation only if the race shows. Sprites, HUD and logic stay on core 0.

Sound: core 0 alone runs the mixer (`audioFill()` after each loop pass and during the splash
and backlight fades), keeping `PcmLead` (50 ms) of 8 kHz samples ahead in `mixer.ring`;
`pcmTick`, a GPTimer interrupt started from `columnTask` so it runs on core 1, writes each sample
to the buzzer's LEDC duty registers via `ledc_ll_*` (8-bit PWM at 156.25 kHz). Anarch's sound
effects and bytebeat music are mixed as its SDL front end does; the stock-firmware chirps are
square waves (`Chirps`). `SFG_GET_SFX_SAMPLE` doesn't parenthesise its arguments: pass plain
variables.

Hot code: `main.cpp` is built with `-Os`, and the per-column/per-pixel functions listed in
`ANARCH_IRAM` (`main/CMakeLists.txt`) are moved to internal RAM by `tools/iram_hot.sh` (renames
their `.text.F`/`.literal.F` sections to the names `IRAM_ATTR` gives). The list holds mangled names
as the current compiler and `-Os` emit them; the build fails if one disappears. Find current names
with `xtensa-esp32s3-elf-objdump -h build/esp-idf/main/CMakeFiles/__idf_main.dir/main.cpp.obj`,
check placement with `xtensa-esp32s3-elf-nm -C build/anarch-bit.elf` (IRAM is 0x4037xxxx).

## Flash layout and persistence

`partitions.csv` keeps IDF's single-app-large offsets so NVS saves survive updates. Saves
(unlocked levels, settings) live in NVS (`SFG_save`/`SFG_load`). The `quicksave` partition
(data 0x40, 64 KB) holds two snapshots, each `[firmware ELF SHA-256][SFG_Game][SFG_Player]
[SFG_CurrentLevel]` (~19 KB), hash written last: the player's quicksave at 0 and the game to
resume at `ResumeAt` (0x8000). A snapshot holds pointers into this build's flash, so any other
build ignores it.

Resume (as in CGB-Bit): `shutdown()` (auto-sleep, low battery) stores the game once the main
loop runs and a level is played or paused in its menu. At boot, after `SFG_game.frameTime` is
set, `resume()` reads the snapshot, erases its hash sector (so it resumes once; the power switch
skips `shutdown()`), and passes it through the quicksave variables to mod 12's `SFG_quickLoad`,
which rebases the level clock and keeps unlocked levels and settings; the player's quicksave is
then read back from flash.

## Performance

Measure on the device (the `CPU load` line); the esp32sim emulator doesn't model cache timing.
With everything on one core, 60 FPS measured 93-131% load on a Bit 3, hence `SFG_FPS 45`; the
two-core rendering, IRAM and `-Os` (ideas from CGB-Bit) aren't measured yet. Already tuned in
`sdkconfig.defaults`: 240 MHz, QIO flash at 80 MHz, 32 KB instruction / 64 KB data cache (HD
textures are read from flash every frame). Fallbacks if load stays over 100%:
`SFG_RAYCASTING_SUBSAMPLE 2` or a lower `SFG_FPS`.

## Conventions

- `tools/demo.cpp` plays a frame-scripted run of level 1 (one-thread rendering); changes that
  affect movement or aiming may need its script retuned before regenerating `demo.gif`.
- Browser installer (`web/`, ESP Web Tools 10.4.0 served from the site, hash-pinned in
  `web/build.sh`): `"new_install_prompt_erase": true` in `web/manifest.json` is essential (no
  Improv Serial: without it every install erases the whole chip). `web/build.sh` cuts the merged
  image into the bootloader + partition table (up to NVS at 0x9000) and the app (0x10000), so
  NVS saves survive; it fails if the left-out range isn't pure padding (partitions moved).
- CI (`.github/workflows/build.yml`): smoke test, then firmware in a digest-pinned
  `espressif/idf:v5.4.1` container; `v*` tags also make a release and deploy the installer to
  GitHub Pages. Actions are pinned to commit SHAs with the version in a comment; Dependabot
  updates them.
- Repository code is CC0; dependencies keep their licences (README "License").
