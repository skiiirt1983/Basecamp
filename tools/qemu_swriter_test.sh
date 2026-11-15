#!/usr/bin/env bash
# Build the slot-writer self-test, run it in Espressif QEMU (esp32s3) with a valid
# app in ota_0 (the test itself), and fail unless it prints SWTEST PASS.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/test_apps/swriter_selftest"
B="$APP/build"
idf.py -C "$APP" -B "$B" build >/dev/null
IMG="$B/flash_qemu.bin"
APPBIN="$B/basecamp_swtest.bin"
esptool.py --chip esp32s3 merge_bin --fill-flash-size 16MB -o "$IMG" \
    --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 "$B/bootloader/bootloader.bin" 0x8000 "$B/partition_table/partition-table.bin" \
    0x90000 "$APPBIN" 0x190000 "$APPBIN" >/dev/null
LOG="$B/qemu.log"
: > "$LOG"
qemu-system-xtensa -nographic -machine esp32s3 -drive file="$IMG",if=mtd,format=raw -m 32M \
    -global driver=timer.esp32s3.timg,property=wdt_disable,value=true > "$LOG" 2>&1 &
QPID=$!
for _ in $(seq 1 "${QEMU_TIMEOUT:-240}"); do
    grep -qaE "SWTEST (PASS|FAIL)" "$LOG" && break
    kill -0 $QPID 2>/dev/null || break
    sleep 1
done
kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true
grep -aE "source image|TEST FAIL|checks,|SWTEST" "$LOG" | tr -d '\r'
grep -qa "SWTEST PASS" "$LOG"
