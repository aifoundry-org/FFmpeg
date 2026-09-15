# Exact ET integer SIMD IDCT candidate

## Selection / build wiring

`src/idct.c` defaults to `ET_IDCT_SIMD=0`, the unchanged scalar control.
To select the candidate, compile with `ET_IDCT_SIMD=1` and add
`src/idct_simd.c` to the target. Device builds already define `ET_DEVICE=1`.
Native builds must additionally define `ET_IDCT_SIMD_MODEL=1`: this explicitly
selects the portable lane model, **not** a native performance implementation.
The device path refuses to compile on a non-ET/non-RISC-V target.

Additional independent flags (both default zero):

* `ET_IDCT_ROWS=1`: eight rows in parallel, exact per-lane DC selection and
  int16 wrapping stores, followed by the selected column path.
* `ET_IDCT_PACKED=1`: conditional aligned FG32 gather + packed two-word stores;
  falls back to the original general gather/scatter path for other alignments.

CMake/runtime/decoder wiring is owned by the parent agent; this change does
not modify them. Suggested CMake options: `ET_IDCT_SIMD` default OFF, append
`src/idct_simd.c` when ON, and define `ET_IDCT_SIMD_MODEL=1` only for native.
The usual Put/Add API names remain unchanged. The scalar Put/Add symbols
remain available as `et_simple_idct_put_scalar` / `et_simple_idct_add_scalar`
when SIMD is selected; section GC can remove them in the device decoder.
The unused plain `ff_simple_idct_int16_8bit` remains scalar.

## Exactness contract

With `ET_IDCT_ROWS=0`, the row pass is the **unchanged** FFmpeg template, including:

* per-row all-AC-zero shortcut (`row[0] * 8`, truncated to int16);
* exact 8-bit weights and row rounding;
* modulo-2^32 intermediate arithmetic;
* arithmetic right shift, followed by wrapping int16 storage.

The column pass runs eight columns in parallel in ET's custom FP registers,
using **integer** `fmul.pi`, `fadd.pi`, `fsub.pi`, and `fsrai.pi`. The naming
of the register file does not imply floating point: no FP arithmetic,
conversion, tensor approximation, or saturation is used in the transform.
All products and additions are modulo 2^32. The column DC bias is exactly
`W4 * (input + 32)`, **not** `W4 * input + 524288`: W4 is 16383, not 16384.
Skipping a zero term in scalar code and adding its zero product in SIMD
are algebraically identical modulo 2^32. Final sums/differences are interpreted
as signed 32-bit integers and shifted arithmetically by 20.

Put applies `fsatu8.pi` to the signed shifted result. Add gathers prediction
bytes (FGB sign-extends), masks with 255, adds them to the signed result, then
applies `fsatu8.pi`. Both scatter precisely eight destination bytes per row.
Put/Add leave the coefficient block in its post-row state, like FFmpeg.

The assembly uses only caller-saved FP registers, with one input-row gather
at a time and all eight accumulators resident. It saves/restores the original
m0 mask and does not modify other masks. SDK GCC generates **no stack frame,
callee-saved register saves, or spills** for the column function.

### Memory and alignment

The original decoder supplies only 8-byte-aligned coefficient blocks. We do
not assume unaligned FLW.PS is safe on hardware. The baseline path uses general
halfword gathers; the optional packed path is explained below. General `fgh.ps` gathers
signed int16 values at offsets 0,2,...,14, without loading neighboring rows
or rounding down an address. General byte gather/scatter handles arbitrary
destination byte alignment and positive/negative row strides. `fg32h.ps`
was deliberately not used: its address calculation wraps inside a 32-byte
window, which would be wrong for some allowed block alignments.

The only `flw.ps` reads a dedicated 32-byte-aligned, 32-byte-long immutable
offset table. SDK object inspection confirms `.rodata` alignment 32. No BSS,
mutable static state, tensor configuration, or hart/shared scratchpad is used.

## Primary local evidence

Sibling `et-platform` sources:

* `sw-sysemu/insns/packed_arith.cpp`: `insn_fmul_pi` writes
  `FS1.u32[e] * FS2.u32[e]`; add/sub use u32; `insn_fsrai_pi` shifts i32;
  `insn_fsatu8_pi` clamps i32 to [0,255].
* `sw-sysemu/insns/packed_loadstore.cpp`: signed halfword/byte gathers and
  low-byte scatter; `fg32h.ps` modulo-32 addressing.
* `sw-sysemu/insns/packed_mask.cpp`: mask save/restore behavior.
* `dnn-library/include/internal/LoadStore.h`: production SDK distinction
  between aligned FG32 and general FG gather/scatter paths and syntax.
* `dnn-library/include/inlining/FullyConnectedInst.h`: production `fmul.pi`.
* `examples/hyenadna/ssm-conv-wide-asm.h`: prior packed-register assembly style.

SDK assembler probes and object inspection were done **inside**
`et-soc1-dev:20260911` using `et-tools/et-env`. Probe artifacts are in ignored
`build-et/idct-probes/`. The final column function is 700 text bytes with
32 bytes of rodata at GCC `-O2`, `-march=rv64imf -mabi=lp64f`.

## Tests run (no device/simulator execution)

One instruction schedule expands either to device assembly or to unsigned
32-bit lane operations. Native tests compare that schedule to an independent
inclusion of FFmpeg's unchanged template, both end-to-end and starting with
arbitrary post-row int16 values. They compare every destination byte including
canaries, every coefficient including canaries, Put and Add, a merely
8-byte-aligned block, arbitrary byte offsets and positive/negative strides.

Passed:

* **333,889 block cases**, Put + Add for each (667,778 comparisons): every
  int16 row-DC value, every int16 column-DC value, all 64 impulse positions at
  18 extreme magnitudes, extreme sign patterns, and 100,000 randomized blocks
  tested both end-to-end and column-only. Random modes: full int16 range,
  [-2048,2047], sparse int16, and low-half-only rows.
* **153,889 block cases**, Put + Add each, with ASan+UBSan (10,000 random).
* Scalar control native compilation with `ET_IDCT_SIMD=0`.
* SDK cross-compilation of both `idct.c` and `idct_simd.c`, assembler acceptance,
  disassembly inspection of arithmetic, branches, gather/scatter and masks.

Reproduce from repository root:

```sh
mkdir -p build-et/idct-probes
cc -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
  -DET_IDCT_SIMD=1 -DET_IDCT_SIMD_MODEL=1 -Iet-kernels/include -I. \
  et-kernels/tests/idct_exact.c et-kernels/src/idct.c et-kernels/src/idct_simd.c \
  -o build-et/idct-probes/idct-exact
build-et/idct-probes/idct-exact

cc -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DET_IDCT_SIMD=1 -DET_IDCT_SIMD_MODEL=1 -Iet-kernels/include -I. \
  et-kernels/tests/idct_exact.c et-kernels/src/idct.c et-kernels/src/idct_simd.c \
  -o build-et/idct-probes/idct-exact-sanitize
build-et/idct-probes/idct-exact-sanitize 10000

et-tools/et-env bash -c '
  for src in idct idct_simd; do
    riscv64-unknown-elf-gcc -std=c11 -O2 -march=rv64imf -mabi=lp64f \
      -mcmodel=medany -mstrict-align -ffreestanding -fno-builtin \
      -fno-tree-vectorize -DET_DEVICE=1 -DET_IDCT_SIMD=1 \
      -Iet-kernels/include -I. -c et-kernels/src/$src.c \
      -o build-et/idct-probes/$src-et.o
  done
  riscv64-unknown-elf-objdump -dr build-et/idct-probes/idct_simd-et.o \
    > build-et/idct-probes/idct_simd-et.lst
'
```

## Required parent validation / limits

No simulator or silicon was launched by this subagent. Native model agreement
is not a hardware result. The parent subsequently reported the baseline
column-only candidate passed single-I/64-hart and SD250 I/P/B/64-hart exact-MD5
hardware gates, with unchanged counters. Its one-point profile reported summed
launch-wait 2.073 s (cached scalar) versus 1.767 s (SIMD columns), full runtime
2.758 versus 2.491 s. Those results are **not** validation or timings of the
new row/packed variants. Please run:

1. Wired native decoder unit tests and independent I/P/B GOP comparisons with
   the explicit lane-model build (including sanitizers).
2. Exclusive device validation vs retained scalar fixtures at 1 and 64 harts,
   including I/P/B, negative residuals, both scan modes, custom matrices,
   field-DCT I, nontrivial quant/DC precision, and clipped predictions.
3. Matched scalar-vs-SIMD single-shire timing. General gather/scatter latency
   may limit speedup on sparse blocks; the row pass remains deliberately
   scalar because its very common DC shortcut is cheap and exact.

Retain `build-et/optimization/idct-columns` as the hardware-validated control.
Gate and measure row-only, packed-only, then their combination independently.

## Follow-up variants: exact SIMD rows and packed I/O

### Rows (`ET_IDCT_ROWS=1`)

`et_idct_simd_rows()` gathers coefficient column i at byte offsets
`0,16,32,...,112` from `block + i`. Thus SIMD lane j is row j. The butterfly
uses the same integer weights, modulo-2^32 arithmetic, rounding 1024, and
arithmetic shift 11 as FFmpeg. It also ORs the seven AC input vectors into
one per-lane value. `feq.pi(ac_or,0)` becomes an all-ones/all-zeros lane mask.
A bitwise select substitutes **the original signed DC shifted left three**
only on DC-only rows. The normal butterfly must not stand in for this
shortcut: W4=16383 would make some DC results differ.

Eight `fsch.ps` operations scatter each result coefficient across the eight
rows. Each store keeps the low 16 bits without saturation. The subsequent
column function sign-extends these int16 results. The lane model compares the
entire post-row coefficient block against the unchanged scalar template, not
just clipped pixels; clipping cannot conceal a row mismatch.

No stronger block alignment is required for this row function. Both vector
passes save/restore m0 and use only caller-saved FP registers. The row body
cross-compiles to 588 text bytes with no stack frame/spills, plus a dedicated
32-byte-aligned offset table. DC-only blocks still do the vector butterfly;
whether that beats the cheap scalar shortcut is a hardware timing question.

### Packed I/O (`ET_IDCT_PACKED=1`)

The runtime guard requires:

* coefficient base aligned to 16 bytes;
* destination and stride aligned to eight bytes;
* nonzero stride (so all destination rows are disjoint).

Otherwise it uses the identical old general-gather/general-scatter columns.
The parent may align the decoder block to 32 bytes (16 is sufficient) to
ensure normal decoding takes the fast path. This is an optimization, not a
new requirement on the public column function.

The fast path uses native `fg32h.ps` with SDK configuration `0x76543210`.
Each 16-byte coefficient row lies wholly in the lower or upper half of one
32-byte window, so the instruction's modulo-32 address computation cannot
wrap. Add uses `fg32b.ps` with SDK configuration `0x398a418820`; an aligned
eight-byte destination similarly cannot wrap. Prediction sign-extension is
still removed with `fandi.pi 255` before the integer Add.

All eight output row vectors are clipped with `fsatu8.pi` then packed with
`fpackrepb.pi`. Its first two 32-bit lanes hold the eight output bytes. Only
after every row is packed, m0 changes once to 3. Eight `fsc32w.ps` operations,
configuration 8, each store just lanes zero and one: **two adjacent words,
eight bytes**, never a 32-byte output vector. The original m0 is restored at
exit. These word scatters use the same proven modulo-32 bounds as the byte
loads. This deliberately uses `FSC32W` instead of assuming that masked
`FSW.PS` at an eight-byte-but-not-32-byte-aligned address is hardware-safe.
The two-word stores remove general byte scatter while retaining a precise
alignment proof. The fast path needs no offset-vector load at all.

The native packed model implements FG32's actual wrapped address formulas,
FPACKREPB's replicated lane mapping, and the m0=3 two-word store explicitly.
It does not merely reuse the generic gather/scatter model.

### Follow-up validation completed (no hardware/simulator runs)

All four `(ROWS,PACKED)` combinations `(0,0),(0,1),(1,0),(1,1)`:

* **334,145 block cases × Put/Add = 668,290 comparisons each**, including
  100,000 random blocks tested both full-transform and column-only.
* **154,145 block cases × Put/Add each** with ASan+UBSan (10,000 random).
* Every int16 row-DC and column-DC value; all 256 DC/non-DC row-lane masks;
  full-range/sparse/extreme inputs; byte/canary and coefficient comparisons.
* Block alignments 0/8/16/24 modulo 32; arbitrary destination byte offsets;
  positive/negative/zero stride. There were **83,678 fast-eligible cases**
  and **250,467 fallback cases** per full run, proving both dispatch paths
  were exercised. Zero stride specifically tests the non-packed fallback.
* SDK cross-compilation of wrapper and implementation with `-Wall -Wextra
  -Werror`, and disassembly inspection. No FP callee-save traffic or spills.
* Baseline `ROWS=0,PACKED=0` column **.text is byte-identical** to the retained
  initial SDK object (`idct-cols-et.o`), 700 bytes. Packed columns including
  runtime dispatch and generic fallback use 1,408 text bytes. SIMD rows add
  588 bytes. Build artifacts: `build-et/idct-probes/idct_simd-r*-p*.{o,lst}`.

To reproduce variants, add `-DET_IDCT_ROWS=$rows -DET_IDCT_PACKED=$packed` to
both native commands above and loop over rows/packed in `0 1`. The same flags
apply to the SDK cross-compile. Scalar control `ET_IDCT_SIMD=0` remains intact;
the ordinary scalar template and scalar Put/Add oracles are not edited.
