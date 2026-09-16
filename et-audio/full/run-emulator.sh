#!/usr/bin/env bash
# Standalone simulation only: no PCIe nodes, health reads, or hardware recovery.
set -euo pipefail
ROOT=$(dirname "$(dirname "$(dirname "$(readlink -f "$0")")")")
[[ ${FF_ET_ALLOW_PCIE:-0} == 0 ]] || exit 2
NAME=${1:?name}; INPUT=${2:?input}; GOLD=${3:?gold}; CHUNK=${4:?chunk}; METER=${5:-0}
KERNEL=${ETAAC_FULL_ELF:?explicit candidate ELF}
[[ $NAME =~ ^[a-zA-Z0-9_-]+$ && $CHUNK =~ ^[0-9]+$ && ($METER == 0 || $METER == 1) ]] || exit 2
[[ $KERNEL == build-et/aac-full/* && $KERNEL != *..* && $INPUT == build-et/aac-full/fixtures/*.input && $INPUT != *..* && $GOLD == build-et/aac-full/fixtures/*.scalar.pcm && $GOLD != *..* ]] || exit 2
OUT="$ROOT/build-et/aac-full/emulator/$NAME"
[[ ! -e $OUT ]] || { echo 'Refusing existing evidence directory' >&2; exit 2; }
FF_ET_ALLOW_PCIE=0 "$ROOT/et-tools/et-env" python3 et-audio/full/check-device.py "/work/$KERNEL" /opt/et/bin/riscv64-unknown-elf-
mkdir -p "$OUT"
sha256sum "$ROOT/$KERNEL" "$ROOT/build-et/aac-full/host/full-emulator-run" "$ROOT/$INPUT" "$ROOT/$GOLD" >"$OUT/provenance.sha256"
rc=0
FF_ET_ALLOW_PCIE=0 FF_ET_SYSEMU=1 "$ROOT/et-tools/et-env" timeout 900 env ETAAC_FULL_EMULATOR_GUARD=1 \
 build-et/aac-full/host/full-emulator-run "/work/$KERNEL" "/work/$INPUT" "/work/$GOLD" "$CHUNK" "$METER" \
 "/work/build-et/aac-full/emulator/$NAME/actual.pcm" "/work/build-et/aac-full/emulator/$NAME/state.bin" \
 >"$OUT/run.jsonl" 2>"$OUT/run.log" || rc=$?
printf '%s\n' "$rc" >"$OUT/exit"
[[ $rc == 0 ]] || exit "$rc"
python3 - "$OUT" "$ROOT/$KERNEL" <<'PY'
import hashlib,json,pathlib,sys
out=pathlib.Path(sys.argv[1]); elf=pathlib.Path(sys.argv[2])
r=[json.loads(s) for s in (out/'run.jsonl').read_text().splitlines() if s.startswith('{')]
assert r[0]['backend']=='sys_emu-shire0-hart0' and r[0]['protocol']==4
assert r[-1]['pass'] and not r[-1]['pcm_mismatches']
assert r[-1]['exact_packet_frames']==r[0]['packets']
assert all(not x['result'] and not x['heap_failures'] and not x['unsupported_calls'] for x in r if x['type']=='status')
assert all(x['stack_guard_ok']==1 for x in r if x['type']=='context_publication')
(out/'PASS.json').write_text(json.dumps({'pass':True,'elf_sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),'config':r[0],'result':r[-1]},indent=2)+'\n')
PY
