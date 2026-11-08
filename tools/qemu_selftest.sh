#!/usr/bin/env bash
# Build the headless self-test, assemble a 16 MB flash image with the custom
# bootloader and the test app in factory + ota_0..2, and run it in Espressif
# QEMU (esp32s3). Needs qemu-system-xtensa (idf_tools.py install qemu-xtensa).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/test_apps/qemu_selftest"
B="$APP/build"
idf.py -C "$APP" -B "$B" build >/dev/null
IMG="$B/flash_qemu.bin"
APPBIN="$B/basecamp_selftest.bin"
esptool.py --chip esp32s3 merge_bin --fill-flash-size 16MB -o "$IMG" \
    --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 "$B/bootloader/bootloader.bin" 0x8000 "$B/partition_table/partition-table.bin" \
    0x90000 "$APPBIN" 0x190000 "$APPBIN" 0x590000 "$APPBIN" 0x990000 "$APPBIN" >/dev/null
LOG="$B/qemu.log"
: > "$LOG"
qemu-system-xtensa -nographic -machine esp32s3 \
    -drive file="$IMG",if=mtd,format=raw -m 32M \
    -global driver=timer.esp32s3.timg,property=wdt_disable,value=true > "$LOG" 2>&1 &
QPID=$!
for _ in $(seq 1 "${QEMU_TIMEOUT:-180}"); do
    grep -qaE "SELFTEST (PASS|FAIL)" "$LOG" && break
    kill -0 $QPID 2>/dev/null || break
    sleep 1
done
kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true
python3 "$ROOT/tools/check_selftest_log.py" "$LOG"
