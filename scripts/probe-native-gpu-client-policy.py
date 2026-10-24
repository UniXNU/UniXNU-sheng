#!/usr/bin/env python3
"""Observe original Sandbox parent-class GPU-client permission and its unrelated-class control."""
import argparse,os,hashlib,json,shutil,subprocess,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--policy',choices=['full','parent-client','unrelated-client'],default='full');p.add_argument('--candidate-base',type=Path,default=ROOT/'build/xnu-prelink/native-owner-original-provider-probe-candidate');a=p.parse_args()
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
if subprocess.run(['pgrep','-f','qemu-system-aarch64'],capture_output=True).returncode==0:raise SystemExit('Another QEMU is running')
kind='offline-metal-processes'+('' if a.policy=='full' else '-'+a.policy);out=ROOT/'build'/kind
fixture=ROOT/'build'/('darwinboot-owner-hvf-'+kind)
if (fixture/'gdb-stop.json').exists():raise SystemExit('Preserve the existing observation before another run')
manifest=json.loads((out/'test-inputs.json').read_text())
for name,digest in manifest['sources'].items():
 if sha(ROOT/name)!=digest:raise SystemExit('Exact actual instrument source required: '+name)
host=ROOT/'build/host-metal/host-metal-originals';qualification=json.loads((host.parent/'qualification.json').read_text())
if sha(host)!=qualification['process_sha256'] or sha(ROOT/'kits/sheng/HostMetalDiagnostic.mm')!=qualification['source_sha256']:raise SystemExit('Exact actual host diagnostic required')
for name,digest in qualification['backend_sources'].items():
 if sha(ROOT/name)!=digest:raise SystemExit('Exact actual host backend required: '+name)
base=a.candidate_base.resolve();plan=json.loads(base.with_suffix('.json').read_text())
if sha(base.with_suffix('.macho'))!=plan['candidate_sha256']:raise SystemExit('Exact immutable candidate required')
cmd=['python3',str(ROOT/'scripts/probe-darwinboot-xnu.py'),'--kernel',str(base.with_suffix('.macho')),'--prelink-report',str(base.with_suffix('.json')),'--output',str(fixture),
 '--virtio-udif-image','.cache/firmware/target-volumes/044-76040-038.dmg','--virtio-extra-block-image','.cache/firmware/target-volumes/044-76034-038.dmg',
 '--virtio-mmio-fdt','build/virtio-block-fixture/qemu-virt.dtb','--root-match-volume-uuid','04769D26-2CA0-4825-B443-E2E5F88300F2',
 '--trust-cache',str(out/'offline-trust.bundle'),'--armvirt','--armcpu','--qemu-vmm-present','--qemu-accel','hvf','--owned-memory-size','0x20000000',
 '--nvram-proxy','--crypto-hash-method','sha2-384','--amfi-platform-only','--load-apfs','--load-iogpu','--inspect-owner-root','--inspect-mac-policies','--inspect-dyld-flags','--check-commpage',
 '--skip-initializer-traces','--timeout','60','--data-ramdisk',str(out/'compiler-data-32.cdr'),'--qemu-ramfb','--inspect-boot-video','--offline-gpu-broker',
 '--load-mobile-graphics','--mobile-graphics-client-probe','--legacy-display-phase','4','--legacy-display-gop','--legacy-display-register','--legacy-display-unit-scale',
 '--legacy-display-logical-pm','--legacy-display-validate','--legacy-display-transfer','--inspect-gop-scanout-bytes','1920000',
 '--native-air-init','--break-symbol','_exception_triage','--entry-process-name','air-native','--entry-register-equals','0:6','--inspect-current-process','--inspect-entry-user-state','--read-saved-user-register','13:100',
 '--read-saved-user-register','14:1000','--capture-qemu-screen']
cmd+=['--legacy-display-retire']
tag='enabled';log=out/('host-'+tag+'-report.json');errors=out/('host-'+tag+'-stderr.log')
oslib=ROOT/'.cache/firmware/target-volumes/os-mount/System/Library'
paths=[oslib/'Frameworks/QuartzCore.framework/default.metallib',oslib/'PrivateFrameworks/RenderBox.framework/default.metallib']
with tempfile.TemporaryDirectory(prefix='rb-present-',dir='/tmp') as directory:
 socket=Path(directory)/'gpu.sock'
 with log.open('w') as report,errors.open('w') as stderr:
  process=subprocess.Popen([str(host),*map(str,paths),'--metal-session-socket',str(socket)],stdout=report,stderr=stderr,env={**os.environ,'MTL_DEBUG_LAYER':'1'})
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
 report_sha256=sha(log),candidate_sha256=plan['candidate_sha256'],api_validation_requested=True,original_desktop=False,adreno_execution=False),indent=2)+'\n')
print('Actual original Sandbox client-policy observation captured; inspect real parent/unrelated outcomes, namespace lifetime and signed permissions')
