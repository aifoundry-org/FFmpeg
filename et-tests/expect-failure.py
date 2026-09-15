#!/usr/bin/env python3
"""Require a controlled nonzero exit, not a signal (FFmpeg may exit 234 for -EINVAL)."""
import resource
import subprocess
import sys

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with open(sys.argv[1], "wb") as log:
    result = subprocess.run(sys.argv[2:], stdout=log, stderr=subprocess.STDOUT)
if result.returncode == 0:
    print("FAIL: negative input silently accepted", file=sys.stderr)
    raise SystemExit(1)
if result.returncode < 0:
    print(f"FAIL: decoder killed by signal {-result.returncode}, not clean rejection", file=sys.stderr)
    raise SystemExit(1)
print(f"controlled rejection: exit {result.returncode}")
