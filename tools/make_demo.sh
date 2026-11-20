#!/bin/sh
# Regenerates demo.gif: tools/demo.cpp plays a scripted run of the patched game with the Bit's
# settings on the PC, make_demo_gif.py writes the GIF.
set -e
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
sh tools/patch_anarch.sh "$TMP/anarch"
c++ -std=c++20 -O2 -w -fwrapv -Imain -I"$TMP/anarch" tools/demo.cpp -o "$TMP/demo"
"$TMP/demo" "$TMP/frames.raw" "$TMP/palette.raw"
python3 tools/make_demo_gif.py "$TMP/frames.raw" "$TMP/palette.raw" demo.gif
