#!/usr/bin/env python3
"""Collect local profile/consumer evidence only; never opens a runtime/device."""
import hashlib,json,pathlib,statistics
ROOT=pathlib.Path(__file__).resolve().parents[2]
BASE=ROOT/'build-et/aac-profile'
FIELDS=('input','imdct_fft','window_overlap','output_finite','pcm_publish')
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def stats(v):return {'samples':v,'median':statistics.median(v),'min':min(v),'max':max(v)}
def collect():
    runs=[]; groups={}; total=consumer_frames=0
    for path in sorted((BASE/'silicon').glob('*/result.txt')):
        d=path.parent; rows=[json.loads(x) for x in (d/'run.jsonl').read_text().splitlines() if x.startswith('{')]
        c,r=rows[0],rows[-1]
        assert c['backend']=='silicon-shire0' and c['protocol']==3 and r['pass']
        assert r['exact_channel_frames']==c['channels']*c['frames']
        for k in ('ce','uce','counts'):assert (d/f'before.{k}').read_bytes()==(d/f'after.{k}').read_bytes()
        phase=[x for x in rows if x['type']=='channel_ticks']
        if not c['profile_enabled']:assert all(x[k]==0 for x in phase for k in FIELDS)
        meters=[x for x in rows if x['type']=='channel_meter']
        if r['consumer']:
            assert len(meters)==c['channels'];consumer_frames+=r['exact_channel_frames']
        total+=r['exact_channel_frames']
        runs.append({'name':d.name,'config':c,'result':r,'channel_phase_samples':phase,
                     'meter_samples':meters,'provenance':(d/'provenance.sha256').read_text().splitlines(),
                     'jsonl_sha256':sha(d/'run.jsonl')})
        if d.name.startswith('bench-'):
            key=f"c{c['channels']}-p{c['profile_enabled']}-m{c['mode']}"
            groups.setdefault(key,[]).append(r)
    benchmarks={}
    for key,values in groups.items():
        assert len(values)==5
        benchmarks[key]={k:stats([v[k] for v in values]) for k in values[0] if k.endswith('_s')}
    phase0=[p for r in runs if r['name'].startswith('bench-c64-p1-m1-')
            for p in r['channel_phase_samples'] if p['channel']==0]
    assert len(phase0)==5 and all(sum(p[k] for k in FIELDS)>0 for p in phase0)
    shares={k:stats([100*p[k]/sum(p[j] for j in FIELDS) for p in phase0]) for k in FIELDS}
    dec=ROOT/'build-et/aac-decoder/rv64imf-lp64f-20260916-r6'
    assert (dec/'RESULT').read_text().startswith('PASS')
    sources=list((ROOT/'et-audio/profile').glob('*'))+list((ROOT/'et-audio/decoder').glob('*'))
    return {'schema':1,'date_utc':'2026-09-16','scope':'shire0 exact scalar synthesis + resident level meter; full AAC decoder compile-only',
        'silicon_pass_runs':len(runs),'exact_synthesis_channel_frames':total,
        'consumer_channel_frames':consumer_frames,'benchmarks_seconds':benchmarks,
        'hart0_measured_phase_share_percent':shares,
        'counter_caveat':'raw HPM3 four-read deltas; firmware configures cycle source only on selected cores; no summed cross-hart CPU-cycle claim; share denominator excludes uninstrumented glue/setup/status publication',
        'consumer_caveat':'includes conservative coefficient/state revalidation plus PCM meter; no optimized CPU meter comparison; diagnostic full PCM readback excluded from resident service but retained',
        'decoder_compile':{'evidence_root':str(dec.relative_to(ROOT)),
            'summary':(dec/'audit/SUMMARY.txt').read_text(),
            'sha256sums':(dec/'SHA256SUMS').read_text().splitlines(),
            'external_symbols':(dec/'audit/external-undefined.txt').read_text().splitlines(),
            'runnable_on_device':False},
        'selected_sha256':{p.name:sha(p) for p in sorted((BASE/'selected').glob('*')) if p.is_file() and p.name!='SHA256SUMS'},
        'sdk_reference_sha256':{str(p):sha(ROOT/p) for p in map(pathlib.Path,[
            '../et-platform/device-minion-runtime/src/MachineMinion/src/main.c',
            '../et-platform/sw-sysemu/insns/packed_float.cpp','../et-platform/sw-sysemu/insns/float.cpp'])},
        'source_sha256':{str(p.relative_to(ROOT)):sha(p) for p in sorted(sources) if p.is_file()},'runs':runs}
if __name__=='__main__':print(json.dumps(collect(),indent=2,sort_keys=True))
