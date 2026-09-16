# Isolated ETSOC AAC runtime adapter

This directory builds a separate static adapter for the AAC experimental
executable. It does **not** alter or link the MPEG-2 runtime source and must not
be put in an executable which also contains `libavcodec`'s ET runtime symbols.

`generate_runtime.py` copies the pinned
`libavcodec/et_runtime.cpp` into the build tree only after checking its exact
SHA-256 (`cb4705b...d8500`). It verifies exactly one substitution each for the
private header, launch-parameter type, and final video launch routine. The
result retains the generic `ff_et_runtime_open/alloc/read/write/free/close`
implementation: its staging, allocation ownership, error/poison behavior,
lifetime cleanup, and ELF normalization are therefore the pinned code. The
only launch symbol is `etaac_rt_launch`; no `ETFrameParams` or
`ff_et_runtime_launch` remains in generated output.

`etaac_rt_launch` accepts only `ETAACParams` from `../protocol.h`. It uses the
shared structural validator and additionally requires every input, state,
output, scratch, and status range to be a complete runtime-owned allocation.
No raw-byte launch API exists.

## Build and test

Use the full SDK image, as for `et-runtime`; configuration/build/test do not
open a device. The unit target shadows SDK headers with a mock and is the only
test that calls `ff_et_runtime_open`:

```sh
cmake -S et-audio/runtime -B et-audio/runtime/build \
  -DBUILD_TESTING=ON -DETAAC_RUNTIME_BUILD_ADAPTER=OFF
cmake --build et-audio/runtime/build
ctest --test-dir et-audio/runtime/build --output-on-failure
```

Leave `ETAAC_RUNTIME_BUILD_ADAPTER=ON` (the default) in the full SDK image to
build the production archive. The production `etaac_runtime` target links the pinned SDK targets
`runtime::etrt_static` and `deviceLayer::deviceLayer`. There is intentionally no
smoke tool and no runtime constructor outside mock tests.
