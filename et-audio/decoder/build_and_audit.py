#!/usr/bin/env python3
"""Private n7.1.1 AAC-float compile-only feasibility build for ETSOC-1.

The script archives the annotated FFmpeg n7.1.1 tag into a new output tree,
cross-compiles only libavcodec/libavutil static archives, and writes an audit.
It never opens an ET runtime or device node and never modifies upstream source.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

PINNED_TAG = "n7.1.1"
PINNED_COMMIT = "db69d06eeeab4f46da15030a80d539efb4503ca8"
ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "build-et/aac-decoder/rv64imf-lp64f-20260916-r6"

CONFIGURE_ARGS = [
    "--enable-cross-compile", "--target-os=none", "--arch=riscv64", "--cpu=generic",
    "--cc=riscv64-unknown-elf-gcc", "--ar=riscv64-unknown-elf-ar",
    "--ranlib=riscv64-unknown-elf-ranlib", "--nm=riscv64-unknown-elf-nm",
    "--disable-autodetect", "--disable-asm", "--disable-runtime-cpudetect",
    "--disable-programs", "--disable-doc", "--disable-network", "--disable-pthreads",
    "--disable-w32threads", "--disable-os2threads", "--disable-avdevice", "--disable-avfilter",
    "--disable-avformat", "--disable-swresample", "--disable-swscale", "--disable-postproc",
    "--disable-everything", "--enable-decoder=aac", "--enable-small", "--disable-debug",
    "--disable-stripping", "--disable-symver",
]
CFLAGS = " ".join((
    "-O2", "-march=rv64imf", "-mabi=lp64f", "-mcmodel=medany", "-ffreestanding",
    "-fno-builtin", "-fno-stack-protector", "-fno-zero-initialized-in-bss",
    "-fno-strict-aliasing", "-ffp-contract=off", "-mno-riscv-attribute",
    # SDK newlib suppresses __int64_t_defined under -ffreestanding, hiding
    # its otherwise available PRId64 macros. This exposes the verified LP64
    # type declaration only; it provides no implementation or successful stub.
    "-D__int64_t_defined=1",
))


def run(cmd: list[str], *, cwd: Path | None, log: Path, env: dict[str, str] | None = None) -> None:
    with log.open("a", encoding="utf-8") as out:
        out.write("\n+ " + " ".join(cmd) + "\n")
        out.flush()
        result = subprocess.run(cmd, cwd=cwd, env=env, stdout=out,
                                stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(cmd)}; see {log}")


def capture(cmd: list[str], *, cwd: Path | None = None) -> str:
    return subprocess.check_output(cmd, cwd=cwd, text=True, stderr=subprocess.STDOUT)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def nm_symbols(text: str, undefined: bool) -> set[str]:
    result: set[str] = set()
    for line in text.splitlines():
        fields = line.split()
        if undefined:
            if len(fields) >= 2 and fields[-2] == "U":
                result.add(fields[-1])
        elif fields:
            result.add(fields[-1])
    return result


def write(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--jobs", type=int, default=max(1, min(8, os.cpu_count() or 1)))
    parser.add_argument("--audit-existing", type=Path, metavar="OUTPUT_ROOT",
                        help="audit an already compiled private output without changing its logs, source, or archives")
    args = parser.parse_args()
    reuse = args.audit_existing is not None
    output = (args.audit_existing if reuse else args.output_root).resolve()
    # et-tools/et-env consumes this host-side switch to decide device exposure but does not
    # forward it by default. Its absence inside the container is therefore the safe default.
    # If explicitly forwarded, only 0 is allowed; this script never needs PCIe.
    pcie_opt_in = os.environ.get("FF_ET_ALLOW_PCIE")
    if pcie_opt_in not in (None, "", "0"):
        raise SystemExit("refusing: FF_ET_ALLOW_PCIE is an explicit PCIe opt-in; omit it or set it to 0")
    if reuse:
        if not output.is_dir():
            raise SystemExit(f"existing output root is absent: {output}")
    elif output.exists():
        raise SystemExit(f"refusing to overwrite existing evidence/build root: {output}")
    if not (ROOT / ".git").exists():
        raise SystemExit(f"not a git checkout: {ROOT}")

    if not reuse:
        output.mkdir(parents=True)
    logs = output / "logs"
    logs.mkdir(exist_ok=reuse)
    command_log = logs / ("audit-rerun.log" if reuse else "commands.log")
    if command_log.exists():
        raise SystemExit(f"refusing to overwrite existing log: {command_log}")
    write(command_log, "AAC float decoder compile-only feasibility audit rerun\n" if reuse else
                       "AAC float decoder compile-only feasibility build\n")
    result_path = output / ("RESULT-AUDIT-RERUN" if reuse else "RESULT")
    try:
        source = output / "source"
        source_tar = output / "ffmpeg-n7.1.1.tar"
        if reuse:
            if not (source / "configure").is_file() or not source_tar.is_file():
                raise RuntimeError("existing output lacks the required private source archive/tree")
            with command_log.open("a", encoding="utf-8") as out:
                out.write("+ audit existing private static archives; no configure, make, link, or execution\n")
        else:
            commit = capture(["git", "rev-parse", f"{PINNED_TAG}^{{commit}}"], cwd=ROOT).strip()
            if commit != PINNED_COMMIT:
                raise RuntimeError(f"pinned tag mismatch: {commit}, expected {PINNED_COMMIT}")
            source.mkdir()
            with command_log.open("a", encoding="utf-8") as out:
                out.write(f"\n+ git archive --format=tar {PINNED_TAG} > {source_tar}\n")
            with source_tar.open("wb") as tar:
                result = subprocess.run(["git", "archive", "--format=tar", PINNED_TAG], cwd=ROOT, stdout=tar)
            if result.returncode:
                raise RuntimeError("git archive failed")
            run(["tar", "-xf", str(source_tar), "-C", str(source)], cwd=None, log=command_log)
            if not (source / "configure").is_file():
                raise RuntimeError("tag archive did not contain configure")
            write(output / "SOURCE.txt", "\n".join((
                f"tag={PINNED_TAG}", f"commit={commit}", f"tar_sha256={sha256(source_tar)}",
                "source_method=git archive of pinned annotated tag", "upstream_tree_is_not_edited",
                f"utc_started={dt.datetime.now(dt.timezone.utc).isoformat()}", "",
            )))

            env = os.environ.copy()
            env.update({"CC": "riscv64-unknown-elf-gcc", "CFLAGS": CFLAGS, "LDFLAGS": ""})
            configure = ["./configure", *CONFIGURE_ARGS, f"--extra-cflags={CFLAGS}"]
            run(configure, cwd=source, log=logs / "configure.log", env=env)
            run(["make", f"-j{args.jobs}", "V=1", "libavutil/libavutil.a", "libavcodec/libavcodec.a"],
                cwd=source, log=logs / "build.log", env=env)

        avcodec = source / "libavcodec/libavcodec.a"
        avutil = source / "libavutil/libavutil.a"
        # ar records basenames. n7.1.1 compiles the float DSP template through aacdec_float.o;
        # there is no standalone aacdec_dsp_float.o archive member.
        required_members = {"aacdec.o", "aacdec_float.o"}
        members = set(capture(["riscv64-unknown-elf-ar", "t", str(avcodec)]).splitlines())
        missing = sorted(required_members - members)
        if missing:
            raise RuntimeError(f"AAC float objects absent from libavcodec.a: {', '.join(missing)}")
        config = (source / "config.h").read_text(encoding="utf-8")
        components = (source / "config_components.h").read_text(encoding="utf-8")
        for define, generated in (("#define CONFIG_AAC_DECODER 1", components),
                                  ("#define CONFIG_AAC_FIXED_DECODER 0", components),
                                  ("#define CONFIG_NETWORK 0", config),
                                  ("#define HAVE_PTHREADS 0", config)):
            if define not in generated:
                raise RuntimeError(f"unexpected configuration: missing {define}")
        if (source / "ffmpeg").exists() or (source / "ffprobe").exists():
            raise RuntimeError("program build artifact present despite --disable-programs")

        audit = output / ("audit-rerun" if reuse else "audit")
        audit.mkdir()
        configured_flags = (source / "ffbuild/config.mak").read_text(encoding="utf-8")
        for flag in ("-ffp-contract=off", "-fno-signed-zeros"):
            if flag not in configured_flags:
                raise RuntimeError(f"missing expected numerical compilation flag: {flag}")
        write(audit / "numerical-flags.txt", "\n".join((
            "-ffp-contract=off=present",
            "-fno-signed-zeros=present (added by FFmpeg configure)",
            "No target arithmetic/output comparison was performed; this is not an exactness claim.",
            "",
        )))
        for archive in (avutil, avcodec):
            name = archive.parent.name
            write(audit / f"{name}.members.txt", capture(["riscv64-unknown-elf-ar", "t", str(archive)]))
            write(audit / f"{name}.undefined.raw.txt",
                  capture(["riscv64-unknown-elf-nm", "-A", "-u", str(archive)]))
            write(audit / f"{name}.defined.raw.txt",
                  capture(["riscv64-unknown-elf-nm", "-A", "-g", "--defined-only", str(archive)]))
            write(audit / f"{name}.sections.txt",
                  capture(["riscv64-unknown-elf-objdump", "-h", str(archive)]))
            write(audit / f"{name}.disassembly.txt",
                  capture(["riscv64-unknown-elf-objdump", "-d", str(archive)]))

        undefined_raw = "".join((audit / f"{p}.undefined.raw.txt").read_text() for p in ("libavutil", "libavcodec"))
        defined_raw = "".join((audit / f"{p}.defined.raw.txt").read_text() for p in ("libavutil", "libavcodec"))
        archive_unresolved = sorted(nm_symbols(undefined_raw, True) - nm_symbols(defined_raw, False))
        write(audit / "archive-external-undefined.txt", "\n".join(archive_unresolved) + "\n")
        # This relocatable, no-startup partial link selects the static closure rooted at the float AAC decoder.
        # It is an audit artifact, not a runnable target executable and never receives libc/libm implementations.
        closure = audit / "aac-float-closure.o"
        run(["riscv64-unknown-elf-ld", "-r", "-u", "ff_aac_decoder", "-o", str(closure),
             "--start-group", str(avcodec), str(avutil), "--end-group"], cwd=None, log=command_log)
        unresolved = sorted(nm_symbols(capture(["riscv64-unknown-elf-nm", "-u", str(closure)]), True))
        write(audit / "external-undefined.txt", "\n".join(unresolved) + "\n")
        closure_size = capture(["riscv64-unknown-elf-size", "-A", str(closure)])
        closure_symbols = capture(["riscv64-unknown-elf-nm", "-S", "--size-sort", str(closure)])
        write(audit / "closure-size.txt", closure_size)
        write(audit / "closure-symbols-size.txt", closure_symbols)
        bss_bytes = sum(int(match) for match in re.findall(r"^\.(?:bss|sbss)\s+(\d+)\s", closure_size, re.MULTILINE))

        categories = {
            "heap": r"^(?:malloc|calloc|realloc|free|memalign|posix_memalign|aligned_alloc)$",
            "libm": r"^(?:acosf?|asin[f]?|atan2?f?|ceilf?|cosf?|expf?|floorf?|hypotf?|logf?|powf?|sinf?|sqrtf?|tanf?)$",
            "threads": r"^(?:pthread_|thrd_|mtx_|cnd_|sem_)",
            "double_softfloat": r"^__(?:add|sub|mul|div|fix|float|extend|trunc|cmp).*(?:df|tf)",
        }
        category_lines: list[str] = []
        for title, pattern in categories.items():
            found = [symbol for symbol in unresolved if re.search(pattern, symbol)]
            write(audit / f"external-{title}.txt", "\n".join(found) + ("\n" if found else ""))
            category_lines.append(f"{title}=" + (", ".join(found) if found else "none"))
        sections = "".join((audit / f"{p}.sections.txt").read_text() for p in ("libavutil", "libavcodec"))
        init_sections = sorted(set(re.findall(r"\s(\.(?:init|fini)_array\S*)\s", sections)))
        bss_sections = sorted(set(re.findall(r"\s(\.(?:bss|sbss|tbss|tdata)\S*)\s", sections)))
        disassembly = "".join((audit / f"{p}.disassembly.txt").read_text() for p in ("libavutil", "libavcodec"))
        # Examine only objdump instruction fields, never symbol/object names such as vlc.o.
        mnemonics = re.findall(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2,8}\s+)+([a-z][a-z0-9.]*)", disassembly, re.MULTILINE)
        forbidden = sorted({mnemonic for mnemonic in mnemonics
                            if mnemonic.startswith("c.") or mnemonic.startswith("v")})
        write(audit / "global-init.txt", "\n".join((
            "constructor_sections=" + (", ".join(init_sections) if init_sections else "none"),
            "tls_or_bss_sections=" + (", ".join(bss_sections) if bss_sections else "none"),
            "Note: C constant-initialized globals are represented by ordinary .data/.rodata; this detects runtime constructor/TLS sections.",
            "",
        )))
        write(audit / "isa-exceptions.txt", "\n".join(forbidden) + ("\n" if forbidden else ""))
        if forbidden:
            raise RuntimeError("forbidden compressed/RVV-looking instructions found; see audit/isa-exceptions.txt")

        source_hits: list[str] = []
        for rel in ("libavcodec/aac", "libavcodec/avcodec.c", "libavutil/mem.c", "libavutil/cpu.c"):
            candidate = source / rel
            if candidate.is_dir():
                command = ["grep", "-RInE", r"av_(malloc|calloc|realloc|free)|\b(malloc|calloc|realloc|free|powf|expf|logf|sinf|cosf|sqrtf)\b|AVOnce|ff_thread_once", str(candidate)]
            elif candidate.exists():
                command = ["grep", "-nE", r"av_(malloc|calloc|realloc|free)|\b(malloc|calloc|realloc|free|powf|expf|logf|sinf|cosf|sqrtf)\b|AVOnce|ff_thread_once", str(candidate)]
            else:
                continue
            result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            source_hits.extend(result.stdout.splitlines())
        write(audit / "source-dependency-hits.txt", "\n".join(source_hits) + ("\n" if source_hits else ""))

        hashes = []
        for artifact in (source_tar, avutil, avcodec):
            hashes.append(f"{sha256(artifact)}  {artifact.relative_to(output)}")
        write(output / ("SHA256SUMS-rerun" if reuse else "SHA256SUMS"), "\n".join(hashes) + "\n")
        write(audit / "SUMMARY.txt", "\n".join((
            "compile=PASS (static archives; relocatable ld -r dependency audit only, no final target executable link or execution)",
            "decoder=CONFIG_AAC_DECODER=1; fixed decoder disabled",
            "target=rv64imf/lp64f; asm/network/threads/programs disabled",
            "archive_members=required AAC float translation units present",
            "constructor_sections=" + (", ".join(init_sections) if init_sections else "none"),
            "tls_or_bss_sections=" + (", ".join(bss_sections) if bss_sections else "none"),
            f"closure_bss_plus_sbss_bytes={bss_bytes}",
            "forbidden_compressed_or_rvv_instructions=none",
            *category_lines,
            "external_undefined=see external-undefined.txt (relocatable closure rooted at ff_aac_decoder; no libc/libm/compiler helpers were linked)",
            "archive_external_undefined=see archive-external-undefined.txt (whole-archive union; intentionally more conservative)",
            "No functions were stubbed. No final target executable link/run, runtime open, emulator, PCIe, or device nodes were used.",
            "",
        )))
        write(result_path, "PASS: private pinned-source static archive compilation and audit completed\n" if not reuse else
              "PASS: audit rerun of preserved private pinned-source static archives completed\n")
        return 0
    except Exception as error:
        write(result_path, f"FAIL: {error}\n")
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
