# AAC phase profiling, resident consumer and decoder portability

Recorded September 16, 2026 (UTC). This follows `ET_AAC_OFFLINE.md`; it does
not replace its controls, evidence or conclusions. The user authorized the
next profiling/consumer work and explicitly permits full AAC decoding on the
ET-SOC1 general-purpose cores. **The coefficient boundary is no longer the
assumed production boundary.** See `ET_AAC_DECODER_DESIGN.md` for the detailed
compressed/coefficient/PCM cache trade-offs and proposed complete decoder path.

## What is actually implemented

- A separate **ABI 3** and private typed runtime/runner under `et-audio/profile/`.
  The exact DSP is generated from a hash-checked copy of the frozen scalar
  source. New status fields are versioned, not hidden in old ABI reserved data.
- Five in-kernel phase measurements, using the existing four aligned HPM3 reads
  only. No counter programming/clearing, `rdcycle`, cache repartition, firmware
  changes, resets or recovery commands. All hardware stays on physical shire 0,
  behind the same shared lock, device-node ownership and health checks.
- A real second-launch **resident PCM peak/above-unity meter**. It checks prior
  successful producer completion, operation/sample count, stream/generation,
  state and bounded input, then reads PCM already on device. Results are exact
  positive IEEE magnitude bits and the integer count of `abs(PCM) > 1.0`.
- An independent compile-only feasibility build of the **full generic C AAC
  float decoder**, not just its transform, in `et-audio/decoder/`.

**Not implemented:** a runnable complete device AAC decoder, an optimized SIMD
FFT/window implementation, a general PCM consumer API, or a full ASR/transcode/
playback pipeline. SIMD is evaluated below as the next measured priority, not
claimed as an accomplished speedup.

## Profile result: focus on transforms, not publication

Same workload as the previous offline study: **64 channels x 512 frames**,
independent clones of two real 48 kHz AAC-LC channel timelines, 64 active harts.
All following profiles use **FAST_FINITE=ON**: the already-improved aligned
finite check, not the slower memcpy-per-float control. Five independent runs
per benchmark arm, fixed order disabled/enabled/consumer, no sample removal.

| Phase | Median share of measured phase ticks, channel 0 / hart 0 |
|---|---:|
| Input metadata/state/finite validation | 8.8% |
| IMDCT / FFT, including rotations and transform-result copy | **67.3%** |
| Window / overlap / associated copies | **14.5%** |
| Output and saved-overlap finite validation | 9.1% |
| PCM eviction / WAIT_CACHEOPS / fences | **0.3%** |

These are **not stall-category counters**, a memory-bandwidth benchmark, or
percentages of complete AAC decode. The denominator is the five measured
regions. Initialization, loop glue, initial saved-state copy and final
state/status publication are outside it. IMDCT has not yet been separated into
FFT-only versus pre/post rotations/copies. A float loop's time can include
memory stalls; this table does not prove its arithmetic alone is the bottleneck.

The SDK firmware source `MachineMinion/src/main.c:mm_setup_default_pmcs`
configures the cycle source on selected cores (hart IDs modulo 16 equal 0/1).
We do not reprogram it or sum overlapping per-hart readings into CPU-cycle
claims. All raw channel readings are retained, but the reported breakdown uses
hart 0, a configured source. The one-hart smoke also produces meaningful raw
deltas; native tests deliberately return zero for these counters.

The same ELF with profiling disabled has median launch/wait **390.436 ms**;
enabled **390.623 ms**. That ~0.05% difference of medians is small relative to
run variation, **not proof of zero profiling overhead**. Disabled mode still
contains the same code layout/conditional instrumentation. This study did not
re-measure an entirely instrumentation-free recompiled ELF concurrently.

### Exact SIMD decision

The profile says to prioritize the **IMDCT/FFT family**, then window loops,
while retaining the already-correct publication mechanism. Removing or weakening
fences for a 0.3% measured region would be the wrong trade-off.

The pinned SDK's `sw-sysemu/insns/packed_float.cpp` models `fadd.ps`, `fsub.ps`
and `fmul.ps` with per-lane F32 arithmetic and rounding, rather than requiring
FP16/BF16 tensor operations. That is encouraging primary-source evidence for
an exact packed-F32 implementation, **not hardware proof of a new kernel**.
The appropriate follow-on is to separate transform rotations/butterflies/copies,
vectorize independent lanes without re-associating scalar expressions, and
validate signed zeros, subnormals, cancellation and rounding on silicon. Keep
FMA contraction off. The current audio legality gate still rejects packed
arithmetic; any future narrowly expanded allowlist needs its own tests.

Input/output validation remains about 18% of measured regions and deserves
attention after the transform. This also limits gains from optimizing only the
window loop. No eightfold SIMD or full-card throughput extrapolation is made.

## Genuine device-resident consumer

Runner mode 2 uploads coefficients once, launches synthesis, checks its small
completion/status block, then launches the meter using the **same resident PCM
allocation**, with no PCM download/re-upload between stages. The consumer is
restricted initially to one complete resident batch and rejects stale/failed
producer tokens, wrong operation/sample count, mismatched streams, poisoned
state and nonfinite input. An operation-tagged status prevents interpreting
profiling ticks as feature values. Failure publishes zero successful samples
and zero result metrics. No implicit fallback is present.

For the 64-channel workload, the feature/status block is **4 KiB**, versus
**128 MiB** of PCM. A separate 4 KiB producer status is also read. This is a
concrete output-payload reduction, not a claim that the complete experiment
transferred only 4 KiB: every test additionally downloaded full PCM and final
state **after** the consumer for correctness validation, in separately recorded
diagnostic fields.

| Ready/resident device work, profiling disabled | Median |
|---|---:|
| Synthesis launch + producer status transfer/check | 390.879 ms |
| Meter launch + feature status transfer | **80.121 ms** |
| Synthesis + meter + status transfers, summed components | **471.242 ms** |

Medians of component sums need not equal sums of component medians. Upload
remains about 35.7 ms; packing about 8 ms; diagnostic PCM readback about 37.5 ms.
Setup remains separate. Summed resident service is **not process wall latency**:
logging/checks between launches and diagnostic comparisons are excluded where
not in the recorded components. Complete samples and component definitions are
in the JSON and runner source.

This meter currently revalidates coefficients and decoder state conservatively,
as well as scanning PCM. That work is included in the 80 ms, so this is not a
pure PCM-read bandwidth figure. Nor does it establish a speed win: meter cost
exceeds the avoided ~38 ms PCM readback, and we have not timed an optimized CPU
implementation of the same consumer. The old CPU synthesis numbers are **not**
an end-to-end CPU baseline for this new two-stage workload. The meter reports
potential full-scale exceedances, not proof that samples have already clipped.

## Full AAC decoder portability: compile passes, integration remains

A fresh private archive of pinned n7.1.1 cross-compiles `libavcodec.a` and
`libavutil.a` for **rv64imf/lp64f**, with the AAC float decoder enabled, native
assembly/network/threads/programs disabled. The actual AAC translation units
and decoder symbol are checked; an `ld -r` closure rooted at `ff_aac_decoder`
exposes real dependencies without pretending to make an executable. No success
stubs, real ET runtime construction, emulator or hardware execution are used in
this compile experiment.

Final evidence: `build-et/aac-decoder/rv64imf-lp64f-20260916-r6/`. Earlier failed
and intermediate attempts remain preserved. An SDK newlib formatting-macro
guard required an explicit build flag; this is documented, not a codec rewrite.
FFmpeg also adds `-fno-signed-zeros`, recorded as a numerical integration caveat.
Cross-compilation alone establishes **no complete-decoder bit-exactness**.

The linked-object audit finds roughly **17.06 MB of BSS/SBSS**, mutable one-time
initialization, allocator functions (`memalign`, `realloc`, `free`), libc/libm
and compiler soft-double/quad helpers. There are no constructor sections or
thread symbols in this configuration. None of these findings makes AAC
architecturally unsuitable for the general-purpose cores. They explain why the
archives cannot simply be plugged into the existing no-BSS freestanding kernel
contract. The generic library also retains additional AAC-family tools; an
explicit initial AAC-LC admission policy is still required.

The next decoder milestone is bounded persistent per-stream allocation,
deliberate global/table initialization and a real library/link contract, then
**compressed packets to exact PCM for one stream**. Parallel independent
contexts and a same-workload optimized CPU comparison follow correctness.
Host demux can remain initially; keeping MP4/file I/O on the host does not imply
keeping the AAC codec there.

## Validation and frozen artifacts

**19 passing silicon runs, 526,464 exact synthesis channel-frame comparisons**.
The resident consumer additionally checked features over **163,904 channel
frames**. Counts include repetitions and clones, not distinct source vectors.
The matrix includes one-hart smokes and a 32-hart split-launch streamed case.
All health remained at the existing baseline: MM hangs 0, exceptions 1;
MinionCeEvent 7, SpCeEvent 1; other CE/all UCE zero. No recovery action was needed.

Native tests compare profile on/off against the original exact DSP; the entire
hash-pinned offline bounds/ownership/split/poison/stale suite is adapted to ABI 3.
Additional meter cases cover stale stream/generation/op/sample tags, poisoned
state, NaN PCM, signed zero, exact +/-1 and values above unity. Native and mock
runtime tests pass under ASan/UBSan for both finite-check variants. Device legality
checks pass with no undefined symbols, BSS, TLS, compressed/RVV, packed or double
FP instructions, illegal counter reads or contracted FMA.

`build-et/aac-profile/selected/` freezes the measured fast ELF and runner. Raw
logs, every timing/channel sample and health snapshots remain in the new output
root. `et-audio/profile/results.py` is a read-only collector for the committed
`ET_AAC_PROFILE.json`. Previous audio/video sources, selected binaries, reports
and evidence remain unchanged. New failures would also create the shared legacy
recovery blocker so older wrappers cannot bypass a profile-specific failure.
