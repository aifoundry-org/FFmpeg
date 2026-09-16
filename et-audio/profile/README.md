# Isolated AAC offline-fast phase profile (ABI 3)

This is a new ABI-3 profiling sibling of `../offline`; it does not alter ABI 1,
ABI 2, their binaries, reports, runtime adapters, or DSP source.  It profiles
the current exact scalar offline-fast synthesis kernel while replaying complete
captured AAC decoder timelines.  It is **not** a standalone hardware runner.

## Measurement contract

Each `ETAACProfileStatus` is a distinct 64-byte ABI-3 status. For producer
operations 0/1, its five `ticks[]` fields are raw `hpmcounter3` deltas per
channel, accumulated over the requested sequential frames: input
metadata/finite validation; IMDCT/FFT including rotations; window/overlap
copies; output/saved-state finite checks; and PCM publication (`et_evict`).
`operation_samples` explicitly tags the status operation and completed sample
count. The counter reads are exclusively the existing `et_cycles()`
four-aligned-read helper. No `rdcycle`, counter programming/reset/clear,
mutable global/BSS measurement state, or cache-policy change is used.

`profile_enable=0` is the wall-time control: it uses the same DSP algorithm but
makes no timing reads and publishes zero producer phase fields.
`profile_enable=1` reports raw values only. The platform's boot-time default
PMU setup programs HPM3 only on selected hart lanes (hart ID modulo 16 of 0 or
1); therefore a phase value is interpretable only for a known configured lane.
In particular, use active-harts=1/channel 0 first; in a 64-hart run inspect
channel 0 before comparing other channels. Zero/unprogrammed values are
nonrepresentative and must not be called universal “cycles.” `ETAAC_COPY`
remains an overhead control and has zero IMDCT/window phase fields.

## Resident consumer (operation 2)

Operation 2 is a genuine in-ELF resident PCM consumer. It is launched only by
runner mode 2 after one full resident synthesis batch (operation 0), without
an intervening PCM H2D/D2H transfer. Before reading PCM it verifies the prior
producer status tag/result/sample count, final state generations/selectors,
and every resident input's metadata/finite coefficients. That full coefficient
scan is deliberate validation overhead and is not optimized away by the meter.
It then verifies PCM
finite values and computes exact integer IEEE positive-magnitude peak and
count of `|PCM| > 1.0f`, with no floating-point arithmetic or rounding. The
consumer status is explicitly tagged as operation 2; `ticks[0]` is peak bits
and `ticks[1]` is clip count. The runner reads this tiny status as part of the
timed resident service; the full PCM download is a separate
`pcm_validation_read_s` diagnostic used only for capture-oracle validation.

The DSP translation unit is generated at build time from
`../dsp/etaac_synth.c` only after an exact SHA-256 check. The instrumentation
adds an explicit measurement argument around unchanged transform/window code;
the original DSP source is not copied or edited. The separately generated
runtime also starts from the hash-checked pinned video runtime and exposes only
`etaac_profile_launch(…, ETAACProfileParams *, …)`.

## No-hardware build and tests

All new products go under `build-et/aac-profile/`. `ETAAC_FAST_FINITE=OFF`
remains the control default, but the measured offline target is the exact
aligned-word finite-check variant, `ETAAC_FAST_FINITE=ON`. Parent silicon runs
must use `device-fast/et_aac_profile.elf` (the guarded wrapper defaults to it)
and `host-fast/profile-run`; `device/` is retained as the control. These
commands do not open hardware:

```sh
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake -S et-audio/profile \
  -B build-et/aac-profile/host -DCMAKE_BUILD_TYPE=Release
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake --build build-et/aac-profile/host -j8
FF_ET_ALLOW_PCIE=0 et-tools/et-env ctest --test-dir build-et/aac-profile/host --output-on-failure
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake -S et-audio/profile \
  -B build-et/aac-profile/device -DET_DEVICE=ON \
  -DCMAKE_TOOLCHAIN_FILE=/opt/et/lib/cmake/riscv64-ec-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake --build build-et/aac-profile/device -j8
# Measured FAST-finite candidate (same exact DSP arithmetic):
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake -S et-audio/profile \
  -B build-et/aac-profile/host-fast -DCMAKE_BUILD_TYPE=Release \
  -DETAAC_BUILD_RUNTIME=ON -DETAAC_FAST_FINITE=ON
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake --build build-et/aac-profile/host-fast -j8
FF_ET_ALLOW_PCIE=0 et-tools/et-env ctest --test-dir build-et/aac-profile/host-fast --output-on-failure
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake -S et-audio/profile \
  -B build-et/aac-profile/device-fast -DET_DEVICE=ON -DETAAC_FAST_FINITE=ON \
  -DCMAKE_TOOLCHAIN_FILE=/opt/et/lib/cmake/riscv64-ec-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake --build build-et/aac-profile/device-fast -j8
# Native + generated-offline-suite + mock runtime under ASan/UBSan:
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake -S et-audio/profile \
  -B build-et/aac-profile/sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DETAAC_BUILD_RUNTIME=OFF \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
FF_ET_ALLOW_PCIE=0 et-tools/et-env cmake --build build-et/aac-profile/sanitize -j8
FF_ET_ALLOW_PCIE=0 et-tools/et-env ctest --test-dir build-et/aac-profile/sanitize --output-on-failure
```

Native tests compare every PCM sample and final state for 64 channels/four
frames against the uninstrumented exact DSP, for profile disabled and enabled,
exercise the resident meter, ABI/finite rejection, and include a generated,
hash-pinned ABI-3 adaptation of the full offline native suite (split offsets,
active-hart ownership, stale/poison/partial-failure coverage). Mock runtime
tests cover typed ABI-3 range, frame, and profile-flag validation without
constructing a real SDK device.
The device target retains the offline scalar legality gates: RV64IMF,
non-vectorized, no contracted FMA, and `check-device.py` validation.

## Guarded execution

Only after separate authorization, use the locked wrapper; it has a new
`build-et/aac-profile/PROFILE_RECOVERY_REQUIRED` marker and also refuses the
older offline/prototype/global recovery markers. After an active real-run
failure it writes its profile marker and, only if absent, the shared
`build-et/silicon/RECOVERY_REQUIRED` marker so older wrappers are blocked too. It keeps physical shire 0,
checks existing owners and before/after health, and never resets/flashes,
clears/programs counters, changes cache settings, or accepts a direct runner.

```sh
FF_ET_ALLOW_PCIE=1 ETAAC_PROFILE_ENABLE=1 \
  et-audio/profile/run-silicon.sh UNIQUE_PROFILE 64 64 64 64 1 0 \
  build-et/aac-offline/captures/stereo-48000.etaaccap
# Control with identical algorithm but no counter reads:
FF_ET_ALLOW_PCIE=1 ETAAC_PROFILE_ENABLE=0 \
  et-audio/profile/run-silicon.sh UNIQUE_CONTROL 2 1 64 64 1 0 \
  build-et/aac-offline/captures/stereo-48000.etaaccap
# One resident synthesis batch followed by resident exact peak/clip meter:
FF_ET_ALLOW_PCIE=1 ETAAC_PROFILE_ENABLE=1 \
  et-audio/profile/run-silicon.sh UNIQUE_METER 2 1 64 64 2 0 \
  build-et/aac-offline/captures/stereo-48000.etaaccap
```

The runner validates exact PCM for every channel/frame and final state, then
prints one `channel_ticks` JSON row per channel/launch. Producer phase JSON
and validation occur after the measured launch/read/download timestamps, so
logging is not charged to `launch_s` or `completion_s`. Mode 2 reports summed
producer/consumer components rather than a pure end-to-end pipeline wall time;
checks/logging between launches remain outside those components. It should be used only
through the wrapper; the wrapper rejects reuse of evidence directories.

Limitations: operation 2 is only a peak/clip meter, initially restricted to
one full resident batch; it is not a general downstream PCM API. This still
isolates synthesis phases rather than parsing/entropy decode, and no claim
about hardware timing, bandwidth, or full AAC decode performance follows from
raw counter fields alone.

## Recorded hardware results

The parent experiment subsequently ran 19 successful guarded shire-0 cases,
including profile controls and the resident meter. See `../../ET_AAC_PROFILE.md`
and `.json` for scope, all samples, phase shares, numerical checks and limits.
`results.py` reads retained evidence only and never initializes the runtime.
