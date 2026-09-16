#!/usr/bin/env python3
"""Offline regression checks using retained reviewed simulator evidence."""
import copy
import importlib.util
import json
import pathlib

root = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('eligibility', root / 'et-audio/full/validate-debug-authorization.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
elf = root / 'build-et/aac-full/device-no-mcode-v1/et_aac_full.elf'
authorization = json.loads((root / 'et-audio/full/silicon-debug-authorization.json').read_text())
assert module.validate(root, elf, authorization)['simulator_pcm_pass'] is False
for mutation in ('elf', 'evidence', 'count', 'mode', 'missing', 'directory'):
    changed = copy.deepcopy(authorization)
    if mutation == 'elf':
        changed['elf_sha256'] = '0' * 64
    elif mutation == 'evidence':
        changed['artifact_sha256']['run.jsonl'] = '0' * 64
    elif mutation == 'count':
        changed['expected_pcm_mismatches'] = 0
    elif mutation == 'mode':
        changed['mode'] = 'pass'
    elif mutation == 'directory':
        changed['simulator_directory'] = '../outside'
    else:
        changed['artifact_sha256'].pop('state.bin')
    try:
        module.validate(root, elf, changed)
    except (AssertionError, ValueError):
        print('PASS rejects', mutation)
    else:
        raise AssertionError('accepted ' + mutation)
print('PASS reviewed eligibility is explicitly NOT numerical success')
