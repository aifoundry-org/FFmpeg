# Full compressed AAC-LC ET kernel (private integration)

This is an experimental complete-decoder integration of pinned FFmpeg n7.1.1.
The private `r6` archive copy is rebuilt with `-mno-fdiv`; the original stays unchanged.  It accepts one resident stream on hart 0 only: ADTS AAC-LC,
44.1/48 kHz, mono/stereo, no CRC and exactly one raw data block per ADTS frame.
It sends a copied raw access unit through the real `avcodec_send_packet` /
`avcodec_receive_frame` API and publishes planar `float[channels][1024]` at
`output[packet]`. The directory also contains isolated ABI4 host/simulator
runners, fixture/oracle builders, and explicitly guarded silicon tooling.
The implementation validates every descriptor, padded packet range and zero
padding, ADTS header/configuration, complete input header, state sequencing,
all five disjoint ABI allocations, and exact output shape.  Unsupported profiles
or a non-1024 FLTP decoder result fail closed.

## Lifecycle

* `INIT` requires a blank private 256-byte state, `first_packet=packets=0`, and
  configures the real decoder from packet 0's ADTS AudioSpecificConfig.
* `DECODE` requires `first_packet == state.next_packet`; the ABI generation is
  already required to be `first_packet + 1`.  A successful contiguous range
  increments that cursor.  Failure leaves it unchanged; discard published
  output for the failed request.
* `CLOSE` destroys the context/frame and clears state.
* `METER` requires the completed full range (`first=0`, `packets=capacity`,
  `generation=1`) and publishes the actual resident PCM absolute-peak bits and
  cumulative above-one sample count retained in private state. This meter is
  fused into decode validation, **not a second scan of resident PCM**; the
  METER request retrieves features and reports zero newly decoded frames.

The final 256 KiB scratch region is a private stack. `entry.S` checks hart 0
and validates the ABI on the firmware stack before switching to it, restoring
the firmware SP before the return syscall. Other harts return before C/global
access.  No generic timer CSR is read; phase ticks use the SDK-approved HPM3
helper where the runtime configured it (otherwise zero).

`init_ticks` covers actual decoder/context initialization; `decode_ticks`
covers packet send/receive plus PCM validation/copy; `pcm_publish_ticks` is
the sum of actual output `et_evict()` publication operations. `frames` and `samples` are completed
in this launch (the latter is per-channel). PCM is finite-checked before copy;
output, state (including poison/close), and status are explicitly evicted.
`decode_ticks` includes PCM publication; do not add these overlapping timers.
`context_publish_ticks` separately measures conservative publication of all
mutable library globals and the live heap/header prefix after each launch.
A 64-byte sentinel at the bottom of the private stack is checked on completion.
`heap_live` must return to zero on successful teardown.

The scratch prefix is a coalescing, reusable bounded allocator backing actual
`malloc`/`free`/`realloc`/`memalign`; allocator failure is surfaced in status.
Newlib syscall hooks return `ENOSYS` and increment `unsupported_calls`. Logging
is an explicit no-op callback. `abort` escapes a protected decoding call through
`longjmp`, rather than trapping or hanging.

## Build / checks (never launch hardware)

The build copies the private owned sources and hashes them beneath a fresh
ignored `build-et/aac-full/device-YYYY-MM-DD/` output. It links the retained
private no-FDIV FFmpeg archives, links the separately rebuilt `rv64imf` scalar
libm/libc/libgcc set (`ETAAC_FULL_SCALAR_LIB_DIR`, defaulting to the private
scalar-libs output), maps all BSS
as file-backed zero-filled `.data` for the legacy loader, and rejects undefined
symbols, NOBITS/TLS/constructors, compressed/RVV/FMA/double/packed instructions,
ET microcode-dependent divide/sqrt/64-bit float conversions, unapproved CSR
access, and non-file-backed or unaligned LOADs. Retained `R_RISCV_64`
relocations are mandatory and bounded: `medany` alone does not rebase FFmpeg's
pointer tables. The final link uses `--emit-relocs --no-relax`.

```sh
FF_ET_ALLOW_PCIE=0 et-tools/et-env env FF_ET_ALLOW_PCIE=0 \
  python3 et-audio/full/build-device.py --output build-et/aac-full/device-local
cc -std=c11 -O2 -Wall -Wextra et-audio/full/arena.c et-audio/full/arena-tests.c \
  -o /tmp/etaac-arena-tests && /tmp/etaac-arena-tests
cc -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  et-audio/full/arena.c et-audio/full/arena-tests.c -o /tmp/etaac-arena-asan && \
  ASAN_OPTIONS=detect_leaks=1 /tmp/etaac-arena-asan
# Real kernel lifecycle/API path against the pinned native scalar oracle:
cc -std=c11 -O2 -Wall -Wextra -Werror -ffp-contract=off \
  -I build-et/aac-prototype/cpu -I . et-audio/full/kernel.c \
  et-audio/full/platform.c et-audio/full/arena.c et-audio/full/full-native-tests.c \
  build-et/aac-prototype/cpu/libavcodec/libavcodec.a \
  build-et/aac-prototype/cpu/libavutil/libavutil.a -lm -lz -pthread \
  -o /tmp/etaac-full-native-test && \
  /tmp/etaac-full-native-test build-et/aac-full/fixtures/stereo-48000-1.input \
  build-et/aac-full/fixtures/stereo-48000-1.scalar.pcm
```

The selected arithmetic is the pinned FFmpeg float AAC decoder, not the
separate `etaac_synth` path. The only private numerical integration is a
**link-time frozen `ff_cbrt_tab[8192]` override**: `build-device.py` calls
`ff_cbrt_tableinit()` from the pinned native CPU archive, emits exact `uint32_t`
bits into that fresh output's `private-source/cbrt_frozen.c`, records both CPU
archive and generated-source SHA-256 in `CBRT_PROVENANCE.txt`, and defines the
corresponding no-op initializer before r6 is searched. No AAC payload is
processed on the host and r6 source/archive files are not edited. This removes
the target libm/double table-generation variability; PNS/sqrt and complete
packet PCM still require the native/device oracle comparison gate.

## Execution gates and evidence

As of the initial integration checkpoint, all five native full-decoder fixtures
pass exact PCM, split-request persistence, fused-meter verification, and empty
heap teardown. Post-INIT padding/length mutations fail without publishing PCM.
Allocator ASan/UBSan and the isolated runtime mock pass. Native success is not
target numerical evidence.

Rejected simulator attempts are retained under `build-et/aac-full/emulator/`:
missing pointer relocations caused an INIT access fault; after fixing relocation,
`fdiv.s` caused an ET microcode trap. No physical device was used for those tests.
Both defects now have explicit final-ELF rejection gates. See `README-ffmpeg.md`
and `README-libs.md` for the private portable-math rebuilds.

Run simulator validation first (PCIe remains disabled):

```sh
ETAAC_FULL_ELF=build-et/aac-full/device-no-mcode-v1/et_aac_full.elf \
 et-audio/full/run-emulator.sh UNIQUE build-et/aac-full/fixtures/stereo-48000-32.input \
 build-et/aac-full/fixtures/stereo-48000-32.scalar.pcm 16 1
```

Only a successful run writes `PASS.json`. Hardware additionally requires
`FF_ET_ALLOW_PCIE=1`, an explicit matching `ETAAC_FULL_ELF`, and
`ETAAC_FULL_EMULATOR_PROOF=build-et/aac-full/emulator/UNIQUE/PASS.json`.
`run-silicon.sh NAME INPUT GOLD CHUNK METER` acquires the shared device lock,
checks retained recovery blockers and the established health baseline, uses
only shire 0/hart 0, and blocks further access on failure. It performs no reset,
firmware change, counter programming/clearing, or cache repartition.

Timings separate runtime/image/buffer setup, decoder INIT, warm DECODE, feature
retrieval, compressed upload, status traffic, diagnostic PCM/state download,
and CLOSE. With METER enabled the transfer-inclusive application service omits
diagnostic PCM readback; the actual read and exact comparison are still retained.
CPU benchmarks must run only after competing builds/simulation/device work stops.
Prior synthesis-only results are context, not equivalent full-decoder throughput.
