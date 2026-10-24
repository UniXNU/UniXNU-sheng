#!/usr/bin/env python3
"""Build a partial native Metal API frontend and its own signed runtime instrument."""
import argparse
import hashlib
import json
import plistlib
from pathlib import Path
import re
import struct
import subprocess
import uuid
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--legacy-swap',action='store_true');p.add_argument('--iosurface',action='store_true');p.add_argument('--bgra',action='store_true');p.add_argument('--present',action='store_true');p.add_argument('--resources',action='store_true');p.add_argument('--compose',action='store_true')
p.add_argument('--async-commands',action='store_true');p.add_argument('--fences',action='store_true');p.add_argument('--sandbox-extension',action='store_true');p.add_argument('--metal-layer',action='store_true');p.add_argument('--renderbox-present',type=int,choices=[1,2]);p.add_argument('--depth-stencil',action='store_true');p.add_argument('--private-texture',action='store_true');p.add_argument('--renderbox-draw',action='store_true');p.add_argument('--renderbox-device',action='store_true');p.add_argument('--plugin-control',choices=['full','without-mount-permission','tampered-code'],default='full');p.add_argument('--plugin-addon',action='store_true');p.add_argument('--plugin-bundle',action='store_true');p.add_argument('--legacy-swap-wait',action='store_true');p.add_argument('--broker-processes',action='store_true');p.add_argument('--broker-policy',choices=['full','without-container','without-client','parent-client','unrelated-client'],default='full')
p.add_argument('--shared-surfaces',action='store_true',help='Pass actual IOSurface Mach rights between two GPU processes')
p.add_argument('--shared-events',action='store_true',help='Real native shared-event Mach capabilities between two processes')
p.add_argument('--shared-notifications',action='store_true',help='Original listener queue delivery and bounded context cancellation')
p.add_argument('--shared-coding',action='store_true',help='Original NSXPC shared-handle secure coding, separate from cross-process authority')
p.add_argument('--shared-cross-process',action='store_true',help='Actual original NSXPC endpoint and handle coding between two native GPU workers')
p.add_argument('--vertex-inputs',action='store_true',help='Actual original QuartzCore vertex layouts and native GPU buffers')
a=p.parse_args()
if a.vertex_inputs and any((a.shared_events, a.shared_notifications, a.shared_coding, a.shared_cross_process, a.async_commands, a.fences, a.metal_layer, a.renderbox_present, a.depth_stencil, a.private_texture, a.renderbox_draw, a.renderbox_device, a.plugin_addon, a.plugin_bundle, a.shared_surfaces, a.sandbox_extension, a.legacy_swap, a.legacy_swap_wait, a.compose, a.resources, a.present)):p.error("Select the independent vertex input instrument")
if a.vertex_inputs:a.broker_processes=True
if a.shared_notifications and (a.shared_coding or a.shared_cross_process):p.error('Select notifications or secure coding as independent instruments')
if a.shared_cross_process:a.shared_coding=True
if a.shared_coding:a.shared_events=True
if a.shared_notifications:a.shared_events=True
if a.shared_events:a.broker_processes=True
if a.async_commands:a.fences=True
if a.fences:a.metal_layer=True
if a.sandbox_extension:a.broker_processes=True
if a.metal_layer:
    if a.renderbox_present or a.depth_stencil or a.private_texture or a.renderbox_draw:p.error('Select one independent layer/renderer instrument')
    a.renderbox_device=True
if a.renderbox_present:
    if a.depth_stencil or a.private_texture or a.renderbox_draw:p.error('Select one independent renderer/presentation instrument')
    a.renderbox_device=True
if a.depth_stencil:
    if a.private_texture or a.renderbox_draw:p.error('Select one independent resource or renderer instrument')
    a.renderbox_device=True
if a.private_texture:
    if a.renderbox_draw:p.error('Select one independent resource or original renderer instrument')
    a.renderbox_device=True
if a.renderbox_draw:a.renderbox_device=True
if a.renderbox_device:
    if a.plugin_control!='full':p.error('RenderBox device observation is a separate instrument')
    a.plugin_addon=True
if a.plugin_control!='full':a.plugin_addon=True
if a.plugin_addon:a.plugin_bundle=True
if a.plugin_bundle:a.shared_surfaces=True
if a.shared_surfaces:a.broker_processes=True
if a.shared_surfaces and a.broker_policy!='full':p.error('Sharing instrument requires both existing scoped diagnostic permissions')
if a.broker_policy!='full' and not a.broker_processes:p.error('Permission controls require the private GPU process instrument')
if a.broker_processes:
    if a.legacy_swap_wait or a.legacy_swap or a.compose or a.resources or a.present:p.error('Select one GPU runtime instrument')
    a.bgra=True
if a.legacy_swap_wait:a.legacy_swap=True
if a.legacy_swap and (a.compose or a.resources or a.present):p.error('Select one presentation instrument')
if a.legacy_swap:a.bgra=True
if a.compose and a.resources:p.error('Select composition or resource instrument')
if a.compose:a.present=True
if a.resources:a.present=True
if a.present:a.bgra=True
if a.bgra:a.iosurface=True
broker_kind='offline-metal-vertex-inputs' if a.vertex_inputs else 'offline-metal-shared-cross-process' if a.shared_cross_process else 'offline-metal-shared-coding' if a.shared_coding else 'offline-metal-shared-notifications' if a.shared_notifications else 'offline-metal-shared-events' if a.shared_events else 'offline-metal-async' if a.async_commands else 'offline-metal-fences' if a.fences else 'offline-sandbox-extension' if a.sandbox_extension else 'offline-metal-layer' if a.metal_layer else ('offline-renderbox-present-'+str(a.renderbox_present)) if a.renderbox_present else 'offline-metal-depth-stencil' if a.depth_stencil else 'offline-metal-private-texture' if a.private_texture else 'offline-renderbox-draw' if a.renderbox_draw else 'offline-renderbox-device' if a.renderbox_device else ('offline-metal-addon'+('' if a.plugin_control=='full' else '-'+a.plugin_control)) if a.plugin_addon else 'offline-metal-plugin' if a.plugin_bundle else 'offline-metal-shared-surfaces' if a.shared_surfaces else 'offline-metal-processes'
kind=(broker_kind+('' if a.broker_policy=='full' else '-'+a.broker_policy)) if a.broker_processes else 'offline-legacy-swap-wait' if a.legacy_swap_wait else 'offline-legacy-swap' if a.legacy_swap else 'offline-gop-composition' if a.compose else 'offline-gop-resources' if a.resources else 'offline-gop-presentation' if a.present else 'offline-metal-bgra' if a.bgra else 'offline-metal-iosurface' if a.iosurface else 'offline-metal-api'
OUT=ROOT/'build'/kind;OUT.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def run(cmd):return subprocess.run([str(x) for x in cmd],check=True,timeout=60)
kit=ROOT/'drivers/AdrenoMetal';source=ROOT/('kits/sheng/OfflineMetalVertexInputs.mm' if a.vertex_inputs else 'kits/sheng/OfflineMetalSharedEvents.mm' if a.shared_events else 'kits/sheng/OriginalMetalAsyncDiagnostic.mm' if a.async_commands else 'kits/sheng/OriginalMetalFenceDiagnostic.mm' if a.fences else 'kits/sheng/OriginalSandboxIOKitDiagnostic.mm' if a.sandbox_extension else 'kits/sheng/OriginalMetalLayerDiagnostic.mm' if a.metal_layer else 'kits/sheng/OriginalRenderBoxPresentation.mm' if a.renderbox_present else 'kits/sheng/OfflineMetalDepthStencil.mm' if a.depth_stencil else 'kits/sheng/OfflineMetalPrivateTexture.mm' if a.private_texture else 'kits/sheng/OfflineRenderBoxDraw.mm' if a.renderbox_draw else 'kits/sheng/OfflineRenderBoxDevice.mm' if a.renderbox_device else 'kits/sheng/OfflineMetalPluginProbe.mm' if a.plugin_bundle else 'kits/sheng/OfflineMetalSharedSurfaces.mm' if a.shared_surfaces else 'kits/sheng/OfflineMetalProcesses.mm' if a.broker_processes else 'kits/sheng/OfflineLegacySwap.mm' if a.legacy_swap else 'kits/sheng/OfflineGOPComposition.mm' if a.compose else 'kits/sheng/OfflineGOPResources.mm' if a.resources else 'kits/sheng/OfflineGOPPresentation.mm' if a.present else 'kits/sheng/OfflineMetalAPIDiagnostic.mm');exe=OUT/'air-native'
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
run(['xcrun','clang++','-target','arm64-apple-ios17.0','-isysroot',sdk,'-std=c++17','-O2','-g','-Wall','-Wextra','-Werror',
    '-DOFFLINE_SHARED_CROSS_PROCESS='+str(int(a.shared_cross_process)),'-DOFFLINE_SHARED_CODING='+str(int(a.shared_coding)),'-DOFFLINE_SHARED_NOTIFICATIONS='+str(int(a.shared_notifications)),'-DOFFLINE_RENDERBOX_PRESENT_FRAMES='+str(a.renderbox_present or 2),'-DOFFLINE_LEGACY_SWAP_WAIT='+str(int(a.legacy_swap_wait)),'-DOFFLINE_PLUGIN_SYSTEM_PATH='+str(int(a.plugin_addon)),
    '-DOFFLINE_METAL_IOSURFACE_DIAGNOSTIC='+str(int(a.iosurface)),'-DOFFLINE_METAL_BGRA_DIAGNOSTIC='+str(int(a.bgra)),'-Wno-deprecated-declarations','-fobjc-arc','-fstack-protector-strong','-I',kit,'-I',ROOT/'drivers/GopFramebuffer',source,*([] if a.plugin_bundle else [kit/'OfflineMetalFrontend.mm']),
    '-framework','Foundation','-framework','Metal','-framework','IOSurface',*(['-framework','CoreGraphics'] if (a.renderbox_draw or a.renderbox_present or a.metal_layer) else []),*(['-framework','QuartzCore'] if a.metal_layer else []),'-o',exe])
plugin=None
consumer=OUT/'sandbox-consumer' if a.sandbox_extension else None
if consumer:consumer.write_bytes(exe.read_bytes());consumer.chmod(0o755)
if a.plugin_bundle:
    directory=OUT/'NativeOfflineMetal.bundle';directory.mkdir(exist_ok=True);plugin=directory/'NativeOfflineMetal'
    (directory/'Info.plist').write_bytes(plistlib.dumps(dict(CFBundleIdentifier='org.blu-sharky.NativeOfflineMetal',CFBundleExecutable='NativeOfflineMetal',CFBundlePackageType='BNDL',CFBundleVersion='1',NSPrincipalClass='NativeOfflineMetalPlugin')))
    run(['xcrun','clang++','-target','arm64-apple-ios17.0','-isysroot',sdk,'-std=c++17','-O2','-g','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-fobjc-arc','-fstack-protector-strong','-dynamiclib','-install_name','@rpath/NativeOfflineMetal.bundle/NativeOfflineMetal','-I',kit,kit/'OfflineMetalPlugin.mm','-framework','Foundation','-framework','Metal','-framework','IOSurface','-o',plugin])
sign=['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.blu-sharky.'+kind]
entitlements={'com.apple.private.security.no-container':True,'com.apple.security.iokit-user-client-class':['NativeOfflineGPUClient','IOSurfaceRootUserClient']} if a.broker_processes else {}
if a.sandbox_extension:entitlements['com.apple.security.iokit-user-client-class']=['NativeOfflineGPUClient','IOSurfaceRootUserClient']
if a.plugin_bundle and not a.plugin_addon:entitlements['com.apple.private.security.no-sandbox']=True
if a.plugin_addon and a.plugin_control!='without-mount-permission':entitlements['com.apple.private.bindfs-allow']=True
if a.broker_processes:
    if a.broker_policy in ('parent-client','unrelated-client'):entitlements['com.apple.security.iokit-user-client-class']=['IOUserClient' if a.broker_policy=='parent-client' else 'OSArray','IOSurfaceRootUserClient']
    if a.broker_policy=='without-container':entitlements.pop('com.apple.private.security.no-container')
    if a.broker_policy=='without-client':entitlements.pop('com.apple.security.iokit-user-client-class')
if entitlements:
    policy=OUT/'diagnostic-entitlements.plist';policy.write_bytes(plistlib.dumps(entitlements));sign+=['--entitlements',policy]
run([*sign,exe]);run(['codesign','--verify','--strict',exe])
if entitlements:
    actual=subprocess.check_output(['codesign','-d','--entitlements',':-',str(exe)],stderr=subprocess.DEVNULL)
    if plistlib.loads(actual)!=entitlements:raise SystemExit('Actual own signed entitlement set differs')
details=subprocess.run(['codesign','--display','--verbose=4',exe],capture_output=True,text=True,check=True).stderr
cdhash=re.search(r'^CDHash=([a-f0-9]{40})$',details,re.M)[1];assert re.search(r'^Page size=4096$',details,re.M)
own_hashes=[cdhash];plugin_report={}
if consumer:
    policy=OUT/'consumer-entitlements.plist';policy.write_bytes(plistlib.dumps({'com.apple.private.security.no-container':True}))
    run(['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.blu-sharky.sandbox-consumer','--entitlements',policy,consumer]);run(['codesign','--verify','--strict',consumer])
    detail=subprocess.run(['codesign','--display','--verbose=4',consumer],capture_output=True,text=True,check=True).stderr
    consumer_cdhash=re.search(r'^CDHash=([a-f0-9]{40})$',detail,re.M)[1];assert re.search(r'^Page size=4096$',detail,re.M)
    own_hashes.append(consumer_cdhash);plugin_report.update(consumer_sha256=sha(consumer),consumer_cdhash=consumer_cdhash)
if plugin:
    run(['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.blu-sharky.NativeOfflineMetal',plugin.parent]);run(['codesign','--verify','--strict',plugin.parent])
    plugin_detail=subprocess.run(['codesign','--display','--verbose=4',plugin.parent],capture_output=True,text=True,check=True).stderr
    plugin_cdhash=re.search(r'^CDHash=([a-f0-9]{40})$',plugin_detail,re.M)[1];assert re.search(r'^Page size=4096$',plugin_detail,re.M)
    own_hashes.append(plugin_cdhash)
    plugin_report=dict(plugin_sha256=sha(plugin),plugin_cdhash=plugin_cdhash,plugin_info_sha256=sha(plugin.parent/'Info.plist'),plugin_resources_sha256=sha(plugin.parent/'_CodeSignature/CodeResources'))
own_hashes.sort()
module=struct.pack('<I16sI',2,uuid.uuid5(uuid.NAMESPACE_URL,kind+':'+':'.join(own_hashes)).bytes,len(own_hashes))+b''.join(bytes.fromhex(h)+bytes([2,0,0,0]) for h in own_hashes)
original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes();assert struct.unpack_from('<I',original)[0]==2
starts=struct.unpack_from('<2I',original,4);modules=[module,original[starts[0]:starts[1]],original[starts[1]:]]
cursor=4+4*len(modules);offsets=[]
for data in modules:offsets.append(cursor);cursor+=len(data)
bundle=struct.pack('<'+'I'*(len(modules)+1),len(modules),*offsets)+b''.join(modules);(OUT/'offline-trust.bundle').write_bytes(bundle)
sources=[source,kit/'OfflineMetalFrontend.mm',kit/'OfflineMetalWire.h',kit/'OfflineMetalFunctionWire.h',kit/'OfflineMetalQueueWire.h',kit/'OfflineMetalTextureWire.h',kit/'OfflineMetalDepthStencilWire.h',kit/'OfflineMetalVertexWire.h',kit/'OfflineMetalFenceWire.h',kit/'OfflineMetalCommandWire.h',kit/'OfflineMetalEventWire.h',kit/'OfflineMetalSharedEventWire.h',kit/'HostMetalCommandServer.mm',kit/'HostMetalCommandServer.h',kit/'OfflineMetalSessionWire.h']
if a.vertex_inputs:sources.append(ROOT/'kits/sheng/OfflineMetalProcesses.mm')
if a.fences:sources.append(ROOT/'kits/sheng/OriginalMetalLayerDiagnostic.mm')
if a.plugin_bundle:sources.extend([ROOT/'kits/sheng/OfflineMetalSharedSurfaces.mm',kit/'OfflineMetalPlugin.mm',ROOT/'kits/sheng/OfflineMetalAddon.h'])
if a.present:sources.append(ROOT/'drivers/GopFramebuffer/NativeGOPWire.h')
if a.renderbox_draw or a.renderbox_present or a.metal_layer or a.private_texture or a.depth_stencil:sources.append(ROOT/'kits/sheng/OfflineRenderBoxDevice.mm')
if a.renderbox_present or a.metal_layer:sources.extend([ROOT/'kits/sheng/OfflineRenderBoxDraw.mm',ROOT/'kits/sheng/HostMetalDiagnostic.mm'])
manifest=OUT/'test-inputs.json';manifest.write_text(json.dumps(dict(sources={str(p.relative_to(ROOT)):sha(p) for p in sources},
    explicit_partial_frontend=True,system_device_discovery=False,adreno_execution=False,original_desktop=False),indent=2)+'\n')
(OUT/'build-report.json').write_text(json.dumps(dict(process_sha256=sha(exe),process_bytes=exe.stat().st_size,source_sha256=sha(source),
    manifest_sha256=sha(manifest),cdhash=cdhash,code_signature_page_bytes=4096,trust_bundle_sha256=hashlib.sha256(bundle).hexdigest(),
    **plugin_report,entitlements=entitlements,original_trust_modules_unchanged=True,offline_test_only=True,native_execution_qualified=False),indent=2)+'\n')
print('PASS: partial real Metal API frontend and explicit own instrument compile as ARM64/iOS17, signed with 4K CodeDirectory')
