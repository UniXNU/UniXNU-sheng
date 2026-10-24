#!/usr/bin/env python3
"""Check an actual bounded QEMU XNU PMGR observation, without rerunning it."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import exports, macho

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--kernel', required=True, type=Path)
p.add_argument('--observation', required=True, type=Path)
p.add_argument('--mode', required=True, choices=['off', 'lifecycle', 'publish', 'refusal', 'amfi'])
a = p.parse_args()
d = a.kernel.read_bytes()
r = json.loads(a.observation.read_text())
names = {name: va for va, labels in exports(d, macho(d)).items() for name in labels}


def require(value, message):
    if not value:
        raise SystemExit(message)


require(r['kernel_sha256'] == hashlib.sha256(d).hexdigest(), 'Wrong kernel observation')
require(not r['bounded_timeout_interrupted'], 'A timeout is not a reached checkpoint')
stop = '_IOKitInitializeTime' if a.mode == 'off' else '_panic' if a.mode == 'refusal' else '_sheng_pmgr_checkpoint'
require(int(r['pc'], 16) == (0xfffffff004aaeb54 if a.mode == 'amfi' else names[stop]),
        'Wrong actual execution checkpoint')
request = f"{names['_sheng_pmgr_observation']:x}:80"
values = struct.unpack('<16Q', bytes.fromhex(r['memory'][request]))
labels = ('stage', 'status', 'parent_size', 'own_size', 'provider', 'nub',
          'bound_provider', 'read_before', 'read_after', 'own_before', 'own_after',
          'nub_before', 'nub_after', 'attached', 'registered', 'mode')
state = dict(zip(labels, values))
if a.mode == 'off':
    require(not any(values), 'Default-off path changed the instrument state')
else:
    require(state['parent_size'] == state['own_size'] == 0x4b8a8, 'Wrong provider ABI size')
    require(state['provider'] == state['bound_provider'] and state['provider'] != 0 and state['nub'] != 0,
            'Missing genuine original nub/provider binding')
    require(state['read_before'] == state['read_after'] == 0xffffffff, 'Original method results differ')
    require(state['mode'] == {'lifecycle': 1, 'publish': 2, 'refusal': 3, 'amfi': 2}[a.mode], 'Wrong mode')
    if a.mode == 'refusal':
        require(state['stage'] == 0x100 + 170 and state['status'] == 0xe00002c7,
                'Unsupported power operation did not reach the real panic with unsupported status')
        require(state['attached'] == state['registered'] == 0, 'Refusal control must stay unpublished')
    else:
        require(state['status'] == 0, 'Runtime instrument reported failure')
        published = a.mode in ('publish', 'amfi')
        require(state['stage'] == (5 if published else 4), 'Wrong lifecycle completion stage')
        require(state['attached'] == int(published) and state['registered'] == (2 if published else 0),
                'Wrong service publication state')
        for prefix in ('own', 'nub'):
            require(state[prefix + '_after'] == state[prefix + '_before'] + int(published),
                    'Wrong retained/released class instance count')
    if a.mode == 'amfi':
        require(r['qemu_accelerator'] == 'tcg' and r['offline_qemu_vmm_present_declared'] is False,
                'AMFI physical-platform branch requires the no-VMM-property TCG fixture')
        require(int(r['x20'], 16) == state['nub'] and int(r['x21'], 16) == 0xffffffff,
                'Original AMFI must retain the genuine nub and the actual read/OR result after write returns')
registers = r['system_registers']
tcr = int(registers['TCR_EL1'], 16)
require((tcr >> 14 & 3) == 0 and (tcr >> 30 & 3) == 2, 'Active CPU translation must be native 4 KiB')
result = dict(evidence='actual bounded QEMU XNU execution', mode=a.mode,
              kernel_sha256=r['kernel_sha256'], accelerator=r['qemu_accelerator'],
              state=state, tcr_el1=registers['TCR_EL1'], hardware_execution=False,
              functional_power_backend=False, amfi_lookup_qualified=a.mode == 'amfi')
a.observation.with_name('pmgr-checked.json').write_text(json.dumps(result, indent=2) + '\n')
print(f'PASS: actual XNU PMGR {a.mode}; active CPU 4 KiB; no hardware/power-backend claim')
