# AAC-LC synthesis prototype — September 16, 2026

## Result

**Real AAC synthesis now runs on ETSOC-1, but this split is not a speed win.**
The user explicitly authorized direct accelerator prototyping after the initial
research-only investigation. Physical **shire 0**, ordinary launches only.

The new scalar F32 kernel performs the AAC-LC 1024/128 inverse MDCT, sine/KBD
windowing, and persistent overlap update. It matches the pinned FFmpeg scalar
float arithmetic **bit for bit**, including saved state. There is no reduced
precision, contracted FMA, video-IDCT substitution or silent CPU fallback.

This is **not a complete offloaded AAC decoder** or an FFmpeg audio backend.
Host FFmpeg parses/dequantizes real AAC files into captured synthesis records;
the experimental runner replays those records on device. Entropy decoding,
stereo/TNS/PNS front-end work, packet handling, AVFrame integration, seek/drain,
priming/trimming and final container playback remain outside the device path.
The captured oracle is the synthesis boundary, not a claim about every later
AAC output-processing operation.

## Measured cost: current boundary loses even with zero DSP work

Five independent process runs per ET arm, 60 synthesis batches per run; median
of each run's **mean** staged service cost. No batch or run was discarded.
CPU stage uses the same 60-frame captured timeline, repeated 500 times per run,
five runs, **one pinned i7-13700K P-core logical CPU (CPU 0)**. This is not the
video experiment's 8-P-core CPU comparison. CPU assembly and normal dispatch
are enabled; a separate scalar-exact control is also shown.

| One ready batch, 1024 samples/channel | Optimized CPU stage | Scalar-exact CPU stage | ET scalar synthesis | ET copy-only control |
|---|---:|---:|---:|---:|
| 2 channels (one stereo stream) | **1.60 us** | 4.78 us | **3.035 ms** | 2.514 ms |
| 64 channels (32 stereo state clones) | **50.33 us** | 145.63 us | **3.541 ms** | 2.767 ms |

The 2-task arm uses 32 active harts (only two do work); the 64-task arm uses 64.
The device layer reports 600 MHz minion boot frequency. SDK image is
`et-soc1-dev:20260911`. No clocks, cache partitions or firmware were changed.

**Staged service** = input packing + H2D/wait + launch/wait + PCM and status
D2H/wait. Setup, CPU oracle work, extra state readback for validation, and exact
comparison are separately timed and excluded. Thus these are **not full AAC
end-to-end decode times**, nor kernel-only times or latency guarantees. Setup
is roughly 0.26 seconds per process in these runs. Every batch also checked the
resident state by reading it back; that diagnostic perturbs pacing and is not a
production asynchronous throughput pipeline. `FF_ET_TIMING` video numbers were
not used to infer any of these audio costs.

The copy arm preserves ABI validation, state/status publication and transfer
shape but does no IMDCT/window DSP. It establishes a useful overhead control
for **this synchronous interface**, not a universal fixed/minimum ET latency.
Even this control is much slower than the CPU stage, including the scalar
arithmetic-exact CPU path. Optimizing the transform alone cannot close the gap.

Retained per-run mean service times (milliseconds):

| Arm | All five runs |
|---|---|
| ET synthesis, 2 tasks | 3.025, 3.080, 3.023, 3.063, 3.035 |
| ET copy, 2 tasks | 2.498, 2.514, 2.514, 2.500, 2.515 |
| ET synthesis, 64 tasks | 3.662, 3.398, 3.652, 3.405, 3.541 |
| ET copy, 64 tasks | 2.125, 2.782, 2.747, 2.767, 2.781 |

The largest observed individual 64-task synthesis service was **13.280 ms**;
it remains included. Offline ready batches do not measure paced-arrival queueing
or batch-fill delay. The 32 stereo streams are independent state **clones of the
same two captured channels**, not 32 diverse codecs/contents running in an
integrated scheduler. No energy measurement or efficiency claim.

### Separate complete-decoder CPU baseline

For a generated **30-second stereo 48 kHz AAC-LC file**, five complete CPU CLI
decodes to the null muxer on CPU 0 took median **20.66 ms** with normal optimized
dispatch, versus **27.15 ms** with `-cpuflags 0`. These include process/setup,
parsing and decoding; they must not be compared as identical workloads with the
stage-only ET row. Full output hashes/sample counts were checked separately.
All five samples and build/input provenance are in `ET_AAC_PROTOTYPE.json` and
ignored `build-et/aac-prototype/baseline-30s-20260916/`.

The optimized CPU and scalar CPU float outputs are **not byte-identical**.
The full-decoder comparison found equal sample counts, maximum absolute
sample difference `1.7881393432617188e-7`, and RMS difference approximately
`6.402e-9`. This is characterization, not a relaxed device acceptance threshold.
Device output is required to equal the scalar reference exactly. Optimized
`av_tx`/`float_dsp` stage output is likewise characterized separately; the
scalar CPU stage test has zero differing samples.

## What was implemented and reused

All new code is under **`et-audio/`**, separate from the video implementation.

- **New DSP:** freestanding scalar float split-radix FFT/IMDCT and AAC window
  sequencing derived from pinned n7.1.1 `libavutil/tx_template.c`, `tx_priv.h`,
  `tx.c`, `float_dsp.c`, and `libavcodec/aac/aacdec_dsp_template.c`. Static table
  generator, source SHA manifest and attribution are in `dsp/`. There is no
  heap/libm/mutable-global requirement in the device DSP, and no audio SIMD yet.
- **New audio ABI and scheduler:** 128-byte `ETAACParams`; fixed independent
  channel slots, fresh generation statuses, owned/disjoint aligned regions,
  finite-input/output checks, transactional overlap update and 1/32/64-hart
  core-first scheduling. Errors cannot publish a valid partial result. Failed
  streams must be stopped; retry/fallback is not silently performed.
- **Reused infrastructure:** unchanged video CRT, bounded memory primitives,
  linker layout, cache eviction/WAIT_CACHEOPS/fences and base ELF checks. The
  audio post-link check additionally forbids FMA contraction, packed operations
  and double FP. Audio kernel `.text` is 8,188 bytes (+28-byte CRT), rodata
  20,672 bytes, BSS/data zero.
- **Isolated runtime adaptation:** a hash-checked generator reuses the existing
  SDK allocation/DMA/error/lifetime code but replaces its MPEG-2-typed launch
  with `etaac_rt_launch`. All five audio buffer ranges are validated. Only
  available physical shire 0 is permitted. Video source itself is untouched.
- **Private fixture integration:** an archived source copy receives only the
  AAC capture hook. The main FFmpeg sources are not patched. Generated fixtures
  cover mono/stereo, 44.1/48 kHz, silence, tones, transients and deterministic
  noise, with all four window sequences. This is a small generated corpus, not
  general AAC conformance coverage or representative commercial music testing.

No AAC Main/LTP, HE-AAC/SBR/PS, LD/ELD, USAC or 960-frame support is claimed.
The API takes already reconstructed spectral coefficients and metadata, not
untrusted AAC packets; a later codec integration must enforce profile policy.

## Validation actually executed

- **28 successful silicon runs:** **26,181 exact synthesis channel-frame
  comparisons**, plus **21,336 exact copy-control channel-frame comparisons**.
  Includes synthetic controls at 1/32/64 active harts, real mono/stereo captures,
  repeated timing arms and continuous resident overlap. Repetitions/clones are
  counted as comparisons, not distinct test vectors or complete audio decodes.
- **351 distinct captured synthesis records** replayed exactly on the native
  implementation, with continuous per-context saved-state checks. Long-lived
  record timelines were then used on silicon; short demux-probe contexts were
  not spliced into them.
- Independent pinned FFmpeg scalar library oracle: both transform sizes,
  32 random vectors per size, and all 64 previous/current sequence/shape
  combinations, checking output and overlap bits and scratch guard preservation.
- Native batch/ownership tests: **3,780 exact channel frames**, all 1/32/64-hart
  configurations and 1/8/32/64 tasks; invalid selectors, stale generations,
  bad counts/ranges/aliasing, NaN rejection, canaries and guarded input-end access.
- ASan/UBSan native protocol/DSP tests and all four captured timelines pass.
  The isolated runtime mock tests and their ASan/UBSan run pass. Final host
  CTest includes native, independent FFmpeg oracle and mock runtime tests.
- Device compile/post-link legality gates pass; final rebuild reproduced the
  exact silicon-tested kernel hash. **No sys_emu test was run in this prototype**;
  no emulator result or complete decoder/API-drain validation is claimed.

One attempted real-capture run stopped in **host preflight before runtime open**:
the capture contained short-lived demux-probe contexts, and the original runner
incorrectly required every context to have 55 frames. The conservative wrapper
created a blocker. Logs/unchanged health were reviewed; the marker was archived
with a resolution, complete timelines were selected explicitly, and a fresh-name
retry passed. That failed attempt remains under
`silicon/real-mono441-h1/`; it is not counted among the 28 silicon passes.

Every device run held the shared lock and checked before/after health. MM hangs
remain 0, MM exceptions 1; MinionCeEvent 7, SpCeEvent 1, other CE and all UCE 0.
No reset/flash/recovery command, counter clear, boot-status query or partition
change was performed. The SDK's normal constructor initialization is unchanged.

## Decision and next step

**Stop SIMD optimization of this split for now.** The measured copy-only control
already fails the proposed 1.2x CPU-performance target by a large margin, even
against the arithmetic-exact scalar CPU baseline. Single-stream playback should
stay on CPU. Fitting a stage within 21.33 ms is not evidence of lower latency or
faster complete decoding.

A later throughput experiment would need a materially different boundary:
substantially more ready work per launch (including multiple sequential frames
per stream for offline use), or audio already resident on ET for additional DSP.
That entails new scheduling/state/latency tradeoffs, not merely SIMD conversion.
A whole-codec port may reduce coefficient upload but adds entropy/dequantization
work; it is not justified by these measurements. No claim that all possible audio
workloads lose, but this concrete first AAC-LC design does.

## Artifacts and reproduction

`et-audio/README.md` contains build, capture, native-test, CPU-stage and explicitly
guarded silicon commands. `et-audio/results.py` reads retained results only.
Compact committed inventory: `ET_AAC_PROTOTYPE.json`. Full raw logs, fixtures,
health snapshots, CPU samples and builds: ignored `build-et/aac-prototype/`.
Successful run directories are immutable; reruns require unique names.

Selected audio ELF SHA256:
`7fccc6753d6f5440447d8e657096dd7ee58b7d7e28ce9f65825427b1d874c9e1`.
The tested runner and baseline hashes are in the JSON inventory. Frozen copies
are under `build-et/aac-prototype/selected/`.

The original video kernel copies still hash to
`44ac57592938cde2c398ef05e281e27fc54d548e6b38ed9847555c24c94152f3`.
Video source, defaults, selected binaries, `ET_OPTIMIZATION.*`, `ET_VALIDATION.*`
and the research-only `ET_AUDIO_FEASIBILITY.md` remain unchanged.
