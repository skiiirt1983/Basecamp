#!/usr/bin/env bash
# Build the three dummy test apps and copy them to dist/dummy_{a,b,c}.bin
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/test_apps/dummy_app"
mkdir -p "$ROOT/dist"
for id in a b c; do
    idf.py -C "$APP" -B "$APP/build_$id" -DDUMMY_ID=$id -DSDKCONFIG="$APP/build_$id/sdkconfig" build
    cp "$APP/build_$id/basecamp_dummy_$id.bin" "$ROOT/dist/dummy_$id.bin"
done
ls -l "$ROOT"/dist/dummy_*.bin
