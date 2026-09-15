#!/usr/bin/env bash
# Optional real SDK/system emulator test. Exit 77 means SKIP, never PASS.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FF=${1:-"$ROOT/build-et/host/ffmpeg"}
OUT=${2:-"$ROOT/et-tests/build-sysemu-results"}
SDK=${FF_ET_SDK:-/opt/et}
skip() { echo "SKIP: SYSEMU memory-check test: $* (no simulator validation performed)" >&2; exit 77; }
[[ ${FF_ET_MEM_CHECK:-0} == 1 ]] || skip 'explicit FF_ET_MEM_CHECK=1 opt-in required'
[[ -x "$FF" ]] || skip "production FFmpeg unavailable: $FF"
[[ -f ${FF_ET_KERNEL:-} ]] || skip 'FF_ET_KERNEL must name a built ET device ELF'
[[ -x "$SDK/bin/sys_emu" ]] || skip "system emulator unavailable in $SDK"
for file in BootromTrampolineToBL2/BootromTrampolineToBL2.elf \
    ServiceProcessorBL2/fast-boot/ServiceProcessorBL2_fast-boot.elf \
    MachineMinion/MachineMinion.elf MasterMinion/MasterMinion.elf WorkerMinion/WorkerMinion.elf; do
    [[ -f "$SDK/lib/esperanto-fw/$file" ]] || skip "SDK firmware missing: $file"
done
export FF_ET_SYSEMU=1 FF_ET_MEM_CHECK=1 FF_ET_ALLOW_PCIE=0
unset FF_ET_TEST_FAIL
# The production runtime maps MEM_CHECK=1 to SysEmuOptions.memcheck (-mem_check)
# and -Werror=memory. No native runtime substitution is permitted by run.sh.
echo 'SYSEMU ONLY: requesting -mem_check and -Werror=memory; hardware NOT accessed.' >&2
exec "$ROOT/et-tests/run.sh" "$FF" "$OUT" sysemu
