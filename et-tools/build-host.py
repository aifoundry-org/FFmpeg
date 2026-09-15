#!/usr/bin/env python3
"""Run inside the full ET SDK environment (et-tools/et-env)."""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / 'build-et'
out.mkdir(exist_ok=True)
def run(args, **kwargs):
    subprocess.run(list(map(str, args)), check=True, **kwargs)
run(['cmake', '-S', root / 'et-runtime', '-B', out / 'runtime',
     '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + str(out / 'sdk')])
run(['cmake', '--build', out / 'runtime', '-j', '8'])
run(['cmake', '--install', out / 'runtime'])
env = os.environ.copy()
env['PKG_CONFIG_PATH'] = str(out / 'sdk/lib/pkgconfig') + ':' + env.get('PKG_CONFIG_PATH', '')
host = out / 'host'
host.mkdir(exist_ok=True)
run([root / 'configure', '--disable-autodetect', '--disable-x86asm', '--disable-doc',
     '--disable-debug', '--disable-everything', '--enable-ffmpeg', '--enable-etsoc',
     '--enable-hwaccel=mpeg2_et', '--enable-avdevice', '--enable-avfilter',
     '--enable-protocol=file,pipe', '--enable-indev=lavfi',
     '--enable-filter=testsrc2,format,scale,null',
     '--enable-decoder=mpeg2video,rawvideo,wrapped_avframe',
     '--enable-encoder=mpeg2video,rawvideo,wrapped_avframe',
     '--enable-parser=mpegvideo', '--enable-demuxer=mpegvideo,rawvideo',
     '--enable-muxer=mpeg2video,framemd5,rawvideo,null'], cwd=host, env=env)
run(['make', '-j', str(min(os.cpu_count() or 4, 16))], cwd=host)
print('Host FFmpeg:', host / 'ffmpeg')
