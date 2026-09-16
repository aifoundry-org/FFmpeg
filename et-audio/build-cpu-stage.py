#!/usr/bin/env python3
"""Build CPU stage baseline using actual pinned optimized DSP dispatch.
Run in SDK container with PCIe disabled. No device interaction.
"""
import pathlib,subprocess
root=pathlib.Path(__file__).resolve().parent.parent
out=root/'build-et/aac-prototype/cpu-stage';out.mkdir(parents=True,exist_ok=True)
source=(root/'et-audio/dsp/tests/test_etaac_ffmpeg.c').read_text()
start=source.index('static void reference_synth(')
end=source.index('\nstatic int compare(',start)
fn=source[start:end]
assert fn.count('float buf[1024], temp[128];')==1
fn=fn.replace('float buf[1024], temp[128];','_Alignas(64) float buf[1024], temp[128];')
fn=fn.replace('mul_window(', 'fdsp->vector_fmul_window(')
(out/'cpu_reference_synth.h').write_text('/* Derived from pinned AAC test reference, LGPL-2.1-or-later. */\n'+fn)
cpu=root/'build-et/aac-prototype/cpu'
subprocess.run(['cc','-std=c11','-O2','-g','-Wall','-Wextra','-ffp-contract=off',
 '-I'+str(root),'-I'+str(cpu),'-I'+str(root/'et-audio'),'-I'+str(out),
 str(root/'et-audio/cpu-stage.c'),str(cpu/'libavcodec/libavcodec.a'),
 str(cpu/'libavutil/libavutil.a'),'-lm','-pthread','-o',str(out/'cpu-stage')],check=True)
