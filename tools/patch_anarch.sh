#!/bin/sh
# Copies Anarch (the submodule) to the given directory and applies mods/*.diff in name order,
# without fuzz, so a mod that no longer fits exactly fails instead of landing in the wrong place.
# Used by the firmware build (main/CMakeLists.txt) and the PC tools.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1
[ -n "$OUT" ] || { echo "usage: $0 <output dir>" >&2; exit 1; }
[ -f "$ROOT/anarch/game.h" ] || { echo "anarch submodule is empty, run: git submodule update --init" >&2; exit 1; }
rm -rf "$OUT"
mkdir -p "$OUT"
cp "$ROOT"/anarch/*.h "$ROOT"/anarch/*.c "$OUT"/ # .c too: some mods also patch the desktop front ends
for mod in "$ROOT"/mods/*.diff; do
	[ -e "$mod" ] || continue
	patch -s -p1 -F0 --no-backup-if-mismatch -d "$OUT" -i "$mod" || { echo "mod $(basename "$mod") does not apply" >&2; exit 1; }
done
