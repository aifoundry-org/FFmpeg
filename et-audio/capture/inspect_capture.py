#!/usr/bin/env python3
"""Validate and summarize an ETAACCP1 capture without interpreting floats."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

MAGIC = b"ETAACCP1"
HEADER_BYTES = 32
RECORD_BYTES = 12312
RECORD_HEADER = struct.Struct("<6I")


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def inspect(path: Path) -> dict:
    raw = path.read_bytes()
    if len(raw) < HEADER_BYTES:
        raise ValueError("short header")
    magic = raw[:8]
    version, header_size, record_size, float_format, endian_tag, flags = struct.unpack_from("<6I", raw, 8)
    if magic != MAGIC:
        raise ValueError(f"magic is {magic!r}, not {MAGIC!r}")
    if (version, header_size, record_size, float_format, endian_tag, flags) != (1, HEADER_BYTES, RECORD_BYTES, 1, 0x01020304, 0):
        raise ValueError("unsupported or malformed ETAACCP1 header")
    payload = len(raw) - HEADER_BYTES
    if payload % RECORD_BYTES:
        raise ValueError(f"trailing/partial record: {payload} payload bytes")
    contexts, sequences, pairs, shapes = {}, {}, {}, {}
    previous_frame = {}
    for offset in range(HEADER_BYTES, len(raw), RECORD_BYTES):
        context, frame, seq, prevseq, shape, prevshape = RECORD_HEADER.unpack_from(raw, offset)
        if context == 0:
            raise ValueError(f"zero context id at byte {offset}")
        if shape > 1 or prevshape > 1:
            raise ValueError(f"non-AAC window shape at byte {offset}")
        expected = previous_frame.get(context, -1) + 1
        if frame != expected:
            raise ValueError(f"context {context} frame {frame}, expected {expected}")
        previous_frame[context] = frame
        contexts[str(context)] = contexts.get(str(context), 0) + 1
        sequences[str(seq)] = sequences.get(str(seq), 0) + 1
        pairs[f"{prevseq}->{seq}"] = pairs.get(f"{prevseq}->{seq}", 0) + 1
        shapes[f"{prevshape}->{shape}"] = shapes.get(f"{prevshape}->{shape}", 0) + 1
    return {
        "path": str(path), "sha256": digest(path), "bytes": len(raw),
        "records": payload // RECORD_BYTES, "contexts": contexts,
        "window_sequence_counts": sequences,
        "window_transition_counts": pairs,
        "window_shape_transition_counts": shapes,
        "short_window_records": sequences.get("2", 0),
        "long_window_records": sum(v for key, v in sequences.items() if key != "2"),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    result = inspect(args.capture)
    encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.json:
        args.json.write_text(encoded)
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, struct.error) as exc:
        print(f"inspect_capture.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
