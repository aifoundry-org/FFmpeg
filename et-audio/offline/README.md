# Offline/resident AAC synthesis experiment

Results: `../../ET_AAC_OFFLINE.md` / `.json`. This is **not a complete AAC
decoder**. It extends the stage replay to multiple sequential frames/channel,
with unchanged exact scalar float DSP. It does not modify ABI 1, video sources,
old defaults, or frozen results. New products use `build-et/aac-offline/`.

## ABI and modes

`ETAACOfflineParams` is a separate 128-byte ABI 2. Buffers are frame-major
`[capacity_frames][channels]`. Capacity is 1–512; channel count 1–64. `generation`
is the first frame generation requested, not a launch token. `frame_offset`
selects a subset inside a resident allocation; `frames` bounds the launch.
Each channel retains sequence, shape, overlap and generation. One worker owns
that channel for the entire launch, preserving sequential dependence. PCM is
published per frame; state/status once per channel. Error after a completed
prefix poisons the slot; **discard the entire failed job**, not just the last
frame. No resume/fallback is automatic. Runtime containment/shape/range checks
cover all input, output, state, scratch and status allocations.

`runner.c` supports:
- `MODE=0`: upload/download each multi-frame batch, resident stream state.
- `MODE=1`: preload all coefficients once; retain all output on device until
  the last launch, then read it back for exact checking.
- `OP=0`: IMDCT + window/overlap. `OP=1`: copy-only overhead control, not AAC.

Tasks beyond the two captured channels are independent clones, not distinct
contents. Final comparisons cover all output samples and final state. The
capture reader rejects broken per-context state chains before runtime open.
No real downstream device consumer, compressed packet parser, general profile
support, arbitrary-length job or multishire scheduling is implemented.

## Build without device access

Use the pinned SDK image via the existing helper, which hides PCIe by default.
All original CPU/DSP prerequisites are described in `../README.md`.

```sh
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake -S et-audio/offline \
  -B build-et/aac-offline/host -DCMAKE_BUILD_TYPE=Release
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake --build build-et/aac-offline/host -j8
FF_ET_ALLOW_PCIE=0 et-tools/et-env ctest --test-dir build-et/aac-offline/host --output-on-failure
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake -S et-audio/offline \
  -B build-et/aac-offline/device -DET_DEVICE=ON \
  -DCMAKE_TOOLCHAIN_FILE=/opt/et/lib/cmake/riscv64-ec-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake --build build-et/aac-offline/device -j8
```

Control defaults to `ETAAC_FAST_FINITE=OFF`. The measured improved variant uses
`-DETAAC_FAST_FINITE=ON` in a **separate** `device-fast` build directory. This
changes bit inspection to aligned uint32 loads with GCC `may_alias`; it retains
all finite checks and exact arithmetic. Same guarded output/coherency policy.
`../check-device.py` rejects unsupported sections/opcodes and contracted FMA.

For native tests without SDK linkage add `-DETAAC_BUILD_RUNTIME=OFF`. Sanitizers:
`-DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'` and the
same `CMAKE_CXX_FLAGS`. Mock tests never construct the real SDK runtime. Both
control/fast native sanitizer builds are retained in the evidence tree.

## Capture and CPU baseline

The tested input is the previous generated 30-second stereo/48 kHz AAC-LC
fixture. The private scalar capture build needs an explicit enabled PCM encoder.
**Use a fresh capture filename; the capture hook itself opens its output for
writing.** Do not overwrite a successful capture.

```sh
# Only if this output does not exist:
test ! -e build-et/aac-offline/captures/stereo-48000.etaaccap && \
FF_ET_ALLOW_PCIE=0 et-tools/et-env env \
  FF_AAC_CAPTURE=/work/build-et/aac-offline/captures/stereo-48000.etaaccap \
  build-et/aac-prototype/capture-src/ffmpeg -hide_banner -nostdin \
  -cpuflags 0 -threads 1 \
  -i build-et/aac-prototype/baseline-30s-20260916/fixtures/stereo-48000.aac \
  -map 0:a:0 -c:a pcm_f32le -f null -
FF_ET_ALLOW_PCIE=0 et-tools/et-env python3 et-audio/offline/build-cpu.py
FF_ET_ALLOW_PCIE=0 et-tools/et-env timeout 60 \
  build-et/aac-offline/cpu/cpu-offline \
  build-et/aac-offline/captures/stereo-48000.etaaccap 64 512 8 5
```

CPU CLI: `CAPTURE CHANNELS FRAMES WORKERS REPEATS [scalar]`. Channels 2/64,
frames 512, workers 1/8. Omit `scalar` for optimized assembly-enabled dispatch.
Each worker has private transform/DSP/scratch; all global initialization occurs
before threads. Explicit affinity assumes this host's verified P-core topology
(0,2,...,14); recheck topology on another machine. Reusable start/done barriers
are timed, creation is not. All PCM is stored; checksums against a serial CPU
reference and scalar-oracle/difference characterization occur outside timing.
The fixed startup broadcast prevents a lost wake between worker/main predicates.
Use an outer timeout to diagnose, not silently discard, failed benchmark runs.

## Silicon safety and reproduction

Only after explicit authorization: shared `../setup/.device.lock`, intended et0
nodes only, physical shire 0, ordinary launches. **Runtime construction itself
initializes exposed devices.** No init-only probe, reset/flash/recovery command,
counter clearing, firmware boot-status query, clock or cache partition change.
The wrapper checks ownership/recovery blockers and before/after CE/UCE/MM health.
It refuses existing output directories; never overwrite retained evidence.

```sh
# NAME CHANNELS HARTS TOTAL_FRAMES FRAMES_PER_LAUNCH MODE OP [CAPTURE]
FF_ET_ALLOW_PCIE=1 et-audio/offline/run-silicon.sh UNIQUE_CONTROL 64 64 512 128 1 0
FF_ET_ALLOW_PCIE=1 \
  ETAAC_KERNEL=build-et/aac-offline/device-fast/et_aac_offline.elf \
  et-audio/offline/run-silicon.sh UNIQUE_FAST 64 64 512 512 1 0
```

`resident_phase_s` is launch/wait + status read/check with input/output resident.
`stage_service_s` additionally includes packing and all input/output transfers.
`cold_stage_s` adds runtime/load/allocation/initial-state upload, **not** full
process/decode/I/O wall time. Extra state readback and exact comparison are
separate diagnostic fields. Logging between launches is excluded from the summed
service fields. Do not interpret these as live-stream request latency, full-codec
speed, pure kernel cycles, or measured LPDDR bandwidth.

`results.py` only reads retained evidence and emits JSON on stdout. Frozen
control/fast/host/CPU binaries are in `build-et/aac-offline/selected/`. Old AAC
and video selected directories and reports remain unchanged.
