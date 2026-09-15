#!/usr/bin/env bash
# HOST-NATIVE ONLY: policy rejection and send/receive/drain after injected errors.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:?Usage: audit.sh NATIVE_BUILD_ROOT OUTPUT_DIR}
OUT=${2:?}; BUILD=$(realpath "$BUILD"); mkdir -p "$OUT"; OUT=$(realpath "$OUT")
FF="$BUILD/ffmpeg/ffmpeg"
mkdir -p "$OUT/fixtures" "$OUT/logs" "$OUT/results"
export FF_ET_KERNEL=NATIVE_TEST_NO_DEVICE_ELF
unset FF_ET_TEST_FAIL FF_ET_SHIRE_MASK FF_ET_SYSEMU FF_ET_MEM_CHECK
printf '%s\n' '*** NATIVE LATE AUDIT: no hardware or simulator execution ***'
encode() {
    local codec=$1 frames=$2 gop=$3 path=$4
    "$FF" -nostdin -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=64x48:rate=25 \
        -frames:v "$frames" -c:v "$codec" -threads 1 -pix_fmt yuv420p -g "$gop" -bf 0 \
        -sc_threshold 1000000000 -q:v 4 -f "$codec" "$path"
}
encode mpeg2video 1 1 "$OUT/fixtures/single-i.m2v"
encode mpeg2video 2 12 "$OUT/fixtures/ip.m2v"
encode mpeg1video 1 1 "$OUT/fixtures/real-mpeg1.m1v"
python3 "$ROOT/et-tests/audit-mutate.py" "$OUT/fixtures/single-i.m2v" "$OUT/fixtures"
# Prove this is a decodable MPEG1 stream, not arbitrary garbage rejected for
# unrelated reasons. Force the same MPEG2 decoder that accepts MPEG1 on CPU.
"$FF" -nostdin -hide_banner -loglevel error -y -xerror -err_detect explode \
    -c:v mpeg2video -i "$OUT/fixtures/real-mpeg1.m1v" -f framemd5 "$OUT/results/mpeg1-cpu.md5"
[[ $(grep -c '^0,' "$OUT/results/mpeg1-cpu.md5") == 1 ]]
passed=0; failed=0
reject() {
    local name=$1 input=$2 diagnostic=$3; shift 3
    if ! python3 "$ROOT/et-tests/expect-failure.py" "$OUT/logs/$name.log" \
        "$FF" -nostdin -hide_banner -loglevel verbose -y -xerror -err_detect explode \
        -c:v mpeg2video -hwaccel et -et_harts 64 "$@" -i "$input" \
        -an -f framemd5 "$OUT/results/$name.md5"; then
        echo "FAIL audit: $name not cleanly rejected"; failed=$((failed+1)); return
    fi
    if ! grep -Eq "$diagnostic" "$OUT/logs/$name.log"; then
        echo "FAIL audit: $name did not report expected ET policy rejection"; failed=$((failed+1)); return
    fi
    if [[ $name == same-size-* ]] && ! grep -q 'ET frame 1:' "$OUT/logs/$name.log"; then
        echo "FAIL audit: $name did not first decode a valid ET I frame"; failed=$((failed+1)); return
    fi
    echo "PASS NATIVE audit policy: $name"; passed=$((passed+1))
}
reject forced-mpeg1 "$OUT/fixtures/real-mpeg1.m1v" 'ET requires MPEG-2'
reject profile-high "$OUT/fixtures/profile-high.m2v" 'ET: only MPEG-2 Main'
reject same-size-profile-change "$OUT/fixtures/same-size-profile-change.m2v" 'ET: only MPEG-2 Main'
reject same-size-chroma-change "$OUT/fixtures/same-size-chroma-change.m2v" 'ET: only MPEG-2 Main|ET requires MPEG-2'
for id in 5 9 a; do
    reject "scalable-$id" "$OUT/fixtures/scalable-$id.m2v" 'ET does not support scalable MPEG-2 extensions'
done
reject unsupported-idct "$OUT/fixtures/single-i.m2v" 'ET uses the scalar simple IDCT' -idct int
printf 'NATIVE audit policy summary: %d passed, %d failed\n' "$passed" "$failed"
# Link public libavcodec API calls against the same built FFmpeg and explicit
# native runtime archives. No production SDK/library path is substituted.
ET_LIBS=$(PKG_CONFIG_PATH="$BUILD/runtime/lib/pkgconfig" pkg-config --libs etsoc)
${CC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror -I"$ROOT" -I"$BUILD/ffmpeg" \
    "$ROOT/et-tests/drain-test.c" -L"$BUILD/ffmpeg/libavcodec" -lavcodec \
    -L"$BUILD/ffmpeg/libavutil" -lavutil $ET_LIBS -pthread -lm -ldl -o "$OUT/drain-test"
if "$OUT/drain-test" "$OUT/fixtures/single-i.m2v" "$OUT/fixtures/ip.m2v" > "$OUT/logs/drain.log" 2>&1; then
    grep 'PASS' "$OUT/logs/drain.log"
    # Twelve exercised failures, not merely a wrapper error before launch/read.
    [[ $(grep -c '^NATIVE TEST: injected' "$OUT/logs/drain.log") == 12 ]]
    [[ $(grep -c 'NATIVE TEST RUNTIME' "$OUT/logs/drain.log") == 16 ]]
else
    tail -70 "$OUT/logs/drain.log" >&2
    echo 'FAIL NATIVE API drain audit' >&2; failed=$((failed+1))
fi
(( failed == 0 ))
