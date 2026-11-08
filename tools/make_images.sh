#!/usr/bin/env bash
# Package a built launcher (build/) into release images in dist/:
#   Basecamp-<ver>-FRESH-INSTALL-USB.bin  16 MB, flash at 0x0. Bootloader,
#       partition table and launcher; nvs, otadata, ota_0..2 and sounds are
#       0xFF (erased). On first boot the launcher downloads the apps.
#   Basecamp-<ver>-launcher-only.bin      flash at 0x90000; apps and settings kept.
#   SHA256SUMS.txt
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build"
VER="$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' "$ROOT/CMakeLists.txt")"
[ -n "$VER" ] || { echo "PROJECT_VER not found"; exit 1; }
OUT="$ROOT/dist"
mkdir -p "$OUT"
rm -f "$OUT"/Basecamp-*.bin "$OUT/SHA256SUMS.txt"

LAUNCHER="$B/basecamp_launcher.bin"
SIZE=$(stat -c %s "$LAUNCHER")
LIMIT=$((0x100000))
echo "launcher: $SIZE bytes of $LIMIT ($(( (LIMIT - SIZE) / 1024 )) KB free)"
[ "$SIZE" -lt "$LIMIT" ] || { echo "launcher does not fit the 1 MB factory partition"; exit 1; }

FRESH="$OUT/Basecamp-$VER-FRESH-INSTALL-USB.bin"
ESPTOOL="${ESPTOOL:-esptool.py}"
"$ESPTOOL" --chip esp32s3 merge_bin --fill-flash-size 16MB -o "$FRESH" \
    --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0     "$B/bootloader/bootloader.bin" \
    0x8000  "$B/partition_table/partition-table.bin" \
    0x90000 "$LAUNCHER"
cp "$LAUNCHER" "$OUT/Basecamp-$VER-launcher-only.bin"

# Sanity: exact size, launcher in place, every other region erased.
python3 - "$FRESH" "$LAUNCHER" <<'PY'
import sys
img = open(sys.argv[1], 'rb').read(); app = open(sys.argv[2], 'rb').read()
assert len(img) == 16 * 1024 * 1024, len(img)
assert img[0x90000:0x90000 + len(app)] == app
def blank(a, b, name):
    assert img[a:b] == b'\xff' * (b - a), name + " not erased"
blank(0x9000, 0x8B000, "nvs/otadata")
blank(0x190000, 0x590000, "ota_0"); blank(0x590000, 0x990000, "ota_1")
blank(0x990000, 0xD90000, "ota_2"); blank(0xD90000, 0x1000000, "sounds")
print("FRESH-INSTALL image OK: launcher in place, nvs/otadata/ota_0-2/sounds erased")
PY
(cd "$OUT" && sha256sum Basecamp-*.bin > SHA256SUMS.txt && cat SHA256SUMS.txt)
