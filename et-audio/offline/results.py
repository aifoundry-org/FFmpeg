#!/usr/bin/env python3
"""Read frozen offline evidence and emit JSON; no runtime/device interaction."""
import datetime,hashlib,json,pathlib,re,statistics
ROOT=pathlib.Path(__file__).resolve().parents[2]
BASE=ROOT/'build-et/aac-offline'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def rows(p):return [json.loads(x) for x in p.read_text().splitlines() if x.startswith('{')]
def distribution(values):return {'samples_s':values,'median_s':statistics.median(values),'min_s':min(values),'max_s':max(values)}
def collect():
    kernels={'control':sha(BASE/'selected/control.elf'),'fast':sha(BASE/'selected/fast.elf')}
    variants={v:k for k,v in kernels.items()};runs=[];groups={};counts={0:0,1:0}
    for p in sorted((BASE/'silicon').glob('*/result.txt')):
        d=p.parent;r=rows(d/'run.jsonl');c=r[0];last=r[-1]
        assert c['backend']=='silicon-shire0' and last['pass']
        assert last['exact_channel_frames']==c['channels']*c['frames']
        launches=[x for x in r if x['type']=='launch']
        assert len(launches)==c['frames']//c['chunk']
        assert [x['start'] for x in launches]==list(range(0,c['frames'],c['chunk']))
        for suffix in ('ce','uce','counts'):
            assert (d/f'before.{suffix}').read_bytes()==(d/f'after.{suffix}').read_bytes()
        prov=(d/'provenance.sha256').read_text().splitlines();variant=variants[prov[0].split()[0]]
        size=int(re.search(r'DRAM size \(B\):\s*(\d+)',(d/'run.log').read_text())[1])
        item={'name':d.name,'variant':variant,'config':c,'result':last,'device_dram_region_bytes':size,
              'provenance':prov,'jsonl_sha256':sha(d/'run.jsonl')}
        runs.append(item);counts[c['operation']]+=last['exact_channel_frames']
        if d.name.startswith('bench-') or re.match(r'fast-c\d+.*-r\d+$',d.name):
            key=f"{variant}-c{c['channels']}-k{c['chunk']}-m{c['mode']}-op{c['operation']}"
            groups.setdefault(key,[]).append(last)
    benchmarks={}
    for key,samples in groups.items():
        assert len(samples)==5,(key,len(samples))
        benchmarks[key]={field:distribution([s[field] for s in samples]) for field in (
            'pack_s','setup_s','upload_s','launch_s','completion_s','download_s',
            'state_validation_s','compare_s','resident_phase_s','stage_service_s','cold_stage_s')}
    cpu={}
    for p in sorted((BASE/'cpu-results').glob('*.json')):
        d=json.loads(p.read_text());assert d['nonfinite_samples']==0
        assert len(d['samples'])==5 and len({s['checksum'] for s in d['samples']})==1
        if d['scalar']:assert d['pcm_different_samples']==d['final_state_different_samples']==0
        cpu[p.stem]={'timing':distribution([s['elapsed_s'] for s in d['samples']]),'details':d,'sha256':sha(p)}
    assert len(cpu)==8
    return {'schema':1,'recorded_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
       'scope':'physical shire0, ordinary launches, scalar AAC synthesis; no complete AAC decoder or full-card throughput claim',
       'kernel_sha256':kernels,'runner_sha256':sha(BASE/'selected/offline-run'),
       'cpu_sha256':sha(BASE/'cpu/cpu-offline'),'capture':json.loads((BASE/'captures/stereo-48000.json').read_text()),
       'silicon_pass_runs':len(runs),'exact_synthesis_channel_frames':counts[0],'exact_copy_channel_frames':counts[1],
       'dram_region_bytes':34265366528,'maximum_test_device_bytes':max(r['config']['device_bytes'] for r in runs),
       'method':{'et':'five independent process runs per matrix arm; all samples retained',
          'cpu':'five replays on reusable threads per configuration; creation/dispatch excluded; includes barriers/copies/full output stores',
          'resident_phase':'launch/wait plus per-launch status read/check; input upload/output readback excluded',
          'stage_service':'pack + upload + resident_phase + PCM readback; setup/extra state validation/comparison excluded',
          'cold_stage':'stage_service plus runtime/load/allocation/initial-state upload; not process wall or full AAC decode',
          'workload':'512 frames/channel from two captured 48kHz stereo channels; 64 channels are independent clones'},
       'benchmarks':benchmarks,'cpu':cpu,'runs':runs,
       'failed_attempts':[
          {'path':'failed-capture/','cause':'CPU capture CLI omitted pcm_f32le output codec; no device access; retried explicitly'},
          {'path':'cpu-results-before-thread-fix/','cause':'8-worker CPU startup condition-variable lost wake; timed loop never began; no PCIe devices in container; fixed broadcast and retained files'}],
       'source_sha256':{str(p.relative_to(ROOT)):sha(p) for p in sorted((ROOT/'et-audio/offline').rglob('*')) if p.is_file() and '__pycache__' not in str(p)}}
if __name__=='__main__':print(json.dumps(collect(),indent=2,sort_keys=True))
