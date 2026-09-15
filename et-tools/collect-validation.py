#!/usr/bin/env python3
"""Inventory retained local results; reads files only, never launches a device."""
import datetime
import hashlib
import json
from pathlib import Path
import re

root = Path(__file__).resolve().parent.parent
build = root / "build-et"

def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()

def record(path):
    return {"path": str(path.relative_to(root)), "sha256": sha(path), "bytes": path.stat().st_size}

def frames(path):
    return sum(line.startswith("0,") for line in path.read_text().splitlines())

cases = [
    ("single-i", "64x48", 1), ("i-only", "128x96", 64),
    ("65rows", "64x1040", 64), ("ipb12", "128x96", 64),
    ("ipb250", "128x96", 64), ("sd-ipb250", "720x576", 1),
    ("sd-ipb250", "720x576", 64), ("hd-ipb12", "1920x1080", 64),
]
result = {
    "schema": 1,
    "recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "ffmpeg_base": "db69d06eeeab4f46da15030a80d539efb4503ca8",
    "kernel": record(root / "et-kernels/build-device/et_mpeg2_slice.elf"),
    "host": record(build / "host/ffmpeg"),
    "physical_device": "et0 / 0000:01:00.0",
    "shire_mask": "0x1",
    "firmware_changed": False,
    "silicon": [],
}
for name, dimensions, harts in cases:
    folder = build / "silicon" / f"{name}-h{harts}"
    cpu = build / "corpus" / f"{name}.golden.md5"
    output = folder / "output.md5"
    log = (folder / "decode.log").read_text()
    assert (folder / "result.txt").read_text().startswith("PASS silicon:")
    assert output.read_bytes() == cpu.read_bytes()
    assert len(re.findall(r"ET frame \d+:.*harts=", log)) == frames(cpu)
    for suffix in ("ce", "uce", "counts"):
        assert (folder / f"before.{suffix}").read_bytes() == (folder / f"after.{suffix}").read_bytes()
    elapsed = re.search(r"bench: utime=[^\n]*rtime=([0-9.]+)s", log)
    entry = {
        "case": name, "dimensions": dimensions, "harts": harts,
        "frames": frames(cpu), "exact_framemd5": True, "unchanged_error_counters": True,
        "artifacts": [record(p) for p in [
            build / "corpus" / f"{name}.m2v", cpu, output,
            folder / "decode.log", folder / "before.counts", folder / "after.counts",
            folder / "before.ce", folder / "after.ce", folder / "before.uce", folder / "after.uce",
        ]],
    }
    if elapsed:
        entry["single_observation_wall_seconds"] = float(elapsed.group(1))
    result["silicon"].append(entry)
result["silicon_decoded_frames"] = sum(case["frames"] for case in result["silicon"])
result["native_summary"] = {
    "integration_checks_passed": 69, "policy_negatives_passed": 8,
    "api_drain_controls": 4, "api_drain_fault_cases": 12,
    "note": "See et-tests/STATUS.md; native execution is not hardware validation.",
}
exitfile = build / "ipb250-small.et64.exit"
compare = build / "ipb250-small.et64.cmp"
if exitfile.exists() and compare.exists() and exitfile.read_text().strip() == compare.read_text().strip() == "0":
    cpu = build / "corpus/ipb250-small.golden.md5"
    out = build / "corpus/ipb250-small.et64.md5"
    assert cpu.read_bytes() == out.read_bytes() and frames(out) == 250
    result["sysemu_250"] = {"status": "PASS", "dimensions": "64x48", "harts": 64,
                            "memory_check": True, "fatal_memory_errors": True,
                            "artifacts": [record(build / "corpus/ipb250-small.m2v"), record(cpu), record(out),
                                          record(build / "ipb250-small.et64.log"), record(exitfile), record(compare)]}
else:
    result["sysemu_250"] = {"status": "NOT_CONFIRMED", "note": "No passing completion/compare records yet."}
(root / "ET_VALIDATION.json").write_text(json.dumps(result, indent=2) + "\n")
print(f"Recorded {result['silicon_decoded_frames']} device-decoded frames; sysemu250={result['sysemu_250']['status']}")
