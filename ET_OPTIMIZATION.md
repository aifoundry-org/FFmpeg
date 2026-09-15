# Single-shire integer SIMD optimization — September 15, 2026

## Result: measured silicon, exact pixels

Physical **shire 0 / mask 0x1**, ordinary per-frame launches. Same production
FFmpeg executable, inputs and full framemd5 sink for both ET kernels. CPU uses
the same build, with default decoder threading. All processes are pinned to
this host's P-core CPUs 0–15. Five independent full-clip runs per path, alternating
ET A/B order; timing instrumentation off. **No samples discarded.**
Hardware reports 600 MHz minions / 400 MHz NoC; host is an i7-13700K.
SDK image and firmware are the pinned versions in `ET_IMPLEMENTATION.md` /
`ET_VALIDATION.md`; no firmware or frequency changes were made.

| 250-frame I/P/B clip | Baseline ET median | Selected ET median | ET speedup | Selected fps | CPU median |
|---|---:|---:|---:|---:|---:|
| 720x576 | 3.286 s | **1.252 s** | **2.625x** | 199.7 | 0.163 s |
| 1920x1080 | 8.150 s | **2.432 s** | **3.351x** | 102.8 | 0.777 s |

These are FFmpeg benchmark wall times including initialization, transfers,
reconstruction and hashing, not kernel-only throughput. The scalar baseline is
commit `0f6deff`, not an artificially slowed control. CPU decoding remains
**7.68x faster at SD and 3.13x faster at HD**. No power-efficiency claim.

The second round had an approximately **3.5-second additive system-time
excursion in both ET arms**. It is retained, not trimmed or reclassified:

| Path | SD samples (seconds) | HD samples (seconds) |
|---|---|---|
| Baseline | 3.275, 6.885, 3.286, 3.290, 3.275 | 8.150, 11.696, 8.125, 8.105, 8.153 |
| Selected | 1.234, 4.801, 1.252, 1.251, 1.252 | 2.419, 6.022, 2.436, 2.429, 2.432 |
| CPU | 0.178, 0.162, 0.164, 0.160, 0.163 | 0.774, 0.776, 0.777, 0.782, 0.781 |

Paired savings are much steadier: **2.023–2.084 s SD**, **5.674–5.731 s HD**.
The kernel/driver cause of the system-time excursions was not diagnosed. Do not
interpret medians as a latency guarantee or infer hardware stall categories.
A separate one-hart SD run passed in **9.430 s**, versus the historical scalar
one-hart observation of 43.321 s; that is a single observation, not the repeated
headline comparison above.

## Selected implementation

1. **Exact eight-lane integer IDCT**, rows and columns. Custom `fmul.pi`,
   add/subtract, shifts and masks preserve modulo-32 arithmetic, per-row DC
   shortcuts, int16 intermediate wrapping and final unsigned clipping. No F32,
   F16 or approximate transform. Fixed vector values never survive a C call;
   helpers use caller-saved registers and preserve masks.
2. **Packed coefficient/output access.** Aligned blocks use proven-window
   FG32 halfword gathers. Eight clipped pixels pack into two words, stored with
   exactly two active lanes. Arbitrary helper alignment retains a safe general
   vector path. The decoder's block scratch is aligned to 32 bytes.
3. **Eight-lane motion compensation**, unsigned byte gathers and packed masked
   ordinary-L1 stores. The four-sample half-pel sum is rounded once; B-picture
   blending is rounded separately. Chroma negative-vector division and all
   reference bounds are unchanged. No masked G/L cache-bypassing stores.
4. **Core-first work assignment.** Worker index is `(hart >> 1) + 32*(hart & 1)`.
   Thirty-six SD slice rows now use all 32 minions before SMT siblings, rather
   than occupying only 18 minions. Full row/cache-line ownership is preserved;
   65/68-row cases cover subsequent iterations. One-hart behavior is unchanged.
5. **Bounded direct bitreader interiors and reused tails.** Original input is
   used directly only when the full reader window plus 64 bytes of lookahead
   lies within the actual slice. Tails use a reused, locally padded 1024-byte
   window. Short headers avoid rebuilding GetBitContext. The public ABI still
   requires **no input padding**; `HAVE_FAST_UNALIGNED=0` remains mandatory.
6. **Aligned RV64 memory primitives**, exact byte tails and no speculative
   unaligned word accesses. Coefficient zeroing no longer uses 128 individual
   byte stores; local windows no longer copy 320 bytes for every block.
7. **Proven DC/mismatch-corner shortcut.** Integer-pixel DC in a bounded range,
   otherwise zero AC except corner -1/0/1, reconstructs exactly as the full
   fixed-point IDCT. Constant Put uses packed words; Add uses integer SIMD.
   All other blocks use the full transform. The integer proof and exhaustive
   tests are in `et-kernels/tests/reconstruct.md`. Three paired 250-frame
   SD **I-only** runs measured 1.294 s median with this shortcut versus
   1.440 s without it (10.1% lower); its I/P/B impact was much smaller.

The selected `.text` is **14,904 bytes** plus 28 bytes CRT; rodata 20,416 bytes,
BSS zero. This is a footprint measurement, not an I-cache-miss measurement.

### Controls tried, not blindly enabled

Compile-time controls remain available for isolated experiments:

- Scalar, RV64 SWAR, paired four-lane and eight-lane SIMD motion. Final matched
  SD medians favored SIMD8 over otherwise matched SWAR (1.269 vs 1.341 s in the
  three-run selection batch; all samples, including one slow round, retained).
- Byte-scatter versus packed L1 motion stores: both correct; byte scatter was
  slower in SD/HD spot checks, so packed stores remain selected.
- FG32-window-proved motion with packed scatter stores: correct, but tied the
  selected motion median and adds code/branches; not selected.
- SIMD columns alone, full rows, generic versus packed IDCT access: all pass;
  selected full rows/packed access. Discovery timings are diagnostic single runs,
  not independently attributable speedup percentages.
- `-O3`: correct, no improvement over the smaller `-O2` selected binary in the
  matched SD selection batch (1.269 vs 1.242 s medians).
- 32-even-hart-only scheduling, with complete reassignment of all rows: correct,
  but slower than core-first 64-hart scheduling on tested SD/HD. Disabled.
- SIMD-prescaled quant matrices: exact and tested, but no observed advantage.
  Disabled; qscale-change invalidation is retained in the experimental control.
- 512/2048-byte windows: no convincing advantage over 1024; not selected.
- Bounds-proved L2 prediction prefetch: correct, no convincing overall gain;
  disabled. No SRAM partition, scratchpad reservation, locks or new barriers.

Defaults are in `et-kernels/CMakeLists.txt` and `scripts/build-device.sh`.
The script explicitly resets selected options before applying user overrides,
so a reused CMake cache cannot silently preserve a previous experimental variant.
Rejected controls compile out of the selected ELF.

## Connection to the requested prior conversations

Read `et-soc1-optimization-techniques` (`cQPUG2J`) and
`hyenadna-et-inference-plan` (`cNKCFB5`), including the later rejected experiments,
not just the early peak-throughput slide summary. Applied lessons:

- Handwritten custom SIMD, independent lanes and register reuse; no assumption
  that the compiler emits the ET vector ISA.
- Complete cache-line ownership, explicit publication and mask restoration.
  Existing bounded eviction / WAIT_CACHEOPS / fence protocol is unchanged.
- Test instruction mix, packing, alignment, occupancy and modest unrolling;
  reject more elaborate variants when matched timing does not improve.
- Do not import a TensorFMA/scratchpad scheme into an exact-integer decoder
  merely because it helped an F32 matmul. No tensor-unit, reduced-precision,
  firmware-cache-mode or persistent-scratchpad changes were made here.
- Avoid the unreliable PMU conclusions from earlier bulk traces. Optional
  `FF_ET_TIMING=1` reports **host API envelopes including waits**, separately
  from clean timing. It does not report per-hart IPC or memory-stall cycles.

## Correctness and hardware safety

The retained inventory contains **105 silicon runs / 18,457 frame comparisons**
across baseline and candidates, all exact full framemd5 comparisons. **4,931
comparisons use the exact selected ELF**, including:

- Five SD and five HD 250-frame runs (plus selection comparisons).
- All 14 positive native-suite streams at **1 and 64 harts**: tiny/edge sizes,
  custom matrices, nonlinear qscale, intra VLC and repeated I/P/B reference reuse.
- The 65-row scheduling case, SD 250 frames at one hart, HD 12 at one hart,
  and three 250-frame SD I-only runs.
- A separate **12-frame I/P/B sys_emu run**, `-mem_check -Werror=memory`, exits 0,
  exact MD5; no emulator performance claim.

Native gates: **115 integration/audit/direct-oracle cases pass**, plus runtime
sanitizers and the ET-disabled dependency gate. IDCT instruction models compare
668,290 Put/Add outputs per flag combination; the DC shortcut has 7,130,112
accepted comparisons, 261,900 refusals and 1,536 raw-IDCT checks. Memory primitives
have 32,784 guarded copy/fill cases. Expanded motion assembly models validate
address sets, masks and exact arithmetic. Long-slice guard tests add 160 valid,
22,351 truncation and 7,680 malformed cases to the earlier tiny-payload tests.
Native models are **not** hardware/ISA emulation; the silicon and sys_emu gates
above are separate evidence. CI runs the new model/guard/sanitizer tests.

Every hardware run held the workspace device lock, used physical shire 0 and
compared pre/post device/MM counters. MM hangs remain **0**, exceptions remain
at the pre-existing **1**; MinionCeEvent **7**, SpCeEvent **1**, all other CE and
all UCE zero, unchanged. No reset/flash/boot-status query/counter clear or clock
change. The runtime's normal initialization is unchanged.

## Reproduce and evidence

```sh
et-kernels/scripts/build-device.sh
et-tools/et-env make -C build-et/host -j16
FF_ET_ALLOW_PCIE=1 FF_ET_CPUSET=0-15 et-tools/test-silicon.sh fresh-name 64 \
  build-et/corpus/sd-ipb250.m2v build-et/corpus/sd-ipb250.golden.md5
# Optional diagnostic run with another unique result name:
FF_ET_ALLOW_PCIE=1 FF_ET_CPUSET=0-15 FF_ET_TIMING=1 et-tools/test-silicon.sh \
  fresh-profile 64 build-et/corpus/sd-ipb250.m2v build-et/corpus/sd-ipb250.golden.md5
python3 et-tools/optimization-results.py  # reads evidence only, never opens ET
```

`FF_ET_CPUSET` is optional and host-specific. Use unique names: the silicon
helper refuses to overwrite a passing result. `FF_ET_KERNEL=/work/...` selects
a frozen control image. Recreate clips with the same encoder command described
in `ET_IMPLEMENTATION.md` (250 frames, GOP 12, 2 B frames, qscale 3; dimensions
720x576 or 1920x1080). CPU goldens use the same production FFmpeg build.

Frozen binaries, all raw samples, input streams, hashes and health snapshots:
`build-et/optimization/`, `build-et/silicon/opt-*`. Committed compact inventory:
`ET_OPTIMIZATION.json`; collector refuses failed/mismatching evidence.

- Baseline kernel: `93d5319440f44d4a741a57fd708959d8908d7105b010dab73a5731829e4fea2d`
- Selected kernel: `44ac57592938cde2c398ef05e281e27fc54d548e6b38ed9847555c24c94152f3`
- Tested host: `ec4651f9825b9ba9fea21b7cb0f9a4a681f19beb4be3c4224775613475b8b6ce`

Remaining opportunities are entropy-decoder/front-end costs and the mandatory
per-frame launch/readback path, not an assumed tensor-unit peak. Supported
stream restrictions, host YUV420P output and single-shire scope are unchanged.
