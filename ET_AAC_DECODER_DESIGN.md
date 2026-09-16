# Compressed-input AAC on ET-SOC1: target and cache boundary

September 16, 2026. This supplements, not replaces, the frozen synthesis and
offline reports. The user explicitly permits running the complete AAC decoder
on the ET-SOC1 general-purpose cores. **Nothing in the architecture requires
entropy decoding, parsing or dequantization to stay on the host.** The earlier
coefficient-input boundary was a prototype choice, not a hardware limitation.
Performance and implementation readiness remain separate questions.

## Target pipeline

Host demux / packet indexing -> upload or cache compressed AAC + configuration
-> device AAC decoding -> resident PCM -> device consumer -> final required
results. Container demux and file/network I/O can initially remain on the host;
that is distinct from keeping the audio codec on the host. An AAC access unit
is not the same thing as an entire MP4 file. Packet boundaries, codec config,
timestamps, priming/trimming and errors still need explicit handling.

The coefficient-input path remains useful as a controlled test fixture and a
way to isolate synthesis from the decoder front end planned to move on-device.
A spectral cache does not inherently require host preprocessing: once the full
decoder exists, ET itself can populate spectral or PCM caches. Cache policy and
where the codec executes are separate choices. It is not the
assumed production boundary anymore. The full decoder will preserve packet
order and all decoder state per independent stream; unlike the synthesis-only
experiment, coupled stereo channels cannot simply be treated as unrelated
compressed streams. Parallelize independent decoder contexts first.

## What the two caches actually contain

**Compressed cache:** original AAC access units plus configuration and packet
metadata. The device must still perform syntax/bit parsing, Huffman decoding,
scale-factor reconstruction, inverse quantization, the applicable stereo/noise/
TNS tools, inverse transforms and overlap/window processing. For the initial
AAC-LC target, support is explicitly gated rather than implying every AAC
profile/extension is implemented. Full input packet length and speculative-read
padding must be validated/owned; malformed input and allocation failures must
fail closed.

**Spectral cache used by the prototype:** already expanded F32 coefficients at
the pre-IMDCT boundary, window sequence/shape, stream/generation metadata and
resident overlap state. Upstream codec tools have already been applied by the
host decoder for this boundary. The device only repeats inverse transform and
window/overlap work. These records are a decoder-specific intermediate, **not
standard AAC packets**, and should not be confused with compact quantized
spectral symbols from the bitstream. Captures for our simple LC fixtures do not
establish support for all post-synthesis tools/channel-coupling configurations.

| Consideration | Cached compressed AAC | Cached expanded coefficients |
|---|---|---|
| Storage / PCIe input | Compact; proportional to encoded bitrate | Roughly F32 PCM scale, much larger at normal music bitrates |
| Host audio-codec work | Can be eliminated once the device decoder exists | Parsing/entropy/tool processing already happened elsewhere |
| ET computation | Full decoder | Synthesis only |
| Implementation | Bounded parser, codec tools, allocator, initialization, state, synthesis | Smaller deterministic fixed-size input ABI |
| Repeated synthesis experiments | Must decode again unless decoded intermediates are retained | Reuses the expensive front end directly |
| Portability of cache | Original packets/config are a standard representation | Tied to decoder version, float contract and stage ABI |
| Seeking / restart | Needs correct decoder configuration and preroll/state | Also needs correct overlap/window state; a coefficient block alone is insufficient |
| Best role here | Proposed production input boundary | Profiling control; optional hot intermediate cache |

Neither cache eliminates chronological state dependencies. A large resident
file does not let us decode arbitrary frames in random order without valid
restart state. Caching once also does not eliminate cold-start upload, or PCM
readback when a host consumer still requires PCM.

## Size example, with units and assumptions

Assume stereo 48 kHz, AAC-LC 1024 samples/channel/frame, and **256 kbit/s total
compressed bitrate**. This is an illustrative bitrate, not a measured size
promise for every file. Decimal MB/GB below; stream state/scratch is additional.

| Data | Bytes/second | Bytes/hour |
|---|---:|---:|
| Compressed audio | 32,000 | 115,200,000 (115.2 MB) |
| Raw 1024-coefficient F32 blocks, two channels | 384,000 | 1,382,400,000 (1.3824 GB) |
| Our 4160-byte coefficient records, two channels | 390,000 | 1,404,000,000 (1.404 GB) |
| F32 PCM | 384,000 | 1,382,400,000 (1.3824 GB) |
| Coefficient records + PCM | 774,000 | 2,786,400,000 (2.7864 GB) |

Here the expanded coefficient input is **12.1875x** the compressed input. This
is a size/transfer-volume ratio, not a decode speedup. More device parsing work
is the trade-off. The previously observed ~31.91 GiB addressable device region
is ample for the tested workload, but it is not a measurement of currently free
memory and is not per-shire capacity.

An important third option is **cached PCM**. For repeated playback or repeated
time-domain processing of the same decoded signal, decode once on-device and
reuse PCM, rather than keeping coefficients and repeating synthesis. Retain the
compressed source for compact backing storage; retain only useful hot PCM or
spectral regions as memory permits. Which representation is best depends on the
consumer, reuse count, random-access requirements and working-set size.

## Resident consumer and numerical contract

A concrete first consumer is a per-channel peak/clip meter that reads all PCM
on-device and returns only peak magnitude and sample count above full scale.
It is actual audio analysis, not a checksum masquerading as processing. For
finite F32 PCM, clearing the sign bit and comparing positive IEEE magnitudes
can produce exact results with integer operations and no extra rounding.

This consumer must observe successfully published PCM and current completion
and state generations; producer failure must prevent consumer execution. No
intermediate PCM download/re-upload is allowed inside the timed resident path.
Any full PCM readback solely for correctness testing is reported separately.
A level meter is not a claim of a full inference, playback, encoder or resampler
pipeline. If PCM must eventually leave the card, charge that transfer too.

The current scalar synthesis oracle remains the acceptance gate. Packed F32
SIMD may be a good implementation, but SIMD must preserve each lane's scalar
multiply/add/subtract ordering, rounding, denormal and signed-zero behavior.
Do not substitute FP16/BF16, tensor reductions, FMA contraction or numerical
tolerances for an exact result. Audit the ET packed ISA and test on hardware
before claiming equivalence; integer-video SIMD success alone is not proof for
floating-point audio.

## Porting and measurement gates

1. Cross-compile the pinned generic C AAC decoder and required avcodec/avutil
   components using rv64imf/lp64f without architecture-specific assembly. Audit
   unresolved symbols, writable globals, heap usage, libm/software-double,
   initialization and packet/frame lifetime. **An archive is not a runnable
   freestanding device kernel.** No dummy success stubs for missing services.
2. Supply bounded persistent per-stream allocation and one-time table setup,
   explicit global-data initialization and an exact arithmetic contract. Decode
   one supported stream first; compare full PCM plus errors/draining to the
   pinned scalar CPU decoder before parallelizing independent streams.
3. Profile synthesis validation/memory, IMDCT/FFT, window/overlap and publication
   separately, retaining all cache fences and safety checks. Counter reads must
   use the existing user-legal HPM mechanism; unprogrammed counters are not
   timings. Instrumentation overhead needs a matched uninstrumented control.
4. Introduce the measured exact SIMD candidate only after a scalar baseline and
   standalone instruction/arithmetic tests pass. Do not assume the branch-heavy
   front end benefits from the same vectorization as FFT/window loops.
5. Measure resident producer/consumer service, transferred bytes, cold setup,
   warm work and diagnostic readback separately. Then compare **full AAC decode
   plus the same consumer** with optimized CPU, not just a synthesis-only CPU
   number. Keep hardware restricted to shire 0 under the existing safety lock.

Primary source references for codec stages are the pinned local
`libavcodec/aac/aacdec.c`, `aacdec_proc_template.c`,
`aacdec_dsp_template.c`, `aacdec_float.c`, `aacdec_tab.c`, `libavcodec/aactab.c`
and `libavutil/tx*.c`. Buffer sizes and measured inventory come from the existing
`et-audio/protocol.h` and frozen `ET_AAC_OFFLINE.json`/raw SDK logs. New compile
and profile evidence, when available, is separate from those frozen artifacts.

## Completed milestones in this iteration

The private full AAC float decoder static-archive cross-compile and dependency
audit passed; a runnable complete device decoder is **not yet implemented**.
The new synthesis profiler and resident meter passed 19 physical shire-0 runs.
The detailed measurements, exactness gates and next SIMD priority are recorded
in `ET_AAC_PROFILE.md` / `.json`. This keeps the compressed-input target and
its concrete progress distinct from the still synthesis-only silicon tests.
