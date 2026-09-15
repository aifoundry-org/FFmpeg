#!/usr/bin/env bash
# Direct scalar-kernel oracle, including syntax deliberately rejected by host policy.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FF=${1:?Usage: run-kernel-native.sh BUILT_FFMPEG NATIVE_KERNEL_SO OUTPUT_DIR}
LIB=${2:?}; OUT=${3:?}
FF=$(realpath "$FF"); LIB=$(realpath "$LIB"); mkdir -p "$OUT"
echo '*** DIRECT NATIVE KERNEL TEST: CPU execution only; NO hardware/simulator ***' >&2
# Upstream kernel test covers custom intra matrix, alternate scan, intra VLC,
# qscale, DC precisions and field-DCT syntax with 1 and 64 logical harts.
# Passing here does NOT relax the host progressive-only acceptance policy.
python3 -u "$ROOT/et-kernels/tests/compare.py" --ffmpeg "$FF" --library "$LIB" \
    --out "$OUT/fixtures" 2>&1 | tee "$OUT/kernel-direct.log"
