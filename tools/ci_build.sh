#!/usr/bin/env bash
# CI build (runs inside espressif/idf via espressif/esp-idf-ci-action):
# host unit tests, launcher + custom bootloader, size report, release images,
# QEMU self-tests (bootloader slot selection, slot writer / metadata / slot scan).
set -euo pipefail
cd "$(dirname "$0")/.."
echo "::group::host unit tests"
tools/host_tests/run.sh
echo "::endgroup::"
idf.py build
echo "::group::idf.py size"
idf.py size
echo "::endgroup::"
tools/make_images.sh
if command -v qemu-system-xtensa >/dev/null 2>&1; then
    echo "::group::QEMU bootloader self-test"
    tools/qemu_selftest.sh
    echo "::endgroup::"
    echo "::group::QEMU slot writer self-test"
    tools/qemu_swriter_test.sh
    echo "::endgroup::"
else
    echo "QEMU not available in this IDF image - self-tests skipped"
fi
