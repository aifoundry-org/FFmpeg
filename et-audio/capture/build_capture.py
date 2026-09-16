#!/usr/bin/env python3
"""Create and build the private scalar AAC fixture-capture FFmpeg copy.

Run this only through: FF_ET_ALLOW_PCIE=0 et-tools/et-env python3 ...
It archives the current committed checkout, then applies exactly one patch to
that copy: libavcodec/aac/aacdec_dsp_template.c.  The repository is untouched.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[2]
PATCH = Path(__file__).resolve().with_name("aacdec_capture.patch")
PROTOTYPE_ROOT = (ROOT / "build-et/aac-prototype").resolve()
DEFAULT = PROTOTYPE_ROOT / "capture-src"


def command(args, **kwargs):
    print("+", " ".join(map(str, args)), flush=True)
    return subprocess.run(args, check=True, **kwargs)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def within_prototype_root(path: Path) -> bool:
    try:
        return os.path.commonpath([str(path), str(PROTOTYPE_ROOT)]) == str(PROTOTYPE_ROOT)
    except ValueError:
        return False


def archive_tree(destination: Path) -> str:
    if not within_prototype_root(destination) or destination == PROTOTYPE_ROOT:
        raise RuntimeError(f"output must be a fresh subdirectory of {PROTOTYPE_ROOT}: {destination}")
    tmp = destination.with_name(destination.name + ".new")
    # Build artifacts are evidence. Never clean, replace, or reuse either path.
    if destination.exists() or destination.is_symlink():
        raise RuntimeError(f"refusing existing output directory: {destination}")
    if tmp.exists() or tmp.is_symlink():
        raise RuntimeError(f"refusing existing interrupted-output directory: {tmp}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    tmp.mkdir()
    revision = subprocess.check_output(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True
    ).strip()
    archive = subprocess.Popen(
        ["git", "-C", str(ROOT), "archive", "--format=tar", revision], stdout=subprocess.PIPE
    )
    assert archive.stdout is not None
    with tarfile.open(fileobj=archive.stdout, mode="r|") as tf:
        # The archive comes from this local git checkout; Python 3.8 lacks
        # tarfile's later filter= argument.
        tf.extractall(tmp)
    if archive.wait() != 0:
        raise RuntimeError(f"git archive failed; preserving incomplete evidence at {tmp}")
    # destination was checked absent above; do not replace it if another process created it.
    if destination.exists() or destination.is_symlink():
        raise RuntimeError(f"output appeared during archive; preserving {tmp} and {destination}")
    tmp.rename(destination)
    return revision


def patch_capture(source: Path) -> None:
    target = source / "libavcodec/aac/aacdec_dsp_template.c"
    before = sha256(target)
    command(["patch", "--batch", "--forward", "-p1", "-i", str(PATCH)], cwd=source)
    after = sha256(target)
    if before == after:
        raise RuntimeError("capture patch did not modify the required template")
    # The patch intentionally names only this file; reject accidental scope expansion.
    text = PATCH.read_text(encoding="utf-8")
    changed = {line[6:] for line in text.splitlines() if line.startswith("+++ b/")}
    if changed != {"libavcodec/aac/aacdec_dsp_template.c"}:
        raise RuntimeError(f"capture patch scope is not exclusive: {sorted(changed)}")


def build(source: Path, jobs: str) -> None:
    gcc_version = subprocess.check_output(["gcc", "-dumpfullversion"], text=True).strip()
    if gcc_version.split(".", 1)[0] != "13":
        raise RuntimeError(f"native capture build requires GCC 13, got {gcc_version}")
    nasm = ROOT / "build-et/aac-prototype/tools/nasm"
    if not nasm.is_file() or not os.access(nasm, os.X_OK):
        raise RuntimeError(f"required local nasm is not executable: {nasm}")
    env = os.environ.copy()
    env["PATH"] = f"{nasm.parent}:{env['PATH']}"
    flags = [
        "--disable-autodetect", "--disable-doc", "--disable-everything", "--disable-etsoc",
        "--disable-network", "--enable-ffmpeg", "--enable-ffprobe", "--enable-debug=3",
        "--enable-protocol=file,pipe", "--enable-decoder=aac", "--enable-encoder=pcm_f32le",
        "--enable-parser=aac", "--enable-demuxer=aac", "--enable-filter=aresample",
        "--enable-muxer=wav,null",
        "--extra-cflags=-ffp-contract=off",
    ]
    command([str(source / "configure"), *flags], cwd=source, env=env)
    command(["make", "-j", jobs], cwd=source, env=env)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--output", type=Path, default=DEFAULT)
    p.add_argument("--jobs", default=os.environ.get("JOBS", "12"))
    p.add_argument("--archive-only", action="store_true")
    args = p.parse_args()
    if os.environ.get("FF_ET_ALLOW_PCIE", "0") != "0":
        raise SystemExit("refusing: capture build must use FF_ET_ALLOW_PCIE=0")
    output = args.output.resolve()
    revision = archive_tree(output)
    patch_capture(output)
    manifest = {
        "source_revision": revision,
        "private_source": str(output),
        "patched_only": "libavcodec/aac/aacdec_dsp_template.c",
        "patch_sha256": sha256(PATCH),
        "ffp_contract": "off",
        "scalar_oracle_runtime": "ffmpeg -cpuflags 0",
    }
    (output / "etaac-capture-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if not args.archive_only:
        build(output, args.jobs)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"build_capture.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
