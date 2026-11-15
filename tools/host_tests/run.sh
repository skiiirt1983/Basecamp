#!/usr/bin/env bash
# Build and run the host unit tests (plain gcc; no ESP-IDF needed).
# Optional argument: directory with the real app images (apps/) for extra checks.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT="${TMPDIR:-/tmp}/basecamp_host_tests"
mkdir -p "$OUT"
gcc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    tools/host_tests/test_apps_logic.c main/apps_logic.c -o "$OUT/test_apps_logic"
"$OUT/test_apps_logic" "${1:-}"
