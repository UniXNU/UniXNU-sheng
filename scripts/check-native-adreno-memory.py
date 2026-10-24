#!/usr/bin/env python3
"""Qualify native XNU memory code without allocating kernel memory or opening GPU."""
import hashlib
import json
import re
import shlex
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho
kit = ROOT / 'drivers/Adreno740'
out = ROOT / 'build/adreno740-memory'
out.mkdir(parents=True, exist_ok=True)
(out / 'report.json').unlink(missing_ok=True)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


# Authored host doubles are deliberately a different include tree. Native
# compilation below uses only the actual private XNU source/header snapshot.
includes = out / 'host-include'
for name in ['IOKit/IOTypes.h', 'IOKit/IOReturn.h', 'IOKit/IOBufferMemoryDescriptor.h',
             'IOKit/IOLib.h', 'kern/task.h']:
    p = includes / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text('#include "XnuMemoryFixture.h"\n')
test = out / 'test-memory'
subprocess.run(['xcrun', 'clang++', '-std=c++23', '-O2', '-Wall', '-Wextra',
                '-Werror', '-fsanitize=address,undefined', '-I', includes,
                '-I', kit / 'tests', '-I', kit, kit / 'NativeAdrenoMemory.cpp',
                kit / 'tests/test_memory.cpp', '-o', test], check=True, timeout=60)
host = subprocess.run([test], check=True, capture_output=True, text=True, timeout=30)
print(host.stdout, end='')
counts = re.search(r'(\d+) allocation profiles, (\d+) allocation/cache/lifetime failure controls, (\d+) argument/CPU refusal controls', host.stdout)
if not counts or tuple(map(int, counts.groups())) != (18, 16, 20):
    raise SystemExit('Expected ownership/refusal coverage absent')
(out / 'host.log').write_text(host.stdout)

smmu = ROOT / 'kits/ArmSmmu4K'
smmu_report_path = ROOT / 'build/arm-smmu-4k-live/report.json'
smmu_report = json.loads(smmu_report_path.read_text())
for name, expected in smmu_report['sources'].items():
    if sha(ROOT / name) != expected:
        raise SystemExit('SMMU qualification stale; rerun check-smmu4k-live.py')
host_flags = ['-O2', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined']
host_smmu_objects = []
for stem in ['ArmSmmu4KContext', 'ArmSmmu4KProgram', 'ArmSmmu4KLive', 'ArmSmmu4KStreams']:
    obj = out / (stem + '-host.o')
    subprocess.run(['xcrun', 'clang', '-std=c11', *host_flags, '-c', smmu / (stem + '.c'),
                    '-o', obj], check=True, timeout=60)
    host_smmu_objects.append(obj)
binding_test = out / 'test-smmu-memory'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', smmu,
                kit / 'NativeAdrenoMemory.cpp', kit / 'NativeAdrenoSmmuMemory.cpp',
                kit / 'tests/test_smmu_memory.cpp', *host_smmu_objects,
                '-o', binding_test], check=True, timeout=60)
binding_host = subprocess.run([binding_test], check=True, capture_output=True, text=True, timeout=30)
print(binding_host.stdout, end='')
if '34 native allocator/table-pool bindings and 4 capacity/scatter/publication/ownership refusals' not in binding_host.stdout:
    raise SystemExit('Actual native memory to native SMMU binding coverage absent')
(out / 'binding-host.log').write_text(binding_host.stdout)

# Independent interval-oracle controls, including nesting and filtered indices.
alias_test = out / 'test-vm-alias'
subprocess.run(['xcrun', 'clang++', '-std=c++17', *host_flags, '-I', kit,
                kit / 'tests/test_vm_alias.cpp', '-o', alias_test], check=True, timeout=60)
alias_host = subprocess.run([alias_test], check=True, capture_output=True, text=True, timeout=30)
print(alias_host.stdout, end='')
if '2048 oracle profiles / 524288 queries; 65536 reversed spans' not in alias_host.stdout:
    raise SystemExit('Physical alias interval coverage absent')
(out / 'vm-alias-host.log').write_text(alias_host.stdout)

# Real VM transaction code over the same software SMMU register peer.
vm_test = out / 'test-vm'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', smmu,
                kit / 'NativeAdrenoMemory.cpp', kit / 'NativeAdrenoSmmuMemory.cpp',
                kit / 'NativeAdrenoVm.cpp', kit / 'tests/test_vm.cpp',
                *host_smmu_objects, '-o', vm_test], check=True, timeout=90)
vm_host = subprocess.run([vm_test], check=True, capture_output=True, text=True, timeout=60)
print(vm_host.stdout, end='')
if '1 native map/hold/unmap transaction, 13 pre-ownership refusals, 1 scattered-span ordering and capacity case and 1 post-hold poison/retention case' not in vm_host.stdout:
    raise SystemExit('Actual native VM transaction coverage absent')
if 'VM alias index/fallback preserve attribute-class refusals before MMIO/ownership; 8192-span different-class map/unmap' not in vm_host.stdout:
    raise SystemExit('Native VM attribute-class alias controls absent')
(out / 'vm-host.log').write_text(vm_host.stdout)

# Real ring/fence code over the same software CP register peer.
ring_kind = out / 'AdrenoPM4-host.o'
subprocess.run(['xcrun', 'clang', '-std=c11', *host_flags, '-c',
                ROOT / 'kits/AdrenoPM4/AdrenoPM4.c', '-o', ring_kind], check=True, timeout=60)
ring_test = out / 'test-ring'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', ROOT / 'kits/AdrenoPM4',
                kit / 'NativeAdrenoRing.cpp', kit / 'tests/test_ring.cpp', ring_kind,
                '-o', ring_test], check=True, timeout=90)
ring_host = subprocess.run([ring_test], check=True, capture_output=True, text=True, timeout=60)
print(ring_host.stdout, end='')
if '1 exact ring programming and submission, 1 wrap submission, 1 refused re-attach, 7 write/1 read attach failures and 4 post-commit poison cases' not in ring_host.stdout:
    raise SystemExit('Actual native ring/fence coverage absent')
(out / 'ring-host.log').write_text(ring_host.stdout)

# Real execbuf code over the SMMU and CP peers.
submit_test = out / 'test-submit'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', smmu, '-I', ROOT / 'kits/AdrenoPM4',
                kit / 'NativeAdrenoMemory.cpp', kit / 'NativeAdrenoSmmuMemory.cpp',
                kit / 'NativeAdrenoVm.cpp', kit / 'NativeAdrenoRing.cpp',
                kit / 'NativeAdrenoSubmit.cpp', kit / 'tests/test_submit.cpp',
                *host_smmu_objects, ring_kind, '-o', submit_test], check=True, timeout=90)
submit_host = subprocess.run([submit_test], check=True, capture_output=True, text=True, timeout=60)
print(submit_host.stdout, end='')
if '1 execbuf submission with exact IB packet and fence, 8 pre-reference refusals, 1 duplicate-buffer reference case and 1 poisoned-commit retention case' not in submit_host.stdout:
    raise SystemExit('Actual native execbuf coverage absent')
(out / 'submit-host.log').write_text(submit_host.stdout)

# Real user-client body over the same peers and the shared wire ABI.
client_test = out / 'test-userclient'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', smmu, '-I', ROOT / 'kits/AdrenoPM4',
                kit / 'NativeAdrenoMemory.cpp', kit / 'NativeAdrenoSmmuMemory.cpp',
                kit / 'NativeAdrenoVm.cpp', kit / 'NativeAdrenoRing.cpp',
                kit / 'NativeAdrenoSubmit.cpp', kit / 'NativeAdrenoUserClient.cpp',
                kit / 'NativeAdrenoTimer.cpp', kit / 'tests/test_userclient.cpp',
                *host_smmu_objects, ring_kind, '-o', client_test], check=True, timeout=120)
client_host = subprocess.run([client_test], check=True, capture_output=True, text=True, timeout=60)
print(client_host.stdout, end='')
if '1 complete user-client transaction with pre-submit wait, 1 CPU mapping lease block, 1 queue lifecycle block, 3 metadata refusals, 1 dispatched transaction, 6 dispatch size/argument refusals, 6 allocation refusals, 7 map refusals and 1 poisoned-commit retention case, 1 two-client shared-ring block with 8 scope/window refusals, 1 late-client first-scope block' not in client_host.stdout:
    raise SystemExit('Actual native user-client coverage absent')
(out / 'userclient-host.log').write_text(client_host.stdout)

# Real GPU timestamp reader over a software counter peer.
timer_test = out / 'test-timer'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, kit / 'NativeAdrenoTimer.cpp',
                kit / 'tests/test_timer.cpp', '-o', timer_test], check=True, timeout=60)
timer_host = subprocess.run([timer_test], check=True, capture_output=True, text=True, timeout=60)
print(timer_host.stdout, end='')
if '1 derived-offset check, 2 stable reads, 1 rollover retry, 1 I/O failure and 1 bounded torn-loop timeout' not in timer_host.stdout:
    raise SystemExit('Actual native timestamp coverage absent')
(out / 'timer-host.log').write_text(timer_host.stdout)

# Real GPU lifecycle sequencing: real ring/CP/IRQ/timer, fake board power.
gpu_test = out / 'test-gpu'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', ROOT / 'kits/AdrenoPM4',
                kit / 'NativeAdrenoRing.cpp', kit / 'NativeAdrenoCpInit.cpp',
                kit / 'NativeAdrenoIrq.cpp', kit / 'NativeAdrenoTimer.cpp',
                kit / 'NativeAdrenoGpu.cpp', kit / 'tests/test_gpu.cpp',
                ring_kind, '-o', gpu_test], check=True, timeout=120)
gpu_host = subprocess.run([gpu_test], check=True, capture_output=True, text=True, timeout=60)
print(gpu_host.stdout, end='')
if '1 full lifecycle start/stop, 3 board-step failures, 3 real component failures and 6 argument refusals' not in gpu_host.stdout:
    raise SystemExit('Actual native GPU lifecycle coverage absent')
(out / 'gpu-host.log').write_text(gpu_host.stdout)

# Userspace client and kernel dispatch agree on the wire format.
client_test = out / 'test-client'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', smmu, '-I', ROOT / 'kits/AdrenoPM4',
                kit / 'NativeAdrenoMemory.cpp', kit / 'NativeAdrenoSmmuMemory.cpp',
                kit / 'NativeAdrenoVm.cpp', kit / 'NativeAdrenoRing.cpp',
                kit / 'NativeAdrenoSubmit.cpp', kit / 'NativeAdrenoUserClient.cpp',
                kit / 'NativeAdrenoTimer.cpp', kit / 'NativeAdrenoClient.cpp',
                kit / 'tests/test_client.cpp',
                *host_smmu_objects, ring_kind, '-o', client_test], check=True, timeout=120)
client_host = subprocess.run([client_test], check=True, capture_output=True, text=True, timeout=60)
print(client_host.stdout, end='')
if '1 userspace-to-kernel round trip including CPU mapping leases and queue lifecycle, 1 version mismatch, 1 altered-size rejection and 8 client-side refusals' not in client_host.stdout:
    raise SystemExit('Actual userspace client round-trip coverage absent')
(out / 'client-host.log').write_text(client_host.stdout)

# Device-tree reg parsing over the real SM8550 GPU/GMU cells.
board_test = out / 'test-board'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, kit / 'NativeAdrenoBoard.cpp',
                kit / 'tests/test_board.cpp', '-o', board_test], check=True, timeout=60)
board_host = subprocess.run([board_test], check=True, capture_output=True, text=True, timeout=60)
print(board_host.stdout, end='')
if '2 real SM8550 reg sets, 2 alternate cell formats, the real GPU iommu specifiers and 19 malformed/limit refusals' not in board_host.stdout:
    raise SystemExit('Actual device-tree parsing coverage absent')
(out / 'board-host.log').write_text(board_host.stdout)

# Kernel-grade SHA-256 and firmware validation against the pinned blob.
firmware_path = ROOT / '.cache/firmware/sheng-gpu/qcom/gmu_gen70200.bin'
firmware_pins = json.loads((ROOT / 'scripts/data/adreno-inputs.json').read_text())['firmware']['files']['qcom/gmu_gen70200.bin']
if sha(firmware_path) != firmware_pins['sha256'] or firmware_path.stat().st_size != firmware_pins['bytes']:
    raise SystemExit('Pinned GMU firmware identity differs')
sha_test = out / 'test-sha256'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, kit / 'NativeAdrenoSha256.cpp',
                kit / 'NativeAdrenoFirmware.cpp', kit / 'tests/test_sha256.cpp',
                '-o', sha_test], check=True, timeout=90)
sha_host = subprocess.run([sha_test, str(firmware_path), firmware_pins['sha256']],
                          check=True, capture_output=True, text=True, timeout=60)
print(sha_host.stdout, end='')
if '4 FIPS vectors and the pinned GMU firmware (66844 bytes) validated, 6 malformed/limit refusals' not in sha_host.stdout:
    raise SystemExit('Firmware validation coverage absent')
(out / 'sha-host.log').write_text(sha_host.stdout)

# GMU payload layout and lower-region mapping over the real allocator and VM.
gmumem_test = out / 'test-gmumem'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', smmu, '-I', ROOT / 'kits/AdrenoPM4',
                kit / 'NativeAdrenoMemory.cpp', kit / 'NativeAdrenoSmmuMemory.cpp',
                kit / 'NativeAdrenoVm.cpp', kit / 'NativeAdrenoGmuMemory.cpp',
                kit / 'tests/test_gmumem.cpp', *host_smmu_objects,
                '-o', gmumem_test], check=True, timeout=120)
gmumem_host = subprocess.run([gmumem_test], check=True, capture_output=True, text=True, timeout=60)
print(gmumem_host.stdout, end='')
if '1 fixed a6xx GMU layout allocation and release, 5 size/range refusals' not in gmumem_host.stdout:
    raise SystemExit('Actual GMU memory layout coverage absent')
(out / 'gmumem-host.log').write_text(gmumem_host.stdout)

gmumap_test = out / 'test-gmumap'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', smmu, '-I', ROOT / 'kits/AdrenoPM4',
                kit / 'NativeAdrenoMemory.cpp', kit / 'NativeAdrenoSmmuMemory.cpp',
                kit / 'NativeAdrenoVm.cpp', kit / 'NativeAdrenoGmuMemory.cpp',
                kit / 'tests/test_gmumap.cpp', *host_smmu_objects,
                '-o', gmumap_test], check=True, timeout=120)
gmumap_host = subprocess.run([gmumap_test], check=True, capture_output=True, text=True, timeout=60)
print(gmumap_host.stdout, end='')
if '1 lower-region fixed-address GMU payload mapping and unmap, 1 pre-allocation refusal and 1 repeat-map refusal' not in gmumap_host.stdout:
    raise SystemExit('Actual GMU lower-region mapping coverage absent')
(out / 'gmumap-host.log').write_text(gmumap_host.stdout)

# GMU cold-start config assembled from the validated view and payload layout.
gmuconfig_test = out / 'test-gmuconfig'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', smmu, '-I', ROOT / 'kits/sheng',
                '-I', ROOT / 'kits/AdrenoFirmware', '-I', out,
                kit / 'NativeAdrenoMemory.cpp', kit / 'NativeAdrenoSmmuMemory.cpp',
                kit / 'NativeAdrenoVm.cpp', kit / 'NativeAdrenoGmuMemory.cpp',
                kit / 'NativeAdrenoGmuConfig.cpp', kit / 'tests/test_gmuconfig.cpp',
                *host_smmu_objects, '-o', gmuconfig_test], check=True, timeout=120)
gmuconfig_host = subprocess.run([gmuconfig_test], check=True, capture_output=True, text=True, timeout=60)
print(gmuconfig_host.stdout, end='')
if '1 assembled GMU config, 7 argument refusals and 1 non-startup-profile refusal' not in gmuconfig_host.stdout:
    raise SystemExit('Actual GMU config assembly coverage absent')
(out / 'gmuconfig-host.log').write_text(gmuconfig_host.stdout)

# Five-window GMU MMIO adapter and time/hash services over fake windows.
gmuplatform_test = out / 'test-gmuplatform'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', ROOT / 'kits/sheng',
                '-I', ROOT / 'kits/AdrenoFirmware', '-I', out,
                kit / 'NativeAdrenoSha256.cpp', kit / 'NativeAdrenoGmuPlatform.cpp',
                kit / 'tests/test_gmuplatform.cpp', '-o', gmuplatform_test],
               check=True, timeout=120)
gmuplatform_host = subprocess.run([gmuplatform_test], check=True, capture_output=True, text=True, timeout=60)
print(gmuplatform_host.stdout, end='')
if '1 five-window adapter with real accesses, 11 bounds/argument refusals, 1 service binding and 1 SHA-256 vector' not in gmuplatform_host.stdout:
    raise SystemExit('Actual GMU platform adapter coverage absent')
(out / 'gmuplatform-host.log').write_text(gmuplatform_host.stdout)

# Upper-region SMMU context composition: prepare and strict step ordering.
smmuctx_test = out / 'test-smmuctx'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', smmu,
                kit / 'NativeAdrenoMemory.cpp', kit / 'NativeAdrenoSmmuMemory.cpp',
                kit / 'NativeAdrenoSmmuContext.cpp', kit / 'tests/test_smmuctx.cpp',
                *host_smmu_objects, '-o', smmuctx_test], check=True, timeout=120)
smmuctx_host = subprocess.run([smmuctx_test], check=True, capture_output=True, text=True, timeout=60)
print(smmuctx_host.stdout, end='')
if '1 prepared upper and 1 prepared lower SMMU context, 6 step-order refusals and 8 prepare refusals' not in smmuctx_host.stdout:
    raise SystemExit('Actual SMMU context composition coverage absent')
(out / 'smmuctx-host.log').write_text(smmuctx_host.stdout)

# The whole GMU context sequence from an unprogrammed SMMU (sheng ID0/ID1/sCR0) over a register peer.
mmio_object = out / 'ArmSmmu4KMmio-host.o'
subprocess.run(['xcrun', 'clang', '-std=c11', *host_flags, '-c', smmu / 'ArmSmmu4KMmio.c',
                '-o', mmio_object], check=True, timeout=60)
gmuctx_test = out / 'test-gmuctx'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', smmu,
                kit / 'NativeAdrenoMemory.cpp', kit / 'NativeAdrenoSmmuMemory.cpp',
                kit / 'NativeAdrenoVm.cpp', kit / 'NativeAdrenoGmuMemory.cpp',
                kit / 'NativeAdrenoSmmuContext.cpp', kit / 'NativeAdrenoGmuContext.cpp',
                kit / 'tests/test_gmuctx.cpp', *host_smmu_objects, mmio_object,
                '-o', gmuctx_test], check=True, timeout=120)
gmuctx_host = subprocess.run([gmuctx_test], check=True, capture_output=True, text=True, timeout=60)
print(gmuctx_host.stdout, end='')
if '1 complete GMU context bring-up with exact routes and TTBR0, 1 non-FAULT S2CR, 1 ignored context write' not in gmuctx_host.stdout:
    raise SystemExit('Actual GMU context bring-up coverage absent')
(out / 'gmuctx-host.log').write_text(gmuctx_host.stdout)

# A7xx power-up register list layout for CP_ME_INIT.
pwrup_test = out / 'test-pwrup'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, kit / 'NativeAdrenoPwrup.cpp',
                kit / 'tests/test_pwrup.cpp', '-o', pwrup_test], check=True, timeout=60)
pwrup_host = subprocess.run([pwrup_test], check=True, capture_output=True, text=True, timeout=60)
print(pwrup_host.stdout, end='')
if '1 exact power-up list image, 1 A740 live-read list, 1 capacity refusal and 5 argument/count refusals' not in pwrup_host.stdout:
    raise SystemExit('Actual power-up list coverage absent')
(out / 'pwrup-host.log').write_text(pwrup_host.stdout)

# The committed A740 preemption list must equal the pinned-catalogue derivation.
derive = subprocess.run([sys.executable, ROOT / 'scripts/derive-a740-pwrup.py', '--check'],
                        capture_output=True, text=True, timeout=60)
if derive.returncode:
    raise SystemExit('A740 power-up registers are stale: ' + derive.stderr[-3000:])
print(derive.stdout, end='')

# The IOKit surface must compile against the real source-kernel headers.
service_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
service_obj = out / 'NativeAdrenoService.o'
service_cmd = []
index = 0
while index < len(service_recipe['arguments']):
    value = service_recipe['arguments'][index]
    if value == service_recipe['file']:
        service_cmd.append(str(kit / 'NativeAdrenoService.cpp'))
    elif value == '-o':
        service_cmd.extend(['-o', str(service_obj)])
        index += 1
    elif value == '-Xclang' and service_recipe['arguments'][index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        service_cmd.append(value)
    index += 1
service_xnu = ROOT / '.cache/xnu-work'
service_cmd += ['-I', str(service_xnu / 'osfmk'), '-I', str(kit), '-I', str(smmu),
                '-I', str(ROOT / 'kits/AdrenoPM4'), '-I', str(ROOT / 'kits/sheng'),
                '-I', str(ROOT / 'kits/AdrenoFirmware'),
                *(['-I', str(ROOT / 'build/sheng-gmu')] if (ROOT / 'build/sheng-gmu/ShengGmuSource.h').exists() else []),
                *(['-I', str(ROOT / 'build/sheng-gmu')] if (ROOT / 'build/sheng-gmu/ShengGmuSource.h').exists() else []),
                '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-incompatible-sysroot',
                '-Wno-c99-designator', '-Wno-reorder-init-list',
                '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier']
service_compile = subprocess.run(service_cmd, cwd=service_recipe['directory'],
                                 capture_output=True, text=True, timeout=120)
(out / 'service.log').write_text(service_compile.stdout + service_compile.stderr)
if service_compile.returncode:
    raise SystemExit('IOKit service did not compile: ' + service_compile.stderr[-6000:])
gmu_cmd = list(service_cmd)
gmu_cmd[gmu_cmd.index(str(service_obj))] = str(out / 'NativeAdrenoGmuService.o')
gmu_source = str(kit / 'NativeAdrenoGmuService.cpp')
gmu_cmd[gmu_cmd.index(str(kit / 'NativeAdrenoService.cpp'))] = gmu_source
gmu_compile = subprocess.run(gmu_cmd, cwd=service_recipe['directory'],
                             capture_output=True, text=True, timeout=120)
(out / 'gmu-service.log').write_text(gmu_compile.stdout + gmu_compile.stderr)
if gmu_compile.returncode:
    raise SystemExit('GMU service did not compile: ' + gmu_compile.stderr[-6000:])
raw = (out / 'NativeAdrenoGmuService.o').read_bytes()
info = macho(raw)
version = next(at for command, at, _ in info['commands'] if command == 0x32)
if (struct.unpack_from('<4I', raw) != (0xfeedfacf, 0x100000c, 0, 1)
        or struct.unpack_from('<II', raw, version + 8) != (2, 0x110000)):
    raise SystemExit('GMU service is not an ordinary ARM64/iOS17 object')
gmu_defined = '\n'.join(subprocess.check_output(
    ['xcrun', 'nm', '-gUj', out / 'NativeAdrenoGmuService.o'], text=True).split())
if 'NativeAdrenoGmuService5startEP9IOService' not in gmu_defined or \
    'NativeAdrenoGmuService4stopEP9IOService' not in gmu_defined:
    raise SystemExit('GMU service methods absent from the object')
smmu_cmd = list(gmu_cmd)
smmu_cmd[smmu_cmd.index(str(out / 'NativeAdrenoGmuService.o'))] = str(out / 'NativeAdrenoSmmuService.o')
smmu_cmd[smmu_cmd.index(gmu_source)] = str(kit / 'NativeAdrenoSmmuService.cpp')
smmu_compile = subprocess.run(smmu_cmd, cwd=service_recipe['directory'],
                              capture_output=True, text=True, timeout=120)
(out / 'smmu-service.log').write_text(smmu_compile.stdout + smmu_compile.stderr)
if smmu_compile.returncode:
    raise SystemExit('SMMU service did not compile: ' + smmu_compile.stderr[-6000:])
raw = (out / 'NativeAdrenoSmmuService.o').read_bytes()
info = macho(raw)
version = next(at for command, at, _ in info['commands'] if command == 0x32)
if (struct.unpack_from('<4I', raw) != (0xfeedfacf, 0x100000c, 0, 1)
        or struct.unpack_from('<II', raw, version + 8) != (2, 0x110000)):
    raise SystemExit('SMMU service is not an ordinary ARM64/iOS17 object')
smmu_defined = '\n'.join(subprocess.check_output(
    ['xcrun', 'nm', '-gUj', out / 'NativeAdrenoSmmuService.o'], text=True).split())
if 'NativeAdrenoSmmuService5startEP9IOService' not in smmu_defined or \
    'NativeAdrenoSmmuService4stopEP9IOService' not in smmu_defined:
    raise SystemExit('SMMU service methods absent from the object')
raw = service_obj.read_bytes()
info = macho(raw)
version = next(at for command, at, _ in info['commands'] if command == 0x32)
if (struct.unpack_from('<4I', raw) != (0xfeedfacf, 0x100000c, 0, 1)
        or struct.unpack_from('<II', raw, version + 8) != (2, 0x110000)):
    raise SystemExit('IOKit service is not an ordinary ARM64/iOS17 object')
service_defined = set(subprocess.check_output(['xcrun', 'nm', '-gUj', service_obj], text=True).split())
joined = '\n'.join(service_defined)
if ('22NativeAdrenoGpuService5startEP9IOService' not in joined or
        '22NativeAdrenoGpuService13newUserClientEP4task' not in joined or
        '21NativeAdrenoGpuClient14externalMethodEjP25IOExternalMethodArguments' not in joined):
    raise SystemExit('IOKit service/client methods absent from the object')

# The userspace side must also build as an ordinary iOS userspace object.
user_obj = out / 'NativeAdrenoClient-user.o'
subprocess.run(['xcrun', 'clang++', '-target', 'arm64-apple-ios17.0', '-std=c++23', '-O2',
                '-Wall', '-Wextra', '-Werror', '-Wno-incompatible-sysroot',
                '-I', kit, '-c', kit / 'NativeAdrenoClient.cpp', '-o', user_obj],
               check=True, timeout=60)
raw = user_obj.read_bytes()
info = macho(raw)
version = next(at for command, at, _ in info['commands'] if command == 0x32)
if (struct.unpack_from('<4I', raw) != (0xfeedfacf, 0x100000c, 0, 1)
        or struct.unpack_from('<II', raw, version + 8) != (2, 0x110000)):
    raise SystemExit('Userspace client is not an ordinary ARM64/iOS17 object')
user_imports = set(subprocess.check_output(['xcrun', 'nm', '-u', user_obj], text=True).split())
if user_imports - {'___stack_chk_fail', '___stack_chk_guard', '_memcpy', '_memset'}:
    raise SystemExit('Userspace client imports unexpected symbols: ' + repr(user_imports))

# The GMU counter offset must equal the pinned XML and SM8550 DT layout.
xml = ROOT / '.cache/adreno-source/drivers/gpu/drm/msm/registers/adreno'
a6xx = (xml / 'a6xx.xml').read_text()
gmu_xml = (xml / 'a6xx_gmu.xml').read_text()
gpu_base = re.search(r'gpu: gpu@([0-9a-f]+)', (ROOT / '.cache/linux-dts/arch/arm64/boot/dts/qcom/sm8550.dtsi').read_text())
gmu_base = re.search(r'gmu: gmu@([0-9a-f]+)', (ROOT / '.cache/linux-dts/arch/arm64/boot/dts/qcom/sm8550.dtsi').read_text())
low = re.search(r'<reg32 offset="(0x[0-9a-f]+)" name="GMU_ALWAYS_ON_COUNTER_L" variants="A6XX-A7XX"', gmu_xml)
high = re.search(r'<reg32 offset="(0x[0-9a-f]+)" name="GMU_ALWAYS_ON_COUNTER_H" variants="A6XX-A7XX"', gmu_xml)
cp_counter = re.search(r'<reg64 offset="(0x[0-9a-f]+)" name="CP_ALWAYS_ON_COUNTER" variants="A6XX-A7XX"', a6xx)
if not low or not high or not gpu_base or not gmu_base or not cp_counter:
    raise SystemExit('Pinned timestamp inputs absent')
mmio_offset = int(gmu_base.group(1), 16) - int(gpu_base.group(1), 16)
expected_low = (int(low.group(1), 16) << 2) - mmio_offset
expected_high = (int(high.group(1), 16) << 2) - mmio_offset
timer_source = (kit / 'NativeAdrenoTimer.cpp').read_text()
if expected_low != 0x14220 or expected_high != 0x14224:
    raise SystemExit('Pinned GMU counter derivation differs')
if not re.search(r'TimerGmuLow = 0x%x' % expected_low, timer_source) or \
    not re.search(r'TimerGmuHigh = 0x%x' % expected_high, timer_source) or \
    int(cp_counter.group(1), 16) != 0x0980:
    raise SystemExit('Timestamp source constants differ from pinned inputs')

# Real interrupt decode over the same software register peer.
irq_test = out / 'test-irq'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', ROOT / 'kits/AdrenoPM4',
                kit / 'NativeAdrenoIrq.cpp', kit / 'tests/test_irq.cpp',
                '-o', irq_test], check=True, timeout=90)
irq_host = subprocess.run([irq_test], check=True, capture_output=True, text=True, timeout=60)
print(irq_host.stdout, end='')
if '1 exact A7XX mask programming, 10 decoded interrupt/status cases, 5 poison-on-unacknowledged cases' not in irq_host.stdout:
    raise SystemExit('Actual native interrupt decode coverage absent')
(out / 'irq-host.log').write_text(irq_host.stdout)

# Real CP init sequence over the same software CP register peer.
cpinit_test = out / 'test-cpinit'
subprocess.run(['xcrun', 'clang++', '-std=c++23', *host_flags, '-I', includes,
                '-I', kit / 'tests', '-I', kit, '-I', ROOT / 'kits/AdrenoPM4',
                kit / 'NativeAdrenoRing.cpp', kit / 'NativeAdrenoCpInit.cpp',
                kit / 'tests/test_cpinit.cpp', ring_kind, '-o', cpinit_test],
               check=True, timeout=90)
cpinit_host = subprocess.run([cpinit_test], check=True, capture_output=True, text=True, timeout=60)
print(cpinit_host.stdout, end='')
if '1 exact CP_ME_INIT sequence with SQE base, 8 pre-MMIO refusals, 1 SQE write failure and 1 ring commit failure' not in cpinit_host.stdout:
    raise SystemExit('Actual native CP init coverage absent')
(out / 'cpinit-host.log').write_text(cpinit_host.stdout)

# CP init opcodes, the SQE base register and the binning bit come from the pin.
xml = ROOT / '.cache/adreno-source/drivers/gpu/drm/msm/registers/adreno'
a6xx = (xml / 'a6xx.xml').read_text()
pm4 = (xml / 'adreno_pm4.xml').read_text()
def pm4_value(name):
    found = re.search(r'<value name="' + name + r'" value="(0x[0-9a-f]+)"', pm4)
    if not found:
        raise SystemExit('Pinned PM4 value absent: ' + name)
    return int(found.group(1), 16)
sqe = re.search(r'<reg(?:32|64) offset="(0x[0-9a-f]+)" name="CP_SQE_INSTR_BASE" variants="A6XX-A7XX"', a6xx)
if not sqe or int(sqe.group(1), 16) != 0x0830:
    raise SystemExit('Pinned CP_SQE_INSTR_BASE offset differs')
thread = re.search(r'<bitfield pos="27" name="CONCURRENT_BIN_DISABLE"', pm4)
if pm4_value('CP_ME_INIT') != 0x48 or pm4_value('CP_THREAD_CONTROL') != 0x17 or not thread:
    raise SystemExit('Pinned CP init opcodes differ')
cpinit_source = (kit / 'NativeAdrenoCpInit.cpp').read_text()
for literal in ['0x0830', '0x17', '0x48', '1u << 27', '0x14b' if '0x14b' in cpinit_source else '(1u<<0)']:
    if literal not in cpinit_source:
        raise SystemExit('CP init source literal absent: ' + literal)

# The A7XX enable set and CP error mask must equal the pinned bitfields.
xml = ROOT / '.cache/adreno-source/drivers/gpu/drm/msm/registers/adreno'
a6xx = (xml / 'a6xx.xml').read_text()
def a7xx_bits(bitset_name):
    start = a6xx.index('<bitset name="' + bitset_name + '"')
    end = a6xx.index('</bitset>', start)
    body = a6xx[start:end]
    bits = {}
    for field in re.finditer(r'<bitfield name="(\w+)" pos="(\d+)" type="boolean"(?: variants="([^"]+)")?/>', body):
        name, pos, variants = field.group(1), int(field.group(2)), field.group(3)
        if variants is None or 'A7XX' in variants:
            bits[name] = pos
    return bits
rbbm = a7xx_bits('A6XX_RBBM_INT_0_MASK')
cpint = a7xx_bits('A6XX_CP_INT')
enabled_names = ['CP_AHB_ERROR', 'RBBM_ATB_ASYNCFIFO_OVERFLOW', 'RBBM_GPC_ERROR',
    'CP_SW', 'CP_HW_ERROR', 'PM4CPINTERRUPT', 'CP_RB_DONE_TS', 'CP_CACHE_FLUSH_TS',
    'RBBM_ATB_BUS_OVERFLOW', 'RBBM_HANG_DETECT', 'UCHE_OOB_ACCESS', 'UCHE_TRAP_INTR',
    'TSBWRITEERROR', 'SWFUSEVIOLATION']
missing = [name for name in enabled_names if name not in rbbm]
if missing:
    raise SystemExit('Pinned A7XX interrupt names absent: ' + repr(missing))
expected_mask = 0
for name in enabled_names:
    expected_mask |= 1 << rbbm[name]
cp_errors = 0
for name, pos in cpint.items():
    if ('LPAC' in name or 'BV' in name or name in ('CP_OPCODE_ERROR', 'CP_UCODE_ERROR',
            'CP_HW_FAULT_ERROR', 'CP_REGISTER_PROTECTION_ERROR', 'CP_AHB_ERROR',
            'CP_VSD_PARITY_ERROR', 'CP_ILLEGAL_INSTR_ERROR')):
        cp_errors |= 1 << pos
mask_source = out / 'mask-dump.cpp'
mask_source.write_text('#include "XnuMemoryFixture.h"\n'
    '#include "NativeAdrenoIrq.h"\n'
    '#include <cstdio>\n'
    'int main(){std::printf("0x%08x 0x%08x\\n",NativeAdrenoIrq::mask(),'
    'NativeAdrenoIrq::cpErrorMask());return 0;}\n')
mask_dump = out / 'mask-dump'
subprocess.run(['xcrun', 'clang++', '-std=c++23', '-O2', '-I', includes,
                '-I', kit / 'tests', '-I', kit, kit / 'NativeAdrenoIrq.cpp', mask_source, '-o', mask_dump],
               check=True, timeout=60)
observed = subprocess.run([mask_dump], check=True, capture_output=True, text=True, timeout=30).stdout.split()
if int(observed[0], 16) != expected_mask or int(observed[1], 16) != cp_errors:
    raise SystemExit('Interrupt masks differ from the pinned bitfields')

# The ring's register/event constants must equal the pinned Linux sources.
xml = ROOT / '.cache/adreno-source/drivers/gpu/drm/msm/registers/adreno'
a6xx = (xml / 'a6xx.xml').read_text()
pm4 = (xml / 'adreno_pm4.xml').read_text()
def reg_offset(name):
    found = re.search(r'<reg(?:32|64) offset="(0x[0-9a-f]+)" name="' + name + '"', a6xx)
    if not found:
        raise SystemExit('Pinned register absent: ' + name)
    return int(found.group(1), 16)
def enum_value(name, variants):
    found = re.search(r'<value name="' + name + r'" value="(0x[0-9a-f]+)" variants="' + variants + '"', pm4)
    if not found:
        raise SystemExit('Pinned enum absent: ' + name)
    return int(found.group(1), 16)
scratch = re.search(r'<array offset="(0x[0-9a-f]+)" name="CP_SCRATCH" stride="1" length="8"', a6xx)
if not scratch:
    raise SystemExit('Pinned CP_SCRATCH array absent')
if (reg_offset('CP_RB_BASE'), reg_offset('CP_RB_CNTL'), reg_offset('CP_RB_RPTR_ADDR'),
        reg_offset('CP_RB_RPTR'), reg_offset('CP_RB_WPTR'), int(scratch.group(1), 16)) != (
        0x0800, 0x0802, 0x0804, 0x0806, 0x0807, 0x0883):
    raise SystemExit('Pinned register offsets differ from the ring source')
if enum_value('CP_EVENT_WRITE7', 'A7XX-') != 0x46 or enum_value('CACHE_CLEAN', 'A7XX-') != 0x31:
    raise SystemExit('Pinned PM4 event/opcode differ from the ring source')
ring_source = (kit / 'NativeAdrenoRing.cpp').read_text()
for literal in ['0x0800', '0x0802', '0x0804', '0x0806', '0x0807', '0x0885',
                '0x46', '0x31', '1u << 27', '1u << 31']:
    if literal not in ring_source:
        raise SystemExit('Ring source literal absent: ' + literal)

recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
record = json.loads(recipe.read_text().strip().rstrip(','))
sources = [kit / name for name in ['NativeAdrenoMemory.cpp', 'NativeAdrenoCpu.cpp', 'NativeAdrenoSmmuMemory.cpp', 'NativeAdrenoVm.cpp', 'NativeAdrenoRing.cpp', 'NativeAdrenoSubmit.cpp', 'NativeAdrenoIrq.cpp', 'NativeAdrenoCpInit.cpp', 'NativeAdrenoUserClient.cpp', 'NativeAdrenoTimer.cpp', 'NativeAdrenoGpu.cpp', 'NativeAdrenoBoard.cpp', 'NativeAdrenoSha256.cpp', 'NativeAdrenoFirmware.cpp', 'NativeAdrenoGmuMemory.cpp', 'NativeAdrenoGmuConfig.cpp', 'NativeAdrenoGmuPlatform.cpp', 'NativeAdrenoSmmuContext.cpp', 'NativeAdrenoKernelTime.cpp', 'NativeAdrenoPwrup.cpp']]
xnu = ROOT / '.cache/xnu-work'
xnu_names = ['iokit/IOKit/IOMemoryDescriptor.h', 'iokit/IOKit/IOBufferMemoryDescriptor.h',
             'iokit/Kernel/IOMemoryDescriptor.cpp', 'iokit/Kernel/IOBufferMemoryDescriptor.cpp',
             'osfmk/arm64/caches_asm.s', 'osfmk/arm/caches.c']
pins = {name: sha(xnu / name) for name in xnu_names}
# Refuse cross-worktree compiler/source paths. No shared mutable build writes.
for value in [record['directory'], record['file'], *record['arguments']]:
    for path in re.findall(r'/Users/[^\s,]+', value):
        if not path.startswith(str(ROOT) + '/'):
            raise SystemExit('Compiler recipe references another workspace')
base = []
index = 0
while index < len(record['arguments']):
    value = record['arguments'][index]
    if value == record['file']:
        base.append('@SOURCE@')
    elif value == '-o':
        base.extend(['-o', '@OUTPUT@'])
        index += 1
    elif value == '-Xclang' and record['arguments'][index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        base.append(value)
    index += 1
base += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
         '-Wno-incompatible-sysroot',
         '-Wno-c99-designator', '-Wno-reorder-init-list',
         '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier',
         '-mgeneral-regs-only', '-I', str(xnu / 'osfmk'), '-I', str(smmu),
         '-I', str(ROOT / 'kits/AdrenoPM4'), '-I', str(ROOT / 'kits/sheng'),
         '-I', str(ROOT / 'kits/AdrenoFirmware')]
objects = []
imports = {}
dependencies = {}
for source in sources:
    obj = out / (source.stem + '.o')
    command = [str(source) if v == '@SOURCE@' else str(obj) if v == '@OUTPUT@' else v for v in base]
    depfile = out / (source.stem + '.d')
    command += ['-MD', '-MF', str(depfile)]
    (out / (source.stem + '-command.json')).write_text(json.dumps(command, indent=2) + '\n')
    compiled = subprocess.run(command, cwd=record['directory'], capture_output=True,
                              text=True, timeout=60)
    (out / (source.stem + '.log')).write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        raise SystemExit(compiled.stderr[-6000:])
    raw = obj.read_bytes()
    info = macho(raw)
    version = next(at for command, at, _ in info['commands'] if command == 0x32)
    if (struct.unpack_from('<4I', raw) != (0xfeedfacf, 0x100000c, 0, 1)
            or struct.unpack_from('<II', raw, version + 8) != (2, 0x110000)):
        raise SystemExit('Ordinary ARM64 subtype0/iOS17 object required')
    imports[obj.name] = subprocess.check_output(['xcrun', 'nm', '-u', obj], text=True).split()
    objects.append(obj)
    for name in shlex.split(depfile.read_text().replace('\\\n', ' ').split(':', 1)[1]):
        path = Path(name)
        if not path.is_absolute():
            path = Path(record['directory']) / path
        path = path.resolve()
        if path.is_relative_to(includes) or path.is_relative_to(kit / 'tests'):
            raise SystemExit('Host fixture included by native compilation')
        dependencies[str(path)] = sha(path)
    analysis_command = [v for v in command if v != '-c']
    analysis_command[analysis_command.index('-o') + 1] = str(out / (source.stem + '.plist'))
    analysis_command += ['--analyze', '-Xanalyzer', '-analyzer-output=text']
    analysis = subprocess.run(analysis_command, cwd=record['directory'], capture_output=True,
                              text=True, timeout=60)
    if analysis.returncode or analysis.stderr.strip():
        raise SystemExit('Native static analysis diagnostic: ' + analysis.stderr[-6000:])
    if source.name == 'NativeAdrenoCpu.cpp':
        asm_path = out / 'NativeAdrenoCpu.s'
        cmd = [v for v in command if v != '-c']
        cmd[cmd.index('-o') + 1] = str(asm_path)
        cmd.append('-S')
        subprocess.run(cmd, cwd=record['directory'], check=True, capture_output=True, timeout=60)
        assembly = asm_path.read_text()
        for register in ['TCR_EL1', 'TTBR0_EL1', 'TTBR1_EL1', 'SCTLR_EL1', 'CTR_EL0']:
            if not re.search(r'\bmrs\s+[^\n]*' + register, assembly, re.I):
                raise SystemExit('Actual CPU/cache register observation absent: ' + register)
        for opcode in ['cvac', 'civac', 'ivac']:
            if not re.search(r'\bdc\s+' + opcode, assembly):
                raise SystemExit('Actual native PoC cache instruction absent: ' + opcode)
        if not re.search(r'\bdsb\s+sy', assembly):
            raise SystemExit('Native cache completion barrier absent')
for name, expected in pins.items():
    if sha(xnu / name) != expected:
        raise SystemExit('XNU input changed during qualification')
for name, expected in dependencies.items():
    if sha(Path(name)) != expected:
        raise SystemExit('Native compile dependency changed during qualification')
pm4_object = out / 'AdrenoPM4.o'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-O2', '-Wall', '-Wextra',
                '-Werror', '-Wno-incompatible-sysroot', '-ffreestanding', '-mgeneral-regs-only',
                '-std=c11', '-c', ROOT / 'kits/AdrenoPM4/AdrenoPM4.c', '-o', pm4_object],
               check=True, timeout=60)
smmu_objects = [smmu_report_path.parent / name for name in smmu_report['objects']]
for p in smmu_objects:
    if sha(p) != smmu_report['objects'][p.name]:
        raise SystemExit('Actual native SMMU object identity differs')
archive = out / 'libAdreno740Memory.a'
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects, *smmu_objects, pm4_object], check=True, timeout=30)
provided = set()
for obj in [*objects, *smmu_objects, pm4_object]:
    provided.update(subprocess.check_output(['xcrun', 'nm', '-gUj', obj], text=True).split())
kernel = ROOT / 'build/xnu/checkpoints/native-4k-user-abi/kernel.macho'
kernel_sha = sha(kernel)
kernel_symbols = set(subprocess.check_output(['xcrun', 'nm', '-gj', kernel], text=True).split())
all_imports = set(sum(imports.values(), []))
for obj in smmu_objects:
    all_imports.update(subprocess.check_output(['xcrun', 'nm', '-u', obj], text=True).split())
unresolved = all_imports - provided - kernel_symbols
if unresolved:
    raise SystemExit('Imports absent from actual source-kernel checkpoint: ' + repr(unresolved))
if sha(kernel) != kernel_sha:
    raise SystemExit('Source-kernel checkpoint changed during symbol check')
report = dict(
    passed=True, checker_sha256=sha(Path(__file__)),
    sources={str(p.relative_to(ROOT)): sha(p) for p in
             [*sources, kit / 'NativeAdrenoMemory.h', kit / 'NativeAdrenoSmmuMemory.h',
              kit / 'NativeAdrenoVm.h', kit / 'tests/XnuMemoryFixture.h',
              kit / 'tests/test_memory.cpp', kit / 'tests/test_smmu_memory.cpp',
              kit / 'tests/test_vm.cpp', kit / 'NativeAdrenoRing.h',
              kit / 'tests/test_ring.cpp', kit / 'NativeAdrenoSubmit.h',
              kit / 'tests/test_submit.cpp', kit / 'NativeAdrenoIrq.h',
              kit / 'tests/test_irq.cpp', kit / 'NativeAdrenoCpInit.h',
              kit / 'tests/test_cpinit.cpp', kit / 'NativeAdrenoABI.h',
              kit / 'NativeAdrenoUserClient.h', kit / 'tests/test_userclient.cpp',
              kit / 'NativeAdrenoTimer.h', kit / 'tests/test_timer.cpp',
              kit / 'NativeAdrenoStatus.h', kit / 'NativeAdrenoGpu.h',
              kit / 'NativeAdrenoClient.h', kit / 'tests/test_client.cpp',
              kit / 'NativeAdrenoBoard.h', kit / 'tests/test_board.cpp',
              kit / 'NativeAdrenoService.cpp', kit / 'NativeAdrenoGmuService.cpp',
              kit / 'NativeAdrenoSmmuService.cpp',
              kit / 'NativeAdrenoSha256.h', kit / 'NativeAdrenoFirmware.h',
              kit / 'NativeAdrenoGmuMemory.h', kit / 'tests/test_gmumem.cpp',
              kit / 'tests/test_gmumap.cpp', kit / 'NativeAdrenoGmuConfig.h',
              kit / 'tests/test_gmuconfig.cpp', kit / 'NativeAdrenoGmuPlatform.h',
              kit / 'tests/test_gmuplatform.cpp', kit / 'NativeAdrenoSmmuContext.h',
              kit / 'tests/test_smmuctx.cpp', kit / 'NativeAdrenoKernelTime.h',
              kit / 'NativeAdrenoGmuContext.h', kit / 'tests/test_gmuctx.cpp',
              kit / 'tests/test_sha256.cpp',
              kit / 'tests/test_gpu.cpp',
              ROOT / 'kits/AdrenoPM4/AdrenoPM4.c']},
    xnu_revision=subprocess.check_output(['git', '-C', xnu, 'rev-parse', 'HEAD'], text=True).strip(),
    xnu_source_sha256=pins, recipe_sha256=sha(recipe),
    native_dependency_sha256=dependencies, native_static_analysis_pass=True,
    xnu_local_patch_sha256=hashlib.sha256(subprocess.check_output(['git', '-C', xnu, 'diff', 'HEAD', '--binary'])).hexdigest(),
    host_profiles=int(counts[1]), host_failure_controls=int(counts[2]),
    host_refusal_controls=int(counts[3]), host_asan_ubsan=True,
    host_table_binding_profiles=34, host_table_binding_refusals=4,
    host_vm_transactions=1, host_vm_refusals=13, host_vm_poison_retention=1,
    host_ring_submissions=1, host_ring_wrap=1, host_ring_attach_write_failures=7,
    host_ring_attach_read_failures=1, host_ring_poison_cases=4,
    ring_constants_match_pinned_linux=True,
    host_execbuf_submissions=1, host_execbuf_refusals=8,
    host_execbuf_duplicate_reference=1, host_execbuf_poison_retention=1,
    host_irq_masks_verified_against_pinned_xml=True, host_irq_decode_cases=10,
    host_irq_poison_cases=5, host_cp_init_sequences=1, host_cp_init_refusals=8,
    host_cp_init_write_failure=1, host_cp_init_commit_failure=1,
    host_userclient_transactions=1, host_userclient_alloc_refusals=6,
    host_userclient_map_refusals=7, host_userclient_submit_wait_refusals=6,
    host_userclient_poison_retention=1, abi_layout_static_asserted=True,
    host_timestamp_derivation_verified=True, host_timestamp_reads=2,
    host_timestamp_rollover_retry=1, host_timestamp_bounded_timeout=1,
    host_gpu_lifecycle_start=1, host_gpu_board_failures=3,
    host_gpu_component_failures=3, host_gpu_argument_refusals=6,
    host_gpu_start_requires_observed_fence=True,
    host_client_round_trip=1, host_client_version_mismatch=1,
    host_client_altered_records=1, host_client_side_refusals=8,
    userspace_client_ordinary_arm64_ios17=True,
    host_board_reg_sets=2, host_board_format_cases=2, host_board_refusals=11,
    iokit_service_compiles_arm64_ios17=True, iokit_service_publishes_nothing=True,
    gmu_service_compiles_arm64_ios17=True, gmu_service_maps_without_access=True,
    smmu_service_compiles_arm64_ios17=True, smmu_service_publishes_cells=True,
    firmware_sha256_vectors=4, firmware_pinned_blob_verified=True,
    firmware_refusal_cases=6, gmu_layout_allocations=1, gmu_layout_refusals=6,
    gmu_upper_mappings=5, gmu_upper_map_refusals=2,
    gmu_configs_assembled=1, gmu_config_refusals=8,
    gmu_windows_bound=5, gmu_window_refusals=11,
    smmu_contexts_prepared=1, smmu_context_step_refusals=6,
    smmu_context_prepare_refusals=7, kernel_time_bound=True,
    embedded_firmware_gate_wired=True, pwrup_list_packed=1,
    pwrup_list_refusals=6, firmware_archive_images=3,
    pwrup_registers_derived=16, pwrup_registers_variant_exact=True,
    pwrup_live_read_list=1, pwrup_read_failure=1,
    smmu_qualification_sha256=sha(smmu_report_path),
    source_kernel_sha256=kernel_sha, source_kernel_imports_resolved=True,
    ordinary_arm64_ios17=True, general_registers_only=True,
    native_cpu_register_reads_present=True, native_poc_cache_instructions_present=True,
    objects={p.name: sha(p) for p in objects}, archive_sha256=sha(archive),
    undefined_imports=imports, native_kernel_allocation_executed=False,
    cpu_registers_actually_observed=False, physical_ownership_qualified=False,
    hardware_cache_qualified=False, gpu_mapping_qualified=False,
    gpu_execution_qualified=False, genuine_fences_qualified=False,
    system_metal_device_published=False)
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: real XNU allocator/raw PA/retained lifetime code cross-builds for ARM64/iOS17; actual PoC instructions present; host doubles only')
