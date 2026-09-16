#!/usr/bin/env python3
"""Compile-only full AAC device image.  It never touches a device/runtime."""
import datetime, hashlib, os, pathlib, shutil, subprocess, sys
ROOT=pathlib.Path(__file__).resolve().parents[2]
if os.environ.get('FF_ET_ALLOW_PCIE') not in (None,'0'): raise SystemExit('FF_ET_ALLOW_PCIE must be 0')
R6=pathlib.Path(os.environ.get('ETAAC_FULL_FFMPEG_DIR',ROOT/'build-et/aac-full/ffmpeg-no-fdiv/source')).resolve()
out=ROOT/'build-et/aac-full'/('device-'+datetime.date.today().isoformat())
if '--output' in sys.argv:
    out=pathlib.Path(sys.argv[sys.argv.index('--output')+1]).resolve()
if out.exists(): raise SystemExit('refusing existing output root: '+str(out))
if not (R6/'libavcodec/libavcodec.a').is_file(): raise SystemExit('missing pinned r6 archive')
out.mkdir(parents=True); src=out/'private-source'; src.mkdir()
freeze_sines='--freeze-sine-windows' in sys.argv
owned=['freeze-sine-windows.py','protocol.h','full-native-tests.c','kernel.c','platform.c','platform.h','arena.c','arena.h','entry.S','linker.ld','build-device.py','check-device.py','arena-tests.c','README.md']
for name in owned:
    p=ROOT/'et-audio/full'/name
    if p.exists(): shutil.copy2(p,src/name)
# Freeze the numerically sensitive AAC cbrt lookup table from the pinned
# native CPU archive. This is build-time immutable table extraction only: no
# AAC packet is decoded/preprocessed on the host. Defining both symbols before
# r6 libavcodec prevents its mutable cbrt_data.o/table generator being pulled.
cpu=ROOT/'build-et/aac-prototype/cpu'
if not (cpu/'libavcodec/libavcodec.a').is_file() or not (cpu/'libavutil/libavutil.a').is_file():
    raise SystemExit('missing pinned native CPU archive required for frozen cbrt table')
dump=out/'dump-cbrt.c'; dump.write_text('''#include <stdint.h>\n#include <stdio.h>\nextern uint32_t ff_cbrt_tab[1<<13]; extern void ff_cbrt_tableinit(void);\nint main(void) { ff_cbrt_tableinit(); puts("/* CPU-frozen n7.1.1 AAC cbrt table; generated, do not edit. */\\n#include <stdint.h>\\nconst uint32_t ff_cbrt_tab[8192] = {"); for (int i=0;i<8192;i++) { if (!(i&7)) fputs(" ",stdout); printf("UINT32_C(0x%08x)%s",ff_cbrt_tab[i],i==8191?"":","); fputc((i&7)==7?'\\n':' ',stdout); } puts("};\\nvoid ff_cbrt_tableinit(void) {} "); return 0; }\n''')
dumper=out/'dump-cbrt'; subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O2','-I'+str(cpu),'-I'+str(ROOT),str(dump),str(cpu/'libavcodec/libavcodec.a'),str(cpu/'libavutil/libavutil.a'),'-lm','-lz','-pthread','-o',str(dumper)],check=True)
frozen=src/'cbrt_frozen.c'
with frozen.open('w') as f: subprocess.run([str(dumper)],check=True,stdout=f)
(src/'CBRT_PROVENANCE.txt').write_text('CPU archive: '+str(cpu/'libavcodec/libavcodec.a')+'\nCPU archive SHA256: '+hashlib.sha256((cpu/'libavcodec/libavcodec.a').read_bytes()).hexdigest()+'\nGenerated source SHA256: '+hashlib.sha256(frozen.read_bytes()).hexdigest()+'\nMethod: ff_cbrt_tableinit() then exact uint32_t[8192] emission; no AAC payload is processed.\n')
if freeze_sines:
    subprocess.run([sys.executable,str(ROOT/'et-audio/full/freeze-sine-windows.py'),str(R6),str(src)],check=True)
with (src/'SHA256SUMS').open('w') as f:
    for p in sorted(src.iterdir()):
        if p.is_file() and p.name!='SHA256SUMS': f.write(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.name+'\n')
cc='/opt/et/bin/riscv64-unknown-elf-gcc'; pre='/opt/et/bin/riscv64-unknown-elf-'
scalar=pathlib.Path(os.environ.get('ETAAC_FULL_SCALAR_LIB_DIR',ROOT/'build-et/aac-full/scalar-libs-no-mcode/lib')).resolve()
for name in ('libm.a','libc.a','libgcc.a'):
    if not (scalar/name).is_file(): raise SystemExit('missing rv64imf scalar library '+str(scalar/name))
flags=['-mno-fdiv','-fstack-usage','-O2','-std=c11','-DET_DEVICE=1','-ffreestanding','-fno-builtin','-fno-zero-initialized-in-bss','-fno-stack-protector','-mcmodel=medany','-mabi=lp64f','-march=rv64imf','-mstrict-align','-ffunction-sections','-fdata-sections','-fno-tree-vectorize','-ffp-contract=off','-fno-fast-math','-fno-strict-aliasing','-mno-riscv-attribute','-D__int64_t_defined=1','-I'+str(R6),'-I'+str(ROOT/'et-audio/full'),'-I'+str(ROOT)]
objects=[]
for name in ['kernel.c','platform.c','arena.c']:
    o=out/(name+'.o'); subprocess.run([cc,*flags,'-c',str(ROOT/'et-audio/full'/name),'-o',str(o)],check=True); objects.append(o)
o=out/'cbrt_frozen.o'; subprocess.run([cc,*flags,'-c',str(frozen),'-o',str(o)],check=True); objects.append(o)
if freeze_sines:
    # Resolve all upstream sinewin symbols before the archive is searched.
    o=out/'sinewin_frozen.o'
    subprocess.run([cc,*flags,'-U__STRICT_ANSI__','-D_XOPEN_SOURCE=600','-I'+str(R6/'libavcodec'),'-c',str(src/'sinewin_frozen.c'),'-o',str(o)],check=True)
    objects.append(o)
o=out/'entry.o'; subprocess.run([cc,*flags,'-c',str(ROOT/'et-audio/full/entry.S'),'-o',str(o)],check=True); objects.append(o)
o=out/'memory.o'; subprocess.run([cc,*flags,'-DET_WIDE_MEM=1','-c',str(ROOT/'et-kernels/src/libc.c'),'-o',str(o)],check=True); objects.append(o)
(src/'LIBRARY_SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p)+'\n' for p in [R6/'libavcodec/libavcodec.a',R6/'libavutil/libavutil.a',scalar/'libm.a',scalar/'libc.a',scalar/'libgcc.a']))
elf=out/'et_aac_full.elf'
link=[cc,'-nostartfiles','-nodefaultlibs','-march=rv64imf','-mabi=lp64f','-mcmodel=medany','-Wl,--gc-sections','-Wl,--emit-relocs','-Wl,--no-relax','-Wl,--entry=_start','-Wl,--defsym=BASE_ADDRESS=0x8005801000','-Wl,-T,'+str(ROOT/'et-audio/full/linker.ld'),'-o',str(elf),*[str(x) for x in objects],str(R6/'libavcodec/libavcodec.a'),str(R6/'libavutil/libavutil.a'),str(scalar/'libm.a'),str(scalar/'libc.a'),str(scalar/'libgcc.a')]
subprocess.run(link,check=True)
subprocess.run([sys.executable,str(ROOT/'et-audio/full/check-device.py'),str(elf),pre],check=True)
for tool,ext,args in [('objcopy','bin',['-O','binary']),('objdump','lst',['-d','-S']),('readelf','readelf',['-lW'])]:
    subprocess.run([pre+tool,*args,str(elf)] if tool!='objcopy' else [pre+tool,*args,str(elf),str(out/'et_aac_full.bin')],stdout=(out/('et_aac_full.'+ext)).open('w') if tool!='objcopy' else None,check=True)
print(out)
