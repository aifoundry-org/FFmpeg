#!/usr/bin/env bash
set -euo pipefail
FF=${1:?Usage: generate-corpus.sh BUILT_FFMPEG OUTDIR}
OUT=${2:?}; mkdir -p "$OUT"; FF=$(realpath "$FF")
ROOT=$(cd "$(dirname "$0")" && pwd)
for group in supported gop negative; do : > "$OUT/$group.list"; done
encode() {
    local name=$1 size=$2 frames=$3; shift 3
    echo "CORPUS: $name ($size, $frames frames)" >&2
    "$FF" -nostdin -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=$size:rate=25" \
        -frames:v "$frames" -an -c:v mpeg2video -threads 1 -pix_fmt yuv420p \
        -q:v 4 -g 12 -bf 0 -sc_threshold 1000000000 "$@" -f mpeg2video "$OUT/$name.m2v"
    printf '%s\n' "$name.m2v" >> "$OUT/${name%%/*}.list"
}
mkdir -p "$OUT/supported" "$OUT/gop"
encode supported/single-i 64x48 1 -g 1
encode supported/i-only 94x62 5 -g 1
encode supported/large-i 178x102 3 -g 1
encode gop/ip 128x96 9
encode gop/ipb 160x112 12 -bf 2
encode gop/edges-ipb 178x102 12 -bf 2
# Many open GOPs exercise anchor-slot reuse, B-frame reordering, and generations.
encode gop/long-ipb-250 128x96 250 -bf 2
encode supported/tiny-i 18x18 2 -g 1
encode supported/intra-vlc 128x80 6 -intra_vlc 1 -g 1
encode supported/nonlinear-qscale 94x62 6 -non_linear_quant 1 -qmax 28 -g 1
matrix=$(python3 -c 'print(",".join(str(8 + i // 8 + i % 8) for i in range(64)))')
inter=$(python3 -c 'print(",".join(str(16 + i // 8 + i % 8) for i in range(64)))')
encode supported/custom-quant 110x78 5 -intra_matrix "$matrix" -inter_matrix "$inter" -g 1
encode supported/combined-flags 94x62 5 -intra_vlc 1 -non_linear_quant 1 -qmax 28 \
    -intra_matrix "$matrix" -inter_matrix "$inter" -g 1
encode gop/custom-quant-ipb 110x78 9 -intra_matrix "$matrix" -inter_matrix "$inter" -bf 2
encode gop/combined-flags-ipb 94x62 9 -intra_vlc 1 -non_linear_quant 1 -qmax 28 \
    -intra_matrix "$matrix" -inter_matrix "$inter" -bf 2
mkdir -p "$OUT/negative"
encode negative/chroma422 64x48 2 -pix_fmt yuv422p -profile:v 0
encode negative/interlace 64x48 2 -flags +ildct+ilme -top 1
# This FFmpeg encoder forces progressive_sequence/frame=0 with alternate_scan.
# Such output is outside the strict progressive subset, even for progressive input.
encode negative/alternate-scan-signaled-interlace 94x62 3 -alternate_scan 1 -g 1
python3 "$ROOT/mutate.py" "$OUT/supported/single-i.m2v" "$OUT/negative"
printf '%s\n' negative/duplicate-row.m2v negative/missing-row.m2v negative/truncated.m2v >> "$OUT/negative.list"
echo 'CORPUS: generated with the supplied built FFmpeg (no downloaded media).' >&2
