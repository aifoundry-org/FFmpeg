#!/bin/sh
# Small independent CPU oracle; does not alter the main FFmpeg build.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
mkdir -p "$ROOT/et-kernels/build-reference"
python3 - "$ROOT" <<'PY'
import subprocess,sys
root=sys.argv[1]
subprocess.run([root+'/configure','--disable-everything','--disable-x86asm',
 '--disable-doc','--disable-debug','--disable-autodetect','--disable-network',
 '--enable-ffmpeg','--enable-protocol=file','--enable-filter=testsrc2,color,format',
 '--enable-indev=lavfi','--enable-encoder=mpeg2video,rawvideo',
 '--enable-decoder=mpeg2video,mpeg1video,wrapped_avframe',
 '--enable-parser=mpegvideo','--enable-muxer=mpeg2video,rawvideo',
 '--enable-demuxer=mpegvideo'],cwd=root+'/et-kernels/build-reference',check=True)
PY
make -C "$ROOT/et-kernels/build-reference" -j "${JOBS:-8}"
