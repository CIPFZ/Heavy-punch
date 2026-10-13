#!/usr/bin/env bash
# Builds and runs the host-side unit tests for the pure C modules.
# Usage: test/host/run_tests.sh   (from repository root or anywhere)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build_host"
CC="${CC:-gcc}"
CFLAGS=(-std=c11 -Wall -Wextra -Werror -I "$ROOT/main")

mkdir -p "$OUT"

"$CC" "${CFLAGS[@]}" "$ROOT/test/host/test_track_config.c" "$ROOT/main/track_config.c" -o "$OUT/test_track_config"
"$CC" "${CFLAGS[@]}" "$ROOT/test/host/test_track_math.c" "$ROOT/main/track_math.c" "$ROOT/main/track_config.c" -o "$OUT/test_track_math"
"$CC" "${CFLAGS[@]}" "$ROOT/test/host/test_track_safety.c" "$ROOT/main/track_safety.c" -o "$OUT/test_track_safety"

"$OUT/test_track_config"
"$OUT/test_track_math"
"$OUT/test_track_safety"
echo "all host tests passed"
