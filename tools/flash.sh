#!/usr/bin/env bash
# Flash the complete Basecamp image set with ONE esptool command:
#   0x0       bootloader (custom slot selector)
#   0x8000    partition table
#   0x9000    nvs       <- erased (0xFF)
#   0x89000   otadata   <- erased (0xFF)
#   0x90000   launcher (factory)
#   0x190000  ota_0  PrintSphere v1.6.2      (or dummy_a with --dummy)
#   0x590000  ota_1  Capsule Radar v1.6.0    (or dummy_b)
#   0x990000  ota_2  LyricFlash v1.0.0       (or dummy_c)
#   0xD90000  sounds    <- erased (0xFF; LittleFS formats itself on first mount)
#
# usage: tools/flash.sh [--dummy] [-p PORT] [-b BAUD] [--print]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
PORT=""; BAUD=921600; DUMMY=0; PRINT=0
while [ $# -gt 0 ]; do
    case "$1" in
        --dummy) DUMMY=1 ;;
        -p|--port) PORT="$2"; shift ;;
        -b|--baud) BAUD="$2"; shift ;;
        --print) PRINT=1 ;;
        *) echo "unknown arg $1"; exit 2 ;;
    esac
    shift
done

if [ $DUMMY = 1 ]; then
    APP0="$DIST/dummy_a.bin"; APP1="$DIST/dummy_b.bin"; APP2="$DIST/dummy_c.bin"
else
    APP0="$ROOT/apps/printsphere_ota-v1.6.2.bin"
    APP1="$ROOT/apps/CapsuleRadar-ota.bin"
    APP2="$ROOT/apps/lyricflash-ota.bin"
    (cd "$ROOT/apps" && sha256sum --quiet -c "$ROOT/tools/apps.sha256") \
        || { echo "app images missing or sha256 mismatch - run tools/fetch_apps.sh"; exit 1; }
fi
for f in "$DIST/bootloader.bin" "$DIST/partition-table.bin" "$DIST/launcher.bin" "$APP0" "$APP1" "$APP2"; do
    [ -f "$f" ] || { echo "missing $f (build + tools/package.sh / tools/build_dummies.sh)"; exit 1; }
done

# 0xFF "erase" images for nvs / otadata / sounds
BLANK="$DIST/blank"; mkdir -p "$BLANK"
mkblank() { [ "$(stat -c %s "$1" 2>/dev/null || echo 0)" = "$2" ] || head -c "$2" /dev/zero | tr '\0' '\377' > "$1"; }
mkblank "$BLANK/nvs.bin"     $((0x80000))
mkblank "$BLANK/otadata.bin" $((0x2000))
mkblank "$BLANK/sounds.bin"  $((0x270000))

ESPTOOL="${ESPTOOL:-esptool.py}"
CMD=("$ESPTOOL" --chip esp32s3 ${PORT:+-p "$PORT"} -b "$BAUD"
     --before default_reset --after hard_reset
     write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB
     0x0      "$DIST/bootloader.bin"
     0x8000   "$DIST/partition-table.bin"
     0x9000   "$BLANK/nvs.bin"
     0x89000  "$BLANK/otadata.bin"
     0x90000  "$DIST/launcher.bin"
     0x190000 "$APP0"
     0x590000 "$APP1"
     0x990000 "$APP2"
     0xD90000 "$BLANK/sounds.bin")

echo "${CMD[*]}"
[ $PRINT = 1 ] || "${CMD[@]}"
