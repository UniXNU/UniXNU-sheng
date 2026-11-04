#!/usr/bin/env python3
"""Verify the packed native Bluetooth runtime and its shared launchd/trust inputs."""
import argparse
import copy
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import struct
import subprocess
import sys
import tempfile
import zlib

root=Path(__file__).resolve().parents[1]
unixnu=root.parent/'UniXNU'
sys.path.insert(0,str(unixnu/'scripts'))
sys.path.insert(0,str(root/'drivers/QcaBluetooth/tools'))
from native_ios27 import _inspect, ORIGINAL_UUID, NATIVE_DYLIB
from unixnu.devicetree import generate
from unixnu.profiles import merge
import tomllib
spec=importlib.util.spec_from_file_location('bluetooth_boot_runtime',unixnu/'scripts/build-boot-runtime.py')
builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)


def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def addon(path):
    data=path.read_bytes()
    assert data[:8]==b'NGPUADD2' and struct.unpack_from('<Q',data,16)[0]==len(data)
    at=24;result={}
    for _ in range(struct.unpack_from('<I',data,8)[0]):
        length,mode,size,packed=struct.unpack_from('<IIQQ',data,at);at+=24
        assert 1<length<=160 and data[at+length-1]==0
        name=data[at:at+length-1].decode();at+=(length+7)&~7
        assert name not in result and not Path(name).is_absolute() and '..' not in Path(name).parts
        body=zlib.decompress(data[at:at+packed]);at+=(packed+7)&~7
        assert len(body)==size
        result[name]=(mode,body)
    assert at==len(data)
    return result

def modules(path):
    data=path.read_bytes();count=struct.unpack_from('<I',data)[0]
    offsets=list(struct.unpack_from('<'+count*'I',data,4))+[len(data)]
    assert 1<=count<=128 and offsets[0]==4+4*count and offsets==sorted(set(offsets))
    result=[];hashes=set()
    for a,b in zip(offsets,offsets[1:]):
        body=data[a:b];version,_,count=struct.unpack_from('<I16sI',body)
        stride={0:20,1:22,2:24}[version];assert len(body)==24+count*stride
        result.append(body)
        hashes.update(body[24+i*stride:44+i*stride].hex() for i in range(count))
    return result,hashes

def decode_adt(data,at=0):
    properties,children=struct.unpack_from('<II',data,at);at+=8;values={}
    for _ in range(properties):
        name=data[at:at+32].split(b'\0')[0].decode();length=struct.unpack_from('<I',data,at+32)[0]&0x7fffffff;at+=36
        values[name]=data[at:at+length];at+=(length+3)&~3
    nodes={}
    for _ in range(children):
        value,at=decode_adt(data,at);nodes[value[0]['name'].rstrip(b'\0').decode()]=value
    return (values,nodes),at

def host_xpc(data):
    xpc=ctypes.CDLL('/usr/lib/system/libxpc.dylib')
    xpc.xpc_create_from_plist.argtypes=[ctypes.c_void_p,ctypes.c_size_t];xpc.xpc_create_from_plist.restype=ctypes.c_void_p
    xpc.xpc_dictionary_get_value.argtypes=[ctypes.c_void_p,ctypes.c_char_p];xpc.xpc_dictionary_get_value.restype=ctypes.c_void_p
    xpc.xpc_dictionary_get_string.argtypes=[ctypes.c_void_p,ctypes.c_char_p];xpc.xpc_dictionary_get_string.restype=ctypes.c_char_p
    xpc.xpc_release.argtypes=[ctypes.c_void_p]
    buf=ctypes.create_string_buffer(data);parsed=xpc.xpc_create_from_plist(buf,len(data));assert parsed
    jobs=xpc.xpc_dictionary_get_value(parsed,b'LaunchDaemons')
    blue=xpc.xpc_dictionary_get_value(jobs,b'/System/Library/LaunchDaemons/com.apple.BlueTool.plist');assert blue
    assert xpc.xpc_dictionary_get_string(blue,b'UserName')==b'root'
    xpc.xpc_release(parsed)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime',type=Path,required=True)
    parser.add_argument('--os-root',type=Path,required=True)
    parser.add_argument('--seed-record',type=Path,help='existing runtime inputs.json that records the preserved Data seed')
    args=parser.parse_args();out=args.runtime.resolve()
    request=json.loads((out/'request.json').read_text());generated=json.loads((out/'generated.json').read_text())
    service=json.loads((out/'bluetooth-service.json').read_text())
    before=addon(Path(request['gpu_addon']));packed=addon(Path(generated['gpu_addon']))
    changed=[name for name,value in before.items() if packed.get(name)!=value]
    allowed=('bluetooth-bin/','bluetooth-lib/','firmware/qca/','wifi-bin/','etc/wifi/','xpc/','daemons/org.unixnu.wifi.')
    assert all(name.startswith(allowed) for name in changed)
    old_modules,_=modules(Path(request['trust_cache']));current_modules,trusted=modules(Path(generated['trust_cache']))
    assert current_modules[:len(old_modules)]==old_modules
    old_inputs=args.seed_record or root.parent/'UniXNU/out/sheng/ipados-27.0.1-24A446/sheng_ipados27/runtime/inputs.json'
    prior=json.loads(old_inputs.read_text())['inputs']['data_ramdisk']
    seed=Path(request['data_ramdisk']);assert str(seed)==prior['path'] and digest(seed)==prior['sha256']
    assert generated['data_ramdisk']==str(seed)
    with tempfile.TemporaryDirectory(prefix='bluetooth-verify-',dir=out) as td:
        work=Path(td)
        for relative,identity in service['files'].items():
            prefix,rest=relative.split('/',1);name={'bin':'bluetooth-bin','lib':'bluetooth-lib'}[prefix]+'/'+rest
            mode,body=packed[name];assert mode==0o755 and len(body)==identity['bytes']
            assert hashlib.sha256(body).hexdigest()==identity['sha256']
            path=work/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(body)
            assert builder.verified_cdhash(path)==identity['cdhash'] and identity['cdhash'] in trusted
            image=_inspect.Image(path);assert image.subtype==0x80000002
        native=work/'bluetooth-bin/bluetoothd'
        original=args.os_root/'usr/sbin/bluetoothd'
        def entitlements(path):
            return plistlib.loads(subprocess.run(['codesign','--display','--entitlements',':-',str(path)],
                check=True,capture_output=True).stdout)
        assert entitlements(native)==entitlements(original)
        image=_inspect.Image(native);assert image.uuid==ORIGINAL_UUID
        assert image.data.count(NATIVE_DYLIB+b'\0')==1
        marker=json.loads(packed['bluetooth-bin/QcaBluetooth.runtime'][1])
        assert marker['files']=={name:item['sha256'] for name,item in service['files'].items()}
        manifest=service['firmware']
        for name,identity in manifest['files'].items():
            mode,body=packed[manifest['firmware_prefix']+'/'+name]
            assert mode==0o644 and len(body)==identity['bytes'] and hashlib.sha256(body).hexdigest()==identity['sha256']
        cache=work/'xpc/launchd.plist';cache.parent.mkdir(exist_ok=True)
        cache.write_bytes(packed['xpc/launchd.plist'][1]);signature=cache.with_suffix('.plist.sig')
        signature.write_bytes(packed['xpc/launchd.plist.sig'][1]);assert builder.verified_cdhash(cache,signature) in trusted
        original_cache=plistlib.loads((args.os_root/'System/Library/xpc/launchd.plist').read_bytes())
        merged=plistlib.loads(cache.read_bytes());jobs=merged['LaunchDaemons'];expected=copy.deepcopy(original_cache)
        for key,job in expected['LaunchDaemons'].items():
            if job['Label'] in service['launchd_job_overrides']:job.update(service['launchd_job_overrides'][job['Label']])
            if job['Label'] in request.get('launchd_job_environment',{}):
                job.setdefault('EnvironmentVariables',{}).update(request['launchd_job_environment'][job['Label']])
        for name,(_,body) in packed.items():
            if name.startswith('daemons/org.unixnu.') and name.endswith('.plist'):
                key='/System/Library/LaunchDaemons/'+Path(name).name
                expected['LaunchDaemons'][key]=plistlib.loads(body)
        assert merged==expected
        assert jobs['/System/Library/LaunchDaemons/com.apple.bluetoothd.plist']==original_cache['LaunchDaemons']['/System/Library/LaunchDaemons/com.apple.bluetoothd.plist']
        host_xpc(cache.read_bytes())
        empty=work/'empty';empty.mkdir()
        blue_only=work/'blue-only.plist'
        builder.merge_launchd_cache(args.os_root/'System/Library/xpc/launchd.plist',empty,blue_only,
            job_overrides=service['launchd_job_overrides'])
        assert len(plistlib.loads(blue_only.read_bytes())['LaunchDaemons'])==len(original_cache['LaunchDaemons'])
        for bad in ({'ProgramArguments':['/bad']},{'UserName':'../root'},{'ThrottleInterval':True},{'EnablePressuredExit':1}):
            try:builder.merge_launchd_cache(args.os_root/'System/Library/xpc/launchd.plist',empty,work/'bad.plist',job_overrides={'com.apple.BlueTool':bad})
            except ValueError:pass
            else:raise AssertionError('invalid launchd override accepted')
        dtb=work/'hardware.dtb'
        subprocess.run(['dtc','-I','dts','-O','dtb','-i',str(root.parent/'UniXNU-sm8550-common/dts'),
                        '-o',str(dtb),str(root/'dts/sheng.dts')],check=True,capture_output=True)
        config=merge(tomllib.loads((unixnu/'profiles/os/ipados-27.0.1-24A446/profile.toml').read_text()),tomllib.loads((root/'board.toml').read_text()))
        encoded=generate(dtb,config,unique_chip_id='11223344')
        (tree,end)=decode_adt(encoded);assert end==len(encoded)
        radio=tree[1]['arm-io'][1]['bluetooth'][0]
        assert radio['device_type']==b'bluetooth\0' and radio['transport-encoding']==struct.pack('<I',1)
        assert radio['transport-speed']==struct.pack('<I',3200000)
    report=dict(passed=True,hardware_verified=False,files=len(packed),stock_jobs=len(original_cache['LaunchDaemons']),
        runtime_jobs=len(jobs),signed_bluetooth_files=3,original_entitlements_preserved=True,
        stock_bluetoothd_job_preserved=True,blue_tool_user='root',host_libxpc_parsed=True,
        original_trust_modules_preserved=len(old_modules),trust_modules=len(current_modules),
        data_seed_preserved=True,bluetooth_adt_encoding='H4 UART.SK',bluetooth_uart_rate=3200000,
        changed_base_files=changed,addon_sha256=digest(Path(generated['gpu_addon'])),trust_sha256=digest(Path(generated['trust_cache'])))
    (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f"Bluetooth runtime: {len(packed)} packed files, three signed arm64e binaries, {len(jobs)} launchd jobs; firmware, entitlements, trust modules, preserved Data seed and little-endian ADT verified")

if __name__=='__main__':main()
