# Experimental AAC-LC synthesis on ETSOC-1

**This is a synthesis-stage prototype, not an FFmpeg audio decoder/backend.**
See `../ET_AAC_PROTOTYPE.md` and `.json` for measured silicon results and limits.
Video source, defaults, binaries and evidence are not modified.

## Scope and arithmetic

`dsp/etaac_synth.c` is freestanding scalar F32 IMDCT + window/overlap processing:
1024 coefficients, or eight groups of 128, sine/KBD shapes and all four window
sequences. Operation order matches the pinned n7.1.1 scalar float CPU code.
Immutable generated tables, no heap/libm/BSS, no reduced precision or contracted
FMA. It does not reuse the video IDCT and contains no audio SIMD yet.

`protocol.h` defines a new versioned 128-byte launch ABI. Each task is one
**channel** synthesis frame, not a compressed AAC packet. A fixed slot retains
its own stream ID, generation, previous sequence/shape and 512 overlap samples.
One batch covers 1–64 independent slots, on 1/32/64 active harts, core-first.
State commits only after successful finite output. Status is generation-tagged
and cache-line isolated. Callers must discard failed output and stop a failed
stream; no implicit CPU fallback exists. Inputs/state/output/scratch/status
ranges must be disjoint, owned and cache-line aligned. The runtime adapter is a
checked generated private copy of the video runtime, not a video ABI cast.

`ETAAC_COPY` is a control that copies coefficient bits through the same ABI,
validation and publication mechanisms; it is not a universal measurement of
minimum launch latency. `runner.c` optionally replays captured real decoder
records with persistent state. Complete context timelines are selected without
splicing state across demux-probe/decoder contexts. Batches larger than the
captured channel count use **independent clones** of those timelines.

## Build (no hardware access)

All products go below ignored `build-et/aac-prototype/`. SDK image:
`et-soc1-dev:20260911`; host `/opt/et` is insufficient. The CPU baseline needs
NASM on PATH. The recorded experiment extracted Ubuntu's NASM into
`build-et/aac-prototype/tools/nasm`; no SDK/device initialization is involved.
Use fresh capture/result roots; scripts refuse existing capture/run evidence.

```sh
FF_ET_ALLOW_PCIE=0 et-tools/et-env env \
  PATH=/work/build-et/aac-prototype/tools:/opt/et/bin:/usr/bin:/bin \
  python3 et-audio/build-cpu.py
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake -S et-audio \
  -B build-et/aac-prototype/host -DCMAKE_BUILD_TYPE=Release
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake --build build-et/aac-prototype/host -j8
FF_ET_ALLOW_PCIE=0 et-tools/et-env ctest --test-dir build-et/aac-prototype/host --output-on-failure
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake -S et-audio \
  -B build-et/aac-prototype/device -DET_DEVICE=ON \
  -DCMAKE_TOOLCHAIN_FILE=/opt/et/lib/cmake/riscv64-ec-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake --build build-et/aac-prototype/device -j8
```

Host tests include the independent FFmpeg oracle when the CPU archives are
available at `ETAAC_CPU_BUILD`. If they are missing, the two remaining tests
are **not** a substitute for that oracle. `-DETAAC_BUILD_RUNTIME=OFF` builds
native DSP/protocol tests without SDK linkage. Sanitizer configurations use
`-fsanitize=address,undefined -fno-omit-frame-pointer`; see the results report.
For mock runtime-only tests use `runtime/README.md`.

`capture/README.md` describes the private FFmpeg source archive, capture patch,
fixture generation and complete-decoder CPU timing. It never changes main-tree
codec sources. After fixtures exist:

```sh
FF_ET_ALLOW_PCIE=0 et-tools/et-env build-et/aac-prototype/host/etaac-replay-native \
  build-et/aac-prototype/captures/stereo-48000.etaaccap
FF_ET_ALLOW_PCIE=0 et-tools/et-env python3 et-audio/build-cpu-stage.py
FF_ET_ALLOW_PCIE=0 et-tools/et-env taskset -c 0 \
  build-et/aac-prototype/cpu-stage/cpu-stage \
  build-et/aac-prototype/captures/stereo-48000.etaaccap 64 500
# Add 'scalar' for the exact-arithmetic CPU control, not the fastest baseline.
```

## Explicitly authorized silicon only

`run-silicon.sh NAME TASKS HARTS FRAMES OP [CAPTURE]` acquires the shared
`../setup/.device.lock`, checks existing owners/recovery markers, exposes only
et0 through the container helper, fixes physical shire 0, and compares before/
after CE/UCE/MM counts. It refuses reused output directories. A runtime open
itself initializes exposed devices. Never bypass the wrapper guard to probe.

```sh
# Only after explicit user approval and coordinated device ownership:
FF_ET_ALLOW_PCIE=1 et-audio/run-silicon.sh UNIQUE_NAME 64 64 60 0 \
  build-et/aac-prototype/captures/stereo-48000.etaaccap
```

No reset/flash/recovery/counter clearing, clock/cache partition changes or
`DM_CMD_GET_FIRMWARE_BOOT_STATUS`. Hardware failures stop subsequent launches.
A preflight failure must also be reviewed before its marker is archived;
unchanged health does not authorize ignoring a real device error.

Timing output distinguishes setup, input packing, scalar oracle, upload, launch,
PCM/status readback, extra state-validation readback and comparison. The staged
service sum excludes the last two validation-only steps and CPU oracle, and
therefore is **not** complete AAC decoding or request latency. CPU-stage timing
uses real optimized FFmpeg `av_tx` and `float_dsp` dispatch on one pinned P-core.
No outlier deletion. `results.py` reads evidence only and writes JSON to stdout;
it never initializes the runtime. Keep previous reports and selected snapshots.
