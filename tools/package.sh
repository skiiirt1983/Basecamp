#!/usr/bin/env bash
# Copy the built bootloader, partition table and launcher from build/ to dist/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build"
mkdir -p "$ROOT/dist"
cp "$B/bootloader/bootloader.bin"            "$ROOT/dist/bootloader.bin"
cp "$B/partition_table/partition-table.bin"  "$ROOT/dist/partition-table.bin"
cp "$B/basecamp_launcher.bin"                "$ROOT/dist/launcher.bin"
ls -l "$ROOT/dist"
