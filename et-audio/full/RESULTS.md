# Full AAC integration checkpoint — September 16, 2026

**Silicon remains blocked: the extended simulator run failed exact PCM.**
No full-decoder silicon launch or ET throughput result exists. The candidate
passes the strengthened static gate, but hardware tooling also requires a
successful simulator result with the same ELF hash, in addition to all
existing shire-0, locking and health guards.

## Verified

- Real decoder INIT → split DECODE → fused meter → CLOSE passes all five
  native scalar PCM fixtures: stereo 48 kHz at 1/32/512 packets and mono
  44.1/48 kHz at 32 packets. No numerical tolerance is used.
- Post-INIT padding/length mutations fail without writing PCM; poisoned state
  cannot be reused. Allocator ASan/UBSan, seven envelope rejection cases,
  no-overwrite checks, and the isolated runtime mock pass.
- Complete FFmpeg, newlib/libm and compiler-helper final link uses actual
  bounded allocation, a private stack, explicit global/heap publication, and
  3,519 checked pointer relocations. No library processing is replaced by
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
The numerical root cause is not established; the next investigation must
isolate the packet-10 divergence offline/in simulation before hardware is
eligible. No hardware access, recovery operation, or marker clearing was
performed following this failure.

Raw PCM, private state, logs, exit status and provenance remain unchanged.
New read-only-derived diagnostics and their raw-artifact hashes are saved in
`build-et/aac-full/emulator/portable-extended-32/pcm-analysis.json` and mirrored
in `RESULTS.json`.

## CPU baseline, not an ET comparison

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
complete one-stream workload. No full-decoder ET speedup or slowdown is claimed.
