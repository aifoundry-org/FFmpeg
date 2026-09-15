# ETSOC-1 audio-codec reuse feasibility

Research checkpoint: September 15, 2026. **No audio implementation, accelerator
access, runtime initialization, simulator run, or new performance measurement.**

## Bottom line

**Yes: reuse the offload infrastructure and engineering methods. No: none of the
shortlisted audio codecs can use the MPEG-2 reconstruction kernels unchanged.**
I do not recommend a single-stream real-time audio port on the present evidence.
For a real many-stream/offline workload, first profile **AAC-LC** (best shared
transform-DSP research target) and **FLAC** (cleanest exact, independent-frame
control) on an optimized CPU. Neither is a demonstrated ET speedup candidate.
MP2 is a smaller fixed-point learning exercise, not the strongest business case.

The recommendation is an inference from the inspected code and known integration
costs, not an audio benchmark. No audio latency, throughput, energy saving or
break-even stream count has been measured.

### Evidence boundary

This checkout is FFmpeg **n7.1.1**, base
`db69d06eeeab4f46da15030a80d539efb4503ca8`, with video work `c9eb864` and handoff
`78600c5`; it is not latest upstream. Source references below are to this tree.
Read together: `ET_IMPLEMENTATION.md`, `ET_OPTIMIZATION.md` / `.json`, and
`et-{runtime,kernels,tests}/README.md`. Historical dialogue/tool context is in
the ignored `build-et/audio-reuse/context/` bundle; prior SIMD/HyenaDNA extracts
are in `build-et/optimization/`. Those archives are evidence, not authorization.

Recorded video measurements: one 600 MHz shire, 64 harts, 400 MHz NoC;
i7-13700K CPU affinity 0–15 (8 P-cores plus SMT), default decoder threading.
Five-run medians for 250 frames, including setup, transfer, decoding and hashing:

| Video only | Scalar ET | Selected ET | CPU | CPU advantage |
|---|---:|---:|---:|---:|
| 720x576 I/P/B | 3.286 s | 1.252 s | 0.163 s | 7.68x |
| 1920x1080 I/P/B | 8.150 s | 2.432 s | 0.777 s | 3.13x |

The CPU build disabled x86 assembly, so it is not the proper ceiling for a new
audio comparison. The 2.63x/3.35x gains are **ET versus scalar ET**, not CPU gains
and not predictors of audio gains. The roughly 3.5 s system-time excursions
remain in the evidence. Of 18,457 exact silicon frame comparisons across all
candidates, 4,931 used the selected ELF. None validates audio. No power was
measured. Do not divide these totals by 250 to invent an audio launch latency.

## Reuse boundary

| Existing component and evidence | Reuse class | Audio work still needed |
|---|---|---|
| `libavcodec/et_runtime.cpp`: owned allocations, aligned DMA staging, ELF checks, synchronous event/error handling, poisoned lifetime; `et-runtime/tests/` | Largely unchanged internal mechanisms | Audio allocation/state ownership and batch lifecycle. Calls on a handle remain serialized; many decoder contexts cannot just call it concurrently. |
| `et_runtime.h:ff_et_runtime_launch`, `ETRuntime::launch_params`, `et_mpeg2_protocol.h` | Small structural refactor **plus new validated ABI** | Launch is typed as `ETFrameParams`, stored as that type, and validates MPEG-2 ABI, slices, planes/references and bounds. Not a generic byte-argument launcher. Keep the video wrapper/checks intact; separately validate versioned audio descriptors, stream IDs, state/output bounds, sample counts and generations. No casting audio into video fields. |
| `et-kernels/src/crt.S`, `libc.c`, `cache.h`, build/link scripts | CRT and bounded memory/cache primitives can carry over under their preconditions; build recipe reusable | Separate audio target/ELF; no BSS/TLS dependency, static tables or explicit allocated state. Whole 64-byte output/state/status ownership; bounded eviction, WAIT_CACHEOPS, fences. Revalidate any new instruction allowlist rather than weakening video checks. |
| `decoder.c` bounded FFmpeg bitreader windows; `scripts/generate.py` provenance/table tests | Reader safety and extraction **patterns**, not a codec front end | New Huffman/Rice/range coding, codebooks and error handling. MPEG-2's proven block-window bound does not cover audio. Vorbis uses little-endian bitreading; Opus range decoding is not MPEG VLC. Tables and mutable initialization need independent treatment. |
| `idct_simd.c`, `motion.h`, `reconstruct.h` | Instruction scheduling, mask preservation, alignment proofs, model/oracle technique | **Replace algorithms.** No reusable 8x8 IDCT, half-pel pixel filter, byte clip/pack or DC/mismatch shortcut for these audio decoders. Audio window multiplication/overlap-add is not video motion blending. |
| Core-first worker mapping and resident video references | Scheduling/residency idea | New batch scheduler and stream-state dependency graph. Preserve packet order per stream; channel coupling and variable work prevent blindly treating channels as slice rows. Reference-slot code is not an audio state manager. |
| `et_mpeg12.c`, `hwaccel_internal.h`, `codec.h` | Failure/drain policy and host-output model | New audio integration. This AVHWAccel path negotiates pixel formats and picture/slice callbacks, not sample formats. Prefer an explicit audio decoder/backend returning ordinary audio AVFrames, with a separately designed shared batching service if needed. A local DSP hook alone cannot batch independent AVCodecContexts. |
| Native instruction models, guard pages, sanitizers, mock runtime, sysemu/silicon gates | Test architecture | New audio oracles, sample/state comparators, fixtures and failure tests. Existing YUV/MB comparisons and the 115 video cases do not test audio. |

No extraction/refactor is performed in this report. Keeping new targets, APIs,
artifacts and tests separate is preferable to destabilizing the frozen video path.

## Codec candidates: inspected implementation, not codec-name analogy

Ranks are **research priority for batched decoding**, not projected performance.
All rows require new codec DSP; all single-stream recommendations are “stay CPU”
unless a later measured workload overturns that judgment. Transform lengths below
are explicit: `av_tx` inverse MDCT lengths describe the coefficient/half-output
size (`libavutil/tx.h`), not a claim that every codec's conventional full transform
has that length.

| Priority / candidate | Pinned source and actual computation/numerics | State and usable parallelism | Feasibility judgment |
|---|---|---|---|
| **1 — AAC-LC**, initially conventional 1024-sample frames | `aac/aacdec.c:init_dsp`, `aacdec_dsp_template.c:imdct_and_windowing`: 1024-point `av_tx` inverse MDCT or eight 128-point calls, windowing and saved overlap; entropy/dequantization, stereo tools and TNS remain. `aac` outputs FLTP; `aac_fixed` S32P with int32 transforms, wide rounded products (`aac_defines.h`, `libavutil/tx_priv.h`, `fixed_dsp.c`). | Saved overlap/window transitions; channel-pair coupling, noise state and profile-specific prediction. Parallel independent streams/channels after prerequisites, short transforms within a frame; not arbitrary dependent frames. | Best **shared new DSP** target across transform codecs. Start LC only. HE-AAC SBR/PS adds QMF/filter/state (`aacsbr_template.c`, `aacpsdsp_template.c`); LD/ELD, 960/768 forms and USAC are not automatically covered. F32 versus fixed must be chosen, not conflated. |
| **2 — FLAC**, initially 16/24-bit | `flacdec.c:decode_residuals`, `decode_subframe_lpc`, `flacdsp.c`, `flacdsp_template.c`: Rice residuals, fixed/LPC reconstruction, channel decorrelation, no MDCT. Conditional int32 versus int64 predictor sums; 32-bit stereo may need a **33-bit** subframe and int64 storage. S16/S32, packed or planar, with defined left shifts. | Frames carry warm-ups and are independent once configuration/framing is available [R1]; predictor samples within a subframe are recurrent. CPU decoder already advertises frame threading. Parallel frames/streams, not naive adjacent predicted samples. | Cleanest exactness/parallelism control. However little expensive reusable DSP, serial entropy/prediction, and strong CPU competition; full-frame decode is more plausible than offloading only LPC or decorrelation. No reason yet to port. |
| **3 — AC-3; E-AC-3 later** | `ac3dec.c:do_imdct`: `av_tx` 256 or two 128 transforms per channel/block, window/delay. 256 output samples/block; AC-3 normally six blocks (1536 samples). `ac3dec_float.c` registers FLTP `ac3`/`eac3`; `ac3dec_fixed.c` registers S16P `ac3_fixed`, not a separately registered `eac3_fixed`. Fixed windowing uses int64 products, Q31 rounding and int16 saturation. | Overlap/delay, coupling, reused block parameters, dither and DRC/downmix policy. Channels are not wholly independent before coupling. E-AC-3 adds AHT, spectral extension and dependent-substream handling (`eac3dec.c`). | Multichannel batches offer work; useful second customer for an already-proven MDCT/window kernel. Greater scope than “another transform size”; defer E-AC-3. |
| **4 — MP2 / MP3** | `mpegaudiodec_template.c`, `mpegaudiodsp_template.c:ff_mpa_synth_filter`, `dct32_template.c`. MP2: 32-subband DCT32 plus polyphase synthesis, **not** video IDCT. MP3 additionally `compute_imdct`/`imdct12`/`imdct36`, Huffman, requantization, antialias and stereo. Fixed decoder uses int32/Q23 intermediates, int64 synthesis sums, residual `dither_state`, shift and S16 clipping; float registrations produce FLT/FLTP instead of S16/S16P. | Both retain synthesis ring/offset and rounding state. MP3 adds bit reservoir (`last_buf`, `main_data_begin`) and hybrid overlap. MP2 frames have no MP3-style reservoir but output synthesis still crosses frames. | MP2 is the smallest controlled fixed-point learning target, but likely too little work per launch. MP3 adds state and entropy, not free IDCT reuse. If batching, one ordered stream per task is simplest. |
| **5 — Vorbis** | `vorbisdec.c:vorbis_parse_audio_packet`: codebooks, floor/residue, inverse coupling, float IMDCT, FLTP. Blocks 64–8192 samples [R2]; `av_tx` length blocksize/2, two configured sizes. | Saved overlap and previous-window shape; setup codebooks per stream, channel coupling and variable blocks. | Potential additional customer for new float transform/window primitives, but significant new parsing/setup and little direct MPEG-2 arithmetic reuse. Compare native `vorbis` and external `libvorbis` separately. |
| **6 — Opus CELT / SILK / hybrid** | `opus/dec_celt.c`: float inverse MDCT lengths 120/240/480/960, transient short blocks, overlap/postfilter; `opus/silk.c`: range decoding, LPC/LTP synthesis, mixed fixed parameter arithmetic and float sample/history arrays. `opus/dec.c` outputs FLTP with resampling/delay machinery. `libopusdec.c` calls external libopus, a different implementation. | Range coder, energy/predictor/postfilter history, SILK sample recurrences, mode switches and hybrid synchronization. CELT frame durations include 2.5–20 ms [R3]; low-latency batching is especially constrained. PLC/FEC behavior and supported APIs/backends require separate scope. | Do not start with interactive Opus. CELT-only batching is a possible restricted experiment **after** new MDCT infrastructure, not an Opus implementation. SILK is a poor fit for the existing transform work. Compare against optimized libopus. |
| **Reject — PCM / G.711** | `pcm.c:pcm_decode_frame`: copy/byte order/sample conversion; A-law/mu-law decode is a table lookup to S16. | Independent samples, trivial arithmetic. | Good negative control: abundant parallelism does not imply enough computation to repay staging/DMA/launch/readback. Only reconsider inside an already device-resident larger pipeline, which this project does not have. |
| **Behind FLAC — ALAC** | `alac.c:rice_decompress`, `lpc_prediction`, `decode_element`, `alacdsp.c`: adaptive Rice history, adaptive LPC, extra bits, stereo decorrelation. int32/uint32 arithmetic, 16-bit coefficients, sign extension and explicitly rounded shift (with a wide rounding expression), not an unspecified float dot product. S16P for 16-bit; S32P for 20/24/32-bit. | Per-frame warm-ups/coefficients; adaptive predictor and Rice dependencies within a frame. Already advertises frame threading; frames/independent streams are better units than samples. | Exactness is testable but adaptation makes SIMD harder than FLAC. No reason to prefer it as the first port without a specifically ALAC-heavy workload. |

Sources for dispatch/output distinctions are the codec registration structs, not
an assumption that `-threads N` parallelizes every decoder. Among these native
registrations, FLAC/ALAC advertise frame threading; the listed native lossy
decoders do not. CPU multistream benchmarks must therefore use independent
contexts/workers rather than relying only on decoder thread options.

## Numerical compatibility is the main kernel boundary

The video kernel's eight `.pi` lanes are 32-bit integers. `fmul.pi` keeps the low
32 product bits, with wrapping adds, signed shifts, int16 row wrapping and
unsigned pixel clipping. These semantics are proved against FFmpeg's simple
IDCT, not arbitrary DSP (`idct_simd.c`, `tests/idct_simd.md`).

This distinction is decisive for fixed audio: MP2/MP3 synthesis needs int64
accumulation and carries rounding residue; AAC/AC-3 Q31 products require the
high product bits and specified rounding; wide FLAC reconstruction cannot be
truncated to 32 bits. The local primary ISA implementation,
`../et-platform/sw-sysemu/insns/packed_arith.cpp`, has `insn_fmulh_pi` and
`insn_fmulhu_pi` as well as low multiply: that suggests possible high/low/carry
sequences, **not** a measured fast int64 vector MAC. These opcodes are outside
the video post-link allowlist and need their own model, tests and device gate.

For float audio, the same platform's `packed_float.cpp` provides `fmul.ps` and
`fmadd.ps` (separate multiply versus fused multiply-add semantics). The selected
video ELF only uses `.ps` for data movement: its correctness/performance says
nothing about float DSP. `libavutil/tx_template.c` contains FFT-based MDCT
codelets, with rotations, scaling and ordering unlike the video 8x8 transform.
Reusing their algorithms would require a freestanding transform plan/table
strategy and new device implementation, not calling an existing ET FFT library.
No such reusable audio transform exists in this work.

**Proposed correctness contract:**

- Lossless FLAC/ALAC and PCM/G.711: exact decoded integer samples, bit depth,
  channel order, sample count and timing, including left justification in S32.
  No SNR tolerance, F16, or float substitution. Compare raw valid samples, not
  padding; verify FLAC stream MD5 where present and applicable, in addition to
  independent PCM comparison.
- Fixed lossy paths: exact output and long-running state evolution against the
  explicitly selected pinned fixed CPU decoder. Do not compare `aac_fixed`
  with float `aac` and silently waive differences. Prove overflow, rounding,
  shift and saturation behavior; sanitizer failures cannot be waved away as DSP.
- Float paths: establish a pinned scalar/reference operation order, constants,
  rounding environment and contraction policy. First seek exact sample/state
  agreement to that reference; separately characterize the optimized CPU path.
  Different FFT order, FMA contraction, denormals or resampling can change bits.
  If exact agreement fails, stop and diagnose; any relaxed acceptance needs an
  explicit later decision, codec conformance vectors, per-sample/ULP and peak
  error, persistent-state/drift tests, plus perceptual metrics—not SNR alone.
  Opus has its own conformance comparison procedure, with test-vector updates in
  RFC 8251 [R3,R4]; conformant does not necessarily mean byte-identical to this
  FFmpeg backend. Its range-coder state and required fixed-point operations still
  need exact agreement, even in a floating-point implementation.

## Scheduling, transfers and break-even

**Single stream:** ordinary decode packets are small, often mono/stereo. At
48 kHz, 1024 AAC samples represent 21.33 ms, 1152 MP2 samples 24 ms and 1536
AC-3 samples 32 ms (duration calculations, not measured decode budgets).
Transform parallelism does not remove entropy, coupling, overlap or predictor
dependencies. Dispatching many tiny stages with intervening DMA is particularly
unattractive. Finishing within a packet duration only proves real-time capacity,
not lower latency than CPU. Actual remaining deadline also includes capture,
packetization, jitter buffering and playback; it must be specified by the user.

**Independent streams/offline frames:** a shire could process many ready tasks
with ordered state per stream. Start with scalar-per-hart tasks; SIMD within a
transform or across identical independent streams is a later layout decision.
Eight lane-wise streams incur packing, variable-length divergence and tail costs.
Group compatible sample rates, modes, sizes and formats without unbounded waits.
64 harts share 32 minions; neither 64x scaling nor the video's optimal worker
count transfers automatically. Compare 1/32/64 active workers if ever approved.

Maintain resident overlap/predictor/RNG state and immutable tables; upload packet
bytes once and return PCM/status once per batch where possible. State writes and
status publication need independent cache lines and generations. After partial
failure, invalidate affected state; do not publish partial PCM or silently replay
on CPU. Flush, seek, discontinuities, reconfiguration and drain require explicit
state transitions. A batch broker, fairness/timeout policy and memory budget are
**new code**. The current synchronous shim supplies no proven DMA/compute overlap.

Illustrative transfer volume: stereo 1024-sample F32 PCM is **8192 bytes**; at
48 kHz this is 384,000 bytes/s per stream. Uploading an equally sized coefficient
array for a transform-only split roughly doubles this bulk traffic before
headers/padding. Full decode uploads compressed data instead, but needs much more
new code. Low bandwidth does not eliminate fixed operation costs. Batching 32
such outputs gives 256 KiB, but all DMA sizes, plane strides and status/state
storage still need explicit alignment. These are arithmetic examples, not PCIe
measurements. Keeping audio on-device for subsequent DSP could change the model;
no such consumer/device-resident AVFrame path is established here.

For B ready frame tasks, a conservative **unmeasured** model is:

```
T_ET(B) = T_host_parse_pack(B) + T_upload_wait(B) + T_launch_wait(B)
        + T_readback_wait(B) + T_validate_copy(B) + T_init / batches_per_session
```

`T_launch_wait` includes kernel work; do not count it twice. Resident state avoids
per-batch state transfers but not initial load/allocation, resets and readback.
Compare `T_ET(B)` with measured `T_CPU(B, P)` for the **same tasks** and P CPU
workers. `T_ET(B)/B` is amortized throughput cost, not a request's response time:
latency includes queue/batch formation plus batch service. Gathering B successive
live packets from one stream can delay the oldest by `(B-1)*frame_duration`
before service (eight AAC frames: about 149 ms). Independent streams avoid that
specific delay only when enough packets are actually ready; use a bounded wait.

For a proposed 1.2x throughput target, the total ET service budget is
`T_CPU(B,P)/1.2`; subtract host work and measured transfers/control before assigning
a device compute budget. Until these inputs are measured the break-even B is
unknown. With offloadable CPU fraction f, even a free, instantaneous offload is
bounded by `1/(1-f)` in a serial-stage model. Below f=1/6, a 1.2x target is already
impossible in that model; passing this screen is not evidence of acceleration.

## Smallest next experiment: CPU-only screening, not a port

**Recommendation now:** stop at research unless there is a concrete many-stream
workload. If there is, request a CPU-only profiling/benchmark pass first; it needs
no ET runtime, SDK probe or audio device source. Do not run it as part of this
report. Screen AAC-LC and FLAC, with MP2 and G.711 as cheap/negative controls.

1. **Build separate CPU baselines.** Pinned n7.1.1 with optimized x86 assembly and
   normal CPU dispatch enabled, symbols for profiling, no ET dependency; preserve
   the video binary/build. Explicitly record decoder name, flags, sample format,
   DRC/downmix, thread settings, CPU affinity and external-library versions.
   Compare fixed/float paths separately, and optimized libopus/libvorbis where
   those codecs are considered. A scalar/bitexact build is an oracle, not the
   performance opponent. No unintended resampling or format conversion.
2. **Small matrix:** 1, 8, 32, 64 independent streams; one pinned P-core, 8 physical
   P-cores (one worker each), and P-cores+SMT (16 workers). Verify topology rather
   than assume CPU numbering. Use long representative mono/stereo 44.1/48 kHz
   assets, AAC long/short-window transitions and FLAC 16/24-bit speech, music,
   noise/silence and differing LPC/block sizes. Add 5.1 AC-3 or other profiles
   only if the real workload needs them. Test both ready/offline batches and
   paced packet arrivals with a declared latency deadline and batch wait cap.
3. **Measure without confusing costs:** cold setup/short clips separately from
   warmed sessions; wall throughput in audio-seconds/s and streams sustained,
   host CPU time, p50/p95/p99/max packet-to-PCM latency and deadline misses.
   Profile entropy, transform/window or predictor, host framing/conversion and
   output. Run at least five independent repetitions, retain every sample and
   publish ranges. Validate outputs outside headline profiling, and include a
   separately labelled end-to-end hashing/output run; null output alone is not
   a correctness test. Use memory-resident inputs or account for filesystem I/O.
4. **First stop criteria (proposed, not user requirements):** no port if CPU meets
   the actual stream/deadline target with agreed headroom (suggest 2x), no genuine
   batched demand exists, or idealized removal of the candidate hotspot cannot
   yield at least 1.2x end-to-end gain. Report measured CPU costs and the maximum
   permissible ET overhead; do not fabricate that overhead from video totals.

### Only if screening passes, and separately authorized

Before implementing a codec, an **audio-sized identity/copy batch** using a new
validated test ABI would measure the unavoidable upload/launch/readback and host
costs for the intended sizes and ready-stream counts. It is an overhead lower
bound, not a decoder, and is not authorized here. Stop if this alone exhausts
the budget at permissible batch sizes or misses the latency deadline.

The smallest useful DSP follow-up is AAC-LC inverse MDCT **plus window/overlap
state**, lengths 1024 and 128, with CPU entropy/dequantization retained. Compare
the whole hybrid path including coefficient upload, PCM readback and packing;
label kernel-only times as diagnostics. Select float with an explicit arithmetic
contract or exact `aac_fixed`—do not select reduced precision for convenience.
If integer losslessness is the actual requirement, substitute a narrowly scoped
16/24-bit **whole FLAC frame** experiment, not a trivial decorrelation microbench.
Do not undertake both ports at once.

Correctness gates before any performance conclusion:

- Native independent CPU oracle, expanded instruction models, ASan/UBSan,
  guarded/truncated/invalid packets, extreme coefficients, alignment/tails and
  1/32/64 task coverage. Audio cases must include silence/impulses/full scale,
  long state evolution, window switches, channel ordering and variable sizes.
- Stateful integration: mixed/interleaved independent contexts, stale generation,
  partial batch failure, drain, seek/flush, reconfiguration, exact sample counts,
  timestamps and priming/trailing-sample trimming. Unsupported profiles must fail
  explicitly; no silent fallback or partial-success output.
- Separate sys_emu fatal-memory checking, then explicitly authorized shire-0
  silicon correctness with recorded binary hashes. Neither a native lane model
  nor emulator timing establishes hardware speed.
- Final stop: any unexplained correctness/state divergence, new device error,
  deadline miss against the agreed requirement, or failure to beat the best
  applicable optimized CPU baseline by the predeclared margin (suggest 1.2x)
  end-to-end at deployable batch sizes. Lower CPU occupancy alone is a different
  benefit and must be reported as such, not called a latency/throughput win.
  No energy claim without separate power measurements.

## Encoding is a different investigation

Encoding may have more work to amortize: forward transforms, psychoacoustics,
quantizer/search loops, or LPC analysis. That does not reuse the video inverse
transform or prove a win. Local examples are `aacenc.c`, `aaccoder.c`,
`aacpsy.c`, `flacenc.c` and `libavcodec/lpc.c`; FLAC prediction of residuals uses
known input samples unlike recurrent decoder reconstruction. A future encoding
study needs CPU quality/bitrate/speed settings and rate-distortion/conformance
comparisons, not decoder-output equality alone. This report recommends no encoder
port and makes no encoder throughput claim.

## Primary external references and local ISA provenance

External references corroborate codec structure/conformance, not this fork's
performance or supported profile set (which comes from the source above).

- **[R1] RFC 9639, Free Lossless Audio Codec**, especially frame independence,
  fixed/LPC subframes, residual coding and channel decorrelation:
  `https://www.rfc-editor.org/rfc/rfc9639`
- **[R2] Xiph.Org Vorbis I specification**, decoding procedure, identification
  header block sizes and window/overlap reconstruction:
  `https://xiph.org/vorbis/doc/Vorbis_I_spec.html`
- **[R3] RFC 6716, Definition of the Opus Audio Codec**, sections 2.1.4, 4.1–4.5,
  6.1 and Appendix A.4 (range decoder, SILK/CELT, state and conformance):
  `https://www.rfc-editor.org/rfc/rfc6716`
- **[R4] RFC 8251, Updates to the Opus Audio Codec**, including decoder fixes and
  updated test vectors: `https://www.rfc-editor.org/info/rfc8251/`

Local ISA evidence inspected at sibling `et-platform` commit
`a1eaa3b7bba2e242790dd8618a8e64046e03b5a2`:
`sw-sysemu/insns/packed_arith.cpp`, `packed_float.cpp`, together with the video's
instruction/model/allowlist code. ISA availability is not a cycle-cost guarantee.
The SIMD/HyenaDNA histories reinforce explicit masks, ownership and matched
measurement; tensor/scratchpad schemes and PMU stall interpretations are not
imported by analogy.

## Preservation and safety

Only this report is added. Selected ELF copies were read/hash-checked, not run:
`build-et/optimization/final.elf` and
`et-kernels/build-device/et_mpeg2_slice.elf` both remain
`44ac57592938cde2c398ef05e281e27fc54d548e6b38ed9847555c24c94152f3`.
Video source/defaults, selected binaries, validation reports and frozen evidence
remain untouched. No new tests are represented as executed.

Any later hardware work requires explicit permission, shared
`../setup/.device.lock` ownership, only intended et0 exposure and physical shire
0 / mask 0x1, ordinary launches, separate artifacts and preserved health baseline.
A runtime constructor itself initializes/resets exposed devices: no init-only
probe here. No reset/flash/recovery, counter clearing, clock/cache repartition or
`DM_CMD_GET_FIRMWARE_BOOT_STATUS`. Preserve bounded accesses,
`HAVE_FAST_UNALIGNED=0`, eviction/WAIT_CACHEOPS/fences and fail-closed behavior.
