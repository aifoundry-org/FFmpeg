# SIMD update — September 15, 2026

Rebuilt with integer-IDCT models, direct/bounded bits, exact DC shortcut,
core-first scheduling and SWAR motion reference. **115/115** integration,
policy/drain and direct-oracle cases pass (69 + 8 + 16 + 22). ASan/UBSan runtime
units and ET-disabled dependency isolation pass again. Logs:
`build-et/optimization/*-regression.log`; results `/tmp/et-opt-{all,audit,kernel}`.
Additional kernel SIMD/guard tests and separately validated hardware results
are in `../ET_OPTIMIZATION.md`. The handoff below is the retained earlier audit.

# Test-agent handoff (2026-09-15)

## Final HOST-NATIVE validation (no simulator/hardware claim)

- **Default complete integration suite: 69 passed, 0 failed.** Freshly rebuilt
  kernel/runtime archives and forced FFmpeg relink, rerun after the late MPEG-1,
  profile/chroma/scalable-extension, IDCT and delayed/drain validity fixes.
  Eight single-I/I-only streams plus six I/P/B full-GOP streams each match CPU
  default and simple IDCT exactly with 1 and 64 active harts (56 comparisons).
  Eight unsupported/malformed cases and five injected runtime errors reject
  cleanly without signals (13 negatives).
  Command: `et-tests/run.sh /tmp/et-test-build/ffmpeg/ffmpeg /tmp/et-test-all native all`.
  Summary `/tmp/et-test-all.log`; individual logs, corpus and hashes under
  `/tmp/et-test-all/`. Latest build log `/tmp/et-test-build-audit.log`.
- **250-frame 128x96 I/P/B open-GOP stress passed all four variants.** Both CPU
  oracle outputs and all four ET outputs contain exactly 250 frame records.
  Every ET decode also logs 250 successfully completed ET frames with the
  requested hart count, preventing an open-runtime/CPU-fallback false positive.
  Stream `gop/long-ipb-250.m2v`, result prefix `long-ipb-250` under the above tree.
- **Late-audit policy regressions: 8 passed, 0 failed.** Genuine encoded MPEG-1
  (first verified CPU-decodable) is rejected when forced through the MPEG2
  decoder with ET requested. Also covers unsupported High profile, same-size
  profile/chroma transitions after a valid ET frame, scalable IDs 5/9/A under a
  Main sequence header, and unsupported integer IDCT. Intended ET diagnostics
  are required, not arbitrary nonzero exits.
- **Public libavcodec API drain audit: 4 controls + 12 fault scenarios passed.**
  Whole-file single-I packet and parser-split I/P packets, 1/64 harts, with
  launch/read failure on I or P. Controls verify pictures really are delayed
  until drain. Fault cases continue submission/receive after errors and repeat
  NULL-packet drain calls: no failed I/P picture leaked. All twelve injections
  and all sixteen native runtime opens were observed.
  Command for both audit groups: `et-tests/audit.sh /tmp/et-test-build /tmp/et-test-audit`.
  Summary `/tmp/et-test-audit.log`; API log `/tmp/et-test-audit/logs/drain.log`.
  Sensitivity was independently confirmed by linking a temporary test-only
  wrapper that bypasses `ff_et_mpeg2_picture_valid`: the harness then fails on
  an invalid drained I frame (`result.n <= completed`). No host source was edited.
- **Direct native kernel oracle: 22 comparisons passed.** Eleven cases with
  1/64 harts, including alternate scan, custom intra matrix, intra VLC, qscale,
  DC precision, field DCT, edge dimensions, HD, and combined flags.
  Command: `et-tests/run-kernel-native.sh /tmp/et-test-build/ffmpeg/ffmpeg /tmp/et-test-build/runtime/kernel/libet_mpeg2_native.so /tmp/et-test-kernel-direct`.
  Log `/tmp/et-test-kernel-direct/kernel-direct.log`. Generated kernel source
  checksums were unchanged by this direct test.
- **ET-disabled configure/build/decode gate passed again after the late audit fixes.**
  Zero ET pkg-config probes, runtime/kernel symbols in libavcodec, or shared ET
  dependencies. Disabled macros, absent advertised accelerator, and CPU
  default/simple IDCT single-I output also checked.
  Command: `JOBS=8 et-tests/check-cpu-disabled.sh /tmp/et-test-disabled`.
  Build log `/tmp/et-test-disabled.log`; details `/tmp/et-test-disabled/`.
- Runtime/stub units plus Python comparison-tool tests passed with ASan/UBSan
  and 64-byte DMA alignment/range checks. Native pkg-config smoke tests passed
  for both the default static kernel and an externally supplied native shared
  kernel library.
- Optional sysemu wrapper prerequisites/opt-in produce explicit SKIP with exit
  77 when unavailable, never PASS. **This agent has not performed any simulator
  or hardware validation.** Main agent owns real sysemu results.

## CI and acceptance policy

- `.github/workflows/etsoc.yml` now mandates supported-I/negative coverage,
  full-GOP including 250 frames, late-audit policy/API-drain checks,
  direct-kernel syntax comparisons, and the CPU-only disabled build. **No continue-on-error remains.**
- `run.sh` defaults to `all`; `supported-i` and `full-gop` select individual
  stages. The old `planned-pb` name is only a backward-compatible `full-gop` alias.
- Built FFmpeg forces nonprogressive sequence/frame flags with alternate scan.
  That generated stream remains an explicit host-policy negative. Direct
  kernel alternate-scan success does not imply host acceptance of interlace;
  a genuinely progressive alternate-scan host fixture is still needed.
- Explicit corpus manifests prevent stale files from entering new test runs.
- Native builder always relinks FFmpeg after external archive rebuilding and
  exposes the same compiled kernel as a shared library for direct tests.

Only et-tests/ and .github/workflows/etsoc.yml were edited. No commit was made.
