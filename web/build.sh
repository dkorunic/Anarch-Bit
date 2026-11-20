#!/bin/sh
# Assembles the browser installer site (GitHub Pages) into <out dir>: the page, a manifest for
# <version>, demo.gif, the merged firmware, and ESP Web Tools (built on Espressif's esptool-js)
# served from the site itself, its npm package checked against a pinned hash.
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
cp "$FIRMWARE" "$OUT/anarch-bit-merged.bin"
python3 - "$ROOT/web/manifest.json" "$OUT/manifest.json" "$VERSION" <<'PY'
import json, sys
manifest = json.load(open(sys.argv[1]))
manifest["version"] = sys.argv[3]
json.dump(manifest, open(sys.argv[2], "w"), indent=2)
PY
echo "site in $OUT"
