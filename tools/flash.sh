#!/usr/bin/env bash
# Flash Basecamp with esptool.
#
#   tools/flash.sh [-p PORT]                 FRESH-INSTALL image at 0x0 (wipes everything;
#                                            the launcher then downloads the apps over Wi-Fi)
#   tools/flash.sh --launcher-only [-p PORT] launcher at 0x90000 (apps + settings kept)
#   tools/flash.sh --offline [-p PORT]       FRESH-INSTALL, then the pinned app bins from
#                                            apps/ (tools/fetch_apps.sh) - no Wi-Fi needed
#   --image FILE   use a downloaded image instead of dist/
#   --print        only print the command(s)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
PORT=""; BAUD=921600; MODE=fresh; IMAGE=""; PRINT=0
while [ $# -gt 0 ]; do
    case "$1" in
        --launcher-only) MODE=launcher ;;
        --offline) MODE=offline ;;
        --image) IMAGE="$2"; shift ;;
        -p|--port) PORT="$2"; shift ;;
        -b|--baud) BAUD="$2"; shift ;;
        --print) PRINT=1 ;;
        *) echo "unknown arg $1"; exit 2 ;;
    esac
    shift
done
ESPTOOL="${ESPTOOL:-esptool.py}"
BASE=("$ESPTOOL" --chip esp32s3 ${PORT:+-p "$PORT"} -b "$BAUD")
run() { echo "$*"; [ $PRINT = 1 ] || "$@"; }
pick() { ls "$DIST"/$1 2>/dev/null | sort -V | tail -1; }

case $MODE in
launcher)
    IMG="${IMAGE:-$(pick 'Basecamp-*-launcher-only.bin')}"
    [ -f "$IMG" ] || { echo "no launcher-only image (build + tools/make_images.sh, or --image)"; exit 1; }
    run "${BASE[@]}" --before default_reset --after hard_reset write_flash 0x90000 "$IMG"
    ;;
fresh|offline)
    IMG="${IMAGE:-$(pick 'Basecamp-*-FRESH-INSTALL-USB.bin')}"
    [ -f "$IMG" ] || { echo "no FRESH-INSTALL image (build + tools/make_images.sh, or --image)"; exit 1; }
    if [ $MODE = fresh ]; then
        run "${BASE[@]}" --before default_reset --after hard_reset write_flash 0x0 "$IMG"
    else
        (cd "$ROOT/apps" && sha256sum --quiet -c "$ROOT/tools/apps.sha256") \
            || { echo "app images missing or sha256 mismatch - run tools/fetch_apps.sh"; exit 1; }
        run "${BASE[@]}" --before default_reset --after no_reset write_flash 0x0 "$IMG"
        run "${BASE[@]}" --before no_reset --after hard_reset write_flash \
            0x190000 "$ROOT/apps/printsphere_ota-v1.6.2.bin" \
            0x590000 "$ROOT/apps/CapsuleRadar-ota.bin" \
            0x990000 "$ROOT/apps/lyricflash-ota.bin"
    fi
    ;;
esac
