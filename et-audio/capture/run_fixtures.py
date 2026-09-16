#!/usr/bin/env python3
"""Generate deterministic AAC-LC inputs and extract scalar float hook fixtures.

The optimized CPU build creates ADTS files.  The separately archived capture
build decodes each one with -cpuflags 0 and writes ETAACCP1 records.  No ET
runtime is linked, opened, or probed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
INSPECT = Path(__file__).with_name("inspect_capture.py")
DEFAULT_ROOT = ROOT / "build-et/aac-prototype"

# aevalsrc random(0) is seeded by its literal argument.  Each source has a
# silence lead-in, tonal region, abrupt transient, then deterministic noise.
MONO = "if(lt(t,0.20),0,if(lt(t,0.70),0.30*sin(2*PI*440*t),if(lt(t,0.720),0.95,0.11*(2*random(0)-1))))"
STEREO_L = "if(lt(t,0.20),0,if(lt(t,0.70),0.28*sin(2*PI*440*t),if(lt(t,0.720),0.93,0.10*(2*random(0)-1))))"
STEREO_R = "if(lt(t,0.20),0,if(lt(t,0.70),0.25*sin(2*PI*659.25*t),if(lt(t,0.720),-0.91,0.10*(2*random(1)-1))))"


def run(args, **kwargs):
    print("+", " ".join(map(str, args)), flush=True)
    return subprocess.run(args, check=True, **kwargs)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def require(binary: Path, label: str) -> None:
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise RuntimeError(f"{label} is not executable: {binary}")


def require_aevalsrc_input_decoder(binary: Path) -> None:
    # aevalsrc exposes its samples as pcm_f64le. This capability check makes a
    # minimal CPU-build omission actionable instead of failing mid-fixture.
    decoders = subprocess.check_output([str(binary), "-hide_banner", "-decoders"], text=True)
    if "pcm_f64le" not in decoders:
        raise RuntimeError("optimized CPU FFmpeg lacks pcm_f64le; enable that decoder to use deterministic aevalsrc fixtures")


def capture_summary(path: Path, json_path: Path) -> dict:
    run([sys.executable, str(INSPECT), str(path), "--json", str(json_path)])
    return json.loads(json_path.read_text())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, default=DEFAULT_ROOT,
                    help="build root containing cpu/ and capture-src/")
    ap.add_argument("--output-root", type=Path,
                    help="fixture/capture destination; defaults to --root and refuses existing evidence")
    ap.add_argument("--cpu-ffmpeg", type=Path)
    ap.add_argument("--capture-ffmpeg", type=Path)
    ap.add_argument("--duration", type=float, default=1.25)
    ap.add_argument("--cases", default="mono-44100,stereo-44100,mono-48000,stereo-48000",
                    help="comma-separated subset of mono-44100, stereo-44100, mono-48000, stereo-48000")
    ap.add_argument("--encode-only", action="store_true",
                    help="generate ADTS only; do not invoke the scalar capture binary")
    args = ap.parse_args()
    if os.environ.get("FF_ET_ALLOW_PCIE", "0") != "0":
        raise SystemExit("refusing: fixtures must run with FF_ET_ALLOW_PCIE=0")
    root = args.root.resolve()
    output_root = (args.output_root or root).resolve()
    cpu = (args.cpu_ffmpeg or root / "cpu/ffmpeg").resolve()
    capture = (args.capture_ffmpeg or root / "capture-src/ffmpeg").resolve()
    require(cpu, "optimized CPU FFmpeg")
    require_aevalsrc_input_decoder(cpu)
    if not args.encode_only:
        require(capture, "private scalar capture FFmpeg")
    requested = {item.strip() for item in args.cases.split(",") if item.strip()}
    valid = {"mono-44100", "stereo-44100", "mono-48000", "stereo-48000"}
    if not requested or requested - valid:
        raise RuntimeError(f"invalid --cases: {sorted(requested - valid)}")
    # Evidence is immutable by default. Choose a fresh --output-root for a rerun.
    if (output_root / "fixture-results.json").exists() or (output_root / "fixtures").exists() or (output_root / "captures").exists():
        raise RuntimeError(f"refusing to overwrite existing fixture evidence at {output_root}; choose a new --output-root")
    fixtures = output_root / "fixtures"
    captures = output_root / "captures"
    fixtures.mkdir(parents=True)
    if not args.encode_only:
        captures.mkdir(parents=True)
    cases = []
    for rate in (44100, 48000):
        for channels in (1, 2):
            name = f"{'mono' if channels == 1 else 'stereo'}-{rate}"
            if name not in requested:
                continue
            adts = fixtures / f"{name}.aac"
            expr = MONO if channels == 1 else f"{STEREO_L}|{STEREO_R}"
            lavfi = f"aevalsrc=exprs='{expr}':s={rate}:d={args.duration}"
            bitrate = "96k" if channels == 1 else "160k"
            run([str(cpu), "-hide_banner", "-nostdin", "-y", "-f", "lavfi", "-i", lavfi,
                 "-threads", "1", "-c:a", "aac", "-profile:a", "aac_low", "-b:a", bitrate,
                 "-f", "adts", str(adts)])
            item = {"name": name, "sample_rate": rate, "channels": channels,
                    "input": str(adts), "input_sha256": sha256(adts)}
            if not args.encode_only:
                cap = captures / f"{name}.etaaccap"
                env = os.environ.copy()
                env["FF_AAC_CAPTURE"] = str(cap)
                run([str(capture), "-hide_banner", "-nostdin", "-cpuflags", "0", "-threads", "1",
                     "-i", str(adts), "-map", "0:a:0", "-c:a", "pcm_f32le", "-f", "null", "-"], env=env)
                summary = capture_summary(cap, captures / f"{name}.json")
                if summary["records"] == 0:
                    raise RuntimeError(f"{name}: no base-1024 AAC capture records")
                item["capture"] = summary
            cases.append(item)
    result = {"generator": "optimized asm-enabled CPU FFmpeg", "duration_seconds": args.duration,
              "encode_only": args.encode_only, "cases": cases}
    if not args.encode_only:
        short = sum(case["capture"]["short_window_records"] for case in cases)
        long = sum(case["capture"]["long_window_records"] for case in cases)
        if not short or not long:
            raise RuntimeError(f"window coverage insufficient: short={short}, long={long}")
        result["capture_cpu_flags"] = 0
        result["aggregate"] = {"short_window_records": short, "long_window_records": long}
    output = output_root / "fixture-results.json"
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(f"wrote {output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        print(f"run_fixtures.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
