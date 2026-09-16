#!/usr/bin/env python3
"""Native correctness and fail-closed input-validation checks for full AAC fixtures."""
import argparse
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile


def digest(path):
    h = hashlib.sha256(); h.update(path.read_bytes()); return h.hexdigest()


def reject(cpu, path, label):
    p = subprocess.run([str(cpu), str(path), "1", "scalar"], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode == 0 or not p.stderr.startswith("input reject:"):
        raise AssertionError(f"{label} was not rejected before decode: rc={p.returncode}, stderr={p.stderr!r}")
    return {"case": label, "returncode": p.returncode, "stderr": p.stderr.splitlines()[0]}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cpu", type=pathlib.Path, required=True)
    ap.add_argument("--fixtures", type=pathlib.Path, required=True)
    ap.add_argument("--native-output", type=pathlib.Path, required=True,
                    help="JSON evidence path under build-et/aac-full/native*")
    args = ap.parse_args()
    manifest = json.loads((args.fixtures / "MANIFEST.json").read_text())
    if not args.cpu.is_file(): ap.error("--cpu is not executable file")
    evidence = {"type": "aac_full_native_fixture_tests", "valid": [], "fail_closed": []}
    with tempfile.TemporaryDirectory(prefix="aac-full-fixture-tests-") as td:
        td = pathlib.Path(td)
        for case in manifest["cases"]:
            inp = args.fixtures / case["input"]
            if digest(inp) != case["input_sha256"]:
                raise AssertionError(f"input hash mismatch: {inp}")
            pcm = td / (case["name"] + ".pcm")
            p = subprocess.run([str(args.cpu), str(inp), str(pcm), "1", "scalar"], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if p.returncode:
                raise AssertionError(f"valid scalar decode failed: {case['name']}: {p.stderr}")
            decoded = json.loads(p.stdout)
            if decoded["scalar"] != 1 or decoded["frames"] != case["packet_count"] or decoded["samples"] != case["packet_count"] * case["channels"] * 1024:
                raise AssertionError(f"bad valid decode accounting: {case['name']}: {decoded}")
            if pcm.stat().st_size != case["packet_count"] * case["channels"] * 1024 * 4:
                raise AssertionError(f"bad PCM shape: {case['name']}")
            if case["scalar_pcm_sha256"] and digest(pcm) != case["scalar_pcm_sha256"]:
                raise AssertionError(f"scalar oracle mismatch: {case['name']}")
            evidence["valid"].append({"case": case["name"], "pcm_sha256": digest(pcm), "result": "pass"})
        # Mutate a fresh copy of the exact 512-packet workload.  Each must fail in
        # full_input_load(), whose diagnostic prefix makes the pre-decode boundary explicit.
        base = args.fixtures / "stereo-48000-512.input"
        raw = bytearray(base.read_bytes())
        trunc = td / "truncated-header.input"; trunc.write_bytes(raw[:63])
        evidence["fail_closed"].append(reject(args.cpu, trunc, "truncated-header"))
        bad_header = bytearray(raw); bad_header[0] ^= 1
        bad_header_p = td / "bad-header.input"; bad_header_p.write_bytes(bad_header)
        evidence["fail_closed"].append(reject(args.cpu, bad_header_p, "header-magic"))
        bad_count = bytearray(raw); struct.pack_into("<I", bad_count, 16, 511)
        bad_count_p = td / "bad-count.input"; bad_count_p.write_bytes(bad_count)
        evidence["fail_closed"].append(reject(args.cpu, bad_count_p, "count-descriptor-layout"))
        over = bytearray(raw); struct.pack_into("<Q", over, 64, 0xfffffffffffffff0)
        overp = td / "overflow-offset.input"; overp.write_bytes(over)
        evidence["fail_closed"].append(reject(args.cpu, overp, "overflow-offset"))
        reserved = bytearray(raw); reserved[28] = 1
        reservedp = td / "reserved-header.input"; reservedp.write_bytes(reserved)
        evidence["fail_closed"].append(reject(args.cpu, reservedp, "reserved-header"))
        first_offset = struct.unpack_from("<Q", raw, 64)[0]; first_len = struct.unpack_from("<I", raw, 72)[0]
        padding = bytearray(raw); padding[first_offset + first_len] = 1
        paddingp = td / "nonzero-padding.input"; paddingp.write_bytes(padding)
        evidence["fail_closed"].append(reject(args.cpu, paddingp, "nonzero-padding"))
        bad_adts = bytearray(raw); bad_adts[first_offset + 1] &= ~1
        bad_adtsp = td / "crc-adts.input"; bad_adtsp.write_bytes(bad_adts)
        evidence["fail_closed"].append(reject(args.cpu, bad_adtsp, "ADTS-CRC-present"))
        existing = td / "must-not-overwrite.pcm"; existing.write_bytes(b"keep")
        p = subprocess.run([str(args.cpu), str(base), str(existing), "1", "scalar"], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if p.returncode == 0 or existing.read_bytes() != b"keep" or "already exists" not in p.stderr:
            raise AssertionError(f"PCM output overwrite guard failed: rc={p.returncode}, stderr={p.stderr!r}")
        evidence["output_no_overwrite"] = "pass"
    args.native_output.parent.mkdir(parents=True, exist_ok=True)
    args.native_output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    print(json.dumps(evidence, sort_keys=True))


if __name__ == "__main__":
    main()
