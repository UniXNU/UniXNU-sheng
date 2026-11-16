#!/usr/bin/env python3
"""Cross-build the static aarch64 charge utility with LLVM and the shared board codec."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu', type=Path, default=ROOT.parent / 'UniXNU')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/linux-charge/sheng-charge')
    parser.add_argument('--llvm', type=Path, default=Path('/opt/homebrew/opt/llvm/bin'))
    args = parser.parse_args()
    clang = args.llvm / 'clang'
    if not clang.is_file():
        found = shutil.which('clang')
        if not found:
            parser.error('clang is required')
        clang = Path(found)
    lld = shutil.which('ld.lld') or '/opt/homebrew/opt/lld/bin/ld.lld'
    sha = args.unixnu.resolve() / 'drivers/Adreno740'
    if not (sha / 'NativeAdrenoSha256.cpp').is_file():
        parser.error('--unixnu needs its Adreno740 submodule')
    resource = subprocess.check_output([str(clang), '-print-resource-dir'], text=True).strip()
    package = ROOT / 'package'
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sheng-charge-') as directory:
        temporary = Path(directory)
        (temporary / 'string.h').write_text('''#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
#ifdef __cplusplus
}
#endif
''')
        flags = ['--target=aarch64-linux-gnu', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                 '-fno-pie', '-nostdinc', '-isystem', str(Path(resource) / 'include'), '-I', str(temporary),
                 '-I', str(package), '-I', str(sha), '-O2', '-Wall', '-Wextra', '-Werror']
        objects = []
        for source in (package / 'ShengChargeAuth.c', sha / 'NativeAdrenoSha256.cpp', package / 'linux-log/charge.cpp'):
            obj = temporary / (source.stem + '.o')
            extra = ['-std=c11'] if source.suffix == '.c' else ['-std=c++17', '-fno-exceptions', '-fno-rtti']
            subprocess.run([str(clang), *flags, *extra, '-c', str(source), '-o', str(obj)], check=True)
            objects.append(str(obj))
        subprocess.run([str(lld), '-static', '-e', '_start', *objects, '-o', str(args.out)], check=True)
    args.out.chmod(0o755)
    print(f'{args.out}: static aarch64 Linux executable, {args.out.stat().st_size} bytes')


if __name__ == '__main__':
    main()
