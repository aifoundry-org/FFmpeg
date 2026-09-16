#!/usr/bin/env python3
"""Five-run, one-stream native CPU decode baseline for an AAC ADTS fixture.

Correctness files/hashes are produced before timing. Timing always decodes to
the null muxer, uses one AAC thread, and is explicitly pinned with taskset -c
0. The optimized arm has default CPU flags; the oracle arm passes -cpuflags 0.
Different float output is reported, never hidden or treated as a pass.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ROOT = ROOT / "build-et/aac-prototype"


def run(args, **kwargs):
    print("+", " ".join(map(str, args)), flush=True)
    return subprocess.run(args, check=True, **kwargs)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def require_exec(path: Path, name: str) -> None:
    if not path.is_file() or not os.access(path, os.X_OK):
        raise RuntimeError(f"{name} is not executable: {path}")


def extract_wave_f32le(wav: Path, output: Path) -> None:
    """Extract the PCM data chunk; the command below explicitly writes f32le."""
    raw = wav.read_bytes()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise RuntimeError(f"not a RIFF/WAVE file: {wav}")
    pos = 12
    data = None
    while pos + 8 <= len(raw):
        tag = raw[pos:pos + 4]
        size = struct.unpack_from("<I", raw, pos + 4)[0]
        start, end = pos + 8, pos + 8 + size
        if end > len(raw):
            raise RuntimeError(f"truncated WAV chunk in {wav}")
        if tag == b"data":
            data = raw[start:end]
            break
        pos = end + (size & 1)
    if data is None or len(data) % 4:
        raise RuntimeError(f"missing/non-f32le WAV data in {wav}")
    output.write_bytes(data)


def decode(binary: Path, input_path: Path, output: Path, scalar: bool) -> None:
    # The intentionally minimal CPU build enables pcm_f32le and WAV but not
    # the raw f32le muxer. Produce PCM-f32 WAV, then hash its byte-identical
    # little-endian data chunk as the requested F32LE output.
    wav = output.with_suffix(".wav")
    cmd = [str(binary), "-hide_banner", "-nostdin"]
    if scalar:
        cmd += ["-cpuflags", "0"]
    cmd += ["-threads", "1", "-i", str(input_path), "-map", "0:a:0", "-c:a", "pcm_f32le",
            "-f", "wav", "-y", str(wav)]
    run(cmd)
    extract_wave_f32le(wav, output)


def float_difference(a: Path, b: Path) -> dict:
    aa, bb = a.read_bytes(), b.read_bytes()
    if len(aa) % 4 or len(bb) % 4:
        raise RuntimeError("f32le output is not 4-byte aligned")
    n = min(len(aa), len(bb)) // 4
    mismatch = 0
    max_abs = 0.0
    sum_sq = 0.0
    for i in range(n):
        x = struct.unpack_from("<f", aa, 4 * i)[0]
        y = struct.unpack_from("<f", bb, 4 * i)[0]
        if x != y:
            mismatch += 1
        d = abs(x - y)
        max_abs = max(max_abs, d)
        sum_sq += d * d
    return {"optimized_bytes": len(aa), "scalar_bytes": len(bb), "compared_samples": n,
            "sample_count_equal": len(aa) == len(bb), "different_samples": mismatch,
            "max_abs_difference": max_abs,
            "rms_difference": math.sqrt(sum_sq / n) if n else 0.0,
            "byte_identical": aa == bb}


def timed_decode(binary: Path, input_path: Path, scalar: bool, runs: int) -> list:
    results = []
    for index in range(runs):
        cmd = ["taskset", "-c", "0", str(binary), "-hide_banner", "-nostdin"]
        if scalar:
            cmd += ["-cpuflags", "0"]
        cmd += ["-threads", "1", "-i", str(input_path), "-map", "0:a:0", "-c:a", "pcm_f32le", "-f", "null", "-"]
        started = time.perf_counter()
        run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        seconds = time.perf_counter() - started
        results.append(seconds)
        print(f"{'scalar' if scalar else 'optimized'} run {index + 1}: {seconds:.9f} s")
    return results


def summary(values: list) -> dict:
    return {"runs_seconds": values, "median_seconds": statistics.median(values),
            "minimum_seconds": min(values), "maximum_seconds": max(values), "run_count": len(values)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path, help="one AAC ADTS stream")
    ap.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    ap.add_argument("--cpu-ffmpeg", type=Path)
    ap.add_argument("--scalar-ffmpeg", type=Path)
    ap.add_argument("--output-dir", type=Path,
                    help="fresh result directory; defaults to --root/cpu-baseline and refuses an existing result")
    ap.add_argument("--runs", type=int, default=5)
    args = ap.parse_args()
    if os.environ.get("FF_ET_ALLOW_PCIE", "0") != "0":
        raise SystemExit("refusing: CPU baseline must use FF_ET_ALLOW_PCIE=0")
    if args.runs != 5:
        raise SystemExit("this baseline is intentionally fixed at five runs")
    root = args.root.resolve()
    cpu = (args.cpu_ffmpeg or root / "cpu/ffmpeg").resolve()
    scalar = (args.scalar_ffmpeg or root / "capture-src/ffmpeg").resolve()
    input_path = args.input.resolve()
    require_exec(cpu, "optimized CPU FFmpeg")
    require_exec(scalar, "scalar capture FFmpeg")
    if not input_path.is_file():
        raise RuntimeError(f"input does not exist: {input_path}")
    out = (args.output_dir or root / "cpu-baseline").resolve()
    result_path = out / f"{input_path.stem}.five-run.json"
    if result_path.exists():
        raise RuntimeError(f"refusing to overwrite existing baseline evidence: {result_path}; choose --output-dir")
    out.mkdir(parents=True, exist_ok=True)
    optimized_pcm = out / f"{input_path.stem}.optimized.f32le"
    scalar_pcm = out / f"{input_path.stem}.scalar.f32le"
    decode(cpu, input_path, optimized_pcm, False)
    decode(scalar, input_path, scalar_pcm, True)
    correctness = {"optimized_f32le": {"path": str(optimized_pcm), "sha256": sha256(optimized_pcm)},
                   "scalar_f32le": {"path": str(scalar_pcm), "sha256": sha256(scalar_pcm)},
                   "comparison": float_difference(optimized_pcm, scalar_pcm)}
    optimized = timed_decode(cpu, input_path, False, args.runs)
    oracle = timed_decode(scalar, input_path, True, args.runs)
    result = {
        "input": str(input_path), "input_sha256": sha256(input_path),
        "method": {"streams": 1, "cpu_affinity": "0", "threads": 1,
                   "timing_output": "null muxer", "wall_clock": "time.perf_counter",
                   "outlier_policy": "all five runs retained"},
        "correctness": correctness,
        "timing": {"optimized_default_cpu_flags": summary(optimized),
                   "scalar_oracle_cpu_flags_0": summary(oracle)},
        "interpretation": "Output differences, if any, are characterized above and not masked by this benchmark. F32LE hashes are extracted from explicitly PCM-f32 WAV data because the minimal build has no raw f32le muxer."
    }
    result_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(f"wrote {result_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError, struct.error) as exc:
        print(f"benchmark_cpu.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
