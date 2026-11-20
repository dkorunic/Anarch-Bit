#!/bin/sh
# Copies Anarch (the submodule) to <output dir> and applies the given mods, or all of mods/*.diff,
# in name order, without fuzz, so a mod that no longer fits exactly fails instead of landing in
# the wrong place. Used by the firmware build (main/CMakeLists.txt passes the mod list, so removing
# a mod changes the command and re-runs it) and the PC tools.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1
[ -n "$OUT" ] || { echo "usage: $0 <output dir> [mod.diff...]" >&2; exit 1; }
shift
[ $# -gt 0 ] || set -- "$ROOT"/mods/*.diff
command -v patch >/dev/null || { echo "patch is not installed" >&2; exit 1; }
[ -f "$ROOT/anarch/game.h" ] || { echo "anarch submodule is empty, run: git submodule update --init" >&2; exit 1; }

# Only ever delete a directory this script made (or an empty one), never e.g. "." by mistake.
MARKER=.patched-by-anarch-bit
if [ -d "$OUT" ] && [ ! -e "$OUT/$MARKER" ] && [ -n "$(ls -A "$OUT")" ]; then
	echo "$OUT exists and wasn't made by $0, refusing to replace it" >&2
	exit 1
fi
rm -rf "$OUT"
mkdir -p "$OUT"
touch "$OUT/$MARKER"
cp "$ROOT"/anarch/*.h "$ROOT"/anarch/*.c "$OUT"/ # .c too: some mods also patch the desktop front ends
for mod in "$@"; do
	[ -e "$mod" ] || continue # no mods at all: the glob stays literal
	mod=$(cd "$(dirname "$mod")" && pwd)/$(basename "$mod") # patch -d changes directory before reading it
	patch -s -p1 -F0 --no-backup-if-mismatch -d "$OUT" -i "$mod" || { echo "mod $(basename "$mod") does not apply" >&2; exit 1; }
done
