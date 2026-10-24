#!/usr/bin/env python3
"""Add the GMU kit and firmware planner objects to the NativeAdreno archive."""
import json, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
c_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))
cxx_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
out = ROOT / 'build/adreno740'
out.mkdir(parents=True, exist_ok=True)
sources = [ROOT / 'kits/Sm8550Platform' / name for name in ('ShengGmu.c', 'ShengGmuCommandDb.c')]
sources += [ROOT / 'kits/sheng/ShengGmuVotes.c']
sources += [ROOT / 'kits/AdrenoFirmware' / name for name in
            ('AdrenoHfi.c', 'AdrenoHfiStartup.c', 'AdrenoHfiVotes.c', 'AdrenoFirmware.c')]
# The SMMU context/stream/live-table kit and the PM4 packet encoder the driver calls into.
sources += sorted(p for p in (ROOT / 'kits/ArmSmmu4K').glob('ArmSmmu4K*.c'))
sources += [ROOT / 'kits/AdrenoPM4/AdrenoPM4.c']
objects = []
sources += sorted(p for p in (ROOT / 'drivers/Adreno740').glob('NativeAdreno*.cpp')
                  if p.name not in ('NativeAdrenoClient.cpp', 'NativeAdrenoService.cpp', 'NativeAdrenoAccelerator.cpp'))
# NativeAdrenoAccelerator.cpp is built staged by build-adreno740-accelerator.py (its initializer must not run at boot).
# NativeAdrenoService.cpp is the archive's NativeAdreno.o from build-arm-driver.py; start from that object only.
for source in sources:
    recipe = c_recipe if source.suffix == '.c' else cxx_recipe
    obj = out / (source.stem + ('.kit.o' if source.suffix == '.c' else '.o'))
    command, i = [], 0
    while i < len(recipe['arguments']):
        value = recipe['arguments'][i]
        if value == recipe['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(obj)]
            i += 1
        elif value in ('-MF', '-MJ'):
            i += 1
        elif value == '-Xclang' and recipe['arguments'][i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        i += 1
    command += ['-I', str(ROOT / 'kits/sheng'), '-I', str(ROOT / 'kits/Sm8550Platform'), '-I', str(ROOT / 'kits/AdrenoFirmware'),
                '-I', str(ROOT / 'build/sheng-gmu'), '-I', str(ROOT / 'kits/AdrenoPM4'), '-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                '-I', str(ROOT / 'drivers/Adreno740'), '-I', str(ROOT / 'build/adreno740/include'), '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-Wno-incompatible-sysroot', '-Wno-c99-designator',
                '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier']
    result = subprocess.run(command, cwd=recipe['directory'], capture_output=True, text=True)
    if result.returncode:
        sys.exit(str(source) + ':\n' + result.stderr[-2500:])
    objects.append(obj)
# GMU and SQE firmware as strong definitions over NativeAdrenoFirmwareDefaults.cpp's weak empty ones. The bytes come
# from the ignored local copies and must match the pins in scripts/data/adreno-inputs.json; nothing here enters Git.
import hashlib
pins = json.loads((ROOT / 'scripts/data/adreno-inputs.json').read_text())['firmware']['files']
local = ROOT / '.cache/firmware/sheng-gpu'
embedded = [('_native_adreno_firmware_image', 'qcom/gmu_gen70200.bin'),
            ('_native_adreno_sqe_firmware_image', 'qcom/a740_sqe.fw'),
            ('_native_adreno_zap_image', 'qcom/sm8550/sheng/a740_zap.mbn')]
assembly = ['.section __DATA_CONST,__const']
for label, key in embedded:
    path = local / key
    blob = path.read_bytes() if path.is_file() else b''
    if len(blob) != pins[key]['bytes'] or hashlib.sha256(blob).hexdigest() != pins[key]['sha256']:
        sys.exit('firmware %s is missing or does not match its pin' % key)
    assembly += ['.p2align 12', '.globl ' + label, label + ':', '.incbin ' + json.dumps(str(path)), label + '_end:',
                 '.p2align 3', '.globl ' + label + '_size', label + '_size:', '.quad ' + label + '_end-' + label]
(out / 'firmware.S').write_text('\n'.join(assembly) + '\n')
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Wno-incompatible-sysroot', '-c', out / 'firmware.S',
                '-o', out / 'firmware.o'], check=True)
objects.append(out / 'firmware.o')
archive = out / 'libAdreno740.a'
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
defined = subprocess.run(['xcrun', 'nm', '-g', archive], capture_output=True, text=True).stdout
missing = [s for s in ('_ShengGmuColdStart', '_ShengGmuBuildStartup',
                       '_ShengGmuCommandDbInputs', '_AdrenoFirmwarePlanGmu',
                       '_ArmSmmu4KAttachStreams', '_ArmSmmu4KBindMmio', '_AdrenoPM4Type7',
                       '_NativeAdrenoGmuContextBringup')
           if (' T ' + s) not in defined and (' S ' + s) not in defined]
if missing:
    sys.exit('Required kit definitions absent: ' + ', '.join(missing))
print('PASS: %d kit objects archived with all eight required definitions present'
      % len(objects))
