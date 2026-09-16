#!/usr/bin/env python3
"""Final-link gate for the full scalar AAC ELF; never runs it."""
import collections, pathlib, re, struct, subprocess, sys
if len(sys.argv)!=3: raise SystemExit('usage: check-device.py ELF TOOLPREFIX')
elf,prefix=sys.argv[1:]
def run(tool,*args): return subprocess.check_output([prefix+tool,*args,elf],text=True)
undefined=run('nm','-u').strip()
if undefined: raise SystemExit('unresolved symbols:\n'+undefined)
sections=run('objdump','-h')
for name,size in re.findall(r'^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s',sections,re.M):
    if name in ('.bss','.sbss','.tdata','.tbss','.init_array','.fini_array','.ctors','.dtors') and int(size,16): raise SystemExit('forbidden zero/TLS section '+name)
disassembly=run('objdump','-d')
if not all(s in disassembly for s in ('evict_va','tensor_wait','hpmcounter3')):
    raise SystemExit('Missing device publication/counter code: was ET_DEVICE defined?')
for line in disassembly.splitlines():
    m=re.match(r'^\s*[0-9a-f]+:\s+([0-9a-f]+)\s+(\S+)',line)
    if not m: continue
    bits,op=m.groups()
    if op.startswith('.') or op in ('unimp','ebreak','wfi','mret','sret'):
        raise SystemExit('unknown/trap/privileged instruction: '+line)
    if len(bits)!=8 or op.startswith(('c.','v.')): raise SystemExit('compressed/RVV instruction: '+line)
    if op.startswith(('fmadd','fmsub','fnmadd','fnmsub')) or op.endswith(('.d','.q','.ps','.pi')): raise SystemExit('unreviewed FP/SIMD instruction: '+line)
    if op in ('fdiv.s','fsqrt.s','fcvt.l.s','fcvt.lu.s','fcvt.s.l','fcvt.s.lu','fence.i','sfence.vma'):
        raise SystemExit('ET microcode-dependent instruction: '+line)
    word=int(bits,16)
    if word&127==0x73 and (word>>12)&7:
        csr=word>>20; mode=(word>>12)&7; source=(word>>15)&31
        if csr not in (1,2,3,0x830,0x89f,0xc03,0xcd0): raise SystemExit('unapproved CSR: '+line)
        if csr in (0xc03,0xcd0) and not (mode==2 and source==0): raise SystemExit('read-only CSR written: '+line)
    if op.startswith(('rdcycle','rdtime','rdinstret')): raise SystemExit('illegal timing CSR: '+line)
for line in run('readelf','-lW').splitlines():
    if not line.lstrip().startswith('LOAD '): continue
    f=line.split(); vals=[int(x,16) for x in f[1:6]]
    if any(x&63 for x in vals) or vals[3]!=vals[4]: raise SystemExit('bad LOAD '+line)
# SDK loadCode moves the image and only rebases R_RISCV_64. Preserve and
# validate those relocations: medany alone does not relocate pointer tables.
b=pathlib.Path(elf).read_bytes(); eh=struct.unpack_from('<16sHHIQQQIHHHHHH',b)
ph=[struct.unpack_from('<IIQQQQQQ',b,eh[5]+i*eh[9]) for i in range(eh[10])]
loads=[p for p in ph if p[0]==1]
if len(loads)!=1 or loads[0][2]!=0x1000 or loads[0][3]!=loads[0][4]:
    raise SystemExit('unsupported SDK relocation/load layout')
p=loads[0]; base,end=p[3],p[3]+p[5]
sh=[struct.unpack_from('<IIQQQQIIQQ',b,eh[6]+i*eh[11]) for i in range(eh[12])]
n64=0; differences=collections.Counter()
for s in sh:
    if s[1]!=4: continue
    if s[9]!=24: raise SystemExit('invalid RELA entry size')
    for off in range(s[4],s[4]+s[5],24):
        addr,info,addend=struct.unpack_from('<QQq',b,off); kind=info&0xffffffff
        if kind not in (0,2,16,17,18,19,23,24,25,35,39,43,51):
            raise SystemExit('unsupported relocation type '+str(kind))
        if kind in (35,39):
            symbols=sh[s[6]]; symbol=struct.unpack_from('<IBBHQQ',b,symbols[4]+(info>>32)*symbols[9])
            if not base<=symbol[4]<=end: raise SystemExit('non-image relative-difference symbol')
            differences[addr]+=1 if kind==35 else -1
        if kind!=2: continue
        if addr%8 or not base<=addr<=end-8: raise SystemExit('out-of-image relocation write')
        value=struct.unpack_from('<Q',b,p[2]+addr-base)[0]
        if not base<=value<=end: raise SystemExit('absolute pointer target outside image')
        n64+=1
if not n64 or any(differences.values()): raise SystemExit('missing pointer relocations or unmatched differences')
print('SDK relocation gate:',n64,'bounded image pointers; balanced relative differences')
# Explicitly audit all referenced code/data is present in file-backed LOADs.
if 'NOBITS' in run('readelf','-SW'): raise SystemExit('NOBITS section remains in device image')
print('full AAC device gate: no undefined/BSS/TLS/compressed/RVV/FMA/illegal CSR; aligned file-backed LOADs')
