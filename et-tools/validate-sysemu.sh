#!/usr/bin/env bash
# Bounded production-runtime emulator gate, not the exhaustive native suite.
set -euo pipefail
FF=$(realpath "${1:?Usage: validate-sysemu.sh PRODUCTION_FFMPEG OUTPUT_DIR}")
OUT=${2:?}; mkdir -p "$OUT"; OUT=$(realpath "$OUT")
[[ -f ${FF_ET_KERNEL:-} ]] || { echo 'Set FF_ET_KERNEL to the built ET ELF.' >&2; exit 2; }
export FF_ET_SYSEMU=1 FF_ET_MEM_CHECK=1 FF_ET_ALLOW_PCIE=0 FF_ET_SHIRE_MASK=1
export GLOG_minloglevel=${GLOG_minloglevel:-1}
one() {
    local name=$1 size=$2 frames=$3 harts=$4 bframes=$5
    "$FF" -nostdin -nostats -v error -f lavfi -i "testsrc2=size=$size:rate=25" \
        -frames:v "$frames" -c:v mpeg2video -g 12 -bf "$bframes" -q:v 3 -y "$OUT/$name.m2v"
    "$FF" -nostdin -nostats -v error -xerror -err_detect explode -i "$OUT/$name.m2v" \
        -f framemd5 -y "$OUT/$name.cpu.md5"
    timeout 2400 "$FF" -nostdin -nostats -v verbose -xerror -err_detect explode \
        -hwaccel et -et_harts "$harts" -i "$OUT/$name.m2v" -f framemd5 \
        -y "$OUT/$name.et.md5" >"$OUT/$name.log" 2>&1
    if grep -q 'NATIVE TEST RUNTIME' "$OUT/$name.log"; then
        echo 'Wrong binary: a native test runtime is not a simulator.' >&2; exit 1
    fi
    cmp "$OUT/$name.cpu.md5" "$OUT/$name.et.md5"
    [[ $(grep -c '^0,' "$OUT/$name.et.md5") == "$frames" ]]
    [[ $(grep -c 'ET frame [0-9].*harts=' "$OUT/$name.log") == "$frames" ]]
    echo "PASS sys_emu memcheck: $name $size frames=$frames harts=$harts"
}
one single-i 64x48 1 1 0
one round-robin-65 64x1040 1 64 0
one ipb12 128x96 12 64 2
# Small spatial dimensions keep the 250-frame lifetime test practical in a
# functional emulator. SD/HD full-GOP cases belong in native/silicon gates.
one ipb250 64x48 250 64 2
