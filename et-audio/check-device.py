#!/usr/bin/env python3
"""Reuse unchanged video ELF legality gates, then enforce scalar FP contract."""
import pathlib, re, subprocess, sys
root=pathlib.Path(__file__).resolve().parent.parent
subprocess.run([sys.executable,str(root/'et-kernels/scripts/check-device.py'),*sys.argv[1:]],check=True)
text=subprocess.check_output([sys.argv[2]+'objdump','-d',sys.argv[1]],text=True)
for line in text.splitlines():
    m=re.match(r'^\s*[0-9a-f]+:\s+([0-9a-f]+)\s+(\S+)',line)
    if not m: continue
    op=m[2]
    if op.startswith(('fmadd','fmsub','fnmadd','fnmsub')) or op.endswith(('.ps','.pi','.d')):
        raise SystemExit('Unreviewed audio arithmetic: '+line)
print('AAC scalar FP contract: no contracted FMA, packed operations or double FP')
