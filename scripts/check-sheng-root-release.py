#!/usr/bin/env python3
"""Audit the final held boot image through Android header -> FAT -> kernel/ADT/config.

Read-only to the image/device. Only an ignored temporary copy of the candidate
is patched to independently reproduce the permitted panic-screen payload.
This proves packaging consistency, not hardware boot or root acceptance.
"""
import argparse
import hashlib
import json
import mmap
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import tempfile
import tomllib
import zlib

ROOT = Path(__file__).resolve().parents[1]


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def boot_layout(header, total):
    require(len(header) >= 48 and header[:8] == b'ANDROID!', 'Android header magic/length')
    ksize, _, rsize, _, second, _, _, page, version, _ = struct.unpack_from('<10I', header, 8)
    require(version == 0 and second == 0 and ksize and rsize, 'Expected v0 kernel + ramdisk only')
    require(2048 <= page <= 65536 and page & (page - 1) == 0, 'Invalid boot page size')
    offset = page * (1 + (ksize + page - 1) // page)
    require(offset + ((rsize + page - 1) // page) * page == total, 'Truncated/trailing boot image')
    return page, ksize, offset, rsize


def parse_adt(data):
    nodes = {}
    def node(at, parent, depth):
        require(depth <= 16 and at + 8 <= len(data), 'ADT node bounds')
        props, children = struct.unpack_from('<II', data, at); at += 8
        require(props <= 128 and children <= 128, 'ADT count bounds')
        values = {}
        for _ in range(props):
            require(at + 36 <= len(data), 'ADT property header bounds')
            raw, size = struct.unpack_from('<32sI', data, at); at += 36
            key = raw.split(b'\0',1)[0].decode('ascii')
            require(key and key not in values and at + ((size + 3) & ~3) <= len(data), 'ADT property bounds/duplicate')
            values[key] = data[at:at+size]; at += (size + 3) & ~3
        require(values.get('name', b'').endswith(b'\0'), 'ADT node name')
        path = parent + '/' + values['name'][:-1].decode('ascii')
        require(path not in nodes and len(nodes) < 512, 'ADT duplicate/node budget')
        nodes[path] = values
        for _ in range(children):
            at = node(at, path, depth + 1)
        return at
    require(node(0, '', 0) == len(data), 'ADT trailing bytes')
    return nodes


def audit_configuration(config, nodes, root_uuid, owned_memory, boot_progress=False, shared_data=False):
    require(config['boot']['default'] == 'xnu' and config['kits']['autoload'] is False, 'Unexpected default/autoload')
    require('driver' not in config and len(config['entry']) == 1, 'Unexpected extra loader/entry')
    entry = config['entry'][0]
    require(entry['id'] == 'xnu' and entry['type'] == 'xnu' and entry['path'] == 'Payloads/kernel.macho'
            and entry['initrd'] == 'Payloads/scratch-data.img', 'Unexpected boot paths')
    args = entry['args'].split()
    expected = ['-v','serial=3','debug=0x14e','kt_fixed_zones=120','pmgr=2','native-ufs-root=1','native-ufs-diagnostics=1']
    if boot_progress:
        expected += ['boot-progress=1']
    if shared_data:
        expected += ['-apfs_shared_datavolume']
    require(args == expected, 'Unexpected/missing/duplicate boot argument or probe')
    chosen = nodes['/device-tree/chosen']
    require(chosen['darwinboot-memory-size'] == struct.pack('<Q', owned_memory), 'Owned memory size mismatch')
    require(chosen['crypto-hash-method'] == b'sha2-384\0'
            and chosen['amfi-only-platform-code'] == struct.pack('<I',1), 'AMFI policy mismatch')
    xml = chosen['root-matching'].rstrip(b'\0')
    matching = plistlib.loads(b'<plist version="1.0">'+xml+b'</plist>')
    require(matching == {'IOProviderClass':'IOMedia','IOPropertyMatch':{'UUID':root_uuid.upper()}}, 'Root UUID mismatch')
    require(nodes['/device-tree/chosen/memory-map']['RAMDisk'] == bytes(16), 'RAMDisk fixup placeholder mismatch')
    require(not any('vmm' in key.lower() for props in nodes.values() for key in props), 'Unexpected VMM property')
    return args


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', required=True, type=Path)
    p.add_argument('--candidate', required=True, type=Path)
    p.add_argument('--payload-report', required=True, type=Path)
    p.add_argument('--package-report', required=True, type=Path)
    p.add_argument('--firmware-report', required=True, type=Path)
    p.add_argument('--data-seed', required=True, type=Path)
    p.add_argument('--original-trust-cache', type=Path, help='Require exact original target trust-cache sidecar')
    p.add_argument('--shared-data', action='store_true', help='Require genuine shared-Data boot argument; no SEP/keybag qualification')
    p.add_argument('--boot-progress', action='store_true', help='Require opt-in bounded progress/verbose screen')
    p.add_argument('--root-uuid', required=True)
    p.add_argument('--owned-memory', type=lambda x:int(x,0), default=0x40000000)
    p.add_argument('--output', required=True, type=Path)
    args = p.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    (out/'report.json').unlink(missing_ok=True)
    payload=json.loads(args.payload_report.read_text()); package=json.loads(args.package_report.read_text())
    candidate=args.candidate.read_bytes()
    require(sha(candidate)==payload['kernel_sha256'], 'Candidate/report mismatch')
    require(struct.unpack_from('<III',candidate) == (0xfeedfacf,0x100000c,0), 'Expected ordinary ARM64 subtype 0')
    require(payload['xnu_probe'] and payload['xnu_reset_after'] is None, 'Unexpected reset probe')
    with args.image.open('rb') as stream, mmap.mmap(stream.fileno(),0,access=mmap.ACCESS_READ) as data:
        page,ksize,offset,rsize=boot_layout(data[:48],len(data))
        board=tomllib.loads((ROOT/'uefi-sheng/Resources/Configs/sheng.toml').read_text())['boot_image']
        require(page==board['pagesize'] and struct.unpack_from('<I',data,12)[0]==board['base']+board['kernel_offset']
                and struct.unpack_from('<I',data,32)[0]==board['base']+board['tags_offset'], 'Android placement/config mismatch')
        require(sha(data)==package['image_sha256'] and len(data)==package['image_bytes'], 'Boot image/package report mismatch')
        require(rsize==payload['payload_image_bytes']==package['payload_bytes'], 'Ramdisk size mismatch')
        require(sha(data[offset:offset+rsize])==payload['payload_image_sha256']==package['payload_sha256'], 'FAT ramdisk hash mismatch')
        require(sha(data[page:page+ksize])==package['kernel_sha256'], 'Firmware+DTB hash mismatch')
        decompressor=zlib.decompressobj(16+zlib.MAX_WBITS)
        firmware=decompressor.decompress(data[page:page+ksize])
        require(decompressor.eof, 'Truncated compressed firmware')
        dtb=decompressor.unused_data
        require(dtb[:4]==b'\xd0\x0d\xfe\xed' and struct.unpack_from('>I',dtb,4)[0]==len(dtb), 'Appended DTB bounds')
        fw_report=json.loads(args.firmware_report.read_text())
        split=fw_report['shim_bytes']
        require(sha(firmware[:split])==fw_report['shim_sha256'] and sha(firmware[split:])==fw_report['fd_sha256']
                and sha(dtb)==fw_report['dtb_sha256'], 'Embedded shim/FD/DTB differ from checked firmware')
    fat=str(args.image.resolve())+'@@'+str(offset)
    def read_file(name):
        return subprocess.check_output(['mtype','-i',fat,'::EFI/UniXNU/'+name],timeout=30)
    config_bytes=read_file('unixnu.toml')
    config=tomllib.loads(config_bytes.decode())
    adt=read_file('Payloads/kernel.macho.adt')
    require(sha(adt)==payload['adt_sha256'], 'Embedded ADT hash mismatch')
    boot_args=audit_configuration(config,parse_adt(adt),args.root_uuid,args.owned_memory,args.boot_progress, args.shared_data)
    require(config['entry'][0]['args']==payload['kernel_args'], 'Embedded args/report mismatch')
    seed=read_file('Payloads/scratch-data.img')
    require(seed==args.data_seed.read_bytes(), 'Embedded Data seed mismatch')
    trust_hash = None
    memory_map = parse_adt(adt)['/device-tree/chosen/memory-map']
    if args.original_trust_cache:
        trust = read_file('Payloads/kernel.macho.trustcache')
        trust_hash = sha(trust)
        require(trust_hash == '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7'
                and trust == args.original_trust_cache.read_bytes(), 'Wrong original trust-cache bytes')
        require(memory_map.get('TrustCache') == bytes(16), 'Missing/stale TrustCache handoff')
    else:
        require('TrustCache' not in memory_map, 'Unexpected TrustCache handoff')
    require(payload.get('original_trust_cache_sha256') == trust_hash, 'Trust-cache report mismatch')
    embedded=read_file('Payloads/kernel.macho')
    require(sha(embedded)==payload['payload_kernel_sha256'], 'Embedded kernel hash mismatch')
    with tempfile.TemporaryDirectory(prefix='kernel-audit-',dir=out) as td:
        regenerated=Path(td)/'kernel.macho';regenerated.write_bytes(candidate)
        with (out/'patch-reproduction.log').open('w') as log:
            subprocess.run(['python3',ROOT/'scripts/patch-xnu-entry-probe.py',regenerated,'--panic-screen','--no-reset'],
                           check=True,timeout=60,stdout=log,stderr=subprocess.STDOUT)
        require(regenerated.read_bytes()==embedded, 'Embedded kernel differs from exact permitted patch reproduction')
    report={'image':args.image.name,'image_sha256':package['image_sha256'],'candidate_sha256':sha(candidate),
            'embedded_kernel_sha256':sha(embedded),'adt_sha256':sha(adt),'data_seed_sha256':sha(seed),
            'original_trust_cache_sha256':trust_hash,'config_sha256':sha(config_bytes),'boot_args':boot_args,'root_uuid':args.root_uuid.upper(),
            'owned_memory':hex(args.owned_memory),'native_cpu_subtype':0,'patch_reproduction':'byte-for-byte match',
            'firmware_components':'embedded shim/FD/DTB match firmware report',
            'hardware_tested':False,'boot_sent':False,'scope':'Offline packaging audit, no runtime page-size or root-acceptance claim'}
    for name,path in [('payload-report.json',args.payload_report),('package-report.json',args.package_report),('firmware-report.json',args.firmware_report)]:
        if path.resolve() != (out/name).resolve():
            shutil.copyfile(path,out/name)
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS: final Android/FAT contents, exact kernel patch reproduction, config, ADT, root UUID, Data seed and firmware components')


if __name__=='__main__':
    main()
