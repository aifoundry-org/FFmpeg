# AAC offline batching and resident processing — September 16, 2026

## Bottom line

**Yes: the card has ample memory to cache these inputs. Larger batches and
residency work, and reduce measured costs substantially. They do not yet make
this scalar, single-shire AAC synthesis prototype faster than the CPU.**

The user explicitly requested offline/device-resident experiments. All hardware
work used physical **shire 0**, normal launches, the shared ownership lock and
unchanged safety checks. No other shires, firmware changes, resets, cache
repartitioning or counter clearing were used. This is still a synthesis-stage
experiment, **not a complete accelerator AAC decoder**.

## What “32 GB” permits

The concrete primary evidence is the SDK inventory printed in the actual run
logs: **34,265,366,528 bytes**, or **31.912109375 GiB**, in the exposed DRAM region.
That supports the user's main point about a 32-GB-class card. The lowercase
**Gb** means gigabits; **GB** means gigabytes (eight times as much). The SDK value
is an addressable region, **not a fresh measurement of free memory or a guarantee
that every byte is allocatable**. Runtime/kernel/state/output allocations and
other users still require budgeting. This memory belongs to the card, not to
each shire separately.

The earlier chat web citation for the memory specification was not verified.
The capacity conclusion here relies on local SDK logs, not that citation or a
successful vendor-page lookup. No memory-capacity stress/probe was necessary.

Illustrative **stereo, 48 kHz** storage (decimal units):

| Representation | Storage per hour |
|---|---:|
| Compressed audio at an assumed 256 kbit/s | 115.2 MB |
| Decoded F32 PCM | 1.3824 GB |
| This prototype's coefficient records plus PCM | 2.7864 GB |

These are arithmetic examples, not measurements of codec bitrates or usable
capacity. Hundreds of hours of compressed audio could fit in principle; an
unbounded live stream, huge multichannel job or large stream population cannot
be assumed to fit. Expanded inputs **plus outputs** fill memory much sooner.

Importantly, this kernel does not parse compressed AAC. Host FFmpeg already
produced the spectral coefficients. Caching compressed packets alone would not
remove host parsing/dequantization or coefficient transfers for this boundary.

The largest test allocated **271,851,520 bytes (259.258 MiB)** on the device:
130 MiB input records, 128 MiB PCM, plus state, scratch and status. That is under
1% of the reported region. It covered 64 channels x 512 frames: **32 stereo
state clones**, each representing 10.923 seconds at 48 kHz. Memory capacity was
not the limiting factor.

## What changed

New, isolated code lives in `et-audio/offline/`; the first AAC prototype and all
video sources, defaults, selected artifacts and evidence remain unchanged.

- A separate 128-byte **ABI 2** carries capacity, frame offset and frames per
  launch. The original ABI is not reinterpreted. All five buffer ranges remain
  owned, disjoint, aligned, bounded and overflow-checked.
- One worker owns a channel throughout the launch and processes its frames
  **sequentially**, retaining overlap in its scratch area. Independent channels
  run in parallel. A later malformed frame poisons a partially advanced stream;
  status records successful prefix length and failure, never partial success.
- PCM still receives explicit bounded eviction/WAIT_CACHEOPS/fence publication.
  Final state/status publish once per channel per launch. No relaxed coherency
  shortcut or firmware-level persistent kernel was used.
- **Streamed mode:** upload one multi-frame batch and download its PCM each
  launch. State persists between launches.
- **Resident mode:** upload all 512 frames/channel once, process subsets using
  offsets with only status readback between launches, then download all PCM
  once for validation. Inputs/output/state remain allocated on device during
  the whole job. No later device consumer was implemented.
- An optional **`ETAAC_FAST_FINITE=ON`** variant replaces per-sample non-builtin
  `memcpy` in the float-bit finite check with an aligned, alias-safe uint32 load.
  It does **not** remove checks, change numerical arithmetic, add SIMD or permit
  non-finite samples. Protocol/scratch alignment proves the load is safe. The
  default OFF build preserves the control; the variant has separate artifacts.

All transforms remain the same exact scalar F32 implementation from the first
prototype. No precision or rounding contract changed.

## Larger batches: controlled sweep

Workload: 64 channels, **512 consecutive frames/channel** (32,768 channel-frame
syntheses), cloned from two real captured AAC-LC channel timelines. This is an
ideal homogeneous independent-stream workload, not 32 diverse audio files.
All rows use the control kernel, 64 active harts on shire 0. Five independent
process runs per arm; median total times, with all samples retained. Mode order
alternates between repetitions.

| Frames/channel/launch | Launches/job | Streamed stage service | Resident stage service | Resident phase only |
|---:|---:|---:|---:|---:|
| 1 | 512 | 1,691.8 ms | 1,438.9 ms | 1,358.2 ms |
| 8 | 64 | 977.0 ms | 908.1 ms | 826.6 ms |
| 32 | 16 | 884.5 ms | 844.0 ms | 762.5 ms |
| 128 | 4 | 848.6 ms | 826.8 ms | 745.6 ms |
| 512 | 1 | 822.9 ms | 838.9 ms | 757.6 ms |

**Stage service** includes packing, input transfer/wait, launches/waits, status
read/check and PCM download/wait. It excludes runtime setup, extra final state
readback and exact comparison. Host file loading/zero-allocation is also outside
this sum. **Resident phase** includes launches and status read/check but excludes
initial input upload and final PCM download. It measures the boundary usable by
a future device-resident pipeline, not such a complete pipeline's performance.

At 512 frames/launch, both modes have one bulk upload, one launch and one bulk
readback; their small timing difference is not evidence of a residency advantage.
The major benefit is amortizing repeated calls. About 10.9 seconds of samples per
stream must already be available for that batch: this is an **offline throughput**
technique, not a low-latency live-stream solution. CPU comparison must use the
same available work, not pretend that batching delay vanished.

## Finite-check improvement and CPU competition

The full-batch copy-only control initially took **459.6 ms resident / 541.1 ms
transfer-inclusive**. Thus attributing all remaining kernel time to the IMDCT
would have been wrong. The preserved-check aligned-load variant gives:

| Same 64-channel x 512-frame job | Time |
|---|---:|
| ET improved synthesis, transfer-inclusive stage service | **469.8 ms** |
| ET improved synthesis, resident phase | **388.3 ms** |
| ET improved copy-only, transfer-inclusive stage service | **183.7 ms** |
| ET improved copy-only, resident phase | **101.8 ms** |
| Optimized CPU synthesis, one P-core | **33.69 ms** |
| Scalar-exact CPU synthesis, one P-core | **86.75 ms** |
| Optimized CPU synthesis, eight P-cores | **8.64 ms** |
| Scalar-exact CPU synthesis, eight P-cores | **12.40 ms** |

The improved resident synthesis is about **11.5x slower than one optimized
P-core**, or 45x slower than eight, and still slower than the exact-scalar CPU
control. With transfers and packing it is about **3.6x faster than this study's
one-frame streamed ET control**, but that is **ET-versus-ET**, not CPU speedup.

Improved-variant input upload and PCM download each cost roughly 36–38 ms;
packing roughly 8 ms. Runtime/load/allocation/initial-state setup adds roughly
0.26 s to a cold job. Setup is separately retained in every run. Returning PCM
for playback/transcoding still pays readback; a future device-resident consumer
might avoid that transfer but has its own compute/ownership costs.

Copy-only is a control for this implementation: it still validates samples,
reads/writes memory and publishes outputs. It is **not a measurement of peak
LPDDR bandwidth or irreducible device overhead**. This experiment does not
identify stall-cycle categories. The result motivates profiling the remaining
validation/publication/transform costs before assuming tensor peaks or expecting
SIMD alone to solve everything.

CPU baselines use the same 512-frame inputs, store **all** PCM into frame-major
buffers and maintain per-channel overlap, with pinned FFmpeg `av_tx` and
`float_dsp` dispatch. One worker is pinned to CPU 0; eight workers use physical
P-cores 0,2,...,14 on the i7-13700K. Reusable threads exclude creation and transform
initialization; each timed replay includes start/completion barriers, aligned
coefficient copies, synthesis and output stores. Five replays/configuration are
retained, unlike ET's five separate processes. Input packing/allocation is
reported separately; its CPU measurement also includes host allocations, unlike
ET's pack-only field. No apples-to-apples cold CPU/ET process speedup is claimed.

The optimized CPU output differs slightly from the scalar oracle, as in the
first prototype; it is characterized, not used as a relaxed device gate.
Scalar CPU is exact. Each threaded replay's full PCM checksum must match its
serial CPU implementation's checksum, checked outside timing. The 2-channel
8-worker case only has two workers doing synthesis and is not an eight-way
parallelism result.

## Validation and safety

**81 successful silicon runs:** **1,882,240 exact synthesis channel-frame
comparisons** and **327,680 exact copy-control comparisons**. These include
repetitions and independent clones, not millions of distinct source vectors.
Every requested PCM sample and final stream state was checked. The new capture
contains 2,818 records (two probe records plus two 1,408-frame timelines); the
long-lived contexts are selected without splicing probe state into them.

- Native split-launch equivalence: 64 channels, 1/8/32 frames/launch, 1/32/64
  active harts, plus ABI/overflow/range/alias and stale/poison/NaN cases.
- Native DSP/kernel and mocked-runtime ASan/UBSan pass for control and fast
  variants. Typed runtime tests reject invalid frame offsets/counts/masks and
  unowned ranges; poisoning/lifetime behavior is inherited from the pinned shim.
- Both device ELFs pass instruction/section/alignment/FMA checks and rebuild to
  the exact silicon-tested hashes. No sys_emu run, general AAC conformance claim,
  multishire run, compressed-packet device parser or complete audio pipeline.
- Health stayed at MM hangs 0, MM exceptions 1, MinionCeEvent 7, SpCeEvent 1;
  other CE and all UCE zero. No new errors, recovery commands or counter clearing.

Two CPU-side development failures are retained: an initial capture command
omitted the explicitly enabled PCM output encoder; and the first 8-worker CPU
baseline hung in startup before timing because a shared condition-variable
signal could wake another worker instead of main. That container exposed **no
PCIe devices** and was stopped after inspection. Broadcasting startup arrivals
fixed the lost wake; final CPU runs also verify parallel output checksums.
Neither failure is silently counted as a pass or removed as a timing outlier.

## Interpretation and next step

The user's caching idea is valid, and memory was abundant. It removes repeated
transfer/control work and makes a much better experiment. It does **not** make
this scalar synthesis port competitive on the one authorized shire.

This result is **not a full-card ceiling**: only one shire is used. A potential
next optimization would profile the remaining in-kernel memory/publication and
FFT/window loops, then consider exact SIMD and a genuinely device-resident
consumer. Multi-shire independent-stream scheduling is a separate scope and
requires explicit approval under the current shire-0 restriction. Neither should
be promised to beat CPU without measurement. Caching compressed AAC would also
require a different/full decoding boundary to exploit it directly.

## Artifacts

New code/reproduction notes: `et-audio/offline/README.md`. Committed compact
inventory: `ET_AAC_OFFLINE.json`, generated by the read-only collector
`et-audio/offline/results.py`. Raw logs, complete samples, failed CPU attempts,
fixtures, binary hashes and unchanged health snapshots remain in ignored
`build-et/aac-offline/`. Successful run names cannot be reused.

Frozen control ELF:
`0b10f891d8da4cfdb9389e53f7ee6e22b932dcf19e3055d4cc832603f54ee326`.
Improved ELF:
`c67777fe96bc5825150670e8a4c7e8b4b8a4ab5573c184532fbe570decc1e353`.
Snapshots are in `build-et/aac-offline/selected/`.

The previous AAC ELF remains
`7fccc6753d6f5440447d8e657096dd7ee58b7d7e28ce9f65825427b1d874c9e1`;
the selected video ELF remains
`44ac57592938cde2c398ef05e281e27fc54d548e6b38ed9847555c24c94152f3`.
Previous code, defaults, reports, binaries and evidence were not overwritten.
