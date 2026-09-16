#!/usr/bin/env python3
"""Generate a private FFmpeg sinewin TU with payload-independent AAC constants.

The normal initializer still runs and copies coefficients into its owned arrays.
Other window lengths retain upstream computation. No AAC payload is read.
"""
import hashlib
import os
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
source, output = map(pathlib.Path, sys.argv[1:3])
cpu = root / 'build-et/aac-prototype/cpu'
lengths = (120, 128, 512, 960, 1024)
for name in ('sinewin_frozen.c', 'sinewin_tablegen.h', 'sine_windows_frozen.h'):
    if (output / name).exists():
        raise SystemExit('refusing existing generated source: ' + name)
dump = output / 'dump-sine-windows.c'
dump.write_text('''#include <stdio.h>
extern void ff_sine_window_init(float *, int);
int main(void) {
    const int lengths[] = {120,128,512,960,1024}; float window[1024];
    puts("/* Generated from pinned native FFmpeg; no packet data. */");
    for (unsigned k=0;k<sizeof(lengths)/sizeof(lengths[0]);k++) {
        int n=lengths[k]; ff_sine_window_init(window,n);
        printf("static const float frozen_sine_%d[%d] = {\\n",n,n);
        for(int i=0;i<n;i++) printf("%af,%s",(double)window[i],i%4==3?"\\n":" ");
        puts("};");
    }
    return 0;
}
''')
archives = [cpu / 'libavcodec/libavcodec.a', cpu / 'libavutil/libavutil.a']
exe = output / 'dump-sine-windows'
subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-ffp-contract=off',
                str(dump), *map(str, archives), '-lm', '-lz', '-pthread', '-o', str(exe)], check=True)
header = output / 'sine_windows_frozen.h'
with header.open('x') as f:
    subprocess.run([str(exe)], check=True, stdout=f)
original = source / 'libavcodec/sinewin_tablegen.h'
text = original.read_text()
needle = '    int i;\n    for(i = 0; i < n; i++)'
assert text.count(needle) == 1
replacement = '    int i;\n    const float *frozen = NULL;\n    switch (n) {\n'
for n in lengths:
    replacement += '    case %d: frozen = frozen_sine_%d; break;\n' % (n, n)
replacement += '    }\n    if (frozen) {\n        for (i = 0; i < n; i++) window[i] = frozen[i];\n        return;\n    }\n    for(i = 0; i < n; i++)'
text = text.replace(needle, replacement)
needle = '// Generate a sine window.\n'
assert text.count(needle) == 1
text = text.replace(needle, '#include "sine_windows_frozen.h"\n\n' + needle)
(output / 'sinewin_tablegen.h').write_text(text)
(output / 'sinewin_frozen.c').write_bytes((source / 'libavcodec/sinewin.c').read_bytes())
paths = [*archives, original, source / 'libavcodec/sinewin.c', header,
         output / 'sinewin_tablegen.h', output / 'sinewin_frozen.c', dump]
(output / 'SINE_PROVENANCE.txt').write_text(
    'Build-time native ff_sine_window_init() for lengths ' + str(lengths) + '.\n'
    'Only mathematical coefficients; no compressed input or PCM is consumed.\n'
    'Normal target initialization copies constants; other lengths compute normally.\n' +
    ''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + str(p) + '\n' for p in paths) +
    'Native dumper dynamic dependencies:\n' + subprocess.check_output(['ldd', str(exe)], text=True))
