#!/usr/bin/env bash
# Download the three prebuilt app-only OTA images into apps/ and verify sha256.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/apps"
cd "$ROOT/apps"

fetch() {  # url file
    if [ ! -f "$2" ]; then
        echo "downloading $2"
        curl -fSL --retry 3 -o "$2.part" "$1" && mv "$2.part" "$2"
    fi
}
fetch https://github.com/cptkirki/PrintSphere/releases/download/v1.6.2/printsphere_ota-v1.6.2.bin printsphere_ota-v1.6.2.bin
fetch https://github.com/socquique/capsule-radar/releases/download/v1.6.0/CapsuleRadar-ota.bin   CapsuleRadar-ota.bin
fetch https://github.com/skiiirt1983/LyricFlash/releases/download/v1.0.0/lyricflash-ota.bin      lyricflash-ota.bin

sha256sum -c "$ROOT/tools/apps.sha256"
