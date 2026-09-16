#!/usr/bin/env bash
# Explicit, locked, shire-0-only AAC ABI-3 profiling experiment. No reset/recovery/counter clear.
set -euo pipefail
ROOT=$(dirname "$(dirname "$(dirname "$(readlink -f "$0")")")")
[[ ${FF_ET_ALLOW_PCIE:-0} == 1 ]] || { echo 'Explicit FF_ET_ALLOW_PCIE=1 required' >&2; exit 2; }
NAME=${1:?name}; TASKS=${2:?channels}; HARTS=${3:?harts}; FRAMES=${4:?frames}; CHUNK=${5:?chunk}; MODE=${6:?mode}; OP=${7:?operation}
CAPTURE=${8:-build-et/aac-offline/captures/stereo-48000.etaaccap}
[[ $NAME =~ ^[a-zA-Z0-9_-]+$ && $TASKS =~ ^[0-9]+$ && $FRAMES =~ ^[0-9]+$ && $CHUNK =~ ^[0-9]+$ && ($HARTS == 1 || $HARTS == 32 || $HARTS == 64) && ($OP == 0 || $OP == 1) && ($MODE == 0 || $MODE == 1 || $MODE == 2) ]] || exit 2
((TASKS>=1 && TASKS<=64 && FRAMES>=1 && FRAMES<=512 && CHUNK>=1 && CHUNK<=FRAMES && FRAMES%CHUNK==0)) || exit 2
(( MODE != 2 || (OP == 0 && CHUNK == FRAMES) )) || exit 2
[[ ${ETAAC_PROFILE_ENABLE:-1} == 0 || ${ETAAC_PROFILE_ENABLE:-1} == 1 ]] || exit 2
[[ ( $CAPTURE == build-et/aac-profile/captures/* || $CAPTURE == build-et/aac-offline/captures/* ) && $CAPTURE != *..* ]] || exit 2
FF_ET_ALLOW_PCIE=0 "$ROOT/et-tools/et-env" build-et/aac-prototype/host/etaac-replay-native "/work/$CAPTURE"
python3 - "$ROOT" "$CAPTURE" "$FRAMES" <<'CHECK_CAPTURE'
import importlib.util,sys
from pathlib import Path
root=Path(sys.argv[1])
spec=importlib.util.spec_from_file_location('capture_inspect',root/'et-audio/capture/inspect_capture.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
d=m.inspect(root/sys.argv[2])
assert max(d['contexts'].values())>=int(sys.argv[3]),'capture has no complete timeline'
CHECK_CAPTURE
OUT="$ROOT/build-et/aac-profile/silicon/$NAME"
[[ ! -e $OUT ]] || { echo "Refusing to overwrite $OUT" >&2; exit 2; }
mkdir -p "$OUT"
BLOCK="$ROOT/build-et/aac-profile/PROFILE_RECOVERY_REQUIRED"
for marker in "$BLOCK" "$ROOT/build-et/aac-offline/RECOVERY_REQUIRED" "$ROOT/build-et/aac-prototype/RECOVERY_REQUIRED" "$ROOT/build-et/silicon/RECOVERY_REQUIRED" \
 "$ROOT/../et-platform/examples/hyenadna/artifacts/device-recovery-required.json" \
 "$ROOT/../et-platform/examples/hyenadna/artifacts/stall-profiling/device-recovery-required.json"; do
 [[ ! -e $marker ]] || { echo "Recovery blocker: $marker" >&2; exit 1; }
done
exec 9>"$ROOT/../setup/.device.lock"
flock -n 9 || { echo 'Another workspace job owns et0' >&2; exit 1; }
sudo -n true
if sudo -n fuser /dev/et0_ops /dev/et0_mgmt >"$OUT/owners.log" 2>&1; then
 echo 'et0 is open; refusing to interfere' >&2; exit 1
fi
export FF_ET_SYSEMU=0 FF_ET_MEM_CHECK=0 FF_ET_DEVICE=0 FF_ET_SHIRE_MASK=1
# The measured offline target is the aligned-load finite-check variant.  The
# default control build remains available only through an explicit override.
KERNEL=${ETAAC_KERNEL:-build-et/aac-profile/device-fast/et_aac_profile.elf}
RUNNER=${ETAAC_RUNNER:-build-et/aac-profile/host-fast/profile-run}
[[ $KERNEL == build-et/aac-profile/* && $KERNEL != *..* && $RUNNER == build-et/aac-profile/* && $RUNNER != *..* ]] || exit 2
sha256sum "$ROOT/$KERNEL" "$ROOT/$RUNNER" >"$OUT/provenance.sha256"
if [[ -n $CAPTURE ]]; then sha256sum "$ROOT/$CAPTURE" >>"$OUT/provenance.sha256"; fi
printf 'capture=%s\n' "$CAPTURE" >"$OUT/capture.txt"
printf 'channels=%s\nharts=%s\nframes=%s\nchunk=%s\nmode=%s\noperation=%s\nprofile=%s\ncpu_affinity=0\n' "$TASKS" "$HARTS" "$FRAMES" "$CHUNK" "$MODE" "$OP" "${ETAAC_PROFILE_ENABLE:-1}" >"$OUT/config.txt"
printf 'kernel=%s\nrunner=%s\nfast_finite=%s\n' "$KERNEL" "$RUNNER" "$([[ $KERNEL == *device-fast/* ]] && echo 1 || echo 0)" >>"$OUT/config.txt"
active=0
trap 's=$?; if [[ $active == 1 && $s != 0 ]]; then msg="AAC profile run failed/interrupted: $OUT status=$s; inspect before any further device access"; printf "%s\n" "$msg" >"$BLOCK"; global="$ROOT/build-et/silicon/RECOVERY_REQUIRED"; if [[ ! -e $global ]]; then printf "%s\n" "$msg" >"$global"; fi; fi' EXIT
snapshot() {
 cat /sys/bus/pci/devices/0000:01:00.0/err_stats/ce_count >"$OUT/$1.ce"
 cat /sys/bus/pci/devices/0000:01:00.0/err_stats/uce_count >"$OUT/$1.uce"
 "$ROOT/et-tools/et-env" timeout 45 dev_mngt_service -n 0 -m DM_CMD_GET_MM_ERROR_COUNT -u 30000 >"$OUT/$1.mm" 2>&1
 grep -Eo 'MM (Hang|Exception) Count: [0-9]+' "$OUT/$1.mm" >"$OUT/$1.counts"
}
active=1
snapshot before
[[ $(grep -c '^MM Hang Count: 0$' "$OUT/before.counts") == 1 ]]
[[ $(grep -c '^MM Exception Count: 1$' "$OUT/before.counts") == 1 ]]
python3 - "$OUT" <<'PY'
import pathlib,sys
p=pathlib.Path(sys.argv[1])
for kind in ('ce','uce'):
 data=dict(line.split(':') for line in (p/f'before.{kind}').read_text().splitlines())
 for k,v in data.items():
  expected={'MinionCeEvent':7,'SpCeEvent':1}.get(k,0) if kind=='ce' else 0
  assert int(v)==expected,(k,v,expected)
PY
printf 'begin %s\n' "$(date -u +%FT%TZ)" >"$OUT/timestamps"
rc=0
"$ROOT/et-tools/et-env" timeout 180 taskset -c 0 env ETAAC_PROFILE_HW_GUARD=1 \
 "$RUNNER" "/work/$KERNEL" "/work/$CAPTURE" "$TASKS" "$HARTS" "$FRAMES" "$CHUNK" "$MODE" "$OP" "${ETAAC_PROFILE_ENABLE:-1}" \
 >"$OUT/run.jsonl" 2>"$OUT/run.log" || rc=$?
printf 'end %s\nexit %s\n' "$(date -u +%FT%TZ)" "$rc" >>"$OUT/timestamps"
snapshot after
cmp "$OUT/before.ce" "$OUT/after.ce"
cmp "$OUT/before.uce" "$OUT/after.uce"
cmp "$OUT/before.counts" "$OUT/after.counts"
if [[ $rc != 0 ]]; then cat "$OUT/run.log" >&2; exit "$rc"; fi
python3 - "$OUT/run.jsonl" "$TASKS" "$FRAMES" "$CHUNK" "$MODE" "${ETAAC_PROFILE_ENABLE:-1}" <<'PY'
import json,sys
rows=[json.loads(s) for s in open(sys.argv[1]) if s.startswith('{')]
assert rows[0]['backend']=='silicon-shire0' and rows[0]['protocol']==3
enabled=int(sys.argv[6])
assert all(x.get('raw_hpmcounter3') is bool(enabled) for x in rows if x['type']=='channel_ticks')
assert rows[-1]['pass'] and rows[-1]['exact_channel_frames']==int(sys.argv[2])*int(sys.argv[3])
if int(sys.argv[5])==2:
 assert rows[-1]['consumer'] and len([x for x in rows if x['type']=='channel_meter'])==int(sys.argv[2])
else:
 assert len([x for x in rows if x['type']=='launch'])==int(sys.argv[3])//int(sys.argv[4])
PY
printf 'PASS profile shire0: tasks=%s harts=%s frames=%s operation=%s exact PCM/state; unchanged health\n' "$TASKS" "$HARTS" "$FRAMES" "$OP" | tee "$OUT/result.txt"
active=0
