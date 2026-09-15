# Exact DC + mismatch corner review (native only)

`test_reconstruct.c` directly includes the unchanged repository
`libavcodec/simple_idct_template.c`, configured for 8-bit samples and 16-bit
coefficients using `et-kernels/include/config.h`. It tests `src/reconstruct.h`
without defining `ET_DEVICE`; no ET hardware or simulator is launched.
It is standalone and needs no FFmpeg library or generated build configuration.

## Reproduce from repository root

```sh
cc -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
  -Iet-kernels/include -I. et-kernels/tests/test_reconstruct.c \
  -o /tmp/test_reconstruct
/tmp/test_reconstruct

cc -std=c11 -O1 -g -fsanitize=address,undefined \
  -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -Iet-kernels/include -I. et-kernels/tests/test_reconstruct.c \
  -o /tmp/test_reconstruct_sanitize
/tmp/test_reconstruct_sanitize
```

Do not pass `-DET_DEVICE=0`: the header's device branch uses `#ifdef`, so the
macro must be **undefined**. `--refusals` runs the refusal-only subset.
Checks remain enabled under `-DNDEBUG`.

## Results and coverage

GCC 9.4.0, little-endian x86-64: both optimized and ASan+UBSan runs pass
(about 1.28 s and 4.42 s respectively):

```
exhaustive oracle comparisons passed: 7105536
PASS: accepted=7130112 refused=261900 Add-lane-model=3545829 raw-IDCT=1536 (native only; no ET execution)
```

* All 512 DC coefficients from -2048 through 2040, divisible by eight, and
  each corner -1, 0, +1. All other AC coefficients are zero.
* All predictions 0..255 for both Put and Add at strides 8, 16, 24, 64, 128,
  -8, -64, -128, and zero; zero tests aliased rows' repeated saturating Add.
  128 explicitly covers field-DCT row spacing. Additional varying-pixel
  patterns detect lane swaps and incorrect packed stores.
* All legal block and destination alignment residues modulo 64 are cycled.
  Entire destination buffers, including gaps and canaries, are compared with
  the oracle. The shortcut must preserve the input block and its canaries.
* The raw, unclipped FFmpeg IDCT is separately checked for every DC/corner
  pair, so clipping cannot hide an incorrect constant residual.
* Every int16 DC outside the accepted set and every int16 corner outside
  -1..1 is rejected. Each of coefficients 1..62 is tested with -32768, -257,
  -1, 1, 256, and 32767, covering both coefficient bytes and every scan word.
* Each non-eight-byte alignment 1..7 is rejected independently for source,
  destination, and stride. Rejection must leave all destination and source
  bytes unchanged for both modes.
* Source blocks touch either end of a readable page bordered by inaccessible
  pages. The block is made read-only during reconstruction. Every output row
  lies in a separate page bordered by inaccessible pages, alternately at
  its beginning/end; positive and negative page-spaced strides are tested.
  Every DC/corner pair and both modes run on these layouts. The remainder
  of every writable row page is a checked canary. Zero-DC Add with every
  corner is also called with a completely inaccessible destination.
* A separate native Add helper models unsigned byte gather, modulo-32-bit
  integer addition, signed saturation, and two packed little-endian stores,
  and compares that result directly with the unchanged oracle. It does NOT
  execute or validate ET opcodes, gather/scatter configuration constants,
  masks, register constraints, or hardware memory behavior.

Oracle SHA-256:
`092d1b0973e329b0b0aaae634e7cf2f5e97c06aaca2ce98450298ae185c3a14b`

Reviewed header SHA-256:
`0b63aae377eadc5c7c30881ccfe6a3e52ef733a081e9e57ff8d9b93e38f1d451`

## Integer proof against this actual FFmpeg template

Let input DC be `D = 8*q`, with `-256 <= q <= 255`, and corner `c` be
-1, 0, or 1. FFmpeg's DC-only first-row shortcut produces eight copies of
`8*D = 64*q`, safely within int16. Rows 1..6 stay zero. For corner +1,
the last row after the row transform is exactly

```
[2, -6, 9, -11, 11, -9, 6, -2]
```

Corner -1 negates this vector; corner zero gives zero. These integers follow
from `(1024 +/- Wk) >> 11` with this template's actual constants, not from
an ideal floating-point cosine transform. In particular each last-row
coefficient `r` has `abs(r) <= 11`.

Each column's pre-shift numerator is therefore

```
16383 * (64*q + 32) + t
= 1048576*q + (524256 - 64*q + t)
```

Here `t` is one of the signed odd-column terms involving `r`, and
`abs(t) <= 22725 * 11 = 249975`. Thus the parenthesized remainder lies in
`[257961, 790615]`, strictly inside `[0, 1048576)`. Arithmetic right shift by
20 yields exactly `q` for every pixel. This proves the raw residual, before
intra clipping or prediction Add. It also accounts for both row rounding
and the column template's unusual `W4 * 32` rounding bias. No approximate
IDCT or tolerance is involved.

The template's unsigned modular accumulators and signed arithmetic right
shift have their existing FFmpeg/target semantics; all mathematical
numerators in this restricted case fit signed 32 bits. Row values fit
int16. The shortcut divides only exact multiples of eight, so negative
truncation is immaterial. Scalar Add ranges from -256 to 510, safely within
`int`; repeated-byte multiplication is unsigned uint64 and bounded by
`UINT64_MAX`. Negative strides convert to uintptr_t only for alignment
checking, which preserves the low alignment bits. Callers still must supply
valid row storage, a readable 128-byte block, and nonoverlapping source and
destination; the helper is not an arbitrary-pointer validator.

## Review finding fixed by parent

The initial header read `block[0]` before checking source alignment. An
odd-byte-aligned source could therefore invoke an unaligned int16 load
before refusal. This was reported immediately, and the parent moved the
alignment check before the first load. The tested version passes all seven
misalignment cases under UBSan. The parent also added a compile-time
big-endian refusal guard. The scan masks assume little-endian source bytes:
word 0 omits its low 16 bits (DC), and word 15 omits its high 16 bits (corner).
The test explicitly requires little endian; this is not big-endian execution
validation. Toolchains lacking `__BYTE_ORDER__` still require the documented
little-endian target assumption.

No remaining native exactness or memory-safety counterexample was found.
These results establish the predicate and scalar implementation's exactness,
not device assembly correctness or full-stream integration. Keep device
assembly validation separate from this native gate.
