#!/usr/bin/env python3
"""ABI4 adapter retaining hash-pinned runtime ownership and error semantics."""
import pathlib,subprocess,sys
root=pathlib.Path(__file__).resolve().parents[2];out=pathlib.Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
subprocess.run([sys.executable,str(root/'et-audio/runtime/generate_runtime.py'),'--source',str(root/'libavcodec/et_runtime.cpp'),'--output',str(out/'v1.cpp')],check=True)
s=(out/'v1.cpp').read_text()
for a,b,n in [('#include "etaac_runtime_private.h"','#include "runtime.h"',1),('ETAACParams','ETAACFullParams',2),('etaac_rt_launch','etaac_full_launch',1),('etaac_params_valid','etaac_full_valid',1)]:
 assert s.count(a)==n;s=s.replace(a,b)
(out/'runtime.cpp').write_text(s)
s=(root/'et-audio/runtime/tests/runtime_test.cpp').read_text()
for a,b in [('#include "etaac_runtime.h"','#include "runtime.h"'),('ETAACParams','ETAACFullParams'),('etaac_rt_launch','etaac_full_launch'),('etaac_params_valid','etaac_full_valid'),('ETAAC_ABI','ETAAC_FULL_ABI'),('ETAAC_SYNTH','ETAAC_FULL_INIT'),('sizeof(ETAACInput)','128'),('sizeof(ETAACState)','ETAAC_FULL_STATE_BYTES'),('ETAAC_SAMPLES','1024'),('ETAAC_HARTS * ETAAC_SCRATCH_FLOATS * sizeof(float)','(ETAAC_FULL_HEAP_BYTES+ETAAC_FULL_STACK_BYTES)'),('sizeof(ETAACStatus)','sizeof(ETAACFullStatus)'),('ETAAC_MAX_TASKS','1')]:s=s.replace(a,b)
assert s.count('p->active_harts = 64;')==1
s=s.replace('p->active_harts = 64;','p->capacity_frames = 1;')
needle='    CHECK(!etaac_full_launch(r, &p, 1));'
assert s.count(needle)==1
s=s.replace(needle,'''    p.operation=ETAAC_FULL_DECODE;p.packets=1;p.first_packet=1;
    CHECK(etaac_full_launch(r,&p,1)==-EINVAL);
    p.first_packet=0;p.generation=2;
    CHECK(etaac_full_launch(r,&p,1)==-EINVAL);
    p.generation=1;
    CHECK(etaac_full_valid(&p));
    p.flags=1;CHECK(etaac_full_launch(r,&p,1)==-EINVAL);p.flags=0;
'''+needle)
(out/'runtime_test.cpp').write_text(s)
