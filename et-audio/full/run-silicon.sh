#!/usr/bin/env bash
# Explicit, locked, shire-0-only full AAC ABI-4 experiment. No reset/recovery/counter clear.
set -euo pipefail
ROOT=$(dirname "$(dirname "$(dirname "$(readlink -f "$0")")")")
[[ ${FF_ET_ALLOW_PCIE:-0} == 1 ]] || { echo 'Explicit FF_ET_ALLOW_PCIE=1 required' >&2; exit 2; }
NAME=${1:?name}; INPUT=${2:?input}; GOLD=${3:?scalar PCM}; CHUNK=${4:?chunk}; METER=${5:-0}
[[ $NAME =~ ^[a-zA-Z0-9_-]+$ && $CHUNK =~ ^[0-9]+$ && ($METER == 0 || $METER == 1) ]] || exit 2
[[ $INPUT == build-et/aac-full/fixtures/*.input && $GOLD == build-et/aac-full/fixtures/*.scalar.pcm && $INPUT != *..* && $GOLD != *..* ]] || exit 2
((CHUNK>=1 && CHUNK<=2048)) || exit 2
python3 - "$ROOT" "$INPUT" "$GOLD" "$CHUNK" <<'FIXTURE'
import pathlib,sys,json,hashlib
r=pathlib.Path(sys.argv[1]); i=r/sys.argv[2]; g=r/sys.argv[3]
m=json.loads((r/'build-et/aac-full/fixtures/MANIFEST.json').read_text())
c=[x for x in m['cases'] if x['input']==i.name and x['scalar_pcm']==g.name]
assert len(c)==1
c=c[0];assert c['packet_count']%int(sys.argv[4])==0
assert hashlib.sha256(i.read_bytes()).hexdigest()==c['input_sha256']
assert hashlib.sha256(g.read_bytes()).hexdigest()==c['scalar_pcm_sha256']
print('PASS immutable compressed input/scalar PCM provenance')
FIXTURE
KERNEL=${ETAAC_FULL_ELF:?Set explicit fully checked full AAC ELF path}
[[ $KERNEL == build-et/aac-full/* && $KERNEL != *..* ]] || exit 2
FF_ET_ALLOW_PCIE=0 "$ROOT/et-tools/et-env" python3 et-audio/full/check-device.py \
 "/work/$KERNEL" /opt/et/bin/riscv64-unknown-elf-
PROOF=${ETAAC_FULL_EMULATOR_PROOF:?Set explicit matching emulator PASS.json before hardware}
[[ $PROOF == build-et/aac-full/emulator/*/PASS.json && $PROOF != *..* ]] || exit 2
python3 - "$ROOT/$PROOF" "$ROOT/$KERNEL" <<'PROOF'
import hashlib,json,pathlib,sys
p=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert p['pass'] and p['config']['backend']=='sys_emu-shire0-hart0'
assert p['elf_sha256']==hashlib.sha256(pathlib.Path(sys.argv[2]).read_bytes()).hexdigest()
PROOF
OUT="$ROOT/build-et/aac-full/silicon/$NAME"
[[ ! -e $OUT ]] || { echo "Refusing to overwrite $OUT" >&2; exit 2; }
mkdir -p "$OUT"
BLOCK="$ROOT/build-et/aac-full/RECOVERY_REQUIRED"
for marker in "$BLOCK" "$ROOT/build-et/aac-profile/PROFILE_RECOVERY_REQUIRED" "$ROOT/build-et/aac-offline/RECOVERY_REQUIRED" "$ROOT/build-et/aac-prototype/RECOVERY_REQUIRED" "$ROOT/build-et/silicon/RECOVERY_REQUIRED" \
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
KERNEL=${ETAAC_FULL_ELF:?Set explicit fully checked full AAC ELF path}
RUNNER=build-et/aac-full/host/full-run
[[ $KERNEL == build-et/aac-full/* && $KERNEL != *..* ]] || exit 2
sha256sum "$ROOT/$KERNEL" "$ROOT/$RUNNER" "$ROOT/$INPUT" "$ROOT/$GOLD" >"$OUT/provenance.sha256"
printf 'input=%s\ngold=%s\nchunk=%s\nmeter=%s\n' "$INPUT" "$GOLD" "$CHUNK" "$METER" >"$OUT/config.txt"
active=0
trap 's=$?; if [[ $active == 1 && $s != 0 ]]; then msg="Full AAC run failed/interrupted: $OUT status=$s; inspect before any further device access"; printf "%s\n" "$msg" >"$BLOCK"; global="$ROOT/build-et/silicon/RECOVERY_REQUIRED"; if [[ ! -e $global ]]; then printf "%s\n" "$msg" >"$global"; fi; fi' EXIT
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
"$ROOT/et-tools/et-env" timeout 180 taskset -c 0 env ETAAC_FULL_HW_GUARD=1 \
 "$RUNNER" "/work/$KERNEL" "/work/$INPUT" "/work/$GOLD" "$CHUNK" "$METER" \
 "/work/build-et/aac-full/silicon/$NAME/actual.pcm" "/work/build-et/aac-full/silicon/$NAME/state.bin" \
 >"$OUT/run.jsonl" 2>"$OUT/run.log" || rc=$?
printf 'end %s\nexit %s\n' "$(date -u +%FT%TZ)" "$rc" >>"$OUT/timestamps"
snapshot after
cmp "$OUT/before.ce" "$OUT/after.ce"
cmp "$OUT/before.uce" "$OUT/after.uce"
cmp "$OUT/before.counts" "$OUT/after.counts"
if [[ $rc != 0 ]]; then cat "$OUT/run.log" >&2; exit "$rc"; fi
python3 - "$OUT/run.jsonl" <<'VERIFY'
import json,sys
r=[json.loads(s) for s in open(sys.argv[1]) if s.startswith('{')]
assert r[0]['backend']=='silicon-shire0-hart0' and r[0]['protocol']==4
assert r[-1]['pass'] and r[-1]['pcm_mismatches']==0
assert r[-1]['exact_packet_frames']==r[0]['packets']
assert all(x['result']==0 and x['unsupported_calls']==0 and x['heap_failures']==0 for x in r if x['type']=='status')
VERIFY
printf 'PASS full compressed AAC decode shire0/hart0: exact PCM and clean completions; unchanged health\n' | tee "$OUT/result.txt"
active=0
