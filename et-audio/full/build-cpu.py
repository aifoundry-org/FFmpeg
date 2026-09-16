#!/usr/bin/env python3
"""Build the native full compressed-packet AAC benchmark and its fixtures.

This is compilation/correctness only.  It never invokes ET hardware, runtime,
or a timed multicore command.  Timed runs are deliberately left to the parent
owner, e.g. taskset -c 0 build-et/aac-full/cpu/aac-full-cpu INPUT 5 scalar.
"""
import argparse
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_FF = ROOT / "build-et/aac-prototype/cpu"
DEFAULT_OUT = ROOT / "build-et/aac-full"


def run(cmd, **kwargs):
    print("+", " ".join(map(str, cmd)), flush=True)
    subprocess.run(cmd, check=True, **kwargs)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ffmpeg-build", type=pathlib.Path, default=DEFAULT_FF,
                    help="pinned optimized CPU FFmpeg static build")
    ap.add_argument("--output-root", type=pathlib.Path, default=DEFAULT_OUT)
    ap.add_argument("--skip-tests", action="store_true")
    args = ap.parse_args()
    ff = args.ffmpeg_build.resolve(); out = args.output_root.resolve()
    codec = ff / "libavcodec/libavcodec.a"; util = ff / "libavutil/libavutil.a"
    if not codec.is_file() or not util.is_file() or not (ff / "config.h").is_file():
        ap.error(f"missing pinned CPU FFmpeg archives/config under {ff}")
    cpu = out / "cpu"; cpu.mkdir(parents=True, exist_ok=True)
    results = out / "cpu-results"; results.mkdir(parents=True, exist_ok=True)
    (results / "README.txt").write_text(
        "Parent-owned timed command (after hardware coordination only):\n"
        "  taskset -c 0 ../cpu/aac-full-cpu ../fixtures/stereo-48000-512.input 5 scalar\n\n"
        "This measures complete compressed-packet AAC decode plus packet-major PCM stores. "
        "It is not stage-equivalent to the retained synthesis-only cpu-stage benchmark. "
        "scalar (-cpuflags 0 equivalent) is the FF CPU oracle; optimized/default output and "
        "timings characterize dispatch only and are not an ET tolerance. Capture JSON stdout here.\n")
    binary = cpu / "aac-full-cpu"
    cc = os.environ.get("CC", "cc")
    # libavcodec/libavutil are intentionally the existing optimized baseline archives;
    # scalar is selected at runtime using av_force_cpu_flags(0), not a metadata frontend.
    run([cc, "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
         "-I", str(ff), "-I", str(ROOT), str(ROOT / "et-audio/full/cpu.c"),
         str(codec), str(util), "-lm", "-lz", "-pthread", "-o", str(binary)])
    run([sys.executable, str(ROOT / "et-audio/full/fixtures.py"), "--output", str(out / "fixtures"), "--cpu", str(binary)])
    if not args.skip_tests:
        run([sys.executable, str(ROOT / "et-audio/full/fixture-tests.py"), "--cpu", str(binary), "--fixtures", str(out / "fixtures"),
             "--native-output", str(out / "native-fixture-tests")])
    print("Built", binary)
    print("CLI: aac-full-cpu INPUT [OUTPUT_PCM] REPEATS [scalar|optimized]")
    print("Timed benchmark (parent-owned only): taskset -c 0", binary, out / "fixtures/stereo-48000-512.input", "5", "scalar")


if __name__ == "__main__":
    main()
