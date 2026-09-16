#!/usr/bin/env python3
"""Create strict, packetized AAC-LC full-decode fixtures.

The source streams are retained unchanged.  This tool only writes the new
build-et/aac-full/fixtures products.  With --cpu it additionally asks the
pinned native FFmpeg driver for scalar F32 packet-major PCM oracles.
"""
import argparse
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "build-et/aac-full/fixtures"
MAGIC = 0x34434141465445
HEADER = struct.Struct("<QQIIII4Q")
DESC = struct.Struct("<QII6Q")

CASES = (
    ("stereo-48000-512", ROOT / "build-et/aac-prototype/baseline-30s-20260916/fixtures/stereo-48000.aac", 512),
    ("stereo-48000-32", ROOT / "build-et/aac-prototype/baseline-30s-20260916/fixtures/stereo-48000.aac", 32),
    ("stereo-48000-1", ROOT / "build-et/aac-prototype/baseline-30s-20260916/fixtures/stereo-48000.aac", 1),
    ("mono-44100-32", ROOT / "build-et/aac-prototype/fixtures/mono-44100.aac", 32),
    ("mono-48000-32", ROOT / "build-et/aac-prototype/fixtures/mono-48000.aac", 32),
)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1024 * 1024), b""):
            h.update(b)
    return h.hexdigest()


def adts_packets(data, source):
    """Split and admit only the deliberately narrow full-experiment profile."""
    packets, pos, config = [], 0, None
    while pos < len(data):
        if len(data) - pos < 7:
            raise ValueError(f"{source}: truncated ADTS header at {pos}")
        h = data[pos:pos + 7]
        if h[0] != 0xff or (h[1] & 0xf7) != 0xf1:
            raise ValueError(f"{source}: non-ADTS/no-CRC/layer-0 header at {pos}")
        profile = (h[2] >> 6) & 3
        sf_index = (h[2] >> 2) & 15
        channels = ((h[2] & 1) << 2) | (h[3] >> 6)
        frame_bytes = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5)
        raw_blocks = h[6] & 3
        rate = {3: 48000, 4: 44100}.get(sf_index)
        if profile != 1 or rate is None or channels not in (1, 2) or raw_blocks != 0:
            raise ValueError(f"{source}: unsupported ADTS AAC-LC config at {pos}")
        if frame_bytes < 7 or pos + frame_bytes > len(data):
            raise ValueError(f"{source}: malformed ADTS frame length at {pos}")
        here = (profile, sf_index, channels, rate)
        if config is not None and here != config:
            raise ValueError(f"{source}: inconsistent ADTS configuration at {pos}")
        config = here
        packets.append(data[pos:pos + frame_bytes])
        pos += frame_bytes
    if not packets:
        raise ValueError(f"{source}: no ADTS packets")
    return packets, config


def align64(n):
    return (n + 63) & ~63


def make_input(source, count):
    source_bytes = source.read_bytes()
    packets, (profile, sf_index, channels, rate) = adts_packets(source_bytes, source)
    if len(packets) < count:
        raise ValueError(f"{source}: need {count} packets; have {len(packets)}")
    packets = packets[:count]
    prefix = HEADER.size + DESC.size * count
    assert prefix % 64 == 0
    offsets, cursor = [], prefix
    for packet in packets:
        offsets.append(cursor)
        cursor += align64(len(packet) + 64)  # at least one full cache line of zero padding
    total = align64(cursor)
    blob = bytearray(total)
    HEADER.pack_into(blob, 0, MAGIC, total, count, channels, rate, 0, 0, 0, 0, 0)
    for i, (offset, packet) in enumerate(zip(offsets, packets)):
        DESC.pack_into(blob, HEADER.size + i * DESC.size, offset, len(packet), 0, 0, 0, 0, 0, 0, 0)
        blob[offset:offset + len(packet)] = packet
    # The intentionally unfilled bytes are the required zero padding.
    validate_shared_envelope(blob)
    return bytes(blob), {"channels": channels, "sample_rate": rate, "adts_profile": profile,
                         "adts_sample_rate_index": sf_index, "packet_count": count,
                         "packet_bytes": [len(x) for x in packets]}


def validate_shared_envelope(blob):
    """Python mirror of protocol.h etaac_full_input_valid for generator assertions."""
    if len(blob) < 128 or len(blob) & 63:
        raise AssertionError("invalid complete input alignment")
    magic, total, count, channels, rate, reserved0, *reserved = HEADER.unpack_from(blob)
    if magic != MAGIC or total != len(blob) or not count or count > 2048 or channels not in (1, 2) or rate not in (44100, 48000) or reserved0 or any(reserved):
        raise AssertionError("invalid full input header")
    cursor = HEADER.size + count * DESC.size
    if cursor > len(blob):
        raise AssertionError("descriptor table exceeds input")
    for i in range(count):
        offset, packet_bytes, reserved0, *reserved = DESC.unpack_from(blob, HEADER.size + i * DESC.size)
        if offset != cursor or packet_bytes < 8 or packet_bytes > 8191 or reserved0 or any(reserved) or packet_bytes + 64 > len(blob) - cursor:
            raise AssertionError(f"invalid descriptor {i}")
        packet = blob[cursor:cursor + packet_bytes]
        sf = (packet[2] >> 2) & 15
        packet_channels = ((packet[2] & 1) << 2) | (packet[3] >> 6)
        packet_length = ((packet[3] & 3) << 11) | (packet[4] << 3) | (packet[5] >> 5)
        if packet[0] != 0xff or packet[1] not in (0xf1, 0xf9) or (packet[2] >> 6) != 1 or sf not in (3, 4) or packet_channels != channels or (48000 if sf == 3 else 44100) != rate or packet_length != packet_bytes or (packet[6] & 3):
            raise AssertionError(f"invalid ADTS packet {i}")
        next_cursor = align64(cursor + packet_bytes + 64)
        if next_cursor > len(blob) or any(blob[cursor + packet_bytes:next_cursor]):
            raise AssertionError(f"invalid zero padding {i}")
        cursor = next_cursor
    if cursor != len(blob):
        raise AssertionError("input does not end exactly after final padded packet")


def atomic_same_or_write(path, data):
    if path.exists():
        if path.read_bytes() != data:
            raise RuntimeError(f"refusing to replace different existing fixture: {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as f:
        f.write(data)
        tmp = pathlib.Path(f.name)
    os.replace(tmp, path)


def generate_oracle(cpu, inp, pcm, count):
    if pcm.exists():
        return
    # The one-packet oracle is exactly packet 0's packet-major planar prefix of
    # the independently decoded 512-packet scalar oracle; avoid an extra run.
    if count == 1:
        full = inp.with_name("stereo-48000-512.scalar.pcm")
        if full.is_file():
            atomic_same_or_write(pcm, full.read_bytes()[:2 * 1024 * 4])
            return
    # The C tool itself uses O_EXCL and will refuse accidental replacement.
    subprocess.run([str(cpu), str(inp), str(pcm), "1", "scalar"], check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", type=pathlib.Path, default=OUT)
    ap.add_argument("--cpu", type=pathlib.Path, help="native aac-full-cpu; generate scalar PCM oracles")
    args = ap.parse_args()
    if args.cpu and not args.cpu.is_file():
        ap.error(f"--cpu is not a file: {args.cpu}")
    args.output.mkdir(parents=True, exist_ok=True)
    cases = []
    for name, source, count in CASES:
        if not source.is_file():
            raise SystemExit(f"required retained source fixture is absent: {source}")
        blob, info = make_input(source, count)
        inp = args.output / f"{name}.input"
        atomic_same_or_write(inp, blob)
        pcm = args.output / f"{name}.scalar.pcm"
        if args.cpu:
            generate_oracle(args.cpu, inp, pcm, count)
        item = {"name": name, "source": str(source.relative_to(ROOT)), "source_sha256": sha256(source),
                "input": inp.name, "input_sha256": sha256(inp), "input_bytes": inp.stat().st_size,
                "scalar_pcm": pcm.name if pcm.exists() else None,
                "scalar_pcm_sha256": sha256(pcm) if pcm.exists() else None,
                "scalar_pcm_bytes": pcm.stat().st_size if pcm.exists() else None, **info}
        expected = count * info["channels"] * 1024 * 4
        if pcm.exists() and pcm.stat().st_size != expected:
            raise RuntimeError(f"bad scalar PCM size for {pcm}: expected {expected}")
        cases.append(item)
    manifest = {
        "format": "ETAACFullInput ABI4", "magic": f"0x{MAGIC:x}", "header_bytes": 64,
        "descriptor_bytes": 64, "adts_policy": "AAC-LC/profile=1, mono/stereo, 48000/44100, no CRC, one raw block",
        "padding_policy": "each packet starts 64-byte aligned and is followed by at least 64 zero bytes; file is 64-byte aligned",
        "oracle": "pinned FFmpeg CPU aac decoder, av_force_cpu_flags(0), one packet -> one 1024-sample frame",
        "benchmark_note": "This is complete compressed-packet decode. It is not comparable as a tolerance or stage-equivalent replacement for the earlier synthesis-only CPU-stage benchmark; optimized dispatch is characterization only.",
        "cases": cases,
    }
    encoded = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    (args.output / "MANIFEST.json").write_text(encoded)
    # Keep an extension that makes this provenance artifact discoverable by tools
    # that distinguish generated fixture manifests from ordinary JSON inputs.
    (args.output / "fixtures.jsonmanifest").write_text(encoded)
    print(json.dumps(manifest, sort_keys=True))


if __name__ == "__main__":
    main()
