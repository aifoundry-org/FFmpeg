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
# RV64IMF plus an explicit ET integer-SIMD allowlist, NOT standard RISC-V V.
# .ps is used solely for raw data movement, not floating-point arithmetic.
# Division/remainder and other unreviewed custom vector opcodes stay forbidden.
vector_ops = {
    'flw.ps', 'fsw.ps', 'fgh.ps', 'fgb.ps', 'fscb.ps', 'fsch.ps',
    'fg32h.ps', 'fg32b.ps', 'fsc32b.ps', 'fsc32w.ps', 'fbcx.ps', 'fbc.ps',
    'fbci.pi', 'fadd.pi', 'fsub.pi', 'fmul.pi', 'faddi.pi', 'for.pi',
    'fand.pi', 'fandi.pi', 'fxor.pi', 'fxori.pi', 'fnot.pi', 'feq.pi', 'fsrai.pi', 'fsrli.pi', 'fslli.pi',
    'fsatu8.pi', 'fpackrepb.pi', 'fpackreph.pi',
}
used_vector_ops = set()
for line in run('objdump','-d').splitlines():
    m=re.match(r'^\s*[0-9a-f]+:\s+([0-9a-f]+)\s+(\S+)',line)
    if not m: continue
    encoded,mnemonic=m.groups()
    if mnemonic.startswith(('rdcycle', 'rdtime', 'rdinstret')):
        raise SystemExit('User-mode illegal counter read: '+line)
    if len(encoded)!=8 or mnemonic.startswith(('c.','v.')):
        raise SystemExit('Unsupported compressed / RISC-V V instruction: '+line)
    if mnemonic.endswith(('.ps', '.pi')):
        if mnemonic not in vector_ops:
            raise SystemExit('Unreviewed custom SIMD instruction: '+line)
        used_vector_ops.add(mnemonic)
print('Device check: no undefined symbols, BSS, TLS, compressed or RISC-V V instructions')
print('Audited ET integer-SIMD/data-movement opcodes:', ', '.join(sorted(used_vector_ops)) or 'none')

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
