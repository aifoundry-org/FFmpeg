#!/usr/bin/env python3
"""Build a new typed adapter; preserve the frozen v1/video runtime sources."""
import pathlib,subprocess,sys
root=pathlib.Path(__file__).resolve().parents[2]
out=pathlib.Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
subprocess.run([sys.executable,str(root/'et-audio/runtime/generate_runtime.py'),
 '--source',str(root/'libavcodec/et_runtime.cpp'),'--output',str(out/'v1.cpp')],check=True)
s=(out/'v1.cpp').read_text()
for old,new,n in [('#include "etaac_runtime_private.h"','#include "runtime.h"',1),
                   ('ETAACParams','ETAACOfflineParams',2),
                   ('etaac_rt_launch','etaac_offline_launch',1),
                   ('etaac_params_valid','etaac_offline_valid',1)]:
 assert s.count(old)==n,(old,s.count(old));s=s.replace(old,new)
(out/'runtime.cpp').write_text(s)
# Reuse the no-device runtime tests, with v2 valid parameters and extra offset gates.
s=(root/'et-audio/runtime/tests/runtime_test.cpp').read_text()
s=s.replace('#include "etaac_runtime.h"','#include "runtime.h"')
s=s.replace('ETAACParams','ETAACOfflineParams').replace('etaac_rt_launch','etaac_offline_launch')
s=s.replace('etaac_params_valid','etaac_offline_valid').replace('ETAAC_ABI','ETAAC_OFFLINE_ABI')
assert s.count('p->generation = 1;')==1
s=s.replace('p->generation = 1;','p->generation = 1; p->capacity_frames=1; p->frames=1;')
needle='    CHECK(!etaac_offline_launch(r, &p, 1));'
assert s.count(needle)==1
s=s.replace(needle,'''    p.frame_offset=1;
    CHECK(etaac_offline_launch(r,&p,1)==-EINVAL);
    p.frame_offset=0;p.frames=513;
    CHECK(etaac_offline_launch(r,&p,1)==-EINVAL);
    p.frames=1;p.reserved2=1;
    CHECK(etaac_offline_launch(r,&p,1)==-EINVAL);
    p.reserved2=0;
'''+needle)
(out/'runtime_test.cpp').write_text(s)
