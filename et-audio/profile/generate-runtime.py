#!/usr/bin/env python3
"""Generate the ABI-3 private adapter from the hash-pinned video runtime."""
import pathlib, subprocess, sys
root=pathlib.Path(__file__).resolve().parents[2]
out=pathlib.Path(sys.argv[1]); out.mkdir(parents=True,exist_ok=True)
# The delegated generator verifies libavcodec/et_runtime.cpp's exact SHA-256.
subprocess.run([sys.executable,str(root/'et-audio/runtime/generate_runtime.py'), '--source',str(root/'libavcodec/et_runtime.cpp'),'--output',str(out/'v1.cpp')],check=True)
s=(out/'v1.cpp').read_text()
for old,new,count in [('#include "etaac_runtime_private.h"','#include "runtime.h"',1),
                      ('ETAACParams','ETAACProfileParams',2),
                      ('etaac_rt_launch','etaac_profile_launch',1),
                      ('etaac_params_valid','etaac_profile_valid',1)]:
    if s.count(old)!=count: raise ValueError(f'{old}: expected {count}, got {s.count(old)}')
    s=s.replace(old,new)
(out/'runtime.cpp').write_text(s)
t=(root/'et-audio/runtime/tests/runtime_test.cpp').read_text()
t=t.replace('#include "etaac_runtime.h"','#include "runtime.h"')
t=t.replace('ETAACParams','ETAACProfileParams').replace('ETAACStatus','ETAACProfileStatus')
t=t.replace('etaac_rt_launch','etaac_profile_launch').replace('etaac_params_valid','etaac_profile_valid').replace('ETAAC_ABI','ETAAC_PROFILE_ABI')
if t.count('p->generation = 1;')!=1: raise ValueError('test generation setup drift')
t=t.replace('p->generation = 1;','p->generation = 1; p->capacity_frames = 1; p->frames = 1;')
needle='    CHECK(!etaac_profile_launch(r, &p, 1));'
if t.count(needle)!=1: raise ValueError('test launch drift')
t=t.replace(needle,'''    p.profile_enable = 2;
    CHECK(!etaac_profile_valid(&p));
    CHECK(etaac_profile_launch(r, &p, 1) == -EINVAL);
    p.profile_enable = 0;
    p.frames = 2;
    CHECK(!etaac_profile_valid(&p));
    p.frames = 1;
'''+needle)
(out/'runtime_test.cpp').write_text(t)
