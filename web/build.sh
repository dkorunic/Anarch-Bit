#!/bin/sh
# Assembles the browser installer site (GitHub Pages) into <out dir>: the page, a manifest for
# <version>, demo.gif, the firmware as two parts cut from the merged image around the NVS and
# phy_init partitions (merge-bin pads them with 0xFF, which would erase the saves), and ESP Web
# Tools (built on Espressif's esptool-js) served from the site itself, its npm package checked
# against a pinned hash. With manifest.json's new_install_prompt_erase, installs keep saves unless
# the player ticks "Erase device" (without it, ESP Web Tools erases the whole chip every time).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1 FIRMWARE=$2 VERSION=$3
if [ -z "$OUT" ] || [ ! -f "$FIRMWARE" ] || [ -z "$VERSION" ]; then
	echo "usage: $0 <out dir> <anarch-bit-merged.bin> <version>" >&2
	exit 1
fi
EWT_VERSION=10.4.0
EWT_INTEGRITY=sha512-3pwkeFFm5Fj7UQo8SJNYK5RXrtNCpq6X9QoI6bMT4GBZWgrJqjn0YvM9ihG74BtMoSFYXfmDtkehuxe50PTMPQ==

[ ! -e "$OUT" ] || [ -z "$(ls -A "$OUT")" ] || { echo "$OUT must be empty or missing" >&2; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -sSfL -o "$TMP/ewt.tgz" "https://registry.npmjs.org/esp-web-tools/-/esp-web-tools-$EWT_VERSION.tgz"
[ "sha512-$(openssl dgst -sha512 -binary "$TMP/ewt.tgz" | base64 | tr -d '\n')" = "$EWT_INTEGRITY" ] \
	|| { echo "esp-web-tools $EWT_VERSION doesn't match its pinned hash" >&2; exit 1; }
tar -xzf "$TMP/ewt.tgz" -C "$TMP"

mkdir -p "$OUT/esp-web-tools"
cp "$TMP"/package/dist/web/*.js "$TMP"/package/LICENSE "$OUT/esp-web-tools/"
cp "$ROOT/web/index.html" "$ROOT/demo.gif" "$OUT/"
python3 - "$ROOT/web/manifest.json" "$OUT" "$VERSION" "$FIRMWARE" <<'PY'
import json, sys
manifest_path, out, version, merged_path = sys.argv[1:]
manifest = json.load(open(manifest_path))
manifest["version"] = version
boot, app = manifest["builds"][0]["parts"]
merged = open(merged_path, "rb").read()
NVS = 0x9000 # partitions.csv: nvs (0x9000) and phy_init up to the app at app["offset"]
# Only padding may be left out, and both parts must start with an ESP image (magic 0xE9): if not,
# the partitions moved and this cut, the NVS offset and manifest.json's offsets need updating.
if len(merged) <= app["offset"] or set(merged[NVS:app["offset"]]) != {0xFF} or merged[0] != 0xE9 or merged[app["offset"]] != 0xE9:
    sys.exit(f"{merged_path} doesn't have the expected layout (bootloader, table, padding to {app['offset']:#x}, app)")
open(f"{out}/{boot['path']}", "wb").write(merged[:NVS])
open(f"{out}/{app['path']}", "wb").write(merged[app["offset"]:])
json.dump(manifest, open(f"{out}/manifest.json", "w"), indent=2)
PY
echo "site in $OUT"
