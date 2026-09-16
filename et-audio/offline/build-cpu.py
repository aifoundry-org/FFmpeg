#!/usr/bin/env python3
"""Build the host-only offline AAC synthesis CPU baseline.

This links only the already-built pinned optimized FFmpeg archives.  It does
not configure FFmpeg, open hardware, or modify the source tree.
"""
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parents[2]
out = root / "build-et/aac-offline/cpu"
out.mkdir(parents=True, exist_ok=True)

source = (root / "et-audio/dsp/tests/test_etaac_ffmpeg.c").read_text()
start = source.index("static void reference_synth(")
end = source.index("\nstatic int compare(", start)
function = source[start:end]
old_signature = """static void reference_synth(float *out, float *saved, const float *coeff,
                            unsigned sequence, unsigned previous_sequence,
                            unsigned shape, unsigned previous_shape,
                            AVTXContext *tx1024, av_tx_fn fn1024,
                            AVTXContext *tx128, av_tx_fn fn128)"""
new_signature = """static void reference_synth(float *out, float *saved, const float *coeff,
                            unsigned sequence, unsigned previous_sequence,
                            unsigned shape, unsigned previous_shape,
                            AVTXContext *tx1024, av_tx_fn fn1024,
                            AVTXContext *tx128, av_tx_fn fn128,
                            AVFloatDSPContext *fdsp, float *buf, float *temp)"""
assert old_signature in function
assert function.count("    float buf[1024], temp[128];") == 1
function = function.replace(old_signature, new_signature)
function = function.replace("    float buf[1024], temp[128];\n", "")
# The CPU baseline intentionally dispatches through each worker's FFmpeg DSP
# context rather than using the scalar helper embedded in the test source.
assert function.count("mul_window(") == 10
function = function.replace("mul_window(", "fdsp->vector_fmul_window(")
(out / "cpu_reference_synth.h").write_text(
    "/* Derived from pinned AAC test reference, LGPL-2.1-or-later. */\n" + function
)

pinned = root / "build-et/aac-prototype/cpu"
for archive in (pinned / "libavcodec/libavcodec.a", pinned / "libavutil/libavutil.a"):
    if not archive.is_file():
        raise SystemExit(f"missing pinned optimized FFmpeg archive: {archive}")

subprocess.run([
    "cc", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
    "-ffp-contract=off", "-I" + str(root), "-I" + str(pinned),
    "-I" + str(root / "et-audio"), "-I" + str(out),
    str(root / "et-audio/offline/cpu.c"),
    str(pinned / "libavcodec/libavcodec.a"),
    str(pinned / "libavutil/libavutil.a"),
    "-lm", "-pthread", "-o", str(out / "cpu-offline"),
], check=True)
