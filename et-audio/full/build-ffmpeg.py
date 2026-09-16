#!/usr/bin/env python3
"""Privately rebuild the pinned r6 FFmpeg AAC archives with no FP divide/sqrt.

Run only in the SDK container and only with PCIe disabled:
  FF_ET_ALLOW_PCIE=0 et-tools/et-env env FF_ET_ALLOW_PCIE=0 \\
    python3 et-audio/full/build-ffmpeg.py

This never edits the retained r6 tree.  It copies its complete configured
``source`` tree to a fresh private output and changes only the copied
ffbuild/config.mak before forcing libavcodec.a and libavutil.a to rebuild.
"""
import argparse
import datetime as dt
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
R6 = ROOT / "build-et/aac-decoder/rv64imf-lp64f-20260916-r6"
INPUT_SOURCE = R6 / "source"
TOOL_PREFIX = "/opt/et/bin/riscv64-unknown-elf-"
ARCHIVES = ("libavcodec/libavcodec.a", "libavutil/libavutil.a")
CFLAGS_APPEND = "CFLAGS += -mno-fdiv\n"


def die(message):
    raise SystemExit(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run_logged(command, log, cwd=None):
    rendered = " ".join(str(part) for part in command)
    print("+", rendered, flush=True)
    log.write("+ " + rendered + "\n")
    log.flush()
    subprocess.run([str(part) for part in command], cwd=str(cwd) if cwd else None,
                   stdout=log, stderr=subprocess.STDOUT, check=True)


def capture(command, cwd=None):
    return subprocess.check_output([str(part) for part in command],
                                   cwd=str(cwd) if cwd else None,
                                   text=True, stderr=subprocess.STDOUT)


def write_hashes(path, named_paths):
    with path.open("w") as handle:
        for name, candidate in named_paths:
            handle.write("{}  {}\n".format(sha256(candidate), name))


def comparable_files(root):
    """Files which must remain byte-identical apart from config.mak.

    Object files, dependency files, and the two rebuilt archives are configured
    build products.  Everything else in the copied configured source tree is
    source/configuration material and is compared after the forced rebuild.
    """
    ignored_suffixes = (".o", ".a", ".d")
    files = {}
    for candidate in root.rglob("*"):
        if not candidate.is_file() or candidate.is_symlink():
            continue
        relative = candidate.relative_to(root)
        if candidate.suffix in ignored_suffixes:
            continue
        files[relative.as_posix()] = candidate
    return files


def verify_private_diff(private_source, report):
    original = comparable_files(INPUT_SOURCE)
    private = comparable_files(private_source)
    all_names = sorted(set(original) | set(private))
    allowed = "ffbuild/config.mak"
    unexpected = []
    for name in all_names:
        if name == allowed:
            continue
        left = original.get(name)
        right = private.get(name)
        if left is None or right is None or sha256(left) != sha256(right):
            unexpected.append(name)
    config = private_source / allowed
    config_text = config.read_text()
    input_config = (INPUT_SOURCE / allowed).read_text()
    expected_config = input_config + ("" if input_config.endswith("\n") else "\n") + CFLAGS_APPEND
    if config_text != expected_config:
        unexpected.append(allowed + " (not exactly original plus CFLAGS += -mno-fdiv)")
    with report.open("w") as handle:
        handle.write("Compared copied source/configuration files after rebuilding.\n")
        handle.write("Allowed source/configuration difference: {}\n".format(allowed))
        handle.write("Unexpected differences: {}\n".format(len(unexpected)))
        for name in unexpected:
            handle.write(name + "\n")
    if unexpected:
        die("private source/configuration verification failed; see " + str(report))


def instruction_hits(objdump, artifact):
    disassembly = capture([objdump, "-d", artifact])
    return [line for line in disassembly.splitlines()
            if re.search(r"\b(?:fdiv|fsqrt)\.s\b", line)]


def main():
    parser = argparse.ArgumentParser(
        description="Copy pinned r6 and rebuild private FFmpeg AAC archives with -mno-fdiv.")
    parser.add_argument("--output", type=pathlib.Path,
                        help="fresh output (default: build-et/aac-full/ffmpeg-no-fdiv)")
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    args = parser.parse_args()

    if os.environ.get("FF_ET_ALLOW_PCIE") not in (None, "0"):
        die("FF_ET_ALLOW_PCIE must be 0; this build never accesses a device")
    if not INPUT_SOURCE.is_dir():
        die("missing retained pinned r6 source: " + str(INPUT_SOURCE))
    for relative in ARCHIVES:
        if not (INPUT_SOURCE / relative).is_file():
            die("missing retained pinned r6 archive: " + str(INPUT_SOURCE / relative))
    gcc = pathlib.Path(TOOL_PREFIX + "gcc")
    objdump = pathlib.Path(TOOL_PREFIX + "objdump")
    nm = pathlib.Path(TOOL_PREFIX + "nm")
    ld = pathlib.Path(TOOL_PREFIX + "ld")
    for tool in (gcc, objdump, nm, ld):
        if not tool.is_file():
            die("run inside et-tools/et-env: missing " + str(tool))
    if shutil.which("make") is None:
        die("run inside et-tools/et-env: missing make")

    output = (args.output or ROOT / "build-et/aac-full/ffmpeg-no-fdiv").resolve()
    if output.exists():
        die("refusing existing output root: " + str(output))
    output.mkdir(parents=True)
    logs = output / "logs"
    audit = output / "audit"
    logs.mkdir()
    audit.mkdir()
    private_source = output / "source"

    # This is a private, metadata-preserving copy of the complete configured
    # r6 source/build tree.  The original remains untouched.
    input_archive_hashes = [(relative, sha256(INPUT_SOURCE / relative))
                            for relative in ARCHIVES]
    shutil.copytree(INPUT_SOURCE, private_source, symlinks=True, copy_function=shutil.copy2)
    (output / "INPUT_ARCHIVE_SHA256SUMS.txt").write_text("".join(
        "{}  r6/source/{}\n".format(digest, relative)
        for relative, digest in input_archive_hashes))
    shutil.copy2(R6 / "SOURCE.txt", output / "INPUT_SOURCE.txt")
    shutil.copy2(R6 / "RESULT", output / "INPUT_RESULT.txt")

    # `make -B` regenerates ffversion.h.  Pin its pre-existing configured
    # revision explicitly: otherwise version.sh can discover the enclosing
    # workspace Git checkout and silently change a copied source header.
    input_version_header = INPUT_SOURCE / "libavutil/ffversion.h"
    version_match = re.search(r'^#define FFMPEG_VERSION "([^"]+)"$',
                              input_version_header.read_text(), re.MULTILINE)
    if not version_match:
        die("could not read pinned configured FFMPEG_VERSION from " +
            str(input_version_header))
    configured_revision = version_match.group(1)

    config = private_source / "ffbuild/config.mak"
    original_config = config.read_text()
    if original_config.endswith("\n"):
        config.write_text(original_config + CFLAGS_APPEND)
    else:
        config.write_text(original_config + "\n" + CFLAGS_APPEND)
    if config.read_text() != original_config + ("" if original_config.endswith("\n") else "\n") + CFLAGS_APPEND:
        die("could not append private -mno-fdiv CFLAGS setting")
    shutil.copy2(config, output / "PRIVATE_CONFIG.mak")

    (logs / "environment.txt").write_text(
        "FF_ET_ALLOW_PCIE={}\nPATH={}\nPWD={}\n".format(
            os.environ.get("FF_ET_ALLOW_PCIE", "<unset>"),
            os.environ.get("PATH", ""), os.getcwd()))
    with (logs / "toolchain.txt").open("w") as handle:
        for command in ([gcc, "--version"], [objdump, "--version"], [nm, "--version"],
                        [ld, "--version"], ["make", "--version"]):
            handle.write("+ " + " ".join(map(str, command)) + "\n")
            handle.write(capture(command))
            handle.write("\n")

    # -B is deliberate: config.mak does not participate in every object
    # dependency graph, so both selected static libraries must be rebuilt.
    with (logs / "build-commands.log").open("w") as handle:
        run_logged(["make", "-B", "V=1", "REVISION={}".format(configured_revision),
                    "-j{}".format(args.jobs), "libavcodec/libavcodec.a",
                    "libavutil/libavutil.a"], handle, cwd=private_source)

    output_archives = [("source/" + relative, private_source / relative)
                       for relative in ARCHIVES]
    for _, archive in output_archives:
        if not archive.is_file():
            die("rebuild did not produce " + str(archive))
    write_hashes(output / "OUTPUT_ARCHIVE_SHA256SUMS.txt", output_archives)
    with (audit / "INPUT_ARCHIVE_POSTBUILD_VERIFICATION.txt").open("w") as handle:
        changed = []
        for relative, expected in input_archive_hashes:
            actual = sha256(INPUT_SOURCE / relative)
            status = "PASS" if actual == expected else "FAIL"
            handle.write("{}  r6/source/{}  expected={} actual={}\n".format(
                status, relative, expected, actual))
            if actual != expected:
                changed.append(relative)
    if changed:
        die("retained r6 input archive changed unexpectedly: " + ", ".join(changed))
    verify_private_diff(private_source, audit / "SOURCE_CONFIGURATION_DIFF.txt")

    # Audit the whole rebuilt archives and the AAC decoder closure selected by
    # the static linker.  The latter is the direct proof for the intended link.
    closure = audit / "aac-float-closure-no-fdiv.o"
    with (logs / "closure-command.log").open("w") as handle:
        run_logged([ld, "-r", "-u", "ff_aac_decoder", "-o", closure,
                    "--start-group", private_source / ARCHIVES[0],
                    private_source / ARCHIVES[1], "--end-group"], handle)
    with (audit / "FP_DIV_SQRT_AUDIT.txt").open("w") as handle:
        total = 0
        for label, artifact in output_archives + [("selected AAC closure", closure)]:
            hits = instruction_hits(objdump, artifact)
            total += len(hits)
            handle.write("{}: fdiv.s/fsqrt.s={}\n".format(label, len(hits)))
            for hit in hits:
                handle.write("  " + hit + "\n")
        handle.write("TOTAL: {}\n".format(total))
    if (audit / "FP_DIV_SQRT_AUDIT.txt").read_text().rstrip().endswith("TOTAL: 0") is False:
        die("fdiv.s/fsqrt.s remains in private archive audit; see " +
            str(audit / "FP_DIV_SQRT_AUDIT.txt"))

    undefined = capture([nm, "-u", private_source / ARCHIVES[0],
                         private_source / ARCHIVES[1]])
    divsf3 = [line for line in undefined.splitlines() if re.search(r"\b__divsf3$", line)]
    (audit / "UNRESOLVED_COMPILER_HELPERS.txt").write_text(
        "Unresolved archive symbols matching __divsf3 (expected before the real compiler-rt final link):\n" +
        ("\n".join(divsf3) if divsf3 else "<none>") + "\n\nAll undefined symbols:\n" + undefined)
    if not divsf3:
        die("expected unresolved __divsf3 from -mno-fdiv rebuild; see " +
            str(audit / "UNRESOLVED_COMPILER_HELPERS.txt"))

    (output / "RESULT").write_text(
        "PASS: private copied r6 libavcodec.a/libavutil.a rebuilt with CFLAGS += -mno-fdiv\n"
        "No fdiv.s or fsqrt.s occurred in either rebuilt archive or the selected AAC closure.\n"
        "__divsf3 remains unresolved in the static archives as expected until final compiler-rt link.\n"
        "No device or simulator was accessed.  Original r6 source/output was not edited.\n")
    # Hash all handoff records and output archives, after every audit succeeds.
    handoff = [path for path in sorted(output.rglob("*"))
               if path.is_file() and path.name != "SHA256SUMS"]
    write_hashes(output / "SHA256SUMS", [(path.relative_to(output).as_posix(), path)
                                          for path in handoff])
    print("PASS:", output)


if __name__ == "__main__":
    main()
