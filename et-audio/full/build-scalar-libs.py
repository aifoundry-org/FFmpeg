#!/usr/bin/env python3
"""Build private RV64IMF/lp64f static libc, libm, and compiler helpers.

Run only in the local SDK container; it neither launches nor opens a device:
  FF_ET_ALLOW_PCIE=0 et-tools/et-env env FF_ET_ALLOW_PCIE=0 \
    python3 et-audio/full/build-scalar-libs.py

The output is deliberately a new ignored build-et/aac-full/scalar-libs-* tree.
It must be added before the installed SDK directories at the final link, e.g.
  -L/work/build-et/aac-full/scalar-libs-YYYY-MM-DD/lib -lm -lc -lgcc
"""
import argparse
import datetime as dt
import difflib
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
TARGET = "riscv64-unknown-elf"
TOOL = "/opt/et/bin/riscv64-unknown-elf-"
NEWLIB_URL = "https://sourceware.org/git/newlib-cygwin.git"
NEWLIB_TAG = "newlib-4.5.0"
NEWLIB_COMMIT = "5e5e51f1dc56a99eb4648c28e00d73b6ea44a8b0"
# This is the SHA-256 of `git archive NEWLIB_COMMIT | gzip -n` below.
NEWLIB_ARCHIVE_SHA256 = "ae2638c677ba42f4d146a8185cb7dff1be8e6ebddbd72e2b23d15b2891079884"
LLVM_TAG = "llvmorg-20.1.8"
LLVM_COMMIT = "023ec9011c9a92cfa8922030eb266d66a10f78f8"
LLVM_URL = "https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-20.1.8.tar.gz"
LLVM_ARCHIVE_SHA256 = "a6cbad9b2243b17e87795817cfff2107d113543a12486586f8a055a2bb044963"

# Keep all FP contraction disabled and forbid hardware scalar divide/square root.
# RV64F permits scalar single precision only; newlib double routines consequently
# use integer/software helpers.  -mno-fdiv covers both fdiv.s and fsqrt.s.
TARGET_CFLAGS = " ".join((
    "-O2", "-march=rv64imf", "-mabi=lp64f", "-mcmodel=medany",
    "-ffunction-sections", "-fdata-sections", "-ffp-contract=off",
    "-fno-fast-math", "-fno-builtin", "-mno-fdiv",
))
CRT_CFLAGS = TARGET_CFLAGS + " -ffreestanding"
# These operations are explicitly MCODE traps in the ET software emulator.
MCODE_CONVERSIONS = ("fcvt.l.s", "fcvt.lu.s", "fcvt.s.l", "fcvt.s.lu")


def die(message: str) -> None:
    raise SystemExit(message)


def run(*args: object, cwd: pathlib.Path | None = None, stdout=None) -> None:
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, stdout=stdout, check=True)


def capture(*args: object, cwd: pathlib.Path | None = None) -> str:
    return subprocess.check_output(list(map(str, args)), cwd=cwd, text=True)


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def require_sha(path: pathlib.Path, expected: str) -> None:
    actual = sha256(path)
    if actual != expected:
        die(f"SHA256 mismatch for {path}: got {actual}, expected {expected}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path,
                        help="new output root (default: build-et/aac-full/scalar-libs-YYYY-MM-DD)")
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument("--offline", action="store_true",
                        help="reuse only SHA-pinned local source archives; never download")
    args = parser.parse_args()
    if os.environ.get("FF_ET_ALLOW_PCIE") not in (None, "0"):
        die("FF_ET_ALLOW_PCIE must be 0; this build never accesses a device")
    if not pathlib.Path(TOOL + "gcc").is_file():
        die("run inside et-tools/et-env: missing " + TOOL + "gcc")

    output = (args.output or ROOT / "build-et/aac-full" /
              ("scalar-libs-" + dt.date.today().isoformat())).resolve()
    if output.exists():
        die("refusing existing output root: " + str(output))
    output.mkdir(parents=True)
    logs = output / "logs"
    sources = output / "sources"
    patches = output / "patches"
    build = output / "build"
    lib = output / "lib"
    for d in (logs, sources, patches, build, lib):
        d.mkdir()

    # Record the required local-source search before using the official pins.
    # Installed libraries/headers are intentionally not treated as source.
    local_search = logs / "LOCAL_SDK_SOURCE_SEARCH.txt"
    names = ("newlib*.tar*", "newlib*.zip", "gcc-*.tar*", "compiler-rt*.tar*",
             "llvm-project*.tar*", "llvm*.tar*")
    find_cmd = ["find", "/opt/et", "/src", "/work", "-type", "f", "("]
    for i, name in enumerate(names):
        if i:
            find_cmd.append("-o")
        find_cmd += ["-iname", name]
    find_cmd += [")", "-print"]
    with local_search.open("w") as f:
        f.write("Local source-archive search run before network acquisition:\n")
        f.write(" ".join(find_cmd) + "\n\n")
        subprocess.run(find_cmd, stdout=f, stderr=subprocess.STDOUT, text=True, check=True)
    local_candidates = [pathlib.Path(x) for x in local_search.read_text().splitlines()
                        if x.startswith("/")]

    # A matching local archive is preferable to downloading.  Only an exact
    # pinned SHA-256 is reused; any other local SDK archive is merely recorded.
    def reuse_local(destination: pathlib.Path, expected: str) -> bool:
        for candidate in local_candidates:
            try:
                if candidate.is_file() and sha256(candidate) == expected:
                    print("+ reuse local pinned source", candidate, flush=True)
                    shutil.copy2(candidate, destination)
                    return True
            except OSError:
                pass
        return False

    # Official Sourceware newlib release tag, pinned to its peeled commit.
    newlib_archive = sources / "newlib-4.5.0.tar.gz"
    newlib = build / "newlib-src"
    if reuse_local(newlib_archive, NEWLIB_ARCHIVE_SHA256):
        newlib.mkdir()
        run("tar", "-xzf", newlib_archive, "-C", newlib)
    else:
        if args.offline:
            die("--offline requested but cached pinned newlib source was unavailable")
        run("git", "init", "-q", newlib)
        run("git", "-C", newlib, "fetch", "--depth=1", NEWLIB_URL, NEWLIB_COMMIT)
        run("git", "-C", newlib, "checkout", "-q", "--detach", "FETCH_HEAD")
        if capture("git", "-C", newlib, "rev-parse", "HEAD").strip() != NEWLIB_COMMIT:
            die("newlib checkout did not resolve to the pinned commit")
        with newlib_archive.open("wb") as f:
            archive = subprocess.Popen(["git", "-C", str(newlib), "archive", "--format=tar", NEWLIB_COMMIT],
                                       stdout=subprocess.PIPE)
            gzip = subprocess.Popen(["gzip", "-n"], stdin=archive.stdout, stdout=f)
            assert archive.stdout is not None
            archive.stdout.close()
            if gzip.wait() or archive.wait():
                die("could not create pinned newlib source archive")
    require_sha(newlib_archive, NEWLIB_ARCHIVE_SHA256)

    # Official LLVM GitHub release-tag archive.  tar.gz is deliberate: the SDK
    # image has no xz program, and this keeps extraction self-contained.
    llvm_archive = sources / "llvm-project-llvmorg-20.1.8.tar.gz"
    if not reuse_local(llvm_archive, LLVM_ARCHIVE_SHA256):
        if args.offline:
            die("--offline requested but cached pinned compiler-rt source was unavailable")
        print("+ download", LLVM_URL, flush=True)
        urllib.request.urlretrieve(LLVM_URL, llvm_archive)
    require_sha(llvm_archive, LLVM_ARCHIVE_SHA256)
    llvm = build / "llvm-project"
    llvm.mkdir()
    run("tar", "-xzf", llvm_archive, "-C", llvm, "--strip-components=1")
    if not (llvm / "compiler-rt/lib/builtins/CMakeLists.txt").is_file():
        die("pinned LLVM archive did not contain compiler-rt builtins")

    # Newlib's RISC-V libm directory substitutes asm fcvt implementations for
    # lrint[f]/llrint[f] (and lround variants).  Omit that *machine libm*
    # directory at configure time so upstream portable libm/common sources are
    # selected instead.  The generic sources use bit extraction and volatile
    # rounding-mode addition.  Their only remaining C conversion is the
    # nonrepresentable-result fallback, whose result is unspecified by C; use
    # deterministic integer saturation there so GCC cannot emit trapped fcvt.
    # cephes_subrf's round-to-float operation is expressed with newlib roundf.
    # This is a narrowly-scoped generated-source patch; libc/machine/riscv
    # remains selected and the downloaded source archive is never modified.
    patch_diffs: list[str] = []

    def patch_once(relative: str, old: str, new: str) -> None:
        path = newlib / relative
        before = path.read_text()
        if before.count(old) != 1:
            die("could not uniquely patch " + relative)
        after = before.replace(old, new)
        path.write_text(after)
        patch_diffs.extend(difflib.unified_diff(
            before.splitlines(keepends=True), after.splitlines(keepends=True),
            fromfile="newlib-4.5.0/" + relative, tofile="newlib-4.5.0/" + relative))

    patch_once(
        "newlib/configure.host",
        "  riscv*)\n\tlibm_machine_dir=riscv\n\tmachine_dir=riscv\n",
        ("  riscv*)\n"
         "\t# ET scalar build: use upstream portable libm/common, not RISC-V asm.\n"
         "\tlibm_machine_dir=\n\t# machine libc remains RISC-V.\n\tmachine_dir=riscv\n"))
    patch_once(
        "newlib/libm/common/sf_lrint.c",
        "#include \"fdlibm.h\"\n",
        "#include \"fdlibm.h\"\n#include <limits.h>\n")
    patch_once(
        "newlib/libm/common/sf_lrint.c",
        "  else\n    {\n      return (long int) x;\n    }\n",
        "  else\n    {\n      /* Nonrepresentable lrintf result is unspecified; avoid RISC-V fcvt. */\n"
        "      return sx ? LONG_MIN : LONG_MAX;\n    }\n")
    patch_once(
        "newlib/libm/common/sf_llrint.c",
        "#include \"fdlibm.h\"\n",
        "#include \"fdlibm.h\"\n#include <limits.h>\n")
    patch_once(
        "newlib/libm/common/sf_llrint.c",
        "  else\n    {\n      return (long long int) x;\n    }\n",
        "  else\n    {\n      /* Nonrepresentable llrintf result is unspecified; avoid RISC-V fcvt. */\n"
        "      return sx ? LLONG_MIN : LLONG_MAX;\n    }\n")
    patch_once(
        "newlib/libm/common/sf_lround.c",
        "#include \"fdlibm.h\"\n",
        "#include \"fdlibm.h\"\n#include <limits.h>\n")
    patch_once(
        "newlib/libm/common/sf_lround.c",
        "  else\n      return (long int) x;\n",
        "  else\n      /* Nonrepresentable lroundf result is unspecified; avoid RISC-V fcvt. */\n"
        "      return sign < 0 ? LONG_MIN : LONG_MAX;\n")
    patch_once(
        "newlib/libm/common/sf_llround.c",
        "#include \"fdlibm.h\"\n",
        "#include \"fdlibm.h\"\n#include <limits.h>\n")
    patch_once(
        "newlib/libm/common/sf_llround.c",
        "  else\n      return (long long int) x;\n",
        "  else\n      /* Nonrepresentable llroundf result is unspecified; avoid RISC-V fcvt. */\n"
        "      return sign < 0 ? LLONG_MIN : LLONG_MAX;\n")
    patch_once(
        "newlib/libm/complex/cephes_subrf.c",
        "\tlong i;\n",
        "")
    patch_once(
        "newlib/libm/complex/cephes_subrf.c",
        "\ti = t;\t/* the multiple */\n\tt = i;\n",
        "\tt = roundf(t);\t/* nearest integer multiple, ties away from zero */\n")
    patch_path = patches / "newlib-riscv-portable-libm.patch"
    patch_path.write_text("".join(patch_diffs))
    patch_sha = sha256(patch_path)

    provenance = output / "PROVENANCE.txt"
    provenance.write_text(
        "Private scalar library build; no device, simulator, or hardware was opened or launched.\n\n"
        f"target: {TARGET}, rv64imf/lp64f\n"
        f"target CFLAGS: {TARGET_CFLAGS}\n"
        "instruction policy: no compressed/RVV/D-Q-packed/fdiv.s/fsqrt.s/"
        "fcvt.l.s/fcvt.lu.s/fcvt.s.l/fcvt.s.lu; FMA is rejected when selected "
        "into the final ELF.\n"
        "newlib libm selection: RISC-V machine libm disabled; portable libm/common "
        "selected (lrintf/llrintf retain their volatile rounding-mode addition).\n"
        f"generated patch: patches/{patch_path.name}\n  SHA256: {patch_sha}\n\n"
        "newlib:\n"
        f"  official URL: {NEWLIB_URL}\n  release tag: {NEWLIB_TAG}\n"
        f"  peeled commit: {NEWLIB_COMMIT}\n"
        f"  archived source: {newlib_archive.name}\n"
        f"  SHA256: {NEWLIB_ARCHIVE_SHA256}\n\n"
        "compiler-rt:\n"
        f"  official URL: {LLVM_URL}\n  release tag: {LLVM_TAG}\n"
        f"  tag commit: {LLVM_COMMIT}\n"
        f"  downloaded source: {llvm_archive.name}\n"
        f"  SHA256: {LLVM_ARCHIVE_SHA256}\n"
    )

    # Full newlib libc and libm.  `CFLAGS_FOR_TARGET` propagates into the
    # target subconfigure/build, unlike host CFLAGS.
    newlib_build = build / "newlib-build"
    newlib_stage = build / "newlib-stage"
    newlib_build.mkdir()
    with (logs / "newlib-configure.log").open("w") as f:
        run(newlib / "configure", f"--target={TARGET}", f"--prefix={newlib_stage}",
            "--disable-multilib", "--disable-newlib-supplied-syscalls", "--disable-nls",
            "--enable-newlib-io-c99-formats", f"CFLAGS_FOR_TARGET={TARGET_CFLAGS}",
            cwd=newlib_build, stdout=f)
    with (logs / "newlib-make.log").open("w") as f:
        run("make", f"-j{args.jobs}", "all-target-newlib", cwd=newlib_build, stdout=f)
    with (logs / "newlib-install.log").open("w") as f:
        run("make", "install-target-newlib", cwd=newlib_build, stdout=f)
    newlib_lib = newlib_stage / TARGET / "lib"
    for name in ("libc.a", "libm.a"):
        if not (newlib_lib / name).is_file():
            die("newlib did not produce " + name)
        shutil.copy2(newlib_lib / name, lib / name)

    # Generic compiler-rt implementations are real arithmetic helpers (not
    # stubs) and need no target libc.  Name the archive libgcc.a so a parent
    # final link that uses -lgcc resolves it before the installed SDK libgcc.
    crt_build = build / "compiler-rt-builtins"
    crt_build.mkdir()
    cmake = [
        "cmake", "-G", "Unix Makefiles", llvm / "compiler-rt/lib/builtins",
        "-DCMAKE_SYSTEM_NAME=Generic", "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
        f"-DCMAKE_C_COMPILER={TOOL}gcc", f"-DCMAKE_ASM_COMPILER={TOOL}gcc",
        f"-DCMAKE_AR={TOOL}ar", f"-DCMAKE_RANLIB={TOOL}ranlib",
        f"-DCMAKE_C_FLAGS={CRT_CFLAGS}",
        "-DCMAKE_ASM_FLAGS=-march=rv64imf -mabi=lp64f -mcmodel=medany",
        "-DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON", f"-DCMAKE_C_COMPILER_TARGET={TARGET}",
        "-DCOMPILER_RT_BUILTINS_ENABLE_PIC=OFF", "-DCOMPILER_RT_BAREMETAL_BUILD=ON",
        "-DCOMPILER_RT_EXCLUDE_ATOMIC_BUILTIN=ON",
    ]
    with (logs / "compiler-rt-configure.log").open("w") as f:
        run(*cmake, cwd=crt_build, stdout=f)
    with (logs / "compiler-rt-make.log").open("w") as f:
        run("make", f"-j{args.jobs}", cwd=crt_build, stdout=f)
    crt = crt_build / "lib/generic/libclang_rt.builtins-riscv64.a"
    if not crt.is_file():
        die("compiler-rt did not produce generic RV64 builtins")
    shutil.copy2(crt, lib / "libgcc.a")
    shutil.copy2(crt, lib / "libclang_rt.builtins-riscv64.a")

    # Archives must be uncompressed RV64IMF and contain neither the simulator-
    # trapping fdiv.s/fsqrt.s/fcvt.l.s/fcvt.lu.s/fcvt.s.l/fcvt.s.lu nor
    # D/Q/packed/RVV instructions.  Explicit fma()
    # implementations may contain fmadd.s in libm; final ELF --gc-sections must
    # prove none was selected.  The audit records (rather than hides) FMA.
    audit = output / "ARCHIVE_AUDIT.txt"
    with audit.open("w") as f:
        for archive in (lib / "libc.a", lib / "libm.a", lib / "libgcc.a"):
            dis = capture(TOOL + "objdump", "-d", archive)
            compressed = []
            rvv = []
            forbidden_fp = []
            fdiv = []
            fsqrt = []
            mcode_conversions = {op: [] for op in MCODE_CONVERSIONS}
            fma = []
            for line in dis.splitlines():
                match = re.match(r"^\s*[0-9a-f]+:\s+([0-9a-f]+)\s+(\S+)", line)
                if not match:
                    continue
                bits, op = match.groups()
                if len(bits) != 8 or op.startswith("c."):
                    compressed.append(line)
                if op.startswith("v."):
                    rvv.append(line)
                if op.endswith((".d", ".q", ".ps", ".pi")):
                    forbidden_fp.append(line)
                if op == "fdiv.s":
                    fdiv.append(line)
                if op == "fsqrt.s":
                    fsqrt.append(line)
                if op in mcode_conversions:
                    mcode_conversions[op].append(line)
                if op.startswith(("fmadd", "fmsub", "fnmadd", "fnmsub")):
                    fma.append(line)
            conversion_counts = " ".join(f"{op}={len(mcode_conversions[op])}"
                                         for op in MCODE_CONVERSIONS)
            f.write(f"{archive.name}: compressed={len(compressed)} rvv={len(rvv)} "
                    f"D/Q/packed={len(forbidden_fp)} fdiv.s={len(fdiv)} "
                    f"fsqrt.s={len(fsqrt)} {conversion_counts} fma={len(fma)}\n")
            if fma:
                f.write("  FMA is present only in archive members; final ELF must be checked after GC.\n")
            if compressed or rvv or forbidden_fp or fdiv or fsqrt or any(mcode_conversions.values()):
                sample = (compressed + rvv + forbidden_fp + fdiv + fsqrt +
                          [line for lines in mcode_conversions.values() for line in lines])[0]
                die(f"scalar archive instruction audit failed for {archive}: {sample}")

    # The source archives and complete logs are sufficient to reproduce this
    # build.  Do not retain multi-gigabyte unpacked source/intermediate trees.
    shutil.rmtree(build)
    sums = output / "SHA256SUMS"
    with sums.open("w") as f:
        for path in sorted([*sources.iterdir(), *patches.iterdir(), *lib.iterdir(), *logs.iterdir(), provenance, audit]):
            if path.is_file():
                f.write(f"{sha256(path)}  {path.relative_to(output)}\n")
    print(output)


if __name__ == "__main__":
    main()
