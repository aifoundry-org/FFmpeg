#!/usr/bin/env python3
"""Deterministic MPEG-2 elementary-stream negative cases from a single-I stream."""
import argparse
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("input", type=Path)
p.add_argument("outdir", type=Path)
a = p.parse_args()
b = a.input.read_bytes()
starts = [i for i in range(len(b) - 3) if b[i:i+3] == b"\x00\x00\x01"]
slices = [(i, starts[n+1] if n+1 < len(starts) else len(b))
          for n, i in enumerate(starts) if 1 <= b[i+3] <= 0xaf]
if len(slices) < 2:
    p.error("need a single-I stream with at least two slice rows")
a.outdir.mkdir(parents=True, exist_ok=True)
i, j = slices[0]
(a.outdir / "duplicate-row.m2v").write_bytes(b[:j] + b[i:j] + b[j:])
(a.outdir / "missing-row.m2v").write_bytes(b[:i] + b[j:])
# Keep final slice start code + quantizer, but remove almost all payload.
i, j = slices[-1]
(a.outdir / "truncated.m2v").write_bytes(b[:i+5])
