#!/usr/bin/env bash
# Explicit, locked, shire-0-only AAC experiment. No reset/recovery/counter clear.
set -euo pipefail
ROOT=$(dirname "$(dirname "$(readlink -f "$0")")")
[[ ${FF_ET_ALLOW_PCIE:-0} == 1 ]] || { echo 'Explicit FF_ET_ALLOW_PCIE=1 required' >&2; exit 2; }
NAME=${1:?name}; TASKS=${2:?tasks}; HARTS=${3:?harts}; FRAMES=${4:?frames}; OP=${5:?operation}
[[ $NAME =~ ^[a-zA-Z0-9_-]+$ && $TASKS =~ ^[0-9]+$ && $FRAMES =~ ^[0-9]+$ && ($HARTS == 1 || $HARTS == 32 || $HARTS == 64) && ($OP == 0 || $OP == 1) ]] || exit 2
((TASKS>=1 && TASKS<=64 && FRAMES>=1 && FRAMES<=1000)) || exit 2
CAPTURE=${6:-}
CAPTURE_ARGS=()
if [[ -n $CAPTURE ]]; then
 [[ $CAPTURE == build-et/aac-prototype/captures/* && $CAPTURE != *..* ]] || exit 2
 CAPTURE_ARGS=("/work/$CAPTURE")
 FF_ET_ALLOW_PCIE=0 "$ROOT/et-tools/et-env" build-et/aac-prototype/host/etaac-replay-native "/work/$CAPTURE"
 python3 - "$ROOT" "$CAPTURE" "$FRAMES" <<'PY'
import importlib.util,sys
from pathlib import Path
root=Path(sys.argv[1])
spec=importlib.util.spec_from_file_location('capture_inspect',root/'et-audio/capture/inspect_capture.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
d=m.inspect(root/sys.argv[2])
assert max(d['contexts'].values())>=int(sys.argv[3]),'capture has no complete timeline'
PY
fi
OUT="$ROOT/build-et/aac-prototype/silicon/$NAME"
[[ ! -e $OUT ]] || { echo "Refusing to overwrite $OUT" >&2; exit 2; }
mkdir -p "$OUT"
BLOCK="$ROOT/build-et/aac-prototype/RECOVERY_REQUIRED"
for marker in "$BLOCK" "$ROOT/build-et/silicon/RECOVERY_REQUIRED" \
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
KERNEL=${ETAAC_KERNEL:-build-et/aac-prototype/device/et_aac_synth.elf}
RUNNER=build-et/aac-prototype/host/etaac-run
[[ $KERNEL == build-et/aac-prototype/* && $KERNEL != *..* ]] || exit 2
sha256sum "$ROOT/$KERNEL" "$ROOT/$RUNNER" >"$OUT/provenance.sha256"
if [[ -n $CAPTURE ]]; then sha256sum "$ROOT/$CAPTURE" >>"$OUT/provenance.sha256"; fi
printf 'capture=%s\n' "$CAPTURE" >"$OUT/capture.txt"
printf 'tasks=%s\nharts=%s\nframes=%s\noperation=%s\ncpu_affinity=0\n' "$TASKS" "$HARTS" "$FRAMES" "$OP" >"$OUT/config.txt"
active=0
trap 's=$?; if [[ $active == 1 && $s != 0 ]]; then printf "AAC run failed/interrupted: %s status=%s; inspect before any further device access\n" "$OUT" "$s" >"$BLOCK"; fi' EXIT
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
"$ROOT/et-tools/et-env" timeout 180 taskset -c 0 env ETAAC_HW_GUARD=1 \
 "$RUNNER" "/work/$KERNEL" "$TASKS" "$HARTS" "$FRAMES" "$OP" "${CAPTURE_ARGS[@]}" \
 >"$OUT/run.jsonl" 2>"$OUT/run.log" || rc=$?
printf 'end %s\nexit %s\n' "$(date -u +%FT%TZ)" "$rc" >>"$OUT/timestamps"
snapshot after
cmp "$OUT/before.ce" "$OUT/after.ce"
cmp "$OUT/before.uce" "$OUT/after.uce"
cmp "$OUT/before.counts" "$OUT/after.counts"
if [[ $rc != 0 ]]; then cat "$OUT/run.log" >&2; exit "$rc"; fi
python3 - "$OUT/run.jsonl" "$TASKS" "$FRAMES" <<'PY'
import json,sys
rows=[json.loads(s) for s in open(sys.argv[1]) if s.startswith('{')]
assert rows[0]['backend']=='silicon-shire0'
assert rows[-1]['pass'] and rows[-1]['exact_channel_frames']==int(sys.argv[2])*int(sys.argv[3])
assert len([x for x in rows if x['type']=='batch' and x['exact']])==int(sys.argv[3])
PY
printf 'PASS shire0: tasks=%s harts=%s frames=%s operation=%s exact PCM/state; unchanged health\n' "$TASKS" "$HARTS" "$FRAMES" "$OP" | tee "$OUT/result.txt"
active=0
