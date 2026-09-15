# ET MPEG-2 slice kernel (I / progressive P / progressive B)

This is **real MPEG-2 decoding with bit-exact ET integer SIMD**, not an output-copy
stub. Scalar controls are retained. The shared wire ABI lives in
`../libavcodec/et_mpeg2_protocol.h`. No hardware is launched by scripts here;
`../et-tools/test-silicon.sh` is the explicit parent gate. Progressive P/B uses
resident references, exact half-pel prediction and FFmpeg residual decoding.
Unsupported motion modes are rejected. See `../ET_OPTIMIZATION.md` for selected
options, measured hardware improvements, rejected controls and limits.

## Build and test

From the FFmpeg repository root:

```sh
cmake -S et-kernels -B et-kernels/build-native
cmake --build et-kernels/build-native -j8
ctest --test-dir et-kernels/build-native --output-on-failure

# Build an independent n7.1.1 CPU oracle (no installed FFmpeg needed):
et-kernels/scripts/build-reference.sh
python3 et-kernels/tests/compare.py
python3 et-kernels/tests/compare_gop.py

# Cross-compile only, in et-soc1-dev:20260911 with the complete /opt/et SDK:
et-kernels/scripts/build-device.sh
```

The device script mounts FFmpeg at `/work` and sibling `et-platform` at
`/src/et-platform:ro`. It uses the SDK toolchain and `DeviceUtils` CMake macro,
not an uberkernel. `ET_DOCKER_IMAGE` and `ET_PLATFORM_SOURCE` can override these
locations. `ET_KERNEL_BUILD=build-et/variant` selects a separate build tree.
The script applies selected defaults before explicit user `-D` overrides.
The output is `et-kernels/build-device/et_mpeg2_slice.elf` (plus `.bin`,
`.lst`, `.map`). Default load address is **0x8005801000**; override with:

```sh
et-kernels/scripts/build-device.sh -DADDRESS=0x8005801000
```

`entry_point(params, environment)` uses the existing GP-SDK environment's shire
mask and ET `hartid` CSR (0xcd0, **not** machine `mhartid`). Launch on precisely
one compute shire. `active_harts=1` uses logical hart 0 only, even if firmware
starts all 64 harts. By default `active_harts=64` maps the worker index to
`(hart >> 1) + 32*(hart & 1)` and then processes `worker, worker+64, ...`.
This fills all minions before SMT siblings. Linear and 32-even-worker variants
remain compile-time controls; every variant must cover every row. The chosen
physical shire need not be shire zero, but all approved tests here use shire 0.
The known ggml-et CRT initializes GP, uses the firmware stack, calls the entry,
and returns via `SYSCALL_RETURN_FROM_KERNEL`. It does not clear BSS.

Native builds explicitly use the shared integer-IDCT instruction model and
SWAR motion reference, not executable ET assembly. Expanded-assembly motion
models and actual silicon tests are separate gates. There is no production
native substitution.

Native API (`include/et_mpeg2.h`):

```c
int et_mpeg2_decode(const ETFrameParams *params, unsigned hart);
```

Addresses are native pointers in the host build. The function returns its
hart's first error and publishes each owned slice's result, completed MB count,
bit count (including start code), `frame_id`, and profiling counter delta.
Native counters are 0. Device counters use **four 16-byte-aligned hpmcounter3
reads**, never user-illegal `rdcycle`. HPM3 is only meaningful on harts whose
counter the runtime programmed; zeros / nonrepresentative per-hart deltas are
possible and must not be interpreted as universal cycle measurements.
Inactive harts do not touch output/status. Call all active logical harts to
finish a frame; harts may be called concurrently with disjoint slice ownership.
The native shared library has no FFmpeg library dependency.

Sanitizer test:

```sh
cmake -S et-kernels -B et-kernels/build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g'
cmake --build et-kernels/build-sanitize -j8
ctest --test-dir et-kernels/build-sanitize --output-on-failure
```

## Exact supported surface / ABI contract

* MPEG-2, 8-bit planar 4:2:0, **I, P and B pictures**. The ABI has no
  chroma-format field: the host must reject other chroma formats and MPEG-1.
* Frame pictures (`picture_structure=3`). I pictures support both frame DCT and
  field DCT within a frame picture. **P/B require progressive_frame=1 and
  frame_pred_frame_dct=1**: frame-based 16x16 motion only. Separate field
  pictures, field/dual-prime motion, concealment MVs, scalable extensions,
  and slice vertical-position extensions are unsupported. The host must
  reject sequence-level unsupported features not represented in the ABI.
* P forward/zero-vector/skipped MBs and B forward/backward/bidirectional/skipped
  MBs, plus intra MBs inside P/B pictures. B skips inherit the prior direction
  and motion predictors; a skip after an intra MB is rejected. Motion-vector
  differential/sign/residual/modulo decoding supports f_code 1..9.
* Bilinear half-pel interpolation and bidirectional averaging match FFmpeg's
  scalar rounding. Chroma 4:2:0 vectors divide toward zero. Predictions must
  lie inside the **padded macroblock** reference dimensions (not merely the
  visible crop); out-of-range vectors are rejected, never silently clipped.
* P requires `ref_fwd_addr`; B requires both `ref_fwd_addr` (earlier display
  anchor) and `ref_bwd_addr` (later display anchor). References use the same
  strides/plane layout/frame size as output, are 64-byte aligned, and cannot
  alias output/input/status. The host owns their allocation, lifetime and
  generations; the kernel never uploads, downloads or selects reference slots.
* Both linear/nonlinear qscale, both intra VLC tables, zigzag/alternate scan,
  all four intra DC precisions, macroblock qscale changes, extra slice bytes,
  and natural-order custom intra/inter/chroma quant matrices are supported.
* Four natural-order `uint16_t[64]` quant matrices, values 1..255; identity IDCT
  permutation. Both luma and chroma inter matrices are used for P/B residuals.
* Each descriptor includes exactly one slice start code and **one complete MB
  row starting at x=0**. No skipped I MBs, partial rows, row-spanning slices,
  duplicate rows, or appended next start codes. The first and last MB must be
  explicitly coded; P/B address increments may describe skipped MBs only
  between those endpoints. Trailing zero alignment bytes
  are accepted. `nb_slices == mb_height`, `mb_height <= 175`, `mb_width <= 1024`.
  Duplicate/full-frame coverage is checked defensively on device as well as
  by the host. Descriptor order need not be raster order.
* Dimensions round up to the specified MB dimensions. Output planes include
  the padded MB area, not only the visible dimensions; edge sizes need not be
  multiples of 16. Width/height must be nonzero.
* Destination/status bases and plane offsets are 64-byte aligned. Both plane
  strides are multiples of 64 and large enough for the padded MB row. Planes
  must be disjoint and contained in `frame_bytes`; input, output and status
  allocations must not alias. Input base is 8-byte aligned, descriptor offset
  4-byte aligned, quant offset 2-byte aligned. Input bytes need **no padding**.
* ABI version must match, `frame_id` must be nonzero, reserved fields zero.
  `slice.quant_scale=0` means no external qscale check; otherwise the descriptor
  must equal the decoded (already mapped) initial slice qscale.
* Malformed input yields an error status, never accepted partial success. A
  failed slice may have already written part of its own row; discard the
  whole failed frame. Status `mb_decoded` counts fully reconstructed MBs only.
* Every written status, including a failure, echoes `params.frame_id`. The
  normal contiguous layout `status_addr = dst_addr + frame_bytes` is accepted
  (the native tests exercise this with only one allocation initialization).
  Invalid active-hart counts / entry environment fail via logical hart zero
  publishing fresh BAD_PARAMS statuses. A null params pointer, invalid status
  address/alignment/count, or an aliased status allocation cannot safely be
  written: these return failure without stores, leaving stale generation
  tokens for the host to reject. No CPU fallback is present.

## FFmpeg reuse / freestanding implementation

`src/idct.c` retains FFmpeg's **unchanged** `simple_idct_template.c` as the
scalar control/oracle at 8-bit, int16-coefficient depth. The selected Put/Add
path uses exact eight-lane row and column integer SIMD in `src/idct_simd.c`.
Packing preserves int16 wrapping, clipping, and the per-row DC special case.
See `tests/idct_simd.md` and `tests/reconstruct.md` for arithmetic proofs/tests.
`src/decoder.c` includes **unchanged** `get_bits.h` for coefficient/VLC machinery;
short headers use equivalent bounded 32-bit extraction. Isolated config headers force
`HAVE_FAST_UNALIGNED=0`, `AV_HAVE_FAST_UNALIGNED=0`, and all architecture-specific
implementations off, independently of the parent FFmpeg configuration.

`scripts/generate.py` reads FFmpeg's table definitions, precomputes immutable
FFmpeg `VLCElem`/`RL_VLC_ELEM` lookup tables, exhaustively checks all source
codewords, and extracts `mpeg2_decode_block_intra` and
`mpeg2_decode_block_non_intra` from `mpeg12dec.c`. MB-type flags are extracted
from the upstream syntax annotations, and MB-address/CBP/MV tables are
precomputed alongside the coefficient tables. The
script records **every** modification: compact standalone state, removal of
logging/bookkeeping, strict DC/escape/VLC/range/end checks, and a compile-time
qscale accessor. By default inverse quantization and mismatch-control arithmetic
remain FFmpeg's original implementation. Experimental `ET_PREQUANT=ON` exactly
prescales uint16 matrices on qscale changes; it is not selected.
Upstream attribution and LGPL notices are retained. No hidden manual code copy
or device-side VLC initialization is used. `generated/sources.sha256` records
the source hashes. Regenerate with:

```sh
python3 et-kernels/scripts/generate.py
```

FFmpeg readers require speculative-read padding. Direct windows are allowed
only when their entire span plus 64 lookahead bytes is INSIDE the actual slice.
Tails reuse an owned 1024-byte window plus local zero padding. Consumed bits
are checked against actual input. Valid intra/inter blocks use less than 196
bytes, below the 256-byte block window. The ABI still needs no caller padding.
There are no global mutable tables or allocations.

The device libc uses aligned RV64 words only when alignment and length prove
safety, with exact byte tails. Compiler builtins/auto-vectorization remain
disabled; SIMD is explicit. The linker **fails** on any BSS/TLS. A post-link
check uses a narrow custom integer-SIMD/data-movement allowlist and still rejects
unresolved symbols, compressed/standard-RISC-V-vector instructions,
user-illegal counter reads, and non-64-byte
LOAD segment offsets/addresses/filesz/memsz. LOAD file and memory sizes must
match; final padding resides **inside** the rodata/data sections. The loader
must handle non-PT_LOAD metadata program headers correctly (the production
runtime normalizes odd non-LOAD headers to PT_NULL for the SDK).
Output MB rows are explicitly evicted
past L2 to L3/DRAM in batches of **at most 16 cache lines**, with FENCE before
and WAIT_CACHEOPS + FENCE after each batch. Every slice status occupies and
flushes its own cache line after its row. Input-buffer DMA coherence and kernel
argument lifetime remain runtime responsibilities.

## Validation achieved

Native tests check grayscale reconstruction, 65 rows with 1/64 active harts,
round-robin ownership, inactive harts, truncation at every byte, partial rows,
overfull rows, duplicates, unsupported picture types, invalid references and
f_codes, generation refresh after invalid launch controls, corruption canaries,
and **9,000 malformed I/P/B payloads** adjacent to an inaccessible guard page.
These pass ASan and UBSan.

`tests/compare.py` verifies **byte-exact** visible YUV against the independently
built FFmpeg CPU decoder with `-idct simple`, for 1 and 64 harts: 128x96,
318x242 edge dimensions, 1920x1080, both intra VLC tables, alternate scan,
nonlinear qscale, all DC precisions, field-DCT flags, custom quant matrix,
and combined flags. It exports `build-native/fixtures/basic.{params,input}`
(with address placeholders zeroed) plus the encoded `.m2v` and reference `.yuv`
for the separate runtime/sysemu harness.

`tests/compare_gop.py` verifies full P-only and I/P/B GOPs against CPU display
order, using **three resident slots initialized only once**, contiguous
pixel/status storage and nonzero per-frame generation tokens. Cases include
128x96, 318x242, 1920x1080, 36-frame open GOPs across reference rollover,
11-bit DC, custom intra/inter matrices, intra VLC1 and nonlinear qscale. All
1/64-hart variants pass byte-exact; the initial six GOP cases also pass with
the kernel instrumented by ASan+UBSan. HD fixtures exercise f_code residuals
through f_code=4 rather than just the no-residual f_code=1 case.

Device ELF compilation and static checks pass. **Native agreement is not a
claim of device execution**: no sysemu or hardware launch is performed by this
directory's scripts. The parent FFmpeg integration has separately passed
sys_emu and approved shire-0 silicon tests; see `../ET_VALIDATION.md` for exact
cases and limitations. Never launch silicon without coordinated ownership.
