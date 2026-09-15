#!/usr/bin/env python3
"""Revalidate retained optimization evidence. Read-only: never opens ET."""
import datetime
import hashlib
import json
from pathlib import Path
import re
import statistics

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build-et'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def frames(path):
    return sum(line.startswith('0,') for line in path.read_text().splitlines())

def timing(path):
    match = re.search(r'bench: utime=([0-9.]+)s stime=([0-9.]+)s rtime=([0-9.]+)s', path.read_text())
    if not match:
        raise ValueError(f'No successful benchmark result: {path}')
    return dict(zip(('user_s', 'system_s', 'wall_s'), map(float, match.groups())))

def series(values):
    return {'samples_s': values, 'median_s': statistics.median(values),
            'min_s': min(values), 'max_s': max(values)}

final_hash = digest(BUILD / 'optimization/final.elf')
cases = []
for result in sorted((BUILD / 'silicon').glob('opt-*/result.txt')):
    folder = result.parent
    config = dict(line.split('=', 1) for line in (folder / 'config.txt').read_text().splitlines())
    gold = ROOT / config['golden']
    output = folder / 'output.md5'
    assert result.read_text().startswith('PASS silicon:')
    assert output.read_bytes() == gold.read_bytes()
    text = (folder / 'decode.log').read_text()
    assert 'NATIVE TEST RUNTIME' not in text
    assert len(re.findall(r'ET frame \d+:.*harts=', text)) == frames(gold)
    assert 'exit 0' in (folder / 'timestamps').read_text()
    for suffix in ('ce', 'uce', 'counts'):
        assert (folder / f'before.{suffix}').read_bytes() == (folder / f'after.{suffix}').read_bytes()
    # These hashes were captured BEFORE the actual launch, not inferred from
    # whatever happens to occupy a build path when this collector is run.
    provenance = (folder / 'provenance.sha256').read_text().splitlines()
    hashes = [line.split()[0] for line in provenance]
    assert hashes[2] == digest(ROOT / config['input'])
    assert hashes[3] == digest(gold)
    cases.append({'name': folder.name, 'frames': frames(output), 'harts': int(config['harts']),
                  'kernel_sha256': hashes[0], 'host_sha256': hashes[1],
                  'input_sha256': hashes[2], 'framemd5_sha256': digest(output),
                  'timing_enabled': config['timing'] == '1',
                  'cpu_affinity': config.get('cpu_affinity', 'unrestricted'),
                  'exact_framemd5': True, 'health_unchanged': True,
                  'log_sha256': digest(folder / 'decode.log'), **timing(folder / 'decode.log')})

benchmarks = {}
for clip, dimensions in [('sd', '720x576'), ('hd', '1920x1080')]:
    entry = {'dimensions': dimensions, 'frames': 250, 'samples_per_path': 5,
             'cpu_affinity': '0-15', 'includes_initialization_transfers_hashing': True,
             'discarded_samples': 0}
    for variant in ('baseline', 'final'):
        selected = [next(c for c in cases if c['name'] == f'opt-bench-{clip}-r{i}-{variant}-h64')
                    for i in range(1, 6)]
        assert all(c['frames'] == 250 and not c['timing_enabled'] for c in selected)
        entry[variant] = series([c['wall_s'] for c in selected])
        entry[variant]['system_samples_s'] = [c['system_s'] for c in selected]
    cpu = []
    for i in range(1, 6):
        prefix = BUILD / f'optimization/bench-{clip}-r{i}-cpu'
        assert prefix.with_suffix('.md5').read_bytes() == (BUILD / f'corpus/{clip}-ipb250.golden.md5').read_bytes()
        cpu.append(timing(prefix.with_suffix('.log'))['wall_s'])
    entry['cpu'] = series(cpu)
    entry['speedup_median_ratio'] = entry['baseline']['median_s'] / entry['final']['median_s']
    entry['optimized_vs_cpu_time_ratio'] = entry['final']['median_s'] / entry['cpu']['median_s']
    entry['paired_time_saved'] = series([b-f for b, f in zip(entry['baseline']['samples_s'], entry['final']['samples_s'])])
    benchmarks[clip] = entry

emu = BUILD / 'optimization/final-sysemu'
assert emu.with_suffix('.exit').read_text().strip() == '0'
assert emu.with_suffix('.cmp').read_text().strip() == '0'
assert emu.with_suffix('.md5').read_bytes() == (BUILD / 'corpus/ipb12.golden.md5').read_bytes()
assert frames(emu.with_suffix('.md5')) == 12
source_paths = sorted(p for p in (ROOT / 'et-kernels').rglob('*')
                      if p.is_file() and p.suffix in ('.c', '.h', '.S', '.ld')
                      and 'build' not in str(p.relative_to(ROOT)))
report = {
    'schema': 1, 'recorded_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'baseline_commit': '0f6deff3c33e1392d8431610ab6dc8519bf03b66',
    'baseline_kernel_sha256': digest(BUILD / 'optimization/baseline.elf'),
    'selected_kernel_sha256': final_hash,
    'scope': 'physical shire 0, mask 0x1, ordinary per-frame launches',
    'environment': {'sdk_image': 'et-soc1-dev:20260911', 'cpu': 'Intel Core i7-13700K',
                    'minion_mhz': 600, 'noc_mhz': 400,
                    'frequency_log_sha256': digest(BUILD / 'optimization/final-frequencies.log')},
    'benchmarks': benchmarks,
    'timing_caveat': 'All samples retained, including round-2 additive ~3.5 s system-time excursions in both ET arms. Medians are not stall/IPC measurements.',
    'silicon_runs': len(cases), 'silicon_frame_comparisons': sum(c['frames'] for c in cases),
    'selected_elf_frame_comparisons': sum(c['frames'] for c in cases if c['kernel_sha256'] == final_hash),
    'sysemu': {'frames': 12, 'harts': 64, 'fatal_memcheck': True,
               'exact_framemd5': True, 'log_sha256': digest(emu.with_suffix('.log'))},
    'source_sha256': {str(p.relative_to(ROOT)): digest(p) for p in source_paths},
    'cases': cases,
}
(ROOT / 'ET_OPTIMIZATION.json').write_text(json.dumps(report, indent=2) + '\n')
print(f"Verified {report['silicon_runs']} silicon runs, {report['silicon_frame_comparisons']} frame comparisons; selected ELF {report['selected_elf_frame_comparisons']}")
for name, bench in benchmarks.items():
    print(name, 'baseline', bench['baseline']['median_s'], 'optimized', bench['final']['median_s'],
          'speedup', round(bench['speedup_median_ratio'], 3), 'CPU', bench['cpu']['median_s'])
