#!/usr/bin/env python3
"""Check pinned sheng GPU resources in compiled DTS; source evidence only."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

# The build tree links kits/sheng to the device package; resolve from the tree, not through the link.
ROOT = Path(__file__).absolute().parents[2]
pins = json.loads((ROOT / 'scripts/data/adreno-inputs.json').read_text())
tree = ROOT / '.cache/linux-dts'
for name, expected in pins['board']['sha256'].items():
    path = tree / name
    data = path.readlink().as_posix().encode() if path.is_symlink() else path.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise SystemExit('Pinned board source differs: ' + name)
output = ROOT / 'build/adreno-firmware'
output.mkdir(parents=True, exist_ok=True)
preprocessed = output / 'sheng.preprocessed.dts'
with preprocessed.open('w') as stream:
    subprocess.run(['clang', '-E', '-P', '-nostdinc', '-undef', '-D__DTS__', '-x', 'assembler-with-cpp',
                    '-I', tree / 'include', tree / 'arch/arm64/boot/dts/qcom/sm8550-xiaomi-sheng.dts'],
                   stdout=stream, check=True)
dtb = output / 'sheng.dtb'
with (output / 'dtc.log').open('w') as log:
    subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', dtb, preprocessed], stderr=log, check=True)


def prop(node, name, kind='x'):
    return subprocess.check_output(['fdtget', '-t', kind, dtb, node, name], text=True).strip()


def cells(node, name):
    return [int(word, 16) for word in prop(node, name).split()]


def require(value, message):
    if not value:
        raise SystemExit(message)


nodes = dict(gpu='/soc/gpu@3d00000', gmu='/soc/gmu@3d6a000',
             smmu='/soc/iommu@3da0000', gpucc='/soc/clock-controller@3d90000')
resources = {}
for name, node in nodes.items():
    values = cells(node, 'reg')
    require(len(values) % 4 == 0, 'Require 64-bit address/size register ranges')
    ranges = [dict(base=(values[i] << 32) | values[i + 1], bytes=(values[i + 2] << 32) | values[i + 3])
              for i in range(0, len(values), 4)]
    resources[name] = dict(node=node, compatible=prop(node, 'compatible', 's'), ranges=ranges)
require(prop(nodes['gpu'], 'status', 's') == 'okay' and
        resources['gpu']['compatible'] == 'qcom,adreno-43050a01 qcom,adreno' and
        resources['gmu']['compatible'] == 'qcom,adreno-gmu-740.1 qcom,adreno-gmu', 'Pinned enabled GPU/GMU identity')
smmu = cells(nodes['smmu'], 'phandle')[0]
require(cells(nodes['gpu'], 'iommus') == [smmu, 0, 0, smmu, 1, 0] and
        cells(nodes['gmu'], 'iommus') == [smmu, 5, 0], 'Exact GPU streams 0/1 and GMU stream 5 must bind the same Adreno SMMU')
zap = nodes['gpu'] + '/zap-shader'
require(prop(zap, 'firmware-name', 's') == 'qcom/sm8550/sheng/a740_zap.mbn', 'Real board-specific ZAP input')
reservation = '/reserved-memory/gpu-micro-code-region@9b09a000'
require(cells(reservation, 'phandle') == cells(zap, 'memory-region') and
        cells(reservation, 'reg') == [0, 0x9b09a000, 0, 0x2000], 'ZAP reservation binding')
subprocess.run(['fdtget', dtb, reservation, 'no-map'], capture_output=True, check=True)
catalog = ROOT / '.cache/adreno-source/drivers/gpu/drm/msm/adreno/a6xx_catalog.c'
require(hashlib.sha256(catalog.read_bytes()).hexdigest() ==
        pins['linux']['files']['drivers/gpu/drm/msm/adreno/a6xx_catalog.c']['sha256'], 'Pinned GPU catalog identity')
record = re.search(r'\.chip_ids = ADRENO_CHIP_IDS\(0x43050a01\)([\s\S]*?)\.preempt_record_size', catalog.read_text())
require(record and all(token in record[1] for token in
        ['ADRENO_7XX_GEN2', 'a740_sqe.fw', 'gmu_gen70200.bin', '.gmem = 3 * SZ_1M', 'a7xx_gpu_funcs']), 'Matching 740 GPU implementation and firmware catalog')
(output / 'sheng-platform.json').write_text(json.dumps(dict(
    linux_commit=pins['linux']['commit'], board_commit=pins['board']['commit'],
    compiled_dtb_sha256=hashlib.sha256(dtb.read_bytes()).hexdigest(), resources=resources,
    gpu_streams=[0, 1], gmu_stream=5, zap_reserved_bytes=8192,
    static_only=True, hardware_registers_observed=False, smmu_page_geometry_qualified=False,
    gmu_started=False, gpu_submission_qualified=False, metal_qualified=False), indent=2) + '\n')
print('PASS: pinned enabled A740 identity, GPU/GMU/SMMU resources, exact stream bindings and ZAP reservation; source evidence only')
