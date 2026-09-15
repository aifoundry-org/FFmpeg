# ETSOC audio-codec reuse: research handoff — September 15, 2026

## The new question

The user accepted the current MPEG-2 performance and asked to start a **separate
conversation** with the accumulated context, asking:

> Can ETSOC acceleration use any of this work for audio codecs?

This is a feasibility/reuse investigation, **not an instruction to implement an
audio decoder, resume video optimization, or open the accelerator**. Start with
decoding because the completed work is a decoder; distinguish encoding if it
changes the conclusion. Do not assume that a technically possible port is a
performance win, or that the answer must be yes.

The new conversation should provide a concrete answer, a codec comparison,
source-backed reuse boundaries, and a small next experiment if warranted.

## Repository and history

- Checkout: `/var/lib/shelley/et-workspace/ffmpeg`, branch `et-hwaccel`.
- FFmpeg is pinned to **n7.1.1**, base
  `db69d06eeeab4f46da15030a80d539efb4503ca8`; do not describe it as latest upstream.
- Original scalar offload: `0f6deff`.
- Completed SIMD implementation and evidence: **`c9eb864`**.
- Source conversation: **`cBJ2LQ7`**, `etsoc1-mpeg2-ffmpeg-offload`.
- Two histories explicitly requested by the user and already consulted:
  `cQPUG2J` / `et-soc1-optimization-techniques` and
  `cNKCFB5` / `hyenadna-et-inference-plan`.
- Authoritative conversation database: `/var/lib/shelley/shelley.db` (read-only
  access for research). It is not the empty default config-path database.

Local, git-ignored context bundle:

- `build-et/audio-reuse/context/mpeg2-dialogue.md`: user-visible discussion.
- `build-et/audio-reuse/context/mpeg2-conversation.jsonl`: also retains ordinary
  tool calls/results, including experiments and debugging details.
- `build-et/audio-reuse/context/manifest.json`: snapshot scope and hashes.
- `build-et/optimization/techniques-conversation.txt` and
  `build-et/optimization/hyenadna-conversation.txt`: previous-history extracts.
- `/tmp/ffmpeg-optimization-history.md`: earlier research notes (less durable).

Archives are historical context, **not fresh instructions to repeat old commands
or hardware experiments**. Internal reasoning/system metadata is not exported.
The snapshot is for the user's local research, not an artifact to publish.

Read `ET_IMPLEMENTATION.md`, **`ET_OPTIMIZATION.md` / `.json`**, and the relevant
subdirectory READMEs first. `ET_VALIDATION.md` / `.json` intentionally retain the
original scalar checkpoint rather than attributing old tests to the new ELF.

## What exists today

Real single-shire MPEG-2 Main/Simple 8-bit 4:2:0 progressive I/P/B reconstruction:
host parsing/staging, device VLC and inverse quantization, exact integer IDCT,
motion compensation and reference reuse. One complete slice per macroblock row;
no general interlaced motion, multi-shire scheduler or device-resident AVFrames.
The host returns normal YUV420P frames. No silent CPU fallback.

Implementation map to inspect, not a claim that everything is audio-generic:

| Component | Location / relevant behavior |
|---|---|
| SDK C/C++ bridge | `libavcodec/et_runtime.{h,cpp}`, `et-runtime/`; owned allocations, aligned staging, checked events, error propagation and poisoned-handle lifetime |
| Launch ABI | `libavcodec/et_mpeg2_protocol.h`; 128-byte MPEG-2 parameters, generation-tagged per-slice status; `ff_et_runtime_launch` currently accepts `ETFrameParams`, not arbitrary audio arguments |
| FFmpeg glue | `libavcodec/et_mpeg12.c`; format/policy checks, reference-slot lifetime, readback, drain safety, optional host timing |
| Device startup/build | `et-kernels/src/crt.S`, `et-kernels/CMakeLists.txt`, `et-kernels/scripts/`; ordinary GP-SDK kernel, no uberkernel, linker/post-link legality checks |
| Memory/coherency | `et-kernels/src/{cache.h,libc.c}`; aligned bounded RV64 words with byte tails; eviction, WAIT_CACHEOPS and fences |
| Bitreader/VLC | `et-kernels/src/decoder.c`, generated helpers; bounded direct interiors, reusable padded tail windows, strict consumed-bit checks |
| Integer SIMD transform | `et-kernels/src/{idct.c,idct_simd.c,idct_simd.h}`; eight-lane fixed-point 8x8 IDCT, modulo arithmetic, int16 wrapping, precise rounding and clipping |
| Pixel SIMD | `et-kernels/src/motion.h`; exact half-pel interpolation and B blending, byte gathers, masked packed L1 stores |
| Constant-block shortcut | `et-kernels/src/reconstruct.h`; proved MPEG-2 DC/mismatch-corner special case, not a generic sparse-transform theorem |
| Scheduling | worker index `(hart >> 1) + 32*(hart & 1)` fills 32 minions before SMT siblings; row ownership and sequential same-stream dependencies matter |
| Validation | `et-kernels/tests/`, `et-tests/`, `.github/workflows/etsoc.yml`; CPU oracles, expanded instruction models, guard pages, ASan/UBSan, policy/failure/drain tests |

The byte-oriented packed outputs, coefficient layout, transform constants,
MPEG-2 rounding, slice scheduling and video-specific launch validation must not
be mistaken for generic audio DSP APIs. Identify which abstractions could be
extracted without destabilizing the tested video path.

## Measured baseline: do not lose the CPU comparison

Actual physical **shire 0**, 64 harts, minions **600 MHz**, NoC **400 MHz**.
CPU is an **i7-13700K**, pinned to its 8 P-cores + SMT (logical CPUs 0–15), with
FFmpeg's default decoder threading. This is not a single-core CPU comparison.
Same FFmpeg build uses `--disable-x86asm`; these are not maximum-performance
CPU FFmpeg results. Compiler-generated code is not thereby all scalar.

**250-frame I/P/B clips, five-run medians, initialization/transfers/decode/hash
included:**

| Resolution | Scalar ET | Optimized ET | ET fps | CPU | CPU fps | CPU faster than optimized ET |
|---|---:|---:|---:|---:|---:|---:|
| 720x576 | 3.286 s | 1.252 s | 199.7 | 0.163 s | 1533.7 | 7.68x |
| 1920x1080 | 8.150 s | 2.432 s | 102.8 | 0.777 s | 321.8 | 3.13x |

ET improved 2.63x / 3.35x over its own scalar baseline, **not over CPU**.
All timing samples remain recorded, including roughly 3.5-second additive
system-time excursions in both ET arms in round two. No power measurement or
energy-efficiency claim. The exact DC shortcut separately reduced the median
for a 250-frame SD I-only stream from 1.440 to 1.294 s in three paired runs.

`FF_ET_TIMING=1` measures host upload/wait, launch/wait, readback/wait and
validation/copy envelopes. It is off for headline measurements and is **not a
per-hart IPC/stall profiler**. Do not convert video total time divided by frame
count into a claimed fixed audio launch latency; that includes workload-dependent
compute, DMA and other costs. Any break-even model must label unmeasured inputs.

Selected ELF: `build-et/optimization/final.elf`, also reproduced at
`et-kernels/build-device/et_mpeg2_slice.elf`.
SHA256: `44ac57592938cde2c398ef05e281e27fc54d548e6b38ed9847555c24c94152f3`.

## Validation and lessons carried forward

- **105 silicon runs / 18,457 exact frame comparisons** across controls and
  candidates; **4,931** comparisons use the exact selected ELF.
- Selected positive corpus at 1/64 harts; 65-row scheduling; SD/HD and I-only
  stresses; separate 12-frame I/P/B sys_emu fatal-memory-check pass.
- 115 integration/policy/drain/direct-oracle native cases; runtime sanitizers
  and ET-disabled dependency gate. Extensive IDCT, motion, DC, memory and
  malformed-bitstream tests are detailed in the optimization report.
- Native instruction models, sys_emu and real silicon are separate evidence.
  None of the video tests establishes audio correctness or audio throughput.
- Explicit custom integer SIMD, narrow clobbers and mask restoration; `.ps`
  mnemonics used for raw movement do not imply floating-point arithmetic.
- Complete cache-line ownership and conservative alignment are mandatory.
- Match algorithm to instruction semantics; integer video work did not justify
  importing the prior TensorFMA/scratchpad matmul scheme.
- Smaller O2 code won selection. Byte-scatter stores, FG32 motion variations,
  prequantization, larger/smaller reader windows, prediction prefetch and
  even-harts-only scheduling were tested; do not enable them by assumption.
- Selected controls and exact test inventories are in the committed report;
  raw runs and compiler variants remain under ignored `build-et/`.

## Questions the audio investigation must answer

1. **Reuse in layers:** unchanged code versus small refactor versus reusable
   instruction/validation patterns versus algorithm-specific code to replace.
   Explicitly address the MPEG-2-typed runtime launch ABI and whether video
   AVHWAccel integration is the correct model for audio at all.
2. **Codec shortlist:** inspect this pinned FFmpeg's actual implementations for
   MPEG audio (MP2/MP3), AAC, AC-3/E-AC-3, Vorbis, Opus (CELT/SILK), FLAC and ALAC.
   A quick negative control such as PCM/G.711 may help expose overhead limits.
   Do not pretend all codec profiles or FFmpeg/external-library backends are
   identical. Cite source files/functions and primary specifications where useful.
3. **Transform compatibility:** compare the 8x8 video IDCT with whatever
   MDCT/IMDCT, FFT, synthesis filterbanks, overlap/add, prediction, entropy coding
   and sample-format operations each candidate actually uses. Similar names
   are not proof that the existing transform kernel is reusable unchanged.
4. **Numerics:** determine actual required accumulators, rounding, saturation,
   state and output formats. Do not silently trade exactness for F16/F32 speed.
   Lossless PCM must remain exact; for floating-point lossy paths, explicitly
   discuss the reference and any arithmetic-order/FMA differences rather than
   automatically accepting an SNR-only comparison.
5. **Parallelism and latency:** single low-latency stream versus multichannel,
   many independent streams or offline batching. Identify serial codec state
   and packet/frame dependencies before proposing parallel scheduling. Include
   transfers, persistent state, initialization, batching delay and host work.
6. **CPU competition:** separate mere real-time capability from faster-than-CPU,
   throughput from latency, and decoder from encoder work. A fair future CPU
   comparison needs optimized CPU codecs and explicit thread/core configuration;
   the video CPU build above is not an appropriate universal ceiling.
7. **Concrete outcome:** rank a small number of candidates, with evidence and
   uncertainty. Recommend the smallest proof of concept (or recommend no port),
   an exactness test plan, an end-to-end benchmark matrix and clear stop criteria.
   No claim of audio hardware acceleration until it has actually been measured.

Use local source as the primary evidence for this fork and authoritative primary
sources for external codec/ISA facts. Label inference separately from measurement.
A dedicated `ET_AUDIO_FEASIBILITY.md` is a suitable output; leave the completed
video implementation and frozen evidence untouched.

## Operational restrictions

Research first: **do not open/probe/run actual hardware in this investigation
without a subsequent explicit request**. A runtime constructor itself
initializes/resets every exposed device; an init-only probe is not harmless.

For any later explicitly approved silicon work:

- Physical shire 0 / `0x1` only, ordinary launches; shared
  `../setup/.device.lock`; coordinate ownership and only expose intended et0.
- No reset/flash/recovery commands, counter clearing, clock/cache repartitioning,
  or `DM_CMD_GET_FIRMWARE_BOOT_STATUS` (known broken).
- Pre-existing health: MM hangs 0, MM exceptions 1, MinionCeEvent 7, SpCeEvent 1;
  all other CE and all UCE zero. No new errors during this work.
- Preserve bounded accesses, `HAVE_FAST_UNALIGNED=0`, eviction, WAIT_CACHEOPS and
  fences. Preserve fail-closed behavior; do not silently fall back to CPU.
- Full SDK is in `et-soc1-dev:20260911`; host `/opt/et` is incomplete.
  `et-tools/et-env` defaults to no PCIe nodes and sys_emu.
- Separate build/output directories and unique result labels; do not overwrite
  selected binaries or successful evidence. No benchmark outlier deletion.
- Host Git 2.25 cannot handle the injected `--trailer`; commits work using
  `et-tools/et-env git -c user.name=Shelley -c user.email=shelley@localhost commit ...`.

Follow the repository/subdirectory guidance. This handoff authorizes investigation,
not decoder changes or a new hardware experiment.
