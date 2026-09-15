# Explicit HOST-NATIVE TESTS — not ET hardware validation

These tests are deliberately separate from `et-runtime`. Nothing in the production
runtime or normal FFmpeg configure path falls back to this implementation.

## Quick start (no SDK, simulator, or hardware required)

From the repository root (C compiler, make, pkg-config, Python 3 required):

```sh
et-tests/unit.sh /tmp/et-unit
et-tests/check-cpu-disabled.sh /tmp/et-disabled
et-tests/build-native-ffmpeg.sh /tmp/et-native
et-tests/run.sh /tmp/et-native/ffmpeg/ffmpeg /tmp/et-results native all
et-tests/audit.sh /tmp/et-native /tmp/et-audit-results
et-tests/run-kernel-native.sh /tmp/et-native/ffmpeg/ffmpeg \
  /tmp/et-native/runtime/kernel/libet_mpeg2_native.so /tmp/et-kernel-results
```

The builder compiles the current kernel `decoder.c` and `idct.c` as a host-native
static archive plus a shared library for direct-kernel tests. It always forces
FFmpeg to relink, including when only the external kernel/runtime archives changed. Alternatively pass an existing native `.a` or `.so` as the second
argument to `build-native-ffmpeg.sh` or `build-native-runtime.sh`. The latter only
builds the separate test runtime and writes `OUTPUT/lib/pkgconfig/etsoc.pc`.
This alternate pkg-config file names `libetsoc_native_test` and
`libetsoc_native_kernel`, never a production SDK library. Use it explicitly via
`PKG_CONFIG_PATH` and `--enable-etsoc --enable-hwaccel=mpeg2_et`.

The mock implements `libavcodec/et_runtime.h`, allocates zeroed 64-byte-aligned
memory, uses host pointers as uint64 addresses, checks owned DMA ranges/alignment,
and calls `int et_mpeg2_decode(const ETFrameParams *, unsigned)` sequentially once
for each active hart. The simulated shire mask is 1. This validates partitioning
and integration, **not concurrent execution, DMA/coherency, device ISA, simulator
memory checking, cycles, or real hardware behavior**. Every open logs a conspicuous
`NATIVE TEST RUNTIME` banner. The runner requires that banner on successful native
decodes and rejects it in sysemu mode. Native mode sets a clearly fake kernel label;
no device ELF is needed or loaded.

`ET_TEST_SANITIZERS=1 et-tests/unit.sh /tmp/et-unit-asan` enables ASan and UBSan on
the runtime/stub unit test. Kernel integration is separately covered by the suite.

## Generated corpus and strict checks

`generate-corpus.sh BUILT_FFMPEG DIRECTORY` uses only the supplied, locally built
FFmpeg and `testsrc2`, never downloaded media. It creates separate `supported/`,
`gop/`, and `negative/` groups with explicit manifests, so stale files from
an older run are not accidentally included. Sizes include 18x18, 64x48, 94x62,
128x96, 160x112, 178x102, 128x80, and 110x78; tiny frames, edge macroblocks, and
non-mod16 dimensions are intentional.

The default **all** gate includes eight progressive single-I/I-only streams and
six full-GOP I/P/B streams, including intra-VLC, nonlinear-qscale, custom
quantization matrices, combined flags, and a **250-frame 128x96 open-GOP stress
case**. It runs 56 exact comparisons plus eight negatives and five runtime failures.
The long case must have exactly 250 CPU and ET-completed frames for each variant.
`run.sh` checks CPU default IDCT and CPU `-idct simple`, with ET `-et_harts 1` and
`64`: `-hwaccel et -xerror -err_detect explode -f framemd5`. Full framemd5 files
are compared by `cmp` (not just a selected frame/hash). Both CPU variants must
match; neither serves as a silent replacement for the other. Every successful
ET decode must also log one ET-completed frame per golden output frame, with the
requested hart count; a runtime-open banner alone cannot hide CPU fallback.

CI gates both I-only and full-GOP stages. Run either stage separately:

```sh
et-tests/run.sh /tmp/et-native/ffmpeg/ffmpeg /tmp/et-i native supported-i
et-tests/run.sh /tmp/et-native/ffmpeg/ffmpeg /tmp/et-gop native full-gop
```

Every failure exits nonzero; no CI step uses `continue-on-error`. The old
`planned-pb` profile name remains only as a backward-compatible alias for the
now-mandatory full-GOP profile.

**Alternate-scan fixture caveat:** this fork's FFmpeg encoder explicitly clears
`progressive_sequence`/`progressive_frame` when `-alternate_scan 1` is selected
(`mpegvideo_enc.c`). That generated I-only stream is therefore a *negative* strict
progressive-subset case, not a successful alternate-scan reconstruction test.
The full-GOP streams remain signaled progressive. `run-kernel-native.sh` separately
runs `et-kernels/tests/compare.py` against the native kernel shared library, covering
alternate scan, custom intra matrices, both VLC tables, qscale, DC precisions,
field-DCT syntax, edge sizes, and HD at 1/64 harts. Those 22 direct comparisons
check reconstruction without asserting that the host accepts nonprogressive
streams. A genuinely progressive alternate-scan host fixture is still needed.

Negative streams exercise chroma422, interlace, alternate-scan output signaled as
nonprogressive, duplicate rows, missing rows, truncated final slice data, and
`-lowres 1`, and grayscale decoding. They must exit nonzero **without a
signal/crash**. Open/allocation/upload/readback/launch fault injection also must
produce an observed injected error and nonzero exit. Logs, generated streams,
and MD5 results are retained under the supplied work directory. Any failure makes
the suite exit nonzero; a missing encoder/filter is a test setup failure, not PASS.

Fault injection is exclusively in the test runtime:

```sh
FF_ET_TEST_FAIL=write:2   # fail the second upload for each runtime instance
FF_ET_TEST_FAIL=launch   # fail every launch
# open, alloc, write, read, launch, free are supported (open has no call selector).
```

The standalone unit test uses a counting stub instead of the MPEG2 kernel to
verify all 64 logical harts are called once, allocation alignment/bounds, failure
propagation, and cleanup. The integration suite uses the real native kernel.

## Late-audit policy and API drain regressions

`audit.sh NATIVE_BUILD_ROOT OUTPUT` is a mandatory native-only CI gate. It uses
the build helper's MPEG-1 encoder to generate a genuine MPEG-1 stream, verifies
that CPU decoding succeeds, then forces `-c:v mpeg2video -hwaccel et` and requires
an explicit ET MPEG-2-only rejection. The seven other policy cases cover an
unsupported High profile, same-size valid-to-unsupported profile/chroma changes,
scalable extension IDs 5/9/A inserted under a Main sequence header, and
`-idct int`. Each requires the intended ET policy diagnostic, not any arbitrary
nonzero exit. Concatenation tests must first complete a valid ET frame.

The script compiles `drain-test.c` against the built public libavcodec API and
the isolated native runtime. Four valid controls verify real delayed output:
a whole single-I file as one packet and a parser-split two-frame I/P stream,
at 1 and 64 harts. Twelve launch/read fault scenarios fail the I or P picture,
continue sending/receiving after errors, and repeatedly call
`avcodec_send_packet(NULL)`/`avcodec_receive_frame`. No failed picture may escape;
any returned valid-prefix frame must match its control fingerprint. The script
also verifies all twelve runtime failures were actually injected. This catches
leaks that CLI `-xerror` tests cannot reach because the CLI stops immediately.

## ET-disabled dependency gate

`check-cpu-disabled.sh OUTPUT` builds and decodes with `--disable-etsoc` and no
visible pkg-config packages. A wrapper records all pkg-config calls and rejects
any `etsoc` probe. The gate verifies disabled config macros, absence of ET runtime
or kernel symbols in libavcodec, absence of ET shared dependencies and advertised
ET acceleration, and a successful CPU encode/decode. CI runs this as a separate
required job: ordinary CPU FFmpeg must not acquire an SDK or test-runtime dependency.

## Locating differences

```sh
python3 et-tests/compare.py golden.framemd5 actual.framemd5
python3 et-tests/compare.py golden.yuv actual.yuv --size 94x62
```

Raw mode requires tightly packed planar yuv420p (no line padding). It reports the
first differing frame, Y/U/V plane, pixel, 16x16 luma macroblock coordinates, byte
offset, and values. Framemd5 mode identifies the first frame record mismatch;
hashes alone cannot identify a macroblock. It exits 1 for mismatches.

## Optional system emulator (never silently substitutes native runtime)

Build the production runtime/FFmpeg and ET device ELF using the project tools,
then run **inside the installed ET SDK environment**:

```sh
FF_ET_MEM_CHECK=1 FF_ET_KERNEL=/path/to/et_mpeg2_slice.elf \
  et-tests/run-sysemu.sh /path/to/production/ffmpeg /tmp/et-sysemu-results
```

The wrapper forces `FF_ET_SYSEMU=1`, disables PCIe opt-in, and uses the production
runtime's `FF_ET_MEM_CHECK=1` mapping to `-mem_check` and `-Werror=memory`.
It checks the simulator, firmware, binary, and kernel prerequisites first. Missing
prerequisites or missing opt-in prints **SKIP** and exits **77**, never zero/PASS.
Simulator/runtime/decode errors after prerequisites pass are failures, not skips.
No hardware validation is claimed. The GitHub workflow gates native I-only,
full-GOP including 250 frames, late-audit policy/API-drain regressions,
direct-kernel syntax checks, and the ET-disabled build. Its
simulator job requires explicit manual selection and a provisioned `et-sdk`
self-hosted runner. An unavailable selected simulator job must not be counted as
successful validation.
