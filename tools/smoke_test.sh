#!/bin/sh
# Builds the patched game for the PC with ASan/UBSan and plays it (tools/smoke_test.cpp).
# Catches memory errors from mod interactions and mods that no longer apply. Runs in CI.
set -e
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
sh tools/patch_anarch.sh "$TMP/anarch"
# -fwrapv as in the firmware: raycastlib relies on signed overflow wrapping.
c++ -std=c++20 -O1 -g -w -fwrapv -fsanitize=address,undefined -fno-sanitize=signed-integer-overflow \
	-fno-sanitize-recover=all -pthread -Imain -I"$TMP/anarch" tools/smoke_test.cpp -o "$TMP/smoke"
"$TMP/smoke"
