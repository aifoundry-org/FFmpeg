#!/usr/bin/env python3
"""Locate first framemd5 mismatch, or raw packed yuv420p pixel/16x16 macroblock."""
import argparse
from pathlib import Path


def compare_md5(left, right):
    def records(path):
        return [line.strip() for line in path.read_text().splitlines()
                if line.strip() and not line.startswith("#")]
    aa, bb = records(left), records(right)
    for n, (a, b) in enumerate(zip(aa, bb)):
        if a != b:
            print(f"frame {n}:\n  golden: {a}\n  actual: {b}")
            print("framemd5 cannot identify a macroblock; use raw mode with packed yuv420p dumps")
            return 1
    if len(aa) != len(bb):
        print(f"frame count mismatch: golden={len(aa)}, actual={len(bb)}")
        return 1
    print(f"identical: {len(aa)} frame records")
    return 0


def compare_raw(left, right, w, h):
    if w <= 0 or h <= 0 or w % 2 or h % 2:
        raise ValueError("packed yuv420p needs positive even dimensions")
    frame_bytes = w * h * 3 // 2
    aa, bb = left.read_bytes(), right.read_bytes()
    for n, (a, b) in enumerate(zip(aa, bb)):
        if a != b:
            frame, offset = divmod(n, frame_bytes)
            if offset < w*h:
                plane, stride, scale = "Y", w, 1
            elif offset < w*h*5//4:
                plane, stride, scale = "U", w//2, 2
                offset -= w*h
            else:
                plane, stride, scale = "V", w//2, 2
                offset -= w*h*5//4
            y, x = divmod(offset, stride)
            print(f"frame={frame} plane={plane} pixel=({x},{y}) "
                  f"macroblock=({x*scale//16},{y*scale//16}) "
                  f"golden={a} actual={b} byte={n}")
            return 1
    if len(aa) != len(bb):
        print(f"length mismatch: golden={len(aa)} actual={len(bb)} bytes; "
              f"first missing frame={min(len(aa), len(bb))//frame_bytes}")
        return 1
    if len(aa) % frame_bytes:
        print(f"invalid raw stream: {len(aa)} bytes is not a whole number of frames")
        return 1
    print(f"identical: {len(aa)//frame_bytes} packed yuv420p frames")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("golden", type=Path)
    p.add_argument("actual", type=Path)
    p.add_argument("--size", help="raw mode WIDTHxHEIGHT; otherwise framemd5")
    a = p.parse_args()
    if a.size:
        w, h = map(int, a.size.lower().split("x"))
        return compare_raw(a.golden, a.actual, w, h)
    return compare_md5(a.golden, a.actual)


if __name__ == "__main__":
    raise SystemExit(main())
