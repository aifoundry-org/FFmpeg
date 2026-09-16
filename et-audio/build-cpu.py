#!/usr/bin/env python3
"""Build a separate asm-enabled CPU reference. Run inside et-tools/et-env.
Requires nasm on PATH; never initializes the ET runtime or changes video outputs.
"""
import os, pathlib, subprocess, sys
root=pathlib.Path(__file__).resolve().parent.parent
out=pathlib.Path(sys.argv[1]) if len(sys.argv)>1 else root/'build-et/aac-prototype/cpu'
out.mkdir(parents=True, exist_ok=True)
flags=['--disable-autodetect','--disable-doc','--disable-everything','--disable-etsoc',
 '--disable-network','--enable-ffmpeg','--enable-ffprobe','--enable-debug=3',
 '--enable-protocol=file,pipe','--enable-indev=lavfi',
 '--enable-filter=aevalsrc,anull,aformat,aresample,sine',
 '--enable-decoder=aac,aac_fixed,pcm_f64le,pcm_f32le,pcm_s16le',
 '--enable-encoder=aac,pcm_f32le,pcm_s16le','--enable-parser=aac',
 '--enable-demuxer=aac,wav,mov','--enable-muxer=adts,wav,framemd5,null',
 '--extra-cflags=-ffp-contract=off']
subprocess.run([str(root/'configure'),*flags],cwd=out,check=True)
subprocess.run(['make','-j',os.environ.get('JOBS','12')],cwd=out,check=True)
