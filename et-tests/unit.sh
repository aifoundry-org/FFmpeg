#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-"$ROOT/et-tests/build-unit"}; mkdir -p "$OUT"
${CC:-cc} -std=c11 -g -O1 -Wall -Wextra -Werror ${ET_TEST_SANITIZERS:+-fsanitize=address,undefined -fno-omit-frame-pointer} \
    -I"$ROOT/libavcodec" "$ROOT/et-tests/native-runtime.c" "$ROOT/et-tests/native-runtime-test.c" -o "$OUT/native-runtime-test"
"$OUT/native-runtime-test"
python3 -m unittest discover -s "$ROOT/et-tests" -p 'test_*.py'
