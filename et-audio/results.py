#!/usr/bin/env python3
"""Collect retained AAC results only; never opens ET. JSON goes to stdout."""
import datetime,hashlib,json,pathlib,statistics
ROOT=pathlib.Path(__file__).resolve().parent.parent
BASE=ROOT/'build-et/aac-prototype'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def distribution(x):
    return {'samples_s':x,'median_s':statistics.median(x),'min_s':min(x),'max_s':max(x)}
def logs(p):return [json.loads(s) for s in p.read_text().splitlines() if s.startswith('{')]
def collect():
    runs=[];groups={};sum_synth=sum_copy=0
    for result in sorted((BASE/'silicon').glob('*/result.txt')):
        d=result.parent;rows=logs(d/'run.jsonl');config=rows[0];batches=[x for x in rows if x['type']=='batch']
        assert config['backend']=='silicon-shire0' and rows[-1]['pass']
        assert len(batches)==config['frames'] and all(x['exact'] for x in batches)
        assert rows[-1]['exact_channel_frames']==config['tasks']*config['frames']
        for suffix in ('ce','uce','counts'):assert (d/f'before.{suffix}').read_bytes()==(d/f'after.{suffix}').read_bytes()
        run={'name':d.name,'config':config,'exact_channel_frames':rows[-1]['exact_channel_frames'],
             'mean_service_s':statistics.mean(x['service_s'] for x in batches),
             'max_service_s':max(x['service_s'] for x in batches),
             'run_jsonl_sha256':sha(d/'run.jsonl'),'provenance':(d/'provenance.sha256').read_text().splitlines()}
        runs.append(run)
        if config['operation']==0:sum_synth+=run['exact_channel_frames']
        else:sum_copy+=run['exact_channel_frames']
        if d.name.startswith('bench-'):
            key=f"et_b{config['tasks']}_op{config['operation']}"
            groups.setdefault(key,[]).append(run['mean_service_s'])
    assert runs
    benchmarks={k:distribution(v) for k,v in groups.items()}
    for mode in ('optimized','scalar'):
        for count in (2,64):
            records=[json.loads(p.read_text()) for p in sorted((BASE/'cpu-stage-results').glob(f'{mode}-b{count}-r*.json'))]
            assert len(records)==5
            if mode=='scalar':assert all(r['scalar_golden_different_samples']==0 for r in records)
            benchmarks[f'cpu_{mode}_b{count}']={**distribution([r['batch_s'] for r in records]),'runs':records}
    for key,d in benchmarks.items():assert len(d['samples_s'])==5,key
    primary=['device/et_aac_synth.elf','host/etaac-run','host/etaac-replay-native',
             'cpu/ffmpeg','cpu-stage/cpu-stage','capture-src/ffmpeg']
    baseline=json.loads((BASE/'baseline-30s-20260916/timing/stereo-48000.five-run.json').read_text())
    return {'schema':1,'recorded_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
      'scope':'physical shire 0; scalar F32 AAC-LC synthesis-stage replay; not a complete offloaded decoder',
      'hardware':{'host':'i7-13700K','host_affinity':'0 (one P-core logical CPU)',
                  'shire_mask':1,'minion_boot_mhz_reported_by_device_layer':600,'sdk_image':'et-soc1-dev:20260911'},
      'method':{'et':'5 independent runs per arm; each is mean service over all 60 batches; all samples retained',
                'service':'pack + upload/wait + launch/wait + PCM/status readback/wait; excludes setup, oracle, extra state readback and comparisons',
                'cpu_stage':'5 runs, each 500 x 60 batches; warmed pinned av_tx and float_dsp; pack plus synthesis; startup excluded',
                'batch_unit':'one 1024-sample channel-synthesis frame per task; 64 tasks clone two channel timelines into 32 independent stereo states',
                'timing_instrumentation':True},
      'silicon_pass_runs':len(runs),'exact_synthesis_channel_frames':sum_synth,'exact_copy_channel_frames':sum_copy,
      'captured_fixture_records':sum(json.loads(p.read_text())['records'] for p in (BASE/'captures').glob('*.json')),'benchmarks':benchmarks,'runs':runs,
      'preflight_failure':{'path':'build-et/aac-prototype/silicon/real-mono441-h1',
                          'cause':'complete-timeline preflight rejected demux probe context; no runtime open; unchanged health',
                          'resolution':'select complete separate timelines without splicing; original marker archived in failed run'},
      'binary_sha256':{x:sha(BASE/x) for x in primary},
      'source_sha256':{str(p.relative_to(ROOT)):sha(p) for p in sorted((ROOT/'et-audio').rglob('*'))
                       if p.is_file() and '__pycache__' not in str(p) and p.name!='test_etaac_ffmpeg'},
      'cpu_full_decode_30s_stereo48':baseline}
if __name__=='__main__':print(json.dumps(collect(),indent=2,sort_keys=True))
