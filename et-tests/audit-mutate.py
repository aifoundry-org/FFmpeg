#!/usr/bin/env python3
"""Late-audit MPEG2 policy fixtures: profile/chroma transitions and scalable IDs."""
import argparse
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("single_i", type=Path)
p.add_argument("out", type=Path)
a = p.parse_args()
b = a.single_i.read_bytes()
starts = [i for i in range(len(b)-3) if b[i:i+3] == b"\x00\x00\x01"]
seqs = [i for i in starts if b[i+3] == 0xb5 and b[i+4] >> 4 == 1]
pictures = [i for i in starts if b[i+3] == 0]
if len(seqs) != 1 or len(pictures) != 1:
    p.error("expected exactly one sequence extension and I picture")
a.out.mkdir(parents=True, exist_ok=True)
s = seqs[0]
assert b[s+4] & 7 in (4, 5), "source must signal Main or Simple"
assert (b[s+5] >> 1) & 3 == 1, "source must signal 4:2:0"
high = bytearray(b)
high[s+4] = (high[s+4] & 0xf8) | 1  # MPEG2 High profile, same coded dimensions.
chroma = bytearray(b)
chroma[s+5] = (chroma[s+5] & ~6) | 4  # Sequence-extension chroma_format=2.
(a.out/"profile-high.m2v").write_bytes(high)
(a.out/"same-size-profile-change.m2v").write_bytes(b + high)
(a.out/"same-size-chroma-change.m2v").write_bytes(b + chroma)
# Keep original Main sequence extension and insert unsupported scalable syntax
# before the picture, so profile-based rejection alone cannot pass these tests.
for extension in (5, 9, 10):
    extra = b"\x00\x00\x01\xb5" + bytes([extension << 4]) + bytes(7)
    (a.out/f"scalable-{extension:x}.m2v").write_bytes(b[:pictures[0]] + extra + b[pictures[0]:])
