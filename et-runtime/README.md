# FFmpeg ET runtime shim

`libetsoc.a` implements the C ABI in `libavcodec/et_runtime.h`. It does not
implement an FFmpeg decoder or contain device code. Build it inside the complete
SDK image; the host `/opt/et` is insufficient.

## Build and install

From the FFmpeg repository root (no hardware access is needed):

```sh
sudo docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/work" -v "$PWD/../et-platform:/src/et-platform:ro" -w /work \
  et-soc1-dev:20260911 sh -c '
    cmake -S et-runtime -B et-runtime/build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/work/et-runtime/install
    cmake --build et-runtime/build -j4
    ctest --test-dir et-runtime/build --output-on-failure
    cmake --install et-runtime/build
  '
```

`-DET_SDK=/other/sdk` changes the build-time SDK prefix (default: `FF_ET_SDK`
environment variable, then `/opt/et`). Both SDK CMake package and module paths
are required, particularly for `Findlibcap.cmake`.

Inside the same container, use:

```sh
export PKG_CONFIG_PATH=/work/et-runtime/install/lib/pkgconfig
pkg-config --cflags --libs etsoc
cc $(pkg-config --cflags etsoc) et-runtime/smoke.c \
  -o et-runtime/build/pc-smoke $(pkg-config --libs etsoc)
```

The `.pc` puts all runtime transitives in `Libs`, including `-lstdc++`, because
FFmpeg configure may not request `pkg-config --static`. It works with a C linker;
`--ld=g++` and `--extra-libs="$(pkg-config --libs etsoc)"` are also available for
FFmpeg integrations which require an explicit C++ linker. The shim and ET runtime
are static archives, but SDK/system dependencies such as g3log and easy_profiler
remain shared. This is **not** a fully static Linux executable. The SDK library
rpath is recorded. Reinstall/regenerate the `.pc` when changing the SDK or prefix.

The named transitive libraries match the pinned `et-soc1-dev:20260911` SDK;
CMake uses the SDK's imported targets for its own builds. When updating the SDK,
revalidate the separate `cc`/pkg-config link above as well as the CMake build.

## Runtime contract

* `FF_ET_SYSEMU=1`: emulator; otherwise PCIe. There is no fallback.
* `FF_ET_MEM_CHECK=1`: emulator memory checking and `-Werror=memory` (supported
  by the pinned SDK). Setting this on PCIe returns `-ENOTSUP`, not a false claim
  that hardware memory checking is active.
* `FF_ET_SDK`: emulator firmware/executable root, default `/opt/et`. All required
  files are checked before starting emulation. Emulator logs use a private
  temporary directory, removed on ordinary close.
* `FF_ET_DEVICE`: decimal SDK-visible device index, default `0`. The emulator
  creates exactly one device. An invalid/unavailable index is an error.
* A NULL kernel path opens the device layer, runtime and stream **without
  loading code**. Launching such a probe handle returns `-EINVAL`.
* Allocation sizes round up to 64. DMA device addresses **and transfer sizes**
  must already be 64-byte aligned and inside owned allocations. Unaligned caller
  host pointers are supported using shim-owned aligned staging. Zero-size DMA is
  a no-op. Free requires an allocation base; freeing address zero is a no-op.
* ELF64 RISC-V program headers are validated before device initialization.
  Loadable segment offsets/filesz/memsz must be 64-byte aligned; the ELF file
  itself is zero-padded to 64 before SDK loading. Non-loadable odd-valued
  program headers are normalized to PT_NULL in the owned copy to work around
  SDK 0.19's `(type & PT_LOAD)` test (otherwise attributes/stack metadata can
  corrupt its allocation-size/base-address calculation). Sections are preserved.
  Linker padding must occur **inside** the final allocatable section so it
  extends the LOAD segment.
* The queried mask is `computeMinionShireMask_`, not a guessed shire count. Each
  launch must select exactly one available compute shire. ABI, owned device
  buffers, slice count and active-hart count are checked.
* Loading, every DMA, and every launch wait for completion and then retrieve the
  stream errors, including after a failed wait. Async failures poison the handle;
  close/reopen rather than continuing. Staging/ELF/inline argument storage lives
  through SDK teardown even when a submission throws. Outstanding work is aborted
  on the shim's stream before teardown; cleanup attempts remain independent.
* Calls on a handle must be serialized by the caller. Errors are negative errno;
  no exceptions cross the C boundary. Diagnostics use bounded, nonallocating
  buffers. Do not pass arbitrary/dangling pointers to this C API.

**Hardware caution:** the SDK runtime constructor initializes/resets **every
exposed device**, even before any kernel launch. `FF_ET_DEVICE` selects a device
for the shim's work, not an SDK-wide initialization filter. Coordinate PCIe
access and expose only intended device nodes. A probe is not harmless to an
unrelated active hardware workload. SDK internals also contain their own fatal
assertions; the shim avoids known invalid-input paths but cannot redefine the
SDK's process-level failure behavior.

## Tests

`ctest` runs mock-SDK tests only; it never opens hardware. A standalone host test
with sanitizers does not require any installed SDK:

```sh
mkdir -p et-runtime/build
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -Iet-runtime/tests/mock -Ilibavcodec libavcodec/et_runtime.cpp \
  et-runtime/tests/runtime_test.cpp -o et-runtime/build/mock-test
ASAN_OPTIONS=detect_leaks=1 et-runtime/build/mock-test
```

An explicit emulator smoke, without exposing any PCIe nodes:

```sh
et-runtime/run-sysemu-smoke.sh
```

It opens a NULL-kernel probe, checks the compute mask, performs a DMA roundtrip
with deliberately unaligned host pointers, verifies bad range/alignment
rejection, and tests explicit free plus close-time allocation cleanup. It enables
fatal memory checking and imposes a 900-second process timeout. A timeout is a
failure, not a passing probe. `et-runtime-smoke --probe` does initialization only;
`--dma` also performs those transfers. These two modes never load/launch code.
`et-runtime-smoke --load path/to/kernel.elf` additionally validates/loads/unloads
that kernel and checks the load event/stream; it does not launch it.

Validated on September 15, 2026: SDK-image CMake build/install and CTest passed;
plain `cc` linked successfully using only `pkg-config --libs etsoc`; missing SDK
probe returned `-ENOENT` without fallback; ASan/UBSan mocked fault-injection tests
passed. Emulator memcheck DMA smoke passed with mask `0xffffffff`, exit 0, in
about 75 seconds. A second emulator run loaded/unloaded the padded production
`et_mpeg2_slice.elf` successfully with fatal memcheck enabled (no kernel launch),
confirming the program-header normalization and synchronous load path. No silicon
operations were performed by these tests. Build and smoke logs are ignored
artifacts under this directory, not source inputs.
