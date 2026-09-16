# Full AAC-LC benchmark — September 16, 2026

**The full decoder is now scalar-bit-exact on ET. This single-hart prototype
is substantially slower than the CPU, not faster.**

## Workload and correctness

- One stereo 48 kHz AAC-LC stream: **512 packets / 1,048,576 float samples /
  10.923 seconds of audio**, 296,128-byte indexed/padded compressed input,
  4,194,304-byte planar PCM output.
- ET-SOC1: **shire 0, hart 0 only**. CPU: **CPU0, Intel Core i7-13700K**.
  Five fresh processes per mode; no CPU timing overlapped builds, simulation,
  or device work. All samples, including extra five-context CPU processes,
  are retained in `BENCHMARKS.json` and the named raw directories.
- Both sides perform complete compressed-packet decoding, finite validation,
  fused peak/above-one statistics, and complete output stores. No host AAC
  decoding is part of ET execution; host oracle PCM is precomputed test data.
- All five target fixtures pass, including mono 44.1/48 kHz, stereo 1/32/512
  packets and split-request persistence. **All ten 512-packet ET benchmark
  processes pass exact PCM, clean lifecycle/stack/heap checks and unchanged
  before/after CE/UCE/MM health.**
- The strict ET reference is the scalar CPU decoder. The optimized native
  reference has its usual different rounding: 751,895 words differ from scalar,
  but the matched-work driver is byte-identical to the retained optimized v2
  reference. No tolerance or optimized-oracle substitution is applied to ET.

## Median codec times

Milliseconds, median of five fresh processes. Codec lifecycle is allocation /
initialization + decode / validate / features / stores + teardown. ET launch
columns also include SDK dispatch/wait and context publication.

| Implementation | Codec setup | Decode + stores/features | Close | Codec lifecycle |
|---|---:|---:|---:|---:|
| CPU generic C dispatch | 0.543 | 6.120 | 0.018 | **6.682** |
| CPU optimized dispatch | 0.484 | 4.382 | 0.019 | **4.877** |
| ET single hart, PCM-return mode | 278.336 | 1,506.413 | 6.232 | **1,791.030** |

Totals are medians of per-run sums, not sums of independently rounded medians.
CPU codec-lifecycle ranges: scalar **6.573–7.428 ms**, optimized
**4.865–4.916 ms**. Every individual sample is retained; none was discarded.

For the codec lifecycle, ET takes **268.1× the scalar CPU time** and **367.3×
the optimized CPU time**. For decode alone, the corresponding ratios are
**246.1×** and **343.8×**. These are measured times for this implementation and
scope, not whole-card capability or hardware peak comparisons.

## ET setup, transfers and resident features

| Measurement | PCM-return median | Fused-feature retrieval median |
|---|---:|---:|
| Runtime / ELF / device-buffer setup | 289.071 ms | 285.794 ms |
| Compressed upload | 0.564 ms | 0.564 ms |
| DECODE status traffic | 0.727 ms | 0.702 ms |
| PCM download (4 MiB) | 4.024 ms | 4.041 ms, diagnostic only |
| METER retrieval launch | — | 5.690 ms |
| METER status traffic | — | 0.631 ms |
| Warm service, transfer-inclusive | **1,511.787 ms** | **1,514.114 ms** |
| Cold service including runtime, codec INIT/CLOSE and status | **2,087.195 ms** | **2,086.506 ms** |

Warm service includes compressed upload, decode and status, plus the requested
output. PCM-return mode includes PCM download. Feature mode instead includes
METER/status retrieval and excludes the diagnostic PCM read; that read is
nevertheless performed and verified in every sample. **METER retrieves features
already accumulated during decode; it is not a separate resident PCM scan.**
Its extra launch/context-publication cost offsets the small readback saving here.

Cold service includes runtime/image/device-buffer setup, codec INIT/CLOSE,
input transfer, status traffic and requested output. It excludes test-only
private-state readback and golden comparison. CPU file loading, envelope
validation, output-buffer allocation and optional file writing are outside CPU
codec timers. Do not compare the cold ET service row to CPU codec time as though
all boundary operations were identical.

CPU ADTS configuration is discovered on the first packet; ET supplies ASC in
INIT. Individual setup/decode boundaries therefore differ. Both codec-lifecycle
subtotals include their actual setup, decoding and teardown. ET coefficients
are frozen at build time for exactness, while the original CPU library retains
its normal initialization; this difference is explicit, not hidden host work.

## Publication accounting

For PCM-return runs, median raw read-only HPM3 measurements:

| Counter region | Raw ticks |
|---|---:|
| DECODE (includes PCM publication) | 900,378,751 |
| PCM eviction/publication subset | 1,508,404 |
| Global/live-heap publication after DECODE | 3,196,209 |

These are raw ticks, not assumed calibrated seconds. PCM publication overlaps
DECODE and must not be added to it. Context publication is separately measured.
No counter programming/clearing, clock or cache repartition, reset, firmware
change, or recovery operation was performed.

## Provenance and limitations

The original completed simulator and first 32-packet hardware attempt both
failed with exactly 407 PCM mismatches. Offline old-newlib `sinf` reproduction
and sine-table-only substitution proved the cause: 17 sine-128 coefficients
rounded differently. The corrected kernel copies build-time native sine
constants during normal initialization, without changing decode processing.

Hardware use followed explicit user authorization, including review of the
numerical-only failure hold. Its two marker files were archived verbatim under
the shared lock; other recovery blockers were not bypassed. The corrected ELF
was admitted using a narrowly pinned reviewed-child diagnostic authorization:
all six existing device objects and library hashes match its completed
simulator parent. **No corrected-emulator PASS is claimed.** Actual silicon
exactness, not a tolerance, establishes acceptance for the tested fixtures.

Frozen binaries: `build-et/aac-full/selected/`. Selected ELF SHA-256:
`deb1c2127c5132e69b28cd244eb16388f106326d211cff378f1e8df246532321`.
Raw CPU results: `build-et/aac-full/cpu-benchmark-matched-v3/`.
Raw ET results: `build-et/aac-full/silicon/bench-fixed-sines-{pcm,meter}-{1..5}/`.
`benchmark-results.py` verifies PCM hashes, provenance, completions and health
before collecting `BENCHMARKS.json`. Earlier sources, manifests, selected
binaries, failed attempts and ordinary CPU v2 timings remain preserved.

Earlier synthesis-only multicore/64-channel results are different workloads
and are not used to claim a full-decoder speedup.
