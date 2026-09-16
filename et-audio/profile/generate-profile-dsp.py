#!/usr/bin/env python3
"""Generate the instrumented exact DSP TU, refusing any base-source drift."""
import hashlib
from pathlib import Path
import sys

PINNED_SHA256 = "9b51aea77bcd7c936c442b3c304f79439674d32cecd84b3f736a0fed7881aa06"

def once(s, old, new, what):
    n=s.count(old)
    if n != 1: raise ValueError(f"{what}: expected one match, got {n}")
    return s.replace(old,new,1)

def main():
    if len(sys.argv)!=3: raise SystemExit("usage: generate-profile-dsp.py BASE OUTPUT")
    base,out=map(Path,sys.argv[1:])
    data=base.read_bytes()
    got=hashlib.sha256(data).hexdigest()
    if got!=PINNED_SHA256: raise ValueError(f"refusing DSP source drift: expected {PINNED_SHA256}, got {got}")
    s=data.decode()
    s=once(s,'#include "synth.h"','#include "profile_synth.h"',"profile include")
    s=once(s,'int etaac_synth(float *out1024, float *saved512, const float *coeff1024,\n                unsigned sequence, unsigned previous_sequence,\n                unsigned shape, unsigned previous_shape, float *scratch)',
           'int etaac_profile_synth(float *out1024, float *saved512, const float *coeff1024,\n                        unsigned sequence, unsigned previous_sequence,\n                        unsigned shape, unsigned previous_shape, float *scratch,\n                        ETAACProfileMeasure *measure)',"public synth symbol")
    s=once(s,'    const int long_to_long =\n', '    uint64_t phase_start = 0;\n    const int long_to_long =\n',"phase local")
    s=once(s,'    if (sequence == ETAAC_EIGHT_SHORT) {\n        for (unsigned i = 0; i < 1024; i += 128)',
           '    if (measure && measure->enabled)\n        phase_start = et_cycles();\n    if (sequence == ETAAC_EIGHT_SHORT) {\n        for (unsigned i = 0; i < 1024; i += 128)',"transform start")
    s=once(s,'    if (long_to_long) {',
           '    if (measure && measure->enabled)\n        measure->imdct_fft_ticks += et_cycles() - phase_start;\n    if (measure && measure->enabled)\n        phase_start = et_cycles();\n\n    if (long_to_long) {',"window start")
    tail='    return 0;\n}\n'
    end=s.rfind(tail)
    if end < 0: raise ValueError("window end: final return missing")
    s=s[:end]+'    if (measure && measure->enabled)\n        measure->window_overlap_ticks += et_cycles() - phase_start;\n'+s[end:]
    out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text(s)
if __name__ == '__main__': main()
