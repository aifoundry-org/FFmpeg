# Standalone MPEG-2 motion primitives

`../src/motion.h` owns the implementation. No decoder, cache, build-system or
launch changes are needed inside this contribution. **No ET hardware or ET
simulator was launched by this contributor.** SIMD remains experimental until
its parent's shire-0 pixel-equivalence and matched-performance gates pass.

Parent-reported integration results: SWAR + column-SIMD IDCT passed SD250 and
HD250 silicon output MD5 checks, with unchanged error counters. Reported SD
launch sum: scalar motion 1.767 s -> SWAR 1.223 s; additionally using the
spread-even-first hart mapping -> 0.849 s. These are the parent's measurements,
not independent runs by this contributor. Modes 0/1/2/3 are now **frozen**. The mode-3 addition reproduced all 12 prior
mode-0/1/2 probe objects byte-for-byte; the mode-4 addition reproduced all 17
prior mode-0/1/2/3 objects byte-for-byte, including the mode-3 scatter alternative.
Evidence: `build-et/motion-probes/frozen-impl-sha256.before` and
`build-et/motion-probes/frozen-before-mode4.sha256`.

## Wiring (parent)

Include `motion.h` from `decoder.c`. Inside `predict_mb`, leave ALL the existing
reference selection, chroma MV division, `sx/sy/hx/hy`, bounds checks and pointer
calculation unchanged. Replace ONLY the nested row/column prediction loop with:

```c
et_mc_predict(dst, src, stride, (unsigned)size,
              (unsigned)hx, (unsigned)hy, (unsigned)have_prediction);
```

Keep `have_prediction = 1` exactly where it is. The call covers one 16x16 luma
or 8x8 chroma plane. It must not be used for an unvalidated rectangle.
Destination/stride must be 8-byte aligned (current protocol guarantees this).
Source needs only byte alignment. Source and destination may not alias.
The current native shared-library build requires no additional source files.

Compile controls (all are numeric preprocessor definitions):

| Control | Meaning |
|---|---|
| `ET_MC_IMPL=0` | scalar control, same rounding as original loop |
| `ET_MC_IMPL=1` | default: eight-pixel RV64 SWAR |
| `ET_MC_IMPL=2` | frozen ET four-lane paired-byte integer SIMD; device only |
| `ET_MC_IMPL=3` | frozen eight-lane integer SIMD, packed L1 eight-byte store |
| `ET_MC_IMPL=4` | SIMD8 proven-window FG32 reads + packed masked L1 FSC32W store |
| `ET_MC_V8_STORE=0` | mode 3 default: `fpackrepb.pi`, `m0=3`, `fsw.ps`, `m0=255` |
| `ET_MC_V8_STORE=1` | mode 3 comparison: eight-lane `fscb.ps`, no per-group mask writes |
| `ET_MC_LOAD=0` | SWAR exact volatile-byte gathers |
| `ET_MC_LOAD=1` | default: aligned ld / two lwu / four lhu / eight lbu, selected by source alignment |
| `ET_MC_XY=0` | SWAR xy2 using independent even/odd-byte 16-bit slots |
| `ET_MC_XY=1` | default: SWAR xy2 using bytewise quotients/remainders |

`LOAD` and `XY` do not affect scalar/SIMD2/SIMD3/SIMD4. Use `ET_MC_IMPL=0` to retain
the 781-frame scalar baseline while gating candidates. The parent's benchmark
matrix is SWAR `IMPL=1,LOAD=1,XY=1`, paired-lane `IMPL=2`, `IMPL=3` packed, and
optionally `IMPL=3,V8_STORE=1` scatter, then `IMPL=4`. Mode 4 always uses packed
FSC32W output; `ET_MC_V8_STORE` affects ONLY mode 3. `ET_MC_V8_STORE` is a header-only compiler
definition; the parent owns exposing it through CMake if desired. Header default
remains SWAR, but an explicit parent build definition takes precedence. No ET
speedup is inferred from host microbenchmarks or instruction counts.

## Exactness and bounds

SWAR rounded average is `(a|b) - (((a^b)>>1)&0x7f7f...)`. Each byte subtraction
is nonnegative, so no borrow crosses byte boundaries. XY2 has two independent
implementations of `(a+b+c+d+2)>>2`:

* even/odd: widen alternating bytes into separate 16-bit slots; largest sum
  including rounding is 1022, so neither slot carries into its neighbor;
* quotient/remainder: high six-bit quotients sum to <=252 per byte; low two-bit
  remainders plus rounding sum to <=14 per byte; their final sum is <=255.

Both shift masks suppress adjacent-byte contamination. Neither uses nested
rounded averages. Second-direction blending is performed AFTER interpolation.
Horizontal neighbors reuse the current packed word plus exactly ONE halo byte;
no aligned-down pointer, two-word overread, speculative row, or tail padding.
All wide source loads stay inside the exact requested eight bytes and are
naturally aligned. Every destination wide operation covers exactly eight bytes.

SIMD2 uses four active 32-bit integer lanes (`m0=0x0f`) and offset table
`{0,2,4,6}`. Each lane computes two pixels, in separate registers, only packing
them after all rounding and blending. A group writes eight bytes through four
aligned halfword scatters. Source is accessed exclusively with byte gathers.
XY2 reuses the overlapping odd/even samples: six gathers predict eight output
pixels, rather than eight gathers for two independent four-pixel groups.
Signed byte gathers are masked to 0..255 BEFORE addition. All arithmetic is
`.pi`, not floating-point, and the largest lane sum is 1022. The only vector
word load is the aligned, 16-byte constant-offset table under the four-lane
mask. No pixel word load can be unaligned. The upper half performs no memory
operation. All masks are saved/restored. Full vector lifetime is inside one
asm block, with explicit caller-saved f0..f6 clobbers, no scalar FP spills.

SIMD3 uses offset table `{0,1,2,3,4,5,6,7}`, all eight lanes during gathering and
integer arithmetic, and `fpackrepb.pi f1,f1` AFTER rounding/blending. The packing
instruction snapshots its source and packs low bytes of lanes 0..3 into output
lane 0, and lanes 4..7 into output lane 1; these two words repeat in lanes 2..7.
The packed store enables **only lanes 0 and 1** and accesses exactly eight bytes.
It then restores `m0=255` before another gather, and restores the caller's entire
mask state on return. Only **ordinary L1 `fsw.ps`** is allowed. Never change this
to `fswl.ps` or `fswg.ps`: A0 does not reliably mask those cache-bypassing stores.
There are no new cache operations: owner-row writes and existing row eviction
remain the coherence contract. The scatter microvariant uses L1 `fscb.ps` only.

`fgb.ps` is SIGNED, so every gather is followed by `fandi.pi ...,255`.
`fsatu8.pi` must NOT replace that mask: it would clamp sign-extended source
bytes 128..255 to zero. Saturation after interpolation is redundant because the
largest sum is 1022, and rounded predictions/blends are already in 0..255.

## Four versus eight lanes: costs, not measured cycles

**Mode 2 already predicts eight pixels per iteration**, as two pixels per lane.
It has no 2x loop-trip penalty versus mode 3: both execute 32 groups for a 16x16
plane and eight groups for 8x8. Mode 2 was a conservative way to make every
memory operation visibly half-lane bounded and save repeated horizontal source
samples. Mode 3 safely enables all lanes because its offsets cover exactly
8 bytes (or the validated ninth byte for hx), then narrows the store mask.

Instruction counts below come from the expanded actual inline-asm strings;
address arithmetic and store mask instructions are included, common loop and
once-per-plane setup/teardown excluded. No issue-width/latency assumptions:

| Eight-pixel mode | SIMD4 paired | SIMD8 packed | SIMD8 byte scatter |
|---|---:|---:|---:|
| put | 8 | 6 | 3 |
| hx | 17 | 12 | 9 |
| hy | 20 | 12 | 9 |
| xy2 | 30 | 20 | 17 |
| avg-copy | 19 | 11 | 8 |
| avg-hx | 28 | 17 | 14 |
| avg-hy | 31 | 17 | 14 |
| avg-xy2 | 41 | 25 | 22 |

Important tradeoff: SIMD4 xy2 uses **6 four-lane gathers = 24 byte loads** per
8 outputs, SIMD8 uses **4 eight-lane gathers = 32 byte loads**. hx similarly
uses 12 vs 16 byte loads. hy/copy use the same lane-load counts. Blending adds
eight destination byte reads in either implementation. Thus fewer issued
instructions need not mean fewer cycles; gather overhead is precisely why
SWAR and SIMD4 remain available.

Packed output costs four instructions (pack, mask=3, L1 store, mask=255) versus
one byte-scatter instruction. In return it performs two aligned word stores,
not eight byte scatters. Hardware timing decides which is better.

Clobber/setup audit:

* Mode 2 clobbers caller-saved **f0..f6**, mode 3 only **f0..f2**; both use `t0`
  and `memory`. No floating-point arithmetic or scalar FP register saves/spills.
* Both have five early-clobber/read-write GPR operands (`s,d,rows,masks,cols`)
  and four read-only operands (`stride,size,step,offsets`). Isolated compiled
  probes use ten GPRs total including t0, all caller-saved, with no stack frame.
* One asm per entire plane: save masks + set initial m0 + offset-table load
  once, restore saved masks once. No function calls or mask saves inside rows.
* SIMD4 and SIMD8 scatter: one initial `mov.m.x` plus final mask restoration.
  SIMD8 packed adds two `mov.m.x` per 8 outputs: **64** extra mask writes per
  luma plane, **16** per chroma plane. No full-mask loads/stores are interleaved
  while m0=3. Tests assert the exact mask-write count and restoration.
* Shared loop overhead plus asm prologue/epilogue is 209 scalar/control
  instructions per 16x16 plane, 65 per 8x8. Compiler dispatch/operand setup is
  extra. The loop's structure and pixel-group count are identical in 2 and 3.

Potential later SWAR alignment hoist: source alignment is invariant because
stride and the 8-byte column step preserve it. A future explicit dispatch to
fixed aligned-load-width bodies could avoid repeated alignment tests, at the
cost of code duplication/instruction-cache footprint. **Not implemented:** the
silicon-validated mode 1 remains frozen pending explicit permission.

## Mode 4: window-proof FG32 / FSC32W candidate

**Source:** each eight-column stripe checks once whether
`stride % 32 == 0 && (src % 32) <= 24-hx`. If true, the eight output columns
plus optional ninth horizontal neighbor fit a single aligned 32-byte window,
and the same fact holds for every source row, including the hy row. Its row
loop uses `fg32b.ps`. If false, its row loop uses general `fgb.ps` instead.
The 16-pixel plane's two stripes decide independently. This includes a safe
fallback for the public primitive's 8-aligned but non-32-aligned strides; the
actual decoder has 64-byte strides. Boundary examples: offset 24 is safe only
without hx; offset 23 is safe with hx; offset 25 is never safe for eight bytes.
Both source gather instructions sign-extend and are followed by `&255`.

**Destination:** 8-byte alignment is preserved by every row and stripe step,
so `dst % 32` is always 0, 8, 16 or 24. An eight-byte destination gather can
never wrap. Thus second-direction destination reads always use `fg32b.ps`,
even when the source needs the general path. After packing, `m0=3` plus
`fsc32w.ps` and config **8** writes exactly the words at `dst` and `dst+4`.
Its two 3-bit config fields are word indices `{0,1}`; higher lanes are inactive.
This is ordinary L1 FSC32W, not a G/L store, and it avoids any assumption that
an eight-byte-aligned pointer supports a full vector FSW store. No new cache
operations or changes to owner-row coherence.

The source/destination byte config is `sum(i << (5*i), i=0..7)`, namely
`0x398a418820`. Unlike a speculative aligned load, an FG32 instruction accesses
only its selected eight byte addresses; the guard is necessary to prevent its
address calculation from wrapping, not permission to read the enclosing block.
The model checks the ISA's actual masked 32-byte address arithmetic, not merely
an idealized contiguous read, and asserts every effective address is precisely
what the interpolation requires.

**Scheduling/cost tradeoff:** to avoid any alignment branch inside the row
loop, mode 4 processes all rows of one eight-column stripe before the next
stripe. The proof executes only once for 8x8, twice for 16x16. This may revisit
source/destination cache lines after processing other rows; mode 3's row-major
order remains available as the cache-locality control. Body instruction counts
are identical to mode-3 packed in the table above, but some gathers/scatters
are restricted-window instructions. Common loop/asm overhead is 175 instructions
per 16x16 and 48 per 8x8, versus mode 3's 209/65 (compiler operand setup excluded).
Lower issue counts do not establish a speedup; source gather cost and cache-line
revisits need the parent's matched hardware comparison.

Mode 4 still clobbers only f0..f2, t0, and memory, saving masks once per plane.
Its extra config/proof/rewind operands use thirteen GPRs in the isolated probe,
all caller-saved, with no stack frame. The fast/general loops duplicate code;
probe text is 3040 B versus mode-3 packed's 1688 B. Mask counts are unchanged:
two mask writes per eight outputs, plus once-per-plane mask setup/restoration.
No source or destination pointer advances past the last used row; the next
stripe rewinds to its valid first row before accessing memory.

## Primary local ISA evidence

Read from sibling `../et-platform` (paths relative to its root):

* `sw-sysemu/insns/packed_arith.cpp`: `fadd.pi`, `faddi.pi`, `fandi.pi`,
  `fsrli.pi`, `fslli.pi`, `for.pi` operate independently on `u32` lanes;
  `fpackrepb.pi` defines the low-byte packing/replication and in-place snapshot.
* `sw-sysemu/insns/packed_loadstore.cpp`: `fgb.ps` sign-extends individual
  byte loads; `fsch.ps` stores each active lane's low 16 bits. Unlike
  `fg32b.ps`, general gathers do not wrap inside a 32-byte window. `fg32b.ps`
  uses 5-bit byte-offset fields; `fsc32w.ps` uses 3-bit word-offset fields and
  the exact 32-byte wrapping formulas modeled by the mode-4 tests.
* `sw-sysemu/insn_util.h`: `GATHER`, `SCATTER`, `INTMV_VD` honor m0 per lane.
* `sw-sysemu/insns/packed_mask.cpp`: `mova.x.m` / `mova.m.x` preserve all eight
  masks; `mov.m.x m0,zero,15` explicitly limits memory to the low half.
* `sw-sysemu/mmu.cpp`: `mmu_loadVLEN` performs memory reads only for masked-in
  elements. Offset-table load is naturally aligned and inside its 16 bytes.
* `dnn-library/include/internal/LoadStore.h`: real SDK usage confirms
  `fgb.ps fd, index(base)` and signed-byte handling.

Corsix's first-hand silicon instruction probe (November 12, 2025), supplied by
the parent, confirms the general gathers, integer arithmetic, packing, and
ordinary masked L1 store instructions are native. It also flags the A0
`fsw[lg].ps` masking caveat. This is legal-ISA evidence, not performance or
MPEG-2 correctness evidence. Source: `https://www.corsix.org/content/et-soc-riscv-instructions`;
retrieved copy: `build-et/motion-probes/corsix-et-soc-riscv-instructions.html`.

The complete `et-soc1-dev:20260911` SDK accepts `.pi` integer opcodes under
`-march=rv64imf -mabi=lp64f`. **`.pw` is not this SDK's spelling**: compile-only
probe rejected `fadd.pw/fandi.pw/faddi.pw/fsrli.pw`; evidence is in
`build-et/motion-probes/pw.log`. `.ps` here is raw load/store naming, not float
arithmetic. Do not substitute FADD/FMA float operations or standard RISC-V V.

## Reproduce tests (no launches)

From repository root:

```sh
sh et-kernels/tests/check_motion.sh
sh et-kernels/tests/check_motion.sh --device-compile
# Optional host-only microbenchmark:
build-et/motion-probes/native-1-1 --bench
```

All generated source probes, binaries, assembly and disassembly stay in
`build-et/motion-probes/`. The second command only cross-compiles via
`et-tools/et-env`; it does not invoke a launcher or emulator. No extra packages
are needed. `CC` can select the native compiler. Assertions stay enabled.

Validation completed:

* All four SWAR LOAD/XY combinations: native **and ASan+UBSan PASS**.
* Scalar control: native PASS.
* Each test executable: 24,000 randomized/adversarial block cases spanning
  both sizes, eight modes, all 64 source offsets within a cache line (including
  32/64-byte crossings), stride and aligned-output offsets; destination
  canaries and unchanged reference checks.
* Each executable: exhaustive 65,536 pair averages, 19^4 carry/sign-edge xy2
  tuples with differing adjacent lanes, 250,000 random full-width xy2 tuples,
  both exact formulas independently checked against a per-byte oracle.
* Source/destination guard pages on BOTH sides of EVERY row; exact first/last
  byte, read-only source, no extra row, and ASan-poisoned reference rectangles.
* `test_motion_vector.py`: **5,120 PASS per original SIMD variant** plus
  **40,960 mode-4 cases**, **56,320 total** native source-level model cases,
  extracted
  from the expanded actual inline-asm strings. Includes random original mask
  state, inactive-half poison, 32/64-byte source crossings, aligned destination
  offsets, exact read/write address sets, store alignment, mask-operation counts
  and rounding. Mode 4 covers all 64 source offsets and strides 24/40/64/128,
  proves both fast/general paths and their selection counts, asserts the
  window check executes once per stripe, and checks exact FG32/FSC32W address
  calculations. Packed stores assert exactly lanes 0 and 1 active. Cache-bypassing
  stores are rejected. This does not execute device machine code or establish
  silicon correctness.
* All 20 IMPL/LOAD/XY compile-control combinations plus SIMD8 scatter: ET SDK `-O2 -mstrict-align
  -ffreestanding -fno-builtin -fno-tree-vectorize -mcmodel=medany` compile and
  assemble with `-Wall -Wextra -Werror`. No BSS/data or undefined symbols in
  the isolated probes; disassembly retained. Probe text (runtime dispatch +
  specialized avg-xy16): scalar 364 B, default SWAR 4396 B, SIMD2 1920 B,
  SIMD3 packed 1688 B, SIMD3 scatter 1580 B, SIMD4 FG32 3040 B. All 17 prior
  scalar/SWAR/SIMD2/SIMD3 objects remain byte-identical after adding mode 4.

Complete rerun logs: `build-et/motion-probes/check-motion.log` (modes 0–3) and
`build-et/motion-probes/check-motion-mode4.log` (modes 0–4). Native benchmark
samples show the intended improvement over the old byte loop, but are NOT
silicon performance evidence. Parent should validate full P/B/Bidir frame
output, all active-hart modes, source boundary cases, and matched HPM-counter
measurements before selecting a device default. Existing scalar controls and
all two-stage rounding semantics are retained.
