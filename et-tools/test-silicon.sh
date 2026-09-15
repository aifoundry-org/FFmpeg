#!/usr/bin/env bash
# Explicitly invoked silicon validation. Never resets/flashes/clears counters.
set -euo pipefail
ROOT=$(dirname "$(dirname "$(readlink -f "$0")")")
[[ ${FF_ET_ALLOW_PCIE:-0} == 1 ]] || { echo 'Set FF_ET_ALLOW_PCIE=1 after coordinating device ownership.' >&2; exit 2; }
NAME=${1:?Usage: test-silicon.sh NAME HARTS INPUT_RELATIVE_TO_REPO GOLDEN_RELATIVE_TO_REPO}
HARTS=${2:?}; INPUT=${3:?}; GOLDEN=${4:?}
[[ $NAME =~ ^[a-zA-Z0-9_-]+$ && ($HARTS == 1 || $HARTS == 64) ]] || exit 2
OUT="$ROOT/build-et/silicon/$NAME-h$HARTS"
mkdir -p "$OUT"
[[ ! -e "$ROOT/build-et/silicon/RECOVERY_REQUIRED" ]] || { echo 'Prior silicon failure requires investigation.' >&2; exit 1; }
exec 9>"$ROOT/../setup/.device.lock"
flock -n 9 || { echo 'Another workspace job owns et0.' >&2; exit 1; }
command -v fuser >/dev/null
sudo -n true
if sudo -n fuser /dev/et0_ops /dev/et0_mgmt >"$OUT/owners.log" 2>&1; then
    echo 'A process has et0 open; refusing to interfere.' >&2; exit 1
fi
for marker in "$ROOT/../et-platform/examples/hyenadna/artifacts/device-recovery-required.json" \
              "$ROOT/../et-platform/examples/hyenadna/artifacts/stall-profiling/device-recovery-required.json"; do
    [[ ! -e $marker ]] || { echo "Recovery blocker exists: $marker" >&2; exit 1; }
done
export FF_ET_SYSEMU=0 FF_ET_MEM_CHECK=0 FF_ET_DEVICE=0 FF_ET_SHIRE_MASK=1
export FF_ET_KERNEL=/work/et-kernels/build-device/et_mpeg2_slice.elf
snapshot() {
    cat /sys/bus/pci/devices/0000:01:00.0/err_stats/ce_count >"$OUT/$1.ce"
    cat /sys/bus/pci/devices/0000:01:00.0/err_stats/uce_count >"$OUT/$1.uce"
    if ! "$ROOT/et-tools/et-env" timeout 45 dev_mngt_service -n 0 \
        -m DM_CMD_GET_MM_ERROR_COUNT -u 30000 >"$OUT/$1.mm" 2>&1; then
        echo "MM query failed during $NAME-h$HARTS; inspect $OUT" >"$ROOT/build-et/silicon/RECOVERY_REQUIRED"
        exit 1
    fi
}
snapshot before
grep -q 'MM Hang Count: 0$' "$OUT/before.mm" || { echo 'Nonzero MM hang count; stopping.' >&2; exit 1; }
"$ROOT/et-tools/et-env" timeout 45 dev_mngt_service -n 0 \
    -m DM_CMD_GET_MODULE_FIRMWARE_REVISIONS -u 30000 >"$OUT/firmware.log" 2>&1
printf 'begin %s\n' "$(date -u +%FT%TZ)" >"$OUT/timestamps"
status=0
"$ROOT/et-tools/et-env" timeout 180 build-et/host/ffmpeg -nostdin -nostats -benchmark -v verbose \
    -xerror -err_detect explode -hwaccel et -et_harts "$HARTS" -i "$INPUT" \
    -f framemd5 -y "/work/build-et/silicon/$NAME-h$HARTS/output.md5" >"$OUT/decode.log" 2>&1 || status=$?
printf 'end %s\nexit %s\n' "$(date -u +%FT%TZ)" "$status" >>"$OUT/timestamps"
snapshot after
# Management log prefixes contain timestamps; compare only actual MM counts.
grep -Eo 'MM (Hang|Exception) Count: [0-9]+' "$OUT/before.mm" >"$OUT/before.counts"
grep -Eo 'MM (Hang|Exception) Count: [0-9]+' "$OUT/after.mm" >"$OUT/after.counts"
if ! cmp "$OUT/before.ce" "$OUT/after.ce" || ! cmp "$OUT/before.uce" "$OUT/after.uce" ||
   ! cmp "$OUT/before.counts" "$OUT/after.counts"; then
    echo "Hardware counters changed during $NAME-h$HARTS; inspect $OUT" >"$ROOT/build-et/silicon/RECOVERY_REQUIRED"
    exit 1
fi
[[ $status == 0 ]] || { cat "$OUT/decode.log" >&2; exit "$status"; }
cmp "$ROOT/$GOLDEN" "$OUT/output.md5"
expected=$(grep -c '^0,' "$ROOT/$GOLDEN")
actual=$(grep -c 'ET frame [0-9].*harts=' "$OUT/decode.log")
[[ $expected == "$actual" ]] || { echo 'Missing ET completion records' >&2; exit 1; }
printf 'PASS silicon: %s harts=%s frames=%s, exact MD5, unchanged health counters\n' "$NAME" "$HARTS" "$actual" | tee "$OUT/result.txt"
