#!/usr/bin/env python3
"""Observe genuine native Mach shared-event rights and original handle lifetime."""
import argparse,hashlib,json,os,shutil,subprocess,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--default-off',action='store_true');p.add_argument('--api-validation',action='store_true');p.add_argument('--notifications',action='store_true');p.add_argument('--coding',action='store_true');p.add_argument('--cross-process',action='store_true');a=p.parse_args()
if a.notifications and (a.coding or a.cross_process):p.error('Select notifications or secure coding as independent instruments')
if a.default_off and a.api_validation:p.error('Select the original launchd control or GPU API validation')
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
if subprocess.run(['pgrep','-f','qemu-system-aarch64'],capture_output=True).returncode==0:raise SystemExit('Another QEMU is running')
kind='offline-metal-shared-cross-process' if a.cross_process else 'offline-metal-shared-coding' if a.coding else 'offline-metal-shared-notifications' if a.notifications else 'offline-metal-shared-events';out=ROOT/'build'/kind
fixture=ROOT/'build'/('darwinboot-owner-hvf-'+kind+('-default-off' if a.default_off else '-api-validation' if a.api_validation else ''))
if (fixture/'gdb-stop.json').exists():raise SystemExit('Preserve the existing observation before another run')
manifest=json.loads((out/'test-inputs.json').read_text())
for name,digest in manifest['sources'].items():
 if sha(ROOT/name)!=digest:raise SystemExit('Exact actual instrument source required: '+name)
host=ROOT/'build/host-metal/host-metal-originals';qualification=json.loads((host.parent/'qualification.json').read_text())
if sha(host)!=qualification['process_sha256'] or sha(ROOT/'kits/sheng/HostMetalDiagnostic.mm')!=qualification['source_sha256']:raise SystemExit('Exact actual host diagnostic required')
for name,digest in qualification['backend_sources'].items():
 if sha(ROOT/name)!=digest:raise SystemExit('Exact actual host backend required: '+name)
base=ROOT/'build/xnu-prelink/native-owner-metal-shared-events-candidate';plan=json.loads(base.with_suffix('.json').read_text())
if sha(base.with_suffix('.macho'))!=plan['candidate_sha256']:raise SystemExit('Exact immutable candidate required')
cmd=['python3',str(ROOT/'scripts/probe-darwinboot-xnu.py'),'--kernel',str(base.with_suffix('.macho')),'--prelink-report',str(base.with_suffix('.json')),'--output',str(fixture),
 '--virtio-udif-image','.cache/firmware/target-volumes/044-76040-038.dmg','--virtio-extra-block-image','.cache/firmware/target-volumes/044-76034-038.dmg',
 '--virtio-mmio-fdt','build/virtio-block-fixture/qemu-virt.dtb','--root-match-volume-uuid','04769D26-2CA0-4825-B443-E2E5F88300F2',
 '--trust-cache',str(out/'offline-trust.bundle'),'--armvirt','--armcpu','--qemu-vmm-present','--qemu-accel','hvf','--owned-memory-size','0x20000000',
 '--nvram-proxy','--crypto-hash-method','sha2-384','--amfi-platform-only','--load-apfs','--load-iogpu','--inspect-owner-root','--inspect-dyld-flags','--check-commpage',
 '--skip-initializer-traces','--timeout','60','--data-ramdisk',str(out/'compiler-data-32.cdr'),'--qemu-ramfb','--inspect-boot-video','--offline-gpu-broker',
 '--load-mobile-graphics','--mobile-graphics-client-probe','--legacy-display-phase','4','--legacy-display-gop','--legacy-display-register','--legacy-display-unit-scale',
 '--legacy-display-logical-pm','--legacy-display-validate','--legacy-display-transfer','--inspect-gop-scanout-bytes','1920000',
 '--native-air-init','--break-symbol','_exception_triage','--entry-process-name','air-native','--entry-register-equals','0:6','--inspect-current-process','--inspect-entry-user-state','--read-saved-user-register','13:100',
 '--capture-qemu-screen']
cmd+=['--legacy-display-retire']
if a.default_off:
 cmd.remove('--offline-gpu-broker');cmd.remove('--native-air-init');cmd.remove('--legacy-display-retire');cmd.remove('--entry-process-name');cmd.remove('air-native');cmd.remove('--entry-register-equals');cmd.remove('0:6');cmd[cmd.index('--break-symbol')+1]='_panic';cmd+=['--follow-target-main']
 subprocess.run(cmd,cwd=ROOT,check=True,timeout=180)
 print('Actual default-off original launchd observation captured; no GPU protocol backend started')
 raise SystemExit(0)
tag='api-validation' if a.api_validation else 'enabled';log=out/('host-'+tag+'-report.json');errors=out/('host-'+tag+'-stderr.log')
oslib=ROOT/'.cache/firmware/target-volumes/os-mount/System/Library'
paths=[oslib/'Frameworks/QuartzCore.framework/default.metallib',oslib/'PrivateFrameworks/RenderBox.framework/default.metallib']
with tempfile.TemporaryDirectory(prefix='rb-present-',dir='/tmp') as directory:
 socket=Path(directory)/'gpu.sock'
 with log.open('w') as report,errors.open('w') as stderr:
  process=subprocess.Popen([str(host),*map(str,paths),'--metal-session-socket',str(socket)],stdout=report,stderr=stderr,env={**os.environ,**({'MTL_DEBUG_LAYER':'1'} if a.api_validation else {})})
  try:
   deadline=time.monotonic()+15
   while not socket.exists():
    if process.poll() is not None or time.monotonic()>=deadline:raise SystemExit('Actual host GPU startup failed')
    time.sleep(.02)
   result=subprocess.run([*cmd,'--virtio-console-socket',str(socket)],cwd=ROOT,timeout=180)
   if result.returncode:raise SystemExit(result.returncode)
   process.wait(timeout=5)
  finally:
   if process.poll() is None:process.terminate();process.wait(timeout=5)
(out/('host-'+tag+'-provenance.json')).write_text(json.dumps(dict(process_sha256=sha(host),qualification_sha256=sha(host.parent/'qualification.json'),
 source_sha256=qualification['source_sha256'],backend_sources=qualification['backend_sources'],host_exit=process.returncode,
 report_sha256=sha(log),candidate_sha256=plan['candidate_sha256'],api_validation_requested=a.api_validation,original_desktop=False,adreno_execution=False),indent=2)+'\n')
print('Actual native shared-event observation captured; inspect genuine Mach right transfers, original handle references and creator-exit GPU work')
