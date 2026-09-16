#!/usr/bin/env python3
"""Retain each full CPU sample; run inside the SDK only after competing work stops."""
import argparse,hashlib,json,pathlib,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',required=True)
p.add_argument('--cpu',default='build-et/aac-full/cpu-matched-v3/cpu/aac-full-cpu')
a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[2];out=pathlib.Path(a.output).resolve()
if out.exists():raise SystemExit('refusing existing output')
out.mkdir(parents=True)
cpu=(root/a.cpu).resolve()
inp=root/'build-et/aac-full/fixtures/stereo-48000-512.input'
records=[]
for mode in ('scalar','optimized'):
 for i,repeats in enumerate([1]*5+[5]):
  cmd=['taskset','-c','0',str(cpu),str(inp),str(repeats),mode]
  r=subprocess.run(cmd,text=True,capture_output=True)
  stem=f'{mode}-{i}'
  (out/(stem+'.stdout')).write_text(r.stdout);(out/(stem+'.stderr')).write_text(r.stderr)
  row={'command':cmd,'returncode':r.returncode,'repeats':repeats,'mode':mode}
  records.append(row)
  (out/'attempts.json').write_text(json.dumps(records,indent=2)+'\n')
  if r.returncode:raise SystemExit(r.returncode)
  row['result']=json.loads(r.stdout)
  assert row['result']['frames']==512*repeats
  assert row['result']['samples']==512*repeats*2*1024
  print(json.dumps(row),flush=True)
report={'scope':'one full stereo 48 kHz AAC-LC stream, 512 packets; CPU0; five fresh processes plus one 5-context repeat process per mode',
 'input_sha256':hashlib.sha256(inp.read_bytes()).hexdigest(),'executable_sha256':hashlib.sha256(cpu.read_bytes()).hexdigest(),'samples':records}
(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
