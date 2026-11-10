#!/usr/bin/env bash
# Repackage a built session image's kernel into a boot.img for flashing to a boot partition and starting with the power key:
#   scripts/build-flash-image.sh <flash-tag> <payload-int-NNN.json> [partition-bytes]
#   - the same kernel and Data seed as that session image (nothing is rebuilt but the packages);
#   - no autoreset: a flashed image must not restart itself into fastboot on a timer;
#   - the payload FAT volume is 176 MiB instead of 192 MiB so the whole image fits a 192 MiB boot partition (a session image is 193 MiB and
#     only boots through `fastboot boot`).
# The image is checked against the partition size (asked of a tablet in fastboot, `fastboot getvar partition-size:boot_b`, or given in
# bytes) and the script prints the flash commands. It never flashes anything.
set -euo pipefail
tag=${1:?usage: build-flash-image.sh <flash-tag> <payload-int-NNN.json> [partition-bytes]}; base=${2:?payload json}; limit=${3:-}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd); cd "$ROOT"
[[ $tag =~ ^[a-z]+-[0-9]+$ ]] || { echo "tag must look like flash-001" >&2; exit 1; }
export PATH=/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH
if [[ -z $limit ]]; then
  limit=$(fastboot getvar partition-size:boot_b 2>&1 | sed -n 's/.*partition-size:boot_b: *\(0x[0-9A-Fa-f]*\).*/\1/p' | head -1 || true)
fi
[[ -n $limit ]] || { echo "No partition size: put the tablet in fastboot or pass the size in bytes" >&2; exit 1; }
limit=$((limit))
python3 - "$tag" "$base" <<'P'
import json,re,sys
tag,base=sys.argv[1:]
text=open(base).read()
old=re.search(r'cpu-desktop-([a-z]+-[0-9]+)-candidate',text).group(1)   # the kernel stays the session image's own
c=json.loads(text)
c=[a for a in c if not a.startswith(('--kernel-arg=autoreset=','--kernel-arg=build-tag='))]
i=c.index('--kernel-arg=touch=4')
c.insert(i+1,'--kernel-arg=build-tag='+tag)
json.dump(c,open(f'build/session-image/payload-{tag}.json','w'),indent=1)
print('kernel from',old)
P
SHENG_FAT_SIZE=176m python3 -c "import json,subprocess;subprocess.check_call(json.load(open('build/session-image/payload-$tag.json')))"
python3 scripts/package-sheng-boot.py --skip-qemu
python3 scripts/package-sheng-xnu-boot.py
IMG=build/sheng/boot/sheng-flash-$tag.img
cp build/sheng/boot/sheng-uefi-xnu.img "$IMG"
size=$(stat -f %z "$IMG")
(( size <= limit )) || { echo "image is $size bytes, the partition holds $limit" >&2; rm -f "$IMG"; exit 1; }
echo "BUILT $IMG ($size bytes, partition $limit) sha256 $(shasum -a 256 "$IMG" | cut -d' ' -f1)"
echo "first test boot, no flash:   fastboot boot $IMG"
echo "flash to the spare slot:     fastboot flash boot_b $IMG && fastboot set_active b   (slot a keeps the Linux boot as the way back)"
