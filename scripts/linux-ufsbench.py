#!/usr/bin/env python3
"""Linux UFS baseline on sheng, unattended. With the tablet in fastboot: build package/linux-log/ufsbench.c (aarch64, static, no libc)
when needed, boot the log reader (mainline kernel, read only), push ufsbench over its USB serial shell, run it on the flashed root image
region of userdata, collect the kernel's UFS link and queue facts, then send the tablet back to fastboot with /bin/rebootbl. Nothing is
mounted or written on UFS. sheng-ufs-bench=1 runs the same tests on the same LBAs under XNU; scripts/compare-ufsbench.py compares them.

usage: linux-ufsbench.py --reader sheng-linux-log.img --out DIR [--serial SERIAL] [--seconds 3]
Output: DIR/bench.txt (one line per test), DIR/{kernel,ufs,queue,dmesg}.txt and DIR/run.txt."""
import argparse, base64, hashlib, importlib.util, shutil, subprocess, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'package/linux-log/ufsbench.c'
FIRST, REGION = 1 << 20, 14304 << 20   # inside the flashed root image (out/.../userdata/userdata-root.json image_bytes)

spec = importlib.util.spec_from_file_location('read_sheng_log', ROOT / 'scripts/read-sheng-log.py')
rsl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rsl)


def build(out):
    binary = out / 'ufsbench'
    clang, lld = shutil.which('clang'), shutil.which('ld.lld')
    if not clang or not lld:
        raise SystemExit('clang and ld.lld are required to build ufsbench')
    with tempfile.TemporaryDirectory() as tmp:
        obj = Path(tmp) / 'ufsbench.o'
        subprocess.run([clang, '--target=aarch64-linux-gnu', '-ffreestanding', '-nostdlib', '-static', '-O2', '-fno-stack-protector',
                        '-fno-pic', '-Wall', '-Wextra', '-Werror', '-c', str(SOURCE), '-o', str(obj)], check=True)
        subprocess.run([lld, '-static', '-e', '_start', str(obj), '-o', str(binary)], check=True)
    return binary.read_bytes()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--reader', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--serial', default='b714917c')
    ap.add_argument('--seconds', type=int, default=3)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    log = (a.out / 'run.txt').open('w')

    def say(text):
        line = f'{time.strftime("%H:%M:%S")} {text}'
        print(line)
        log.write(line + '\n')
        log.flush()

    def present():
        return a.serial in subprocess.run(['fastboot', 'devices'], capture_output=True, text=True).stdout

    binary = build(a.out)
    if not present():
        raise SystemExit('tablet not in fastboot')
    subprocess.run(['fastboot', '-s', a.serial, 'boot', str(a.reader)], check=True)
    say('reader booted')
    port = rsl.Port(rsl.find_port(180))
    time.sleep(4)
    port.send('\n')
    time.sleep(0.5)
    port.send('stty -echo 2>/dev/null; echo R$((40+2))R\n')
    port.read_until(b'R42R', 60)

    def run(command, timeout=120):
        port.send('echo @@B$((40+2))@@; ' + command + '; echo @@E$((40+2))@@\n')
        data = port.read_until(b'@@E42@@', timeout)
        return data[data.rindex(b'@@B42@@') + 7:data.rindex(b'@@E42@@')].strip().decode(errors='replace')

    text = base64.b64encode(binary).decode()
    run('rm -f /run/b.b64')
    for i in range(0, len(text), 900):   # short lines: the tty's canonical line limit
        run(f"echo '{text[i:i + 900]}' >> /run/b.b64", 30)
    got = run('base64 -d /run/b.b64 > /run/ufsbench && chmod 755 /run/ufsbench && md5sum /run/ufsbench')
    if hashlib.md5(binary).hexdigest() not in got:
        raise SystemExit(f'push failed: {got}')
    part = run("for u in /sys/class/block/*/uevent; do grep -q '^PARTNAME=userdata$' $u && basename $(dirname $u); done")
    disk = run(f'basename $(readlink -f /sys/class/block/{part}/..)')
    say(f'userdata=/dev/{part} disk={disk}')
    facts = {
        'kernel': 'uname -a',
        'ufs': 'for d in /sys/bus/platform/devices/*ufshc*; do echo $d; for f in $d/power_info/* $d/device_descriptor/specification_version '
               '$d/device_descriptor/queue_depth $d/auto_hibern8 $d/wb_on $d/clkscale_enable; do [ -e $f ] && echo "$f=$(cat $f 2>&1)"; done; done',
        'queue': f'for f in nr_requests scheduler max_sectors_kb nr_hw_queues; do echo "$f=$(cat /sys/block/{disk}/queue/$f 2>/dev/null)"; done',
        'dmesg': "dmesg | grep -iE 'ufs|mcq|gear' | tail -40",
    }
    for name, command in facts.items():
        (a.out / f'{name}.txt').write_text(run(command, 60) + '\n')
    bench = run(f'/run/ufsbench /dev/{part} {FIRST} {REGION} {a.seconds}', 600)
    (a.out / 'bench.txt').write_text(bench + '\n')
    say('bench done')
    print(bench)
    port.send('/bin/rebootbl\n')
    for _ in range(90):
        time.sleep(2)
        if present():
            say('back in fastboot')
            return
    say('the tablet did not come back to fastboot')


if __name__ == '__main__':
    main()
