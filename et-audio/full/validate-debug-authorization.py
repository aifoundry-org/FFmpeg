#!/usr/bin/env python3
"""Narrow user-authorized silicon diagnostic eligibility; never a PCM PASS."""
import hashlib
import json
import pathlib
import sys


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(root, elf, authorization):
    assert authorization['mode'] == 'user-authorized-numerical-debug-only'
    assert digest(elf) == authorization['elf_sha256'], 'unreviewed candidate'
    evidence = root / authorization['simulator_directory']
    evidence.resolve().relative_to((root / 'build-et/aac-full/emulator').resolve())
    required = {'actual.pcm', 'state.bin', 'run.jsonl', 'run.log', 'exit',
                'provenance.sha256', 'scope.txt'}
    assert set(authorization['artifact_sha256']) == required
    for name, expected in authorization['artifact_sha256'].items():
        assert digest(evidence / name) == expected, ('changed evidence', name)
    assert (evidence / 'exit').read_text().strip() == '1'
    assert not (evidence / 'PASS.json').exists()
    provenance = []
    for line in (evidence / 'provenance.sha256').read_text().splitlines():
        expected, name = line.split(None, 1)
        # Original host absolute paths must also resolve inside this checkout.
        relative = pathlib.Path(name).parts
        index = relative.index('build-et')
        path = root.joinpath(*relative[index:])
        path.resolve().relative_to(root.resolve())
        assert digest(path) == expected, ('changed input/binary', name)
        provenance.append(expected)
    assert len(provenance) == 4
    if 'parent_elf' in authorization:
        parent = root / authorization['parent_elf']
        assert digest(parent) == authorization['simulator_elf_sha256'] == provenance[0]
        required_objects = {'kernel.c.o', 'platform.c.o', 'arena.c.o', 'entry.o',
                            'memory.o', 'cbrt_frozen.o', 'private-source/LIBRARY_SHA256SUMS'}
        assert set(authorization['unchanged_objects']) == required_objects
        for name, expected in authorization['unchanged_objects'].items():
            assert digest(parent.parent / name) == digest(elf.parent / name) == expected
        assert authorization['fix_evidence_sha256'] and authorization['reviewed_fix']
        for name, expected in authorization['fix_evidence_sha256'].items():
            assert digest(root / name) == expected, ('changed fix evidence', name)
    else:
        assert provenance[0] == digest(elf)
    rows = [json.loads(line) for line in (evidence / 'run.jsonl').read_text().splitlines()]
    config, result = rows[0], rows[-1]
    assert config['backend'] == 'sys_emu-shire0-hart0' and config['protocol'] == 4
    assert (config['packets'], config['channels'], config['chunk'], config['meter']) == (32, 2, 16, 1)
    statuses = [x for x in rows if x['type'] == 'status']
    contexts = [x for x in rows if x['type'] == 'context_publication']
    assert [x['operation'] for x in statuses] == [0, 1, 1, 3, 2]
    assert [x['generation'] for x in statuses] == [1, 1, 17, 1, 33]
    assert [x['frames'] for x in statuses] == [0, 16, 16, 0, 0]
    assert all(x['result'] == x['heap_failures'] == x['unsupported_calls'] == 0 for x in statuses)
    assert len(contexts) == 5 and all(x['stack_guard_ok'] == 1 for x in contexts)
    assert contexts[-1]['operation'] == 2 and contexts[-1]['heap_live'] == 0
    assert result['pass'] is False
    assert result['pcm_mismatches'] == authorization['expected_pcm_mismatches'] == 407
    return {'mode': authorization['mode'], 'elf_sha256': digest(elf),
            'simulator_directory': authorization['simulator_directory'],
            'simulator_pcm_pass': False, 'simulator_lifecycle_complete': True,
            'authorization': authorization['authorization'],
            'constraints': authorization['constraints'],
            'reviewed_fix': authorization.get('reviewed_fix'),
            'simulator_elf_sha256': provenance[0]}


if __name__ == '__main__':
    root = pathlib.Path(sys.argv[1]).resolve()
    name = sys.argv[3] if len(sys.argv) == 4 else 'silicon-debug-authorization.json'
    assert name in ('silicon-debug-authorization.json', 'silicon-fixed-sines-authorization.json')
    authorization = json.loads((root / 'et-audio/full' / name).read_text())
    print(json.dumps(validate(root, root / sys.argv[2], authorization), sort_keys=True))
