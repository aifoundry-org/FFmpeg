#!/usr/bin/env python3
"""Fail the device build if unsupported storage/instructions slipped in."""
import pathlib, re, subprocess, sys
elf, prefix = sys.argv[1:]
def run(tool, *args):
    return subprocess.check_output([prefix+tool, *args, elf], text=True)
undefined=run('nm','-u').strip()
if undefined:
    raise SystemExit('Unresolved device symbols:\n'+undefined)
sections=run('objdump','-h')
for name,size in re.findall(r'^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s',sections,re.M):
    if name in ('.bss','.sbss','.tdata','.tbss') and int(size,16):
        raise SystemExit('Forbidden device section: '+name)
# RV64IMF scalar code only. ET CSRs are intentionally allowed.
for line in run('objdump','-d').splitlines():
    m=re.match(r'^\s*[0-9a-f]+:\s+([0-9a-f]+)\s+(\S+)',line)
    if not m: continue
    encoded,mnemonic=m.groups()
    if mnemonic.startswith(('rdcycle', 'rdtime', 'rdinstret')):
        raise SystemExit('User-mode illegal counter read: '+line)
    if len(encoded)!=8 or mnemonic.startswith(('c.','v.')) or '.ps' in mnemonic:
        raise SystemExit('Non-scalar / compressed instruction: '+line)
print('Device check: no undefined symbols, BSS, TLS, compressed or vector instructions')

# Hardware DMA and the runtime ELF loader require cache-line-sized LOADs.
loads = 0
for line in run('readelf', '-lW').splitlines():
    if not line.lstrip().startswith('LOAD '): continue
    fields = line.split()
    offset, va, pa, filesz, memsz = [int(v,16) for v in fields[1:6]]
    if any(v & 63 for v in [offset,va,pa,filesz,memsz]):
        raise SystemExit('Unaligned LOAD: '+line)
    if memsz != filesz:
        raise SystemExit('LOAD contains zero-fill memory: '+line)
    loads += 1
if not loads: raise SystemExit('Missing LOAD segment')
print('Device check: LOAD offset/addresses/filesz/memsz multiples of 64; no rdcycle')
