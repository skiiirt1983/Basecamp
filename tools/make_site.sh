#!/usr/bin/env bash
# Assemble the GitHub Pages site into site_out/ from site/ and the images in dist/.
# Firmware is copied next to the page (same origin) because release assets have no CORS headers.
# Update-only image goes to 0x90000 (= 589824): that is the launcher partition.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' "$ROOT/CMakeLists.txt")"
OUT="$ROOT/site_out"
rm -rf "$OUT"; mkdir -p "$OUT/firmware"
cp -a "$ROOT/site/." "$OUT/"
cp "$ROOT/dist/Basecamp-$VER-FRESH-INSTALL-USB.bin" "$OUT/firmware/fresh.bin"
cp "$ROOT/dist/Basecamp-$VER-launcher-only.bin" "$OUT/firmware/launcher.bin"
mk() { # name path offset
cat > "$OUT/$1" <<JSON
{"name":"Basecamp","version":"$VER","new_install_prompt_erase":false,
 "builds":[{"chipFamily":"ESP32-S3","parts":[{"path":"$2","offset":$3}]}]}
JSON
}
mk manifest-fresh.json  firmware/fresh.bin 0
mk manifest-update.json firmware/launcher.bin 589824
python3 -c "import json;[json.load(open('$OUT/'+f)) for f in ('manifest-fresh.json','manifest-update.json')]"
echo "site_out ready for $VER"
