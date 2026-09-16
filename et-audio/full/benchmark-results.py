#!/usr/bin/env python3
"""Read-only verification/collection of the completed full AAC benchmark."""
import hashlib
import json
import pathlib
import statistics

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASE = ROOT / 'build-et/aac-full'
OUT = ROOT / 'et-audio/full/BENCHMARKS.json'
if OUT.exists():
    raise SystemExit('refusing existing committed benchmark inventory')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stats(values):
    return {'median': statistics.median(values), 'min': min(values),
            'max': max(values), 'samples': values}


manifest = json.loads((BASE / 'fixtures/MANIFEST.json').read_text())
fixture = next(x for x in manifest['cases'] if x['name'] == 'stereo-48000-512')
golden = BASE / 'fixtures' / fixture['scalar_pcm']
assert sha(golden) == fixture['scalar_pcm_sha256']
cpu = json.loads((BASE / 'cpu-benchmark-matched-v3/results.json').read_text())
assert cpu['input_sha256'] == fixture['input_sha256']
assert cpu['executable_sha256'] == sha(BASE / 'cpu-matched-v3/cpu/aac-full-cpu')
cs = {}
for mode in ('scalar', 'optimized'):
    records = [x for x in cpu['samples'] if x['mode'] == mode]
    assert len(records) == 6
    for sample in records:
        r = sample['result']
        assert sample['returncode'] == 0 and r['matched_features'] is True
        assert r['frames'] == 512 * sample['repeats'] and r['samples'] == 1048576 * sample['repeats']
    fresh = [x['result'] for x in records if x['repeats'] == 1]
    assert len(fresh) == 5
    cs[mode] = {k: stats([r[k] for r in fresh]) for k in
                ('setup_table_context_s', 'decode_output_store_s', 'decoder_close_s', 'total_s')}

silicon = []
es = {}
elf = BASE / 'device-frozen-sines-v2/et_aac_full.elf'
for mode, meter in (('pcm', 0), ('meter', 1)):
    records = []
    for index in range(1, 6):
        path = BASE / 'silicon' / f'bench-fixed-sines-{mode}-{index}'
        rows = [json.loads(x) for x in (path / 'run.jsonl').read_text().splitlines()]
        cfg, result = rows[0], rows[-1]
        assert cfg['backend'] == 'silicon-shire0-hart0' and cfg['protocol'] == 4
        assert (cfg['packets'], cfg['channels'], cfg['sample_rate'], cfg['chunk'], cfg['meter']) == (512, 2, 48000, 512, meter)
        assert result['pass'] is True and result['pcm_mismatches'] == 0
        assert result['exact_packet_frames'] == 512 and result['exact_channel_frames'] == 1024
        assert sha(path / 'actual.pcm') == fixture['scalar_pcm_sha256']
        assert (path / 'timestamps').read_text().rstrip().endswith('exit 0')
        for kind in ('ce', 'uce', 'counts'):
            assert (path / ('before.' + kind)).read_bytes() == (path / ('after.' + kind)).read_bytes()
        statuses = [x for x in rows if x['type'] == 'status']
        contexts = [x for x in rows if x['type'] == 'context_publication']
        assert [x['operation'] for x in statuses] == ([0, 1, 3, 2] if meter else [0, 1, 2])
        assert all(x['result'] == x['heap_failures'] == x['unsupported_calls'] == 0 for x in statuses)
        assert len(contexts) == len(statuses) and all(x['stack_guard_ok'] == 1 for x in contexts)
        assert contexts[-1]['heap_live'] == 0
        expected_hashes = [sha(elf), sha(BASE / 'host/full-run'), fixture['input_sha256'], fixture['scalar_pcm_sha256']]
        assert [x.split()[0] for x in (path / 'provenance.sha256').read_text().splitlines()] == expected_hashes
        decode = next(x for x in statuses if x['operation'] == 1)
        decode_context = next(x for x in contexts if x['operation'] == 1)
        # CPU scalar's fused statistics are checked against the exact ET scalar PCM.
        scalar = next(x['result'] for x in cpu['samples'] if x['mode'] == 'scalar' and x['repeats'] == 1)
        assert decode['consumer_peak_bits'] == scalar['consumer_peak_bits']
        assert decode['consumer_above_one'] == scalar['consumer_above_one']
        result = dict(result)
        result['codec_lifecycle_s'] = sum(result[k] for k in ('decoder_init_s', 'decode_s', 'decoder_close_s'))
        result['cold_service_s'] = sum(result[k] for k in ('runtime_setup_s', 'decoder_init_s', 'init_status_io_s', 'transfer_inclusive_service_s', 'decoder_close_s', 'close_status_io_s'))
        result['decode_ticks'] = decode['decode_ticks']
        result['pcm_publication_ticks'] = decode['publication_ticks']
        result['decode_context_publication_ticks'] = decode_context['ticks']
        sample = {'path': str(path.relative_to(ROOT)), 'mode': mode, 'result': result,
                  'statuses': statuses, 'contexts': contexts,
                  'artifact_sha256': {p.name: sha(p) for p in path.iterdir() if p.is_file()}}
        silicon.append(sample)
        records.append(result)
    keys = ('runtime_setup_s', 'decoder_init_s', 'decode_s', 'decoder_close_s',
            'upload_s', 'pcm_readback_s', 'consumer_s', 'init_status_io_s',
            'decode_status_io_s', 'close_status_io_s', 'consumer_status_io_s',
            'resident_service_s', 'transfer_inclusive_service_s', 'codec_lifecycle_s',
            'cold_service_s', 'decode_ticks', 'pcm_publication_ticks', 'decode_context_publication_ticks')
    es[mode] = {k: stats([r[k] for r in records]) for k in keys}

ratios = {}
for mode in cs:
    ratios[mode] = {
        'ET_over_CPU_decode_time': es['pcm']['decode_s']['median'] / cs[mode]['decode_output_store_s']['median'],
        'ET_over_CPU_codec_lifecycle_time': es['pcm']['codec_lifecycle_s']['median'] / cs[mode]['total_s']['median']}
report = {
    'date_utc': '2026-09-16',
    'scope': 'Full compressed AAC-LC, one stereo48k stream, 512 packets/1048576 float samples; ET shire0/hart0 versus CPU0; five fresh processes per mode. CPU samples ran separately from all builds, simulation and device work.',
    'audio_duration_s': 512 * 1024 / 48000,
    'input_sha256': fixture['input_sha256'], 'scalar_pcm_sha256': fixture['scalar_pcm_sha256'],
    'elf_sha256': sha(elf), 'runner_sha256': sha(BASE / 'host/full-run'),
    'cpu_environment': (BASE / 'cpu-matched-v3/environment.txt').read_text(),
    'cpu_samples': cpu, 'cpu_fresh_statistics_s': cs, 'silicon_samples': silicon,
    'silicon_statistics': es, 'ratios': ratios,
    'optimized_reference': json.loads((BASE / 'cpu-matched-v3/correctness/comparison.json').read_text()),
    'caveats': [
        'CPU and ET both decode, validate finiteness, calculate fused peak/above-one, and store complete planar PCM. CPU checksums are additionally observed.',
        'The strict ET oracle is scalar CPU PCM. Optimized native FFmpeg is unchanged from its previous reference but not scalar-bit-exact; no tolerance or oracle substitution is used.',
        'CPU file load/envelope check/output-buffer allocation/file writing are outside codec timers. Device buffers/image/runtime are separately timed; device compressed upload and PCM download are separately retained.',
        'CPU uses ADTS discovery on first packet, target uses ASC during INIT; setup/decode phase boundaries are not identical. Codec-lifecycle subtotals include setup, decoding and teardown on each side.',
        'ET launch times include SDK dispatch/wait and publication. decode_ticks includes PCM publication; never add those overlapping counters. Context publication is separate. HPM3 readings are raw ticks, not assumed clock-calibrated seconds.',
        'METER retrieves features accumulated during decode; it is not a separate PCM scan. Meter-service total excludes diagnostic PCM download, which is still done for exact validation.',
        'Cold ET service includes runtime/image/buffers, codec INIT/CLOSE, input upload, status I/O and requested output. Diagnostic private-state download and golden comparison are excluded.',
        'CPU five-context extra processes are retained separately and not pooled into the five-fresh-process medians.',
        'This measures a scalar single-hart prototype, not whole-card performance or a hardware peak. Earlier synthesis-only multicore workloads are not equivalent.',
        'Corrected ELF hardware validation used explicit user-authorized reviewed-child diagnostic eligibility. The parent emulator PCM failed; no corrected-emulator PASS is claimed.'
    ]}
OUT.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'ratios': ratios, 'ET_pcm_cold_service_median_s': es['pcm']['cold_service_s']['median']}, indent=2))
