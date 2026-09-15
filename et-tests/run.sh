#!/usr/bin/env bash
# Native mode is explicitly labelled. This never discovers or touches hardware.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
FF=${1:?Usage: run.sh BUILT_FFMPEG WORKDIR [native|sysemu] [supported-i|full-gop|all]}
OUT=${2:?}; MODE=${3:-native}; PROFILE=${4:-all}; FF=$(realpath "$FF")
# Backward-compatible name; P/B is now mandatory supported coverage.
[[ $PROFILE != planned-pb ]] || PROFILE=full-gop
case "$MODE" in
    native) echo '*** NATIVE TEST ONLY — CPU kernel execution; NOT ET hardware validation ***' >&2
        export FF_ET_KERNEL=NATIVE_TEST_NO_DEVICE_ELF
        unset FF_ET_SYSEMU FF_ET_MEM_CHECK FF_ET_SHIRE_MASK ;;
    sysemu) echo '*** SYSEMU TEST — simulator, NOT ET hardware validation ***' >&2
        [[ ${FF_ET_SYSEMU:-0} == 1 && ${FF_ET_MEM_CHECK:-0} == 1 ]] || {
            echo 'sysemu mode requires FF_ET_SYSEMU=1 FF_ET_MEM_CHECK=1' >&2; exit 1; } ;;
    *) echo "Unknown test mode: $MODE" >&2; exit 2 ;;
esac
unset FF_ET_TEST_FAIL
mkdir -p "$OUT/logs" "$OUT/results"
"$ROOT/generate-corpus.sh" "$FF" "$OUT/corpus"
passes=0; failures=0
report_failure() { echo "FAIL: $*" >&2; failures=$((failures+1)); }
case "$PROFILE" in
    supported-i) manifests=("$OUT/corpus/supported.list") ;;
    full-gop) manifests=("$OUT/corpus/gop.list") ;;
    all) manifests=("$OUT/corpus/supported.list" "$OUT/corpus/gop.list") ;;
    *) echo "Unknown profile: $PROFILE" >&2; exit 2 ;;
esac
streams=()
for manifest in "${manifests[@]}"; do
    while IFS= read -r relative; do streams+=("$OUT/corpus/$relative"); done < "$manifest"
done
for stream in "${streams[@]}"; do
    name=$(basename "$stream" .m2v)
    for idct in default simple; do
        args=(); [[ $idct == default ]] || args=(-idct simple)
        if ! "$FF" -nostdin -hide_banner -loglevel verbose -y -xerror -err_detect explode \
            "${args[@]}" -i "$stream" -an -pix_fmt yuv420p -f framemd5 \
            "$OUT/results/$name.cpu-$idct.md5" >"$OUT/logs/$name.cpu-$idct.log" 2>&1; then
            report_failure "$name CPU $idct decode"; continue
        fi
        expected_frames=$(grep -c '^0,' "$OUT/results/$name.cpu-$idct.md5" || true)
        if (( expected_frames == 0 )); then
            report_failure "$name CPU $idct produced no frames"; continue
        fi
        if [[ $name == long-ipb-250 && $expected_frames != 250 ]]; then
            report_failure "$name CPU $idct did not decode exactly 250 frames"; continue
        fi
        for harts in 1 64; do
            stem="$name.$idct.et-$harts"
            if ! "$FF" -nostdin -hide_banner -loglevel verbose -y -xerror -err_detect explode \
                "${args[@]}" -hwaccel et -et_harts "$harts" -i "$stream" -an -pix_fmt yuv420p \
                -f framemd5 "$OUT/results/$stem.md5" >"$OUT/logs/$stem.log" 2>&1; then
                report_failure "$stem decode (see $OUT/logs/$stem.log)"; continue
            fi
            if [[ $MODE == native ]] && ! grep -q 'NATIVE TEST RUNTIME' "$OUT/logs/$stem.log"; then
                report_failure "$stem did not use the explicit native test runtime"; continue
            fi
            if [[ $MODE == sysemu ]] && grep -q 'NATIVE TEST RUNTIME' "$OUT/logs/$stem.log"; then
                report_failure "$stem unexpectedly used native runtime"; continue
            fi
            completed=$(grep -c "ET frame .* harts=$harts " "$OUT/logs/$stem.log" || true)
            if [[ $completed != "$expected_frames" ]]; then
                report_failure "$stem logged $completed ET-completed frames, expected $expected_frames (no silent CPU fallback)"; continue
            fi
            if cmp -s "$OUT/results/$name.cpu-$idct.md5" "$OUT/results/$stem.md5"; then
                echo "PASS ($MODE): $stem exact framemd5"; passes=$((passes+1))
            else
                report_failure "$stem framemd5 mismatch"
                python3 "$ROOT/compare.py" "$OUT/results/$name.cpu-$idct.md5" "$OUT/results/$stem.md5" || true
            fi
        done
    done
done
expect_failure() {
    local name=$1 input=$2; shift 2
    local rc=0
    python3 "$ROOT/expect-failure.py" "$OUT/logs/negative-$name.log" \
        "$FF" -nostdin -hide_banner -loglevel verbose -y -xerror -err_detect explode \
        -hwaccel et -et_harts 64 "$@" -i "$input" -an -f framemd5 \
        "$OUT/results/negative-$name.md5" || rc=$?
    if (( rc != 0 )); then report_failure "negative $name (see log)"
    else echo "PASS ($MODE): negative $name rejected"; passes=$((passes+1)); fi
}
if [[ $PROFILE != full-gop ]]; then
while IFS= read -r relative; do
    stream="$OUT/corpus/$relative"
    expect_failure "$(basename "$stream" .m2v)" "$stream"
done < "$OUT/corpus/negative.list"
expect_failure lowres "$OUT/corpus/supported/single-i.m2v" -lowres 1
expect_failure gray "$OUT/corpus/supported/single-i.m2v" -flags +gray
if [[ $MODE == native ]]; then
    for fault in open alloc write read launch; do
        export FF_ET_TEST_FAIL="$fault"
        expect_failure "runtime-$fault" "$OUT/corpus/supported/single-i.m2v"
        if ! grep -q 'injected' "$OUT/logs/negative-runtime-$fault.log"; then
            report_failure "runtime-$fault rejected without observing injected error"
        fi
    done
    unset FF_ET_TEST_FAIL
fi
fi
printf '\n%s %s TEST SUMMARY: %d passed, %d failed. NO HARDWARE CLAIM.\n' "$MODE" "$PROFILE" "$passes" "$failures"
(( failures == 0 ))
