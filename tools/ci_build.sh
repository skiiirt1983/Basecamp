#!/usr/bin/env bash
# CI build (runs inside espressif/idf via espressif/esp-idf-ci-action):
# launcher + custom bootloader, size report, release images, QEMU self-test.
set -euo pipefail
cd "$(dirname "$0")/.."
idf.py build
echo "::group::idf.py size"
idf.py size
echo "::endgroup::"
tools/make_images.sh
if command -v qemu-system-xtensa >/dev/null 2>&1; then
    echo "::group::QEMU bootloader self-test"
    tools/qemu_selftest.sh
    echo "::endgroup::"
else
    echo "QEMU not available in this IDF image - bootloader self-test skipped"
fi
