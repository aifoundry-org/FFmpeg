# Full AAC integration and validation — September 16, 2026

**Fixed and benchmarked.** The corrected decoder passes all five fixtures on
shire 0/hart 0 and ten complete 512-packet benchmark runs, with exact scalar
PCM, clean teardown and unchanged recorded device health. Across the full
investigation, 17 physical runs comprise 16 exact passes and one retained
numerical failure. No device reset/recovery operation was performed.

For 512 stereo packets, matched codec-lifecycle medians are **6.682 ms scalar
CPU**, **4.877 ms optimized CPU**, and **1,791.030 ms ET single hart**. Full
method, phase accounting, numerical caveats and every benchmark sample are in
`BENCHMARKS.md` / `BENCHMARKS.json`. Earlier failures below remain historical
evidence, not hidden or converted to passes.

## Verified

- Real decoder INIT → split DECODE → fused meter → CLOSE passes all five
  native scalar PCM fixtures: stereo 48 kHz at 1/32/512 packets and mono
  44.1/48 kHz at 32 packets. No numerical tolerance is used.
- Post-INIT padding/length mutations fail without writing PCM; poisoned state
  cannot be reused. Allocator ASan/UBSan, seven envelope rejection cases,
  no-overwrite checks, and the isolated runtime mock pass.
- Complete FFmpeg, newlib/libm and compiler-helper final link uses actual
  bounded allocation, a private stack, explicit global/heap publication, and
  checked pointer relocations (3,519 originally; 3,510 in the corrected ELF). No library processing is replaced by
  fake-success stubs. Tables frozen at build time contain no packet data.
- All 89 source hashes in the four earlier manifests still match. The prior
  video (`optimization/opt2`), AAC prototype, offline and profile selected
  kernel hashes remain unchanged.

## Failures retained, not counted as passes

1. SDK libraries contained compressed instructions: rebuilt private libraries.
2. Zero instruction alignment padding: replaced with full-width NOP fill.
3. Missing ELF relocations: simulator INIT access fault; now retain and bound
   pointer relocations with `--emit-relocs --no-relax`.
4. Floating divide triggered an ET microcode exception: privately rebuild
   FFmpeg and libraries with `-mno-fdiv` and portable rounding functions. The
   gate now rejects divide/sqrt and 64-bit float-conversion microcode ops,
   unknown/trapping instructions, and unapproved CSR access.
5. The portable 32-packet simulator attempt hit its 900-second limit before
   INIT completed. Host-only debugger samples observed target `sqrt` code;
   this does not prove that initialization will finish. No success is inferred.
6. A reduced virtual-topology probe was stopped without reaching runner
   configuration. An editing race in its running shell wrapper produced exit
   127; its overwritten startup log is explicitly recovered from the tool
   transcript. Standard SDK virtual topology is restored. Subsequent long
   jobs use an in-memory wrapper snapshot.

## Extended simulator result: failed numerical acceptance

`build-et/aac-full/emulator/portable-extended-32` completed within its
3,600-second cap and exited **1**, not a timeout. Its recorded ELF, runner,
compressed input and scalar oracle hashes all still match. No `PASS.json`
was produced.

The unchanged `device-no-mcode-v1/et_aac_full.elf` completed INIT, two
16-packet DECODE requests, fused-meter verification, and CLOSE. All five
status results were zero, unsupported-call and heap-failure counters were
zero, all stack guards passed, and live heap returned to zero on CLOSE.
Nevertheless, **407 of 65,536 PCM words differ** from the scalar oracle:

| Packet index (zero-based) | Mismatched words |
|---|---:|
| 10 | 249 |
| 11 | 158 |

The first mismatch is word 20,691: packet 10, channel 0, sample 211.
Both channels are affected. The runner correctly reports zero accepted exact
packet frames despite completing 32 decoded packets. No tolerance is applied;
the matching meter features do not establish PCM equality.

INIT took about 20.2 minutes and the two decode launches about 9.0 minutes
combined **in the simulator**. These wall times are not silicon performance.
The numerical root cause was not established at the simulator checkpoint.
No hardware access occurred until the user's subsequent explicit authorization
for numerical debugging (below). No recovery operation or marker clearing has
been performed.

Raw PCM, private state, logs, exit status and provenance remain unchanged.
New read-only-derived diagnostics and their raw-artifact hashes are saved in
`build-et/aac-full/emulator/portable-extended-32/pcm-analysis.json` and mirrored
in `RESULTS.json`.

## User-authorized silicon diagnostics

The normal emulator-PASS requirement remains the default. The separate
`ETAAC_FULL_NUMERICAL_DEBUG=1` route accepts only the reviewed ELF and completed
simulator evidence pinned by hash in `silicon-debug-authorization.json`.
It explicitly records that simulator PCM did **not** pass. Static instruction,
shared-lock, device ownership, health-baseline, and exact-output checks remain
unchanged. Six offline mutation checks reject altered eligibility evidence.

1. `silicon/debug-portable-smoke-1`: one packet, full lifecycle, exact PCM,
   unchanged before/after CE/UCE/MM counters; **PASS**.
2. `silicon/debug-portable-stereo-32`: two 16-packet requests and meter retrieval,
   full lifecycle and clean completion/stack/heap checks, unchanged CE/UCE/MM
   counters; **FAIL**, 407 PCM words differ from the scalar oracle.

The second run's entire 262,144-byte PCM output is **byte-identical to the
simulator output**, not merely the same mismatch count. This establishes that
the numerical defect reproduces on silicon. It does not establish its precise
arithmetic cause or guarantee that future kernels cannot wedge the device.

Saved diagnostic timings for that failed 32-packet run:

| Phase | Host wall time |
|---|---:|
| Runtime/image/buffer setup | 292.656 ms |
| Decoder INIT launch | 285.790 ms |
| Two DECODE launches combined | 101.687 ms |
| METER retrieval launch | 5.712 ms |
| PCM readback | 0.496 ms |
| CLOSE launch | 6.214 ms |

Launch times include kernel publication; separate status I/O and all other
samples remain in `RESULTS.json` and raw logs. These are **failed-correctness
diagnostics**, not an approved throughput comparison against the 512-packet CPU
baseline. No 512-packet or mono follow-up was launched on that faulty ELF.

The wrapper wrote `build-et/aac-full/RECOVERY_REQUIRED` and
`build-et/silicon/RECOVERY_REQUIRED` because of exit 1. Card access stopped
while offline diagnosis proceeded. Saved health data showed no new hang or
exception. Both marker files were later archived verbatim under the shared
lock after their numerical-only cause was proved and fixed; the user had
explicitly authorized debugging and then requested the full benchmark.
The review is retained at
`build-et/aac-full/reviews/sine-window-numerical-hold/review.json`.

## Proven cause, fix and acceptance

Native API tracing rules out PNS and pulse data for this fixture. The first
visible mismatch in packet 10 is copied from saved overlap produced by packet
9's short sequence. The selected rv64imf newlib uses old `sf_sin.c`, not modern
`common/sinf.c`; 17/128 short-window coefficients differ by one ULP from native
host generation. Injecting only that table into native FFmpeg reproduces the
entire failed target PCM byte-for-byte. An independent whole-`sinf` override
also reproduces it. The earlier modern-sinf control was not representative;
its falsification conclusion is withdrawn and its artifacts remain archived.

`freeze-sine-windows.py` generates payload-independent constants from the pinned
native initializer for AAC lengths 120/128/512/960/1024. A private upstream
sinewin object copies those coefficients during normal initialization; all
other lengths retain normal computation. All six existing device objects and
all library hashes match the previously completed simulator parent. No decoder
or FFT processing is replaced, and no AAC payload is used to generate tables.

The old-sinf native reproducer plus this fix passes all five golden fixtures,
repeat accounting and malformed-input tests. The new ELF passes the unchanged
static instruction/relocation gate. A tightly pinned reviewed-child diagnostic
authorization then admitted hardware validation, rather than inventing a
corrected-emulator PASS. All five fixtures passed on the card, followed by ten
512-packet benchmark passes, including fused-meter verification. No new health
change or failure marker occurred. Frozen binaries and hashes are retained in
`build-et/aac-full/selected/`.

The matched CPU v3 driver additionally performs the same finite validation and
fused statistics as ET. It remains scalar-bit-exact for all five fixtures;
optimized output is byte-identical to the retained optimized native reference,
which is distinct from scalar. Benchmark samples were taken only after builds
and correctness work finished, without overlapping CPU and ET runs.

## Historical ordinary CPU baseline (superseded for matched comparison)

One full stereo 48 kHz stream, 512 packets (1,048,576 float samples), CPU0.
Five fresh processes per mode, with complete output stores. Median codec
setup + decode + teardown:

| CPU dispatch | Median |
|---|---:|
| Generic C (`cpuflags=0`) | 6.127 ms |
| Optimized | 4.398 ms |

Input file loading, envelope validation, PCM allocation and optional file
writing are outside these codec timers. CPU ADTS configuration is discovered
on first packet; the device passes ASC during INIT, so individual setup/decode
phase boundaries differ. Generic C dispatch is not a promise that the native
compiler emitted no SIMD instructions.

A repeat-loop double increment was fixed **before** these benchmarks. Tests
now verify exact frame/sample counts for 2 and 5 repetitions; teardown is
separately timed. The old CPU executable and earlier diagnostic outputs remain
preserved. All twelve timed process results, including the extra five-context
repeat process per mode, are retained in `RESULTS.json` and
`build-et/aac-full/cpu-benchmark-v2/`.

Earlier 64-channel synthesis-only ET/CPU results are not equivalent to this
complete one-stream workload. The completed matched-work comparison is reported
separately in `BENCHMARKS.md`; these earlier ordinary CPU samples are not used
as its primary baseline.
