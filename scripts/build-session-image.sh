#!/usr/bin/env bash
# Build one hardware boot image for a session: scripts/build-session-image.sh <tag> [extra --kernel-arg=... values]
#   an extra --kernel-arg=name=value replaces the template's value for that name (e.g. boot-diag-resume=0 turns it off)
#   tag      session-prefixed label, e.g. ks-001, gpu-003, int-001 (docs/parallel-development.md); also shown on the diagnostic page
# Inputs that are not in Git (they carry the platform seeds): build/session-image/{prelink,payload}.template.json, with @TAG@ in the names.
# Fails on any step; prints the image, its size and whether the kernel really changed.
set -euo pipefail
tag=${1:?usage: build-session-image.sh <tag> [--kernel-arg=...]}; shift
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd); cd "$ROOT"
[[ $tag =~ ^[a-z]+-[0-9]+$ ]] || { echo "tag must look like ks-001" >&2; exit 1; }
T=build/session-image
for f in prelink payload; do [[ -f $T/$f.template.json ]] || { echo "missing $T/$f.template.json" >&2; exit 1; }; done
# A rerun of the same tag replaces its earlier candidate (emit_prelink refuses to overwrite).
rm -f build/xnu-prelink/native-owner-ufs-cpu-desktop-$tag-candidate.macho build/xnu-prelink/native-owner-ufs-cpu-desktop-$tag-candidate.json
sed "s/@TAG@/$tag/g" $T/prelink.template.json > $T/prelink-$tag.json
python3 - "$tag" "$@" <<'P'
import json,sys
tag,extra=sys.argv[1],sys.argv[2:]
c=json.loads(open('build/session-image/payload.template.json').read().replace('@TAG@',tag))
# An extra --kernel-arg=<name>=<value> replaces the template's argument of the same name (no duplicate boot-args).
names={a.split('=',2)[1] for a in extra if a.startswith('--kernel-arg=') and a.count('=')>=2}
c=[a for a in c if not (a.startswith('--kernel-arg=') and a.count('=')>=2 and a.split('=',2)[1] in names)]
# --data-ramdisk=PATH and --owner-trust-cache=PATH replace the template's Data seed and its original trust cache.
if any(a.startswith('--owner-trust-cache=') for a in extra):c=[a for a in c if not a.startswith('--original-trust-cache=')]
if any(a.startswith('--data-ramdisk=') for a in extra):c=[a for a in c if not a.startswith('--data-ramdisk=')]
i=c.index('--kernel-arg=sheng-touch=4')
c[i+1:i+1]=[f'--kernel-arg=sheng-build-tag={tag}']+extra
json.dump(c,open(f'build/session-image/payload-{tag}.json','w'),indent=1)
P
export PATH=/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH
# libShengUfs.a and libRamBlock.a are linked into the kernel; rebuild them from the driver sources (stale archives once hid new symbols).
python3 scripts/build-arm-driver.py ShengUfs
python3 scripts/build-arm-driver.py RamBlock
# libArmCpu.a (XNU_ARMCPU) carries the SPI attach/dispatch the GPU probe calls.
python3 scripts/build-arm-driver.py ArmCpu
python3 scripts/build-virtio-mmio-bindings.py --component NativeRamBlock --directory build/ram-block
python3 scripts/build-gop-legacy-display.py
# NativeAdreno is linked into the kernel (XNU_NATIVE_ADRENO); the kits step appends to the service archive.
python3 scripts/build-arm-driver.py Adreno740
python3 scripts/build-adreno740-kits.py
# The staged AGXAccelerator (original IOAcceleratorES descendant for the Metal discovery) is appended to that archive.
python3 scripts/build-adreno740-accelerator.py
python3 scripts/build-cpu-desktop-kernel.py
python3 -c "import json,subprocess;subprocess.check_call(json.load(open('build/session-image/prelink-$tag.json')))"
K=build/xnu-prelink/native-owner-ufs-cpu-desktop-$tag-candidate.macho
python3 scripts/check-panic-entry-coverage.py --kernel "$K"
python3 -c "import json,subprocess;subprocess.check_call(json.load(open('build/session-image/payload-$tag.json')))"
export PATH=/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH
python3 scripts/package-sheng-boot.py --skip-qemu
python3 scripts/package-sheng-xnu-boot.py
IMG=build/sheng/boot/sheng-uefi-xnu-$tag.img
cp build/sheng/boot/sheng-uefi-xnu.img "$IMG"
size=$(stat -f %z "$IMG"); [[ $size == 202502144 ]] || { echo "unexpected image size $size" >&2; exit 1; }
echo "BUILT $IMG ($size bytes) sha256 $(shasum -a 256 "$IMG" | cut -d' ' -f1)"
echo "kernel: $K  (grep it for a string of your new code before asking for a boot)"
