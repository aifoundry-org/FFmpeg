# ETSOC-1 MPEG-2 validation - September 15, 2026

**Historical scalar checkpoint (`0f6deff`).** The later exact integer-SIMD
implementation and its separately retained evidence are in `ET_OPTIMIZATION.md`
and `ET_OPTIMIZATION.json`; the baseline results below are not overwritten.

## Scope and identity

- FFmpeg n7.1.1 private fork, `et-hwaccel`; base
  `db69d06eeeab4f46da15030a80d539efb4503ca8`.
- SDK container `et-soc1-dev:20260911`, image
  `sha256:83156df076a38c05327b58aa01e84cc61f52dca0c423921ea607a1fd2922ed27`.
- Device ELF SHA-256:
  `93d5319440f44d4a741a57fd708959d8908d7105b010dab73a5731829e4fea2d`.
- Host FFmpeg SHA-256:
  `a581069db4fd2637088fbb188db186840668aee681bb525af002c5a1f88db5f1`.
- Real accelerator: et0, PCIe `0000:01:00.0`, **physical shire 0 only**,
  ordinary launches, minion 600 MHz / NoC 400 MHz queried during testing.
- Firmware readback: release 1.4.1, BL1/BL2 0.21.2, master minion 0.24.0.
- No firmware flashing, error-counter clearing, frequency changes, management
  resets, PCI resets or boot-status queries were performed. The runtime's normal
  device initialization path was used. PCIe tests were explicitly requested by
  the user after emulator validation, with the workspace lock and no other
  process owning either device node.

## Actual accelerator results

Every row below exited zero, produced **byte-identical full framemd5 output**
to the CPU path of the same FFmpeg build, and logged exactly one ET completion
per decoded frame. Pre/post MM and hardware error counters matched in each run.

| Clip | Dimensions | Frames | Harts | Result |
|---|---:|---:|---:|---|
| Single I | 64x48 | 1 | 1 | PASS |
| I-only | 128x96 | 5 | 64 | PASS |
| Round-robin second iteration | 64x1040 (65 MB rows) | 1 | 64 | PASS |
| I/P/B | 128x96 | 12 | 64 | PASS |
| I/P/B, repeated reference reuse | 128x96 | 250 | 64 | PASS |
| SD I/P/B | 720x576 | 250 | 1 | PASS |
| SD I/P/B | 720x576 | 250 | 64 | PASS |
| HD I/P/B | 1920x1080 (68 MB rows) | 12 | 64 | PASS |

**781 device-decoded frames**, in addition to the CPU-only probe. The explicit
`-hwaccel et -et_probe 1` mode also initialized the real runtime, decoded the
single-I input on CPU, matched its MD5, and exited zero without new errors.

Health throughout the silicon tests:

- MM Hang Count: **0 -> 0**.
- MM Exception Count: **1 -> 1**, pre-existing from earlier work.
- MinionCeEvent: **7 -> 7**, SpCeEvent: **1 -> 1**, both pre-existing.
- All other CE counters and all UCE counters remained zero.
- No device-node owner remained after completion.

Raw logs, inputs, MD5s and pre/post snapshots are under `build-et/silicon/` and
`build-et/corpus/` (ignored build artifacts). `ET_VALIDATION.json` preserves
hashes and a compact machine-readable result inventory.

### Initial scaling observation (not an optimized benchmark)

Same 250-frame 720x576 stream, shire 0, full FFmpeg decode plus framemd5:

| Path | FFmpeg benchmark wall time | Frames/s |
|---|---:|---:|
| CPU, default threading | 0.154 s | 1623 |
| ET, 1 hart | 43.321 s | 5.77 |
| ET, 64 harts | 3.146 s | 79.5 |

64 harts are **13.8x faster than one hart**, but this scalar baseline is still
**20.4x slower than CPU** end-to-end on that clip. These are single observations,
not medians/confidence intervals. Both paths use the same minimal build with
x86 assembly disabled; timing includes initialization, transfers and hashing,
not just the device kernel. A separate emulator was running on another host
thread. Do not infer power efficiency, I-cache misses, or production throughput
from these numbers. The correct starting point for Phase 5 is profiling, not
claiming acceleration over CPU.

## System emulator (real RISC-V kernel, not native substitution)

`FF_ET_SYSEMU=1 FF_ET_MEM_CHECK=1`, enabling `-mem_check -Werror=memory`:

- Runtime open/close and host-unaligned DMA roundtrip: PASS.
- Production ELF load/unload: PASS.
- Single I 64x48, one hart: PASS, exact full framemd5.
- Five I pictures 128x96, 64 harts: PASS, exact full framemd5.
- 65 rows at 64x1040, 64 harts: PASS, exact full framemd5.
- Twelve I/P/B frames 128x96, 64 harts: PASS, exact full framemd5.
- **250-frame 64x48 I/P/B lifetime test, 64 harts: PASS**. Exit 0, exactly
  250 frames/completion records, zero decode errors, byte-identical complete
  framemd5. The emulator process terminated normally; no test remains running.

The earlier 250-frame 128x96 emulator run was manually stopped (exit 137) after
25 completed frames to replace verbose per-poll SDK logging and reduce the
spatial workload. It is **not** a passing 250-frame test. Emulator wall times are
not silicon performance measurements. Evidence is under `build-et/*.log` and
`et-runtime/sysemu-*.log`.

## Host-native and failure-path results

These execute the same scalar kernel as native host code, not on ET:

- Full native FFmpeg integration: **69/69 checks pass**, including 250-frame
  I/P/B with CPU default/simple IDCT and both 1/64-hart scheduling.
- Direct kernel single-I comparisons: **22/22 pass**, including custom matrix,
  alternate scan, both intra VLCs, nonlinear qscale, DC precision, edge sizes,
  field DCT and HD.
- Standalone GOP comparisons: 264 frame decodes pass, including three resident
  slots with generation-tagged statuses and reference rollover.
- Eight targeted policy negatives pass: forced MPEG-1, unsupported profile,
  same-size profile/chroma changes, scalable extensions and unsupported IDCT.
- Direct public libavcodec drain tests: four valid controls and twelve injected
  launch/read failure scenarios pass. Failed I/P pictures never emerge on drain.
- Address/undefined-behavior sanitizer tests pass, including 9,000 malformed
  I/P/B guard-page payloads and controlled runtime submission/wait/DMA failures.
- `--disable-etsoc` build/decode passes with no ET pkg-config probes, symbols or
  runtime dependencies. Runtime C/pkg-config linking and mock-SDK CTest pass.
- `FF_ET_DUMP_MB=1` single-I raw dump compared byte-exact with CPU; the MB diff
  tool identifies plane/pixel/macroblock coordinates for mismatches.

Detailed native commands and logs: `et-tests/STATUS.md`, `/tmp/et-test-all/`,
`/tmp/et-test-audit/`, `build-et/final-tests/`. Hosted CI was added and its commands
were exercised locally; no remote workflow run or public PR is claimed.

## Remaining limitations

Phases 0-3 are implemented for the explicitly supported subset and silicon was
successfully exercised. Multi-shire pipelining (Phase 4), SIMD/other Phase 5
optimizations, general interlaced/slice support and the complete FATE corpus are
not implemented or validated. The host continues to reject unsupported streams
instead of using CPU fallback. See `ET_IMPLEMENTATION.md` for the interface and
corrected dependency constraints on frame-level parallelism.
