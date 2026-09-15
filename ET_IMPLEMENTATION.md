# ETSOC-1 MPEG-2 offload

Implementation on branch `et-hwaccel`, based on FFmpeg **n7.1.1**, commit
`db69d06eeeab4f46da15030a80d539efb4503ca8`. This is a pinned private fork, not a
claim to track current upstream. The referenced `spike/` artifacts and
`FEASIBILITY.md` were absent on this machine; the hook was reconstructed here.
The existing HyenaDNA runtime, CRT, cache and profiling work informed the port.

## Delivered scope

Host FFmpeg parses MPEG-2 headers and stages complete slices. One ordinary
(non-uber) kernel launch reconstructs a frame on **one selected shire**. The
host retains normal `yuv420p` AVFrames; filters, rawvideo, framemd5 and null
output require no hardware-frame transfer API.

- Real intra/inter VLC and inverse quantization, bit-exact integer-SIMD simple
  IDCT, forward/backward half-pel prediction and bidirectional averaging.
- `-et_harts 1` serializes slices; 64-hart scheduling fills all 32 minions before
  assigning SMT siblings, retaining disjoint complete-row ownership.
- Selected SIMD/bounded-reader optimizations, controls and repeated hardware
  measurements are documented in `ET_OPTIMIZATION.md`.
- Three device output slots hold both references plus the next destination.
  Successful references stay resident. No host reference upload per frame.
- One packed H2D transfer of quant matrices, slice descriptors and padded
  payload; one inline 128-byte launch; one D2H transfer of planes plus statuses.
  Kernel loading and the initial three zeroed frame allocations are setup work.
- Runtime waits and checks stream errors after load, DMA and launch. Failures
  never silently substitute CPU reconstruction. Close/reopen a failed offload
  context; codec flushing is not transparent device recovery. The explicit
  probe is the only CPU reconstruction mode with ET initialization.
- Slice statuses have separate cache lines and nonzero frame generations.
  A stale or incomplete device result cannot be published as a decoded frame.
- Optional ET build: `--disable-etsoc` has no SDK, C++ runtime or device dependency.

### Supported / rejected inputs

The host accepts **MPEG-2 Main/Simple, 8-bit 4:2:0, progressive frame pictures**,
I/P/B, frame prediction, even dimensions up to 4096x2800, and one complete slice
per MB row starting at x=0. Width/height need not be multiples of 16. Both qscale
modes, intra VLC tables, DC precisions and natural-order custom quant matrices
are transported. CPU `-idct auto` and `-idct simple` are supported; other explicit
IDCT choices are rejected rather than silently ignored.

This is not a general MPEG-2 hardware decoder yet. Partial-row or row-spanning
slices, duplicate/missing rows, MPEG-1, 4:2:2/4:4:4, grayscale, lowres, cropped
slice skipping, separate field pictures, interlaced motion, dual-prime,
concealment MVs and scalable extensions are rejected. Motion references must
remain within the padded coded picture; unrestricted malformed-vector edge
emulation is not implemented. Kernel f_codes are limited to 1..9.

The device's intra path additionally handles field-DCT placement, tested
separately. The host deliberately retains the narrower progressive contract.
FFmpeg's encoder marks `-alternate_scan 1` output nonprogressive, so those
encoder fixtures are host-policy negatives even though direct kernel tests
verify the alternate scan arithmetic. Do not claim all FATE MPEG-2 inputs pass.

**One-row slices are an enforced subset, not a property of all MPEG-2 streams.**

## Build on this machine

The complete SDK is in `et-soc1-dev:20260911`; host `/opt/et` is incomplete.
`et-tools/et-env` exposes **no PCIe nodes by default** and defaults to sys_emu.
It mounts this checkout at `/work` and sibling `et-platform` read-only.

```sh
et-kernels/scripts/build-device.sh
et-tools/et-env python3 et-tools/build-host.py
et-tools/et-env build-et/host/ffmpeg -hide_banner -hwaccels
```

The script builds a minimal validation FFmpeg (MPEG-2, lavfi testsrc2, rawvideo,
framemd5, null, scale). To build a general FFmpeg instead, install the shim as
above and use ordinary FFmpeg configure options with `--enable-etsoc` and
`PKG_CONFIG_PATH=/work/build-et/sdk/lib/pkgconfig`. `libetsoc.a` is the C++ shim;
its pkg-config metadata carries the pinned SDK's transitive libraries.

### Run and compare

Inside the full SDK environment, or prefix commands with `et-tools/et-env`:

```sh
build-et/host/ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=25 \
  -frames:v 12 -c:v mpeg2video -g 12 -bf 2 -q:v 3 -y build-et/clip.m2v
build-et/host/ffmpeg -v error -i build-et/clip.m2v \
  -f framemd5 -y build-et/golden.md5
FF_ET_SYSEMU=1 FF_ET_MEM_CHECK=1 \
FF_ET_KERNEL=/work/et-kernels/build-device/et_mpeg2_slice.elf \
  build-et/host/ffmpeg -nostdin -xerror -hwaccel et -et_harts 64 \
  -i build-et/clip.m2v -f framemd5 -y build-et/device.md5
cmp build-et/golden.md5 build-et/device.md5
```

Use paths visible **inside** the container for `FF_ET_KERNEL` and dumps.
`FF_ET_MEM_CHECK=1` enables both `-mem_check` and `-Werror=memory`.
`GLOG_minloglevel=1` in the container helper suppresses the SDK's very large
per-poll INFO log; warnings, errors, FFmpeg logs and memory-check failures remain.
Use `GLOG_minloglevel=0` when SDK polling diagnostics are needed.

Explicit Phase-0 probe (CPU reconstruction, not an offload result):

```sh
FF_ET_SYSEMU=1 build-et/host/ffmpeg -hwaccel et -et_probe 1 \
  -i build-et/clip.m2v -f null -
```

`FF_ET_SHIRE_MASK` may select one available physical shire, default `0x1`.
`-et_shires` currently accepts **only 1**; larger values fail explicitly.
`FF_ET_DEVICE` selects the SDK-visible device index, default 0.

**PCIe is opt-in.** The attached accelerator passed the recorded shire-0 tests
in `ET_VALIDATION.md` (original scalar baseline) and `ET_OPTIMIZATION.md`
(selected integer SIMD, including repeated 250-frame SD/HD I/P/B streams).
The SDK constructor initializes/resets every exposed device as part of opening
its runtime (the same path used by the existing backend). Coordinate
ownership first; do not run alongside another hardware workload.
`FF_ET_ALLOW_PCIE=1` exposes et0 to the container; `FF_ET_SYSEMU=0` selects PCIe.
`et-tools/test-silicon.sh` acquires the shared workspace lock, refuses an existing
owner/recovery blocker, and compares pre/post MM and device error counters. It
never clears counters or issues reset/flash/recovery commands. For example:

```sh
FF_ET_ALLOW_PCIE=1 et-tools/test-silicon.sh ipb 64 build-et/clip.m2v build-et/golden.md5
```

## Tests and debugging

```sh
# Host-native integration, explicitly NOT device execution:
et-tests/build-native-ffmpeg.sh /tmp/et-native
et-tests/run.sh /tmp/et-native/ffmpeg/ffmpeg /tmp/et-results native all
et-tests/check-cpu-disabled.sh /tmp/et-disabled

# Device kernel and malformed-input tests:
cmake -S et-kernels -B et-kernels/build-native
cmake --build et-kernels/build-native -j8
ctest --test-dir et-kernels/build-native --output-on-failure
```

`et-tests/README.md` documents native, fault-injection and sanitizer tests;
`et-kernels/README.md` documents the kernel oracle and generated table checks;
`et-runtime/README.md` documents SDK tests and failure handling.
`.github/workflows/etsoc.yml` makes native I/P/B and ET-disabled checks mandatory.
The emulator job is explicit and requires a provisioned SDK runner. It runs
`et-tools/validate-sysemu.sh`, a bounded subset with a small 250-frame lifetime
stress, rather than repeating the entire native matrix through the slow
emulator. Missing prerequisites are nonzero, never an alleged pass. Silicon is
manually tested, not automatically run in CI.

`FF_ET_DUMP_MB=1` writes tightly packed `et-frame-NNNNNNNN.yuv` in
`FF_ET_DUMP_DIR` (which must exist). IDs are **decode order**, not display order;
logs include picture type. Extract the matching CPU frame, then:

```sh
python3 et-tests/compare.py golden.yuv et-frame-00000001.yuv --size 128x96
```

It reports the first differing plane/pixel/MB. Dumping remains available for a
failed reconstruction whose readback succeeded.

`FF_ET_TIMING=1` logs per-frame upload/wait, launch/wait, readback/wait and
validation/copy microseconds, plus successful-frame totals. These are host API
envelopes, **not device execution/stall counters**. The default makes no extra
clock calls; keep timing disabled for headline benchmarks.

## Deliberate changes from the proposed design

- Compact generated FFmpeg coefficient helpers plus specialized motion glue avoid
  pulling in the entire generic MPEG decoder. No mutable VLC initialization,
  heap allocation or atomics are needed on device. The generated provenance
  file records source hashes and all extraction transformations are scripted.
- The 128-byte ABI uses frame-base pointers and shared plane offsets, not eleven
  plane pointers. Four quant matrices are carried (including distinct chroma
  matrices), not merely two. Slice status records are generation-tagged.
- All device DMA and LOAD segments are 64-byte aligned. Each staged slice gets
  its own 64-byte read-ahead padding; the kernel also uses bounded local windows
  around FFmpeg's speculative bitreader. `HAVE_FAST_UNALIGNED=0` is enforced.
- Output/status cache eviction uses <=16 lines per operation, WAIT_CACHEOPS
  and fences. Normal per-frame firmware boundaries are retained.
- ET user mode cannot use ordinary `rdcycle` or machine `mhartid` here. The
  implementation uses ET `hartid` and four aligned HPM3 reads (RTLMIN-6496).
  Emulated cycle counts are diagnostics, **not silicon throughput measurements**.
- The pinned SDK treats odd program-header types as loadable. The runtime shim
  normalizes non-LOAD headers in its private ELF copy before SDK loading.
- Fail-closed negotiation, error/drain handling and test wiring require more
  upstream changes than the nine-line probe estimate. Correctness takes priority
  over preserving that estimate.

## Remaining phases

1. Broaden bitstream coverage and run the applicable FATE corpus. Interlaced
   motion and general slice layouts need explicit ownership and syntax support.
2. Multi-shire scheduling remains unimplemented. Dependent I/P frames cannot
   simply run concurrently: P[n+1] consumes P[n]. Independent streams/GOPs or
   dependency-ready B pictures need a scheduler and a reference visibility
   policy. A shire-count option alone is not a valid implementation.
3. Single-shire SIMD is now implemented and hardware-gated. Repeated medians:
   250 SD frames improved **3.286 -> 1.252 s**, HD **8.150 -> 2.432 s**.
   CPU remains faster. See `ET_OPTIMIZATION.md` for all samples, system-time
   excursions, rejected candidates and exact selected-binary identity.
4. Further entropy/front-end and launch/readback optimization, compressed
   instruction builds, two-pass VLD and device-resident AVFrames remain open.

See `ET_VALIDATION.md` for the historical baseline and `ET_OPTIMIZATION.md` for
current optimized results and their limitations.
