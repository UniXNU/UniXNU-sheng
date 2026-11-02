#!/usr/bin/env python3
"""Exercise ring packets, preemption and queue ordering against host register peers."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
kit = ROOT / 'drivers/Adreno740'
out = ROOT / 'build/adreno740-scheduling'
inc = out / 'include'
for name in ['IOKit/IOTypes.h', 'IOKit/IOReturn.h', 'IOKit/IOBufferMemoryDescriptor.h', 'IOKit/IOLib.h', 'kern/task.h']:
    f = inc / name
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text('#include "XnuMemoryFixture.h"\n')
flags = ['-O2', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined']
includes = [arg for p in [inc, kit / 'tests', kit, ROOT / 'kits/ArmSmmu4K', ROOT / 'kits/AdrenoPM4'] for arg in ['-I', p]]
objects = []
for name in ['ArmSmmu4KContext', 'ArmSmmu4KProgram', 'ArmSmmu4KLive', 'ArmSmmu4KStreams', 'AdrenoPM4']:
    directory = 'AdrenoPM4' if name == 'AdrenoPM4' else 'ArmSmmu4K'
    obj = out / (name + '.o')
    subprocess.run(['xcrun', 'clang', '-std=c11', *flags, '-c', ROOT / 'kits' / directory / (name + '.c'), '-o', obj], check=True)
    objects.append(obj)
for test, sources, linked in [
    ('preempt', ['NativeAdrenoPreempt'], []),
    ('ring', ['NativeAdrenoRing', 'NativeAdrenoPreempt'], objects[-1:]),
    ('submit', ['NativeAdrenoMemory', 'NativeAdrenoSmmuMemory', 'NativeAdrenoVm', 'NativeAdrenoRing', 'NativeAdrenoPreempt', 'NativeAdrenoSubmit', 'tests/HostKernelHooks'], objects),
]:
    binary = out / ('test-' + test)
    subprocess.run(['xcrun', 'clang++', '-std=c++23', *flags, *includes,
                    *(kit / (name + '.cpp') for name in sources), kit / ('tests/test_' + test + '.cpp'), *linked, '-o', binary], check=True)
    result = subprocess.run([binary], capture_output=True, text=True, timeout=90)
    (out / (test + '.log')).write_text(result.stdout + result.stderr)
    print(result.stdout, end='')
    if result.returncode:
        raise SystemExit(result.stderr or f'{test} failed: {result.returncode}')
