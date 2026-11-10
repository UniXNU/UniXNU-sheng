#!/usr/bin/env python3
"""Pinned display-source audit and offline board observer qualification; no device IO."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET

sys.path.append(str(Path.cwd() / 'scripts'))
from github_download import atomic_write, fetch_bytes

ROOT = Path(__file__).resolve().parents[1]
if shutil.disk_usage(ROOT).free < 5 * 1024**3 + 128 * 1024**2:
    raise SystemExit('Preserve the 5-GiB floor and a bounded build/download allowance')
OUT = ROOT / 'build/sheng-dpu-observation'
OUT.mkdir(parents=True, exist_ok=True)
PIN = json.loads((ROOT / 'scripts/data/sheng-display-inputs.json').read_text())
KIT = ROOT / 'kits/sheng'


def source(name, dts=False):
    expected = PIN['dts_files' if dts else 'files'][name]
    path = ROOT / ('.cache/linux-dts' if dts else '.cache/display-reference') / name
    if not path.is_file():
        url = PIN['repository'].replace('https://github.com/', 'https://raw.githubusercontent.com/') + '/' + PIN['commit'] + '/' + name
        data = fetch_bytes(url, max_bytes=expected['bytes'])
        if len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
            raise SystemExit('Pinned source download identity differs: ' + name)
        atomic_write(path, data)
    data = path.read_bytes()
    if len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
        raise SystemExit('Pinned source identity differs: ' + name)
    return data.decode()


sources = {name: source(name) for name in PIN['files']}
dts = {name: source(name, True) for name in PIN['dts_files']}
base = 'drivers/gpu/drm/msm/disp/dpu1/'


def define(text, name):
    match = re.search(r'^#define\s+' + re.escape(name) + r'\s+(0x[0-9a-fA-F]+)\b', text, re.M)
    if not match:
        raise SystemExit('Source constant missing: ' + name)
    return int(match[1], 16)


def block(text, pattern):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    match = re.search(pattern + r'\s*\{', text)
    if not match:
        raise SystemExit('Source block missing: ' + pattern)
    start = match.end()
    depth = 1
    for i in range(start, len(text)):
        depth += (text[i] == '{') - (text[i] == '}')
        if depth == 0:
            return text[start:i]
    raise SystemExit('Unterminated source block')


soc = dts['arch/arm64/boot/dts/qcom/sm8550.dtsi']
mdss = block(soc, r'\bmdss:\s*[^{}]+')
mdp = block(soc, r'\bmdss_mdp:\s*[^{}]+')
reg = lambda text: [int(v, 16) for v in re.search(r'\breg\s*=\s*<0\s+(0x[0-9a-f]+)\s+0\s+(0x[0-9a-f]+)>', text).groups()]
mdss_reg, mdp_reg = reg(mdss), reg(mdp)
spi = int(re.search(r'interrupts\s*=\s*<GIC_SPI\s+(\d+)\s+IRQ_TYPE_LEVEL_HIGH\s+0>', mdss)[1])
if not re.search(r'interrupt-parent\s*=\s*<&mdss>\s*;\s*interrupts\s*=\s*<0>', mdp):
    raise SystemExit('MDP parent IRQ topology differs')
catalog = sources[base + 'catalog/dpu_9_0_sm8550.h']
intfs = block(catalog, r'static const struct dpu_intf_cfg sm8550_intf\[\]\s*=')
interfaces = []
for entry in re.findall(r'\{([^{}]+)\}', intfs):
    if not re.search(r'\.type\s*=\s*INTF_DSI\b', entry):
        continue
    interfaces.append(dict(interface=int(re.search(r'\.name\s*=\s*"intf_(\d+)"', entry)[1]),
                           base=int(re.search(r'\.base\s*=\s*(0x[0-9a-f]+)', entry)[1], 16),
                           controller=int(re.search(r'\.controller_id\s*=\s*MSM_DSI_CONTROLLER_(\d+)', entry)[1]),
                           vsync_bit=int(re.search(r'\.intr_vsync\s*=\s*DPU_IRQ_IDX\(MDP_SSPP_TOP0_INTR,\s*(\d+)\)', entry)[1])))
interfaces.sort(key=lambda x: x['controller'])
if len(interfaces) != 2 or [x['controller'] for x in interfaces] != [0, 1]:
    raise SystemExit('Expected two DSI source interfaces')
hwio = sources[base + 'dpu_hwio.h']
intf = sources[base + 'dpu_hw_intf.c']
ns = {'x': 'http://nouveau.freedesktop.org/'}
xml = ET.fromstring(sources['drivers/gpu/drm/msm/registers/display/mdss.xml'])
xmlreg = lambda name: int(xml.find('.//x:reg32[@name="' + name + '"]', ns).attrib['offset'], 16)
facts = dict(SourceMdssBytes=mdss_reg[1], SourceMdpBytes=mdp_reg[1], SourceHwVersion=xmlreg('HW_VERSION'),
             SourceMdssIrq=xmlreg('HW_INTR_STATUS'), SourceIrqEnable=define(hwio, 'INTR_EN'), SourceIrqStatus=define(hwio, 'INTR_STATUS'))
for name, original in [('Status', 'INTF_STATUS'), ('TimingEnable', 'INTF_TIMING_ENGINE_EN'), ('CountEnable', 'INTF_FRAME_LINE_COUNT_EN'),
                       ('CommandTrigger', 'INTF_DSI_CMD_MODE_TRIGGER_EN'), ('Frame', 'INTF_FRAME_COUNT'), ('Line', 'INTF_LINE_COUNT'),
                       ('Hsync', 'INTF_HSYNC_CTL'), ('Vperiod', 'INTF_VSYNC_PERIOD_F0')]:
    facts['Source' + name] = define(intf, original)
if not re.search(r'core_rev\s*=\s*readl_relaxed\(dpu_kms->mmio\s*\+\s*0x0\)', sources[base + 'dpu_kms.c']):
    raise SystemExit('MDP hardware version read differs')
if not re.search(r'core_major_ver\s*>=\s*5\)\s*s->is_en\s*=\s*DPU_REG_READ\(c, INTF_STATUS\)\s*&\s*BIT\(0\)', intf):
    raise SystemExit('Source active-status semantics differ')
if not re.search(r'DPU_REG_WRITE\(c, INTF_FRAME_LINE_COUNT_EN, 0x3\)', intf):
    raise SystemExit('Source counter-enable semantics differ')
if not re.search(r'if\s*\(!dpu_encoder_phys_vid_is_master\(phys_enc\)\)\s*goto end;', sources[base + 'dpu_encoder_phys_vid.c']):
    raise SystemExit('Source master-only VBL rule differs')
header = '#include <stdint.h>\nenum {' + ','.join('%s=0x%x' % item for item in facts.items()) + '};\n'
header += 'static const uint32_t SourceIntfBase[]={' + ','.join(hex(x['base']) for x in interfaces) + '};\n'
header += 'static const unsigned SourceVsyncBits[]={' + ','.join(str(x['vsync_bit']) for x in interfaces) + '};\n'
header += 'static const uint32_t SourceIntfOffsets[]={' + ','.join(hex(facts['Source' + x]) for x in ['Status', 'TimingEnable', 'CountEnable', 'CommandTrigger', 'Frame', 'Line', 'Hsync', 'Vperiod']) + '};\n'
(OUT / 'ShengDpuSourceFacts.h').write_text(header)
test = OUT / 'test-observation'
subprocess.run(['xcrun', 'clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-I', KIT, '-I', OUT, KIT / 'ShengDpuObservation.c', KIT / 'test-dpu-observation.c', '-o', test], check=True, timeout=60)
run = subprocess.run([test], text=True, capture_output=True, timeout=30)
(OUT / 'host.log').write_text(run.stdout + run.stderr)
if run.returncode or not run.stdout.startswith('PASS: source-bound DPU observation:'):
    raise SystemExit('DPU observer host control failed: ' + run.stdout + run.stderr)
print(run.stdout, end='')
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
flags = ['-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-O2', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-mgeneral-regs-only']
obj = OUT / 'ShengDpuObservation.o'
subprocess.run(['xcrun', 'clang', *flags, '-std=c11', '-c', KIT / 'ShengDpuObservation.c', '-o', obj], check=True, timeout=30)
raw = obj.read_bytes()
if struct.unpack_from('<4I', raw) != (0xfeedfacf, 0x100000c, 0, 1):
    raise SystemExit('Ordinary ARM64 subtype0 object required')
offset = 32
version_found = False
for _ in range(struct.unpack_from('<I', raw, 16)[0]):
    command, length = struct.unpack_from('<II', raw, offset)
    if command == 0x32:
        version_found = True
        if struct.unpack_from('<II', raw, offset + 8) != (2, 0x110000):
            raise SystemExit('iOS17 object required')
    offset += length
if not version_found:
    raise SystemExit('iOS17 build version absent')
imports = subprocess.check_output(['xcrun', 'nm', '-u', obj], text=True).split()
if set(imports) - {'_memcpy', '_memset', '___stack_chk_fail', '___stack_chk_guard'}:
    raise SystemExit('Unexpected runtime dependency: ' + repr(imports))
analysis = subprocess.run(['xcrun', 'clang', *flags, '-std=c11', '--analyze', '-Xanalyzer', '-analyzer-output=text',
                           KIT / 'ShengDpuObservation.c', '-o', OUT / 'analysis.plist'], check=True, capture_output=True, text=True, timeout=30)
if analysis.stderr.strip():
    raise SystemExit(analysis.stderr)
boundary = OUT / 'boundary.cpp'
boundary.write_text('#include "ShengDpuObservation.h"\nextern "C" ShengDpuObservationStatus boundary(const ShengDpuObservationIo *io, ShengDpuObservation *s, ShengDpuObservationResult *r) {return ShengDpuObserve(io,s,r);}\n')
boundary_obj = OUT / 'boundary.o'
subprocess.run(['xcrun', 'clang++', *flags, '-std=c++17', '-I', KIT, '-c', boundary, '-o', boundary_obj], check=True, timeout=30)
boundary_imports = subprocess.check_output(['xcrun', 'nm', '-u', boundary_obj], text=True).split()
if boundary_imports != ['_ShengDpuObserve']:
    raise SystemExit('C++ observer boundary differs: ' + repr(boundary_imports))
report = dict(evidence='pinned-source audit, host register peers and native cross-build only; no device access',
              linux_revision=PIN['commit'], source_facts=facts, candidate_mdss_resource=mdss_reg, candidate_mdp_resource=mdp_reg,
              source_mdss_gic_spi=spi, source_mdp_parent_child=0, source_interfaces=interfaces,
              observer_sha256=hashlib.sha256((KIT / 'ShengDpuObservation.c').read_bytes()).hexdigest(), imports=imports, cpp_boundary_imports=boundary_imports,
              active_hardware_qualified=False, native4k_hardware_qualified=False, logical_master_qualified=False,
              vblank_delivery_implemented=False, presentation_or_120fps_qualified=False)
(OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: source-bound DPU register sampling, ordinary ARM64/iOS17 cross-build and C++ boundary; no hardware/VBL/presentation qualification')
