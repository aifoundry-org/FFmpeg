# Private AAC-LC fixture capture and CPU baseline

This directory is the only tracked source for the capture workflow. All
products are intentionally ignored below `build-et/aac-prototype/`; it does not
edit the main FFmpeg tree, the video paths, or open ET hardware.

## Record ABI

`aac_capture_format.h` defines `ETAACCP1`: a fixed 32-byte little-endian file
header followed by fixed 12,312-byte records. A record captures the current and
previous window sequence/shape, process-local overlap-state context and frame
number, then pre-IMDCT coefficient/state and post-window output/state. The
capture hook is compiled only in the float decoder translation unit and only in
`imdct_and_windowing` (the 1024-sample AAC-LC path).

## Run

The CPU reference must have been built first in
`build-et/aac-prototype/cpu` with the same `-ffp-contract=off` contract. Since
`aevalsrc` exposes PCM F64 samples, that CPU configuration must also enable
`pcm_f64le` decoding; `run_fixtures.py` checks this explicitly. The local NASM
executable is required at `build-et/aac-prototype/tools/nasm`.

```sh
# If building the optimized CPU binary here, expose the required local NASM.
FF_ET_ALLOW_PCIE=0 et-tools/et-env bash -lc \
  'PATH=/work/build-et/aac-prototype/tools:$PATH python3 et-audio/build-cpu.py'
FF_ET_ALLOW_PCIE=0 et-tools/et-env python3 et-audio/capture/build_capture.py
FF_ET_ALLOW_PCIE=0 et-tools/et-env python3 et-audio/capture/run_fixtures.py
FF_ET_ALLOW_PCIE=0 et-tools/et-env python3 et-audio/capture/benchmark_cpu.py \
  build-et/aac-prototype/fixtures/stereo-48000.aac
```

`build_capture.py` uses `git archive HEAD` to make the private
`build-et/aac-prototype/capture-src` tree, applies one patch whose only target
is `libavcodec/aac/aacdec_dsp_template.c`, and builds it with
`-ffp-contract=off`. Fixture decoding passes `-cpuflags 0`, forcing FFmpeg's
libavutil CPU flags to zero for the scalar oracle. It does not rely on disabling
x86 assembly at configure time.

`run_fixtures.py` uses the optimized native CPU binary to encode deterministic
silence/tone/transient/noise AAC-LC ADTS sources for mono and stereo at 44.1 and
48 kHz. It writes each ADTS hash, capture hash, record count, and window
coverage to `fixture-results.json`; it fails unless both short and long windows
are observed. It refuses an output root containing prior fixture evidence. For a
CPU-only long-stream baseline, use a fresh output root and `--encode-only`, for
example `--output-root build-et/aac-prototype/baseline-30s-YYYYMMDD --cases
stereo-48000 --duration 30 --encode-only`.

`benchmark_cpu.py` runs exactly five wall-clock decodes of **one** stream per
arm with `taskset -c 0`, default AAC `-threads 1`, and the null muxer. It keeps
an existing result immutable; select a fresh `--output-dir` for a rerun. It keeps
all samples. Before timing, it emits optimized/default and scalar/`-cpuflags 0`
F32LE hashes (the explicitly PCM-f32 WAV data chunks are extracted because this
minimal build has no raw f32le muxer) and exact float-difference characterization
separately; an optimized-versus-scalar difference is reported rather than silently accepted.
