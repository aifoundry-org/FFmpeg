# Private full AAC decoder compile-only feasibility

This directory is separate from the synthesis-only experiments. It asks only
whether the pinned **FFmpeg n7.1.1** float AAC decoder and its minimal
`libavcodec`/`libavutil` archive closure compile for freestanding `rv64imf` /
`lp64f` general-purpose cores. It is not an ET kernel, runtime adapter,
runnable decoder, benchmark, or hardware/emulator test. The audit creates only
a relocatable `ld -r` closure; it never makes a final target executable.

`build_and_audit.py` creates a private `git archive` of tag `n7.1.1`
(`db69d06eeeab4f46da15030a80d539efb4503ca8`) in a fresh output root and never
edits upstream source. It enables only the float AAC decoder and disables fixed
AAC, asm, network, threads, programs, and other FFmpeg libraries. It refuses an
existing output root and does not stub functions.

`et-tools/et-env` consumes the host `FF_ET_ALLOW_PCIE` switch to select exposed
device nodes but does not forward it. The script treats an absent inner variable
as that safe default, and rejects an explicitly forwarded nonzero value. Use the
following explicit command for both layers:

```sh
FF_ET_ALLOW_PCIE=0 et-tools/et-env env FF_ET_ALLOW_PCIE=0 \
  python3 et-audio/decoder/build_and_audit.py
```

Products go under the dated default
`build-et/aac-decoder/rv64imf-lp64f-20260916-r6/` (or a fresh explicit
`--output-root`): private source tar/tree, logs, static archives, hashes, and
symbol/global-init/heap/libm/ISA audits. `--audit-existing OUTPUT_ROOT` makes a
new audit-rerun record without replacing retained source, build, or logs.

## Result — 2026-09-16

**Compile feasibility: pass; runnable decoder: not established.** The retained
fresh result `build-et/aac-decoder/rv64imf-lp64f-20260916-r6/RESULT` is PASS:
`CONFIG_AAC_DECODER=1`, `CONFIG_AAC_FIXED_DECODER=0`, and archive members
`aacdec.o` / `aacdec_float.o` are present. `aacdec_float.o` includes the float
DSP template in n7.1.1; there is no separate `aacdec_dsp_float.o` member. The
symbol-rooted closure is rooted at `ff_aac_decoder`. Exact source/archive hashes
and command/configure/build logs are retained in that output.

No compressed/RVV instructions or constructor sections were found. Genuine
remaining integration work is substantial: the closure has **17,061,200 bytes**
of `.bss + .sbss`; mutable AAC `AVOnce` table initialization; allocator
requirements (`free`, `memalign`, `realloc`); libc/libm calls; and RV64
compiler double/quad soft-float helpers. No threads are linked, but the
no-thread `ff_thread_once` is inlined, so first initialization must be
serialized by the eventual runtime and BSS must be initialized deliberately.

The source already includes the real `<inttypes.h>` in `dict.c`. With this SDK,
`-ffreestanding` suppresses newlib's `__int64_t_defined` formatting guard and
hides `PRId64`; the build restores that verified LP64 header guard with
`-D__int64_t_defined=1`. This is a header portability adaptation only—no source
edit or fake implementation. FFmpeg also appends `-fno-signed-zeros`; the build
requests `-ffp-contract=off`, but there is **no target numerical/exactness
claim** until a linked runtime is checked against AAC-LC packet/PCM references.

The configured AAC archive includes SBR/USAC-related translation units, so
configure is not an AAC-LC admission policy. A runnable follow-on needs a real
freestanding libc/libm/libgcc/allocator contract, BSS and one-time-init
ownership, packet/PCM buffer ownership, an explicit LC gate (object type,
rate/layout, unsupported tools), a final link, and packet-level correctness
validation.
