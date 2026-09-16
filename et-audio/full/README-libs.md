# Private scalar final-link libraries

Build only in the SDK container, with PCIe disabled:

```sh
FF_ET_ALLOW_PCIE=0 et-tools/et-env env FF_ET_ALLOW_PCIE=0 \
  python3 et-audio/full/build-scalar-libs.py
```

The script writes a fresh ignored `build-et/aac-full/scalar-libs-YYYY-MM-DD/`.
For the simulator-safe divide/square-root/64-bit-conversion variant, use the
cached sources only:

```sh
FF_ET_ALLOW_PCIE=0 et-tools/et-env env FF_ET_ALLOW_PCIE=0 \
  python3 et-audio/full/build-scalar-libs.py --offline \
    --output build-et/aac-full/scalar-libs-no-mcode
```

Use its `lib/` **before** SDK defaults in the final link:

```sh
-L/work/build-et/aac-full/scalar-libs-YYYY-MM-DD/lib -lm -lc -lgcc
```

`libc.a` and `libm.a` are full newlib 4.5.0 static builds for
`rv64imf/lp64f` with `-mno-fdiv`; that compiler switch prevents both `fdiv.s`
and `fsqrt.s`. The builder also disables only newlib's RISC-V **libm** machine
directory, selecting upstream portable `libm/common` implementations instead.
In particular, `lrintf`/`llrintf` retain their generic volatile rounding-mode
addition and bit extraction rather than RISC-V `fcvt.l.s` asm. Their
nonrepresentable-result fallback (where C leaves the numeric result
unspecified) is integer saturation solely to avoid a compiler-emitted `fcvt`;
representable inputs retain newlib's rounding-mode semantics. `libgcc.a` is
LLVM compiler-rt generic builtins under that name, so it supplies real
arithmetic helpers including `__divsf3` without target-libc dependence.
`PROVENANCE.txt`, `SHA256SUMS`, and `ARCHIVE_AUDIT.txt` record source pins,
hashes, flags, and instruction audit.

The archive audit rejects compressed, RVV, D/Q/packed, `fdiv.s`, `fsqrt.s`,
`fcvt.l.s`, `fcvt.lu.s`, `fcvt.s.l`, and `fcvt.s.lu` instructions. Some unused explicit `fma` routines in full libm can
contain `fmadd.s`; the final ELF must still be linked with `--gc-sections` and
pass `check-device.py`, which rejects selected FMA instructions.
`FULL_LINK_SMOKE.txt` is an offline link and structural gate only. It is not a hardware launch or a numerical
correctness claim: validate scalar software-double/libm behavior against the
CPU oracle; do not assume bit-exactness from the build flags.
