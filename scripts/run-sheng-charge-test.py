#!/usr/bin/env python3
"""Boot the charge reader, detach authentication, collect logs after a charger swap and return to fastboot."""
import argparse
import glob
import gzip
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('read_sheng_log', ROOT / 'scripts/read-sheng-log.py')
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


def detached_command(wait_pd, snapshot=False):
    operation = '' if snapshot else f'--authenticate --wait-pd {wait_pd} '
    job = f'''echo "$$" > /run/charge.started
/bin/sheng-charge-setup {operation}--out /run/charge.log
result=$?
dmesg > /run/charge-dmesg.txt
echo "$result" > /run/charge.status'''
    # Ignored SIGHUP survives exec; setsid creates a session without a controlling terminal.
    launch = "trap '' HUP\nexec setsid /bin/sh -c " + shlex.quote(job)
    return ('/bin/sh -c ' + shlex.quote(launch) +
            ' </dev/null >/run/charge-console.log 2>&1 & echo detached')


class ChargeTest:
    def __init__(self, args):
        self.args = args
        self.started = time.monotonic()
        self.end = self.started + args.timeout
        self.work_end = self.end - 30
        self.shell = None
        self.booted = False
        self.report = dict(serial=args.serial, reader=str(args.reader), wait_pd=args.wait_pd,
                           mode='snapshot' if args.snapshot else 'charger-swap',
                           timeout=args.timeout, fastboot=False, auth_exit=None)

    def remaining(self, limit, end=None):
        remaining = (self.end if end is None else end) - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('charge test reached its time limit')
        return min(limit, remaining)

    def fastboot(self, *command, timeout=10):
        return subprocess.run(['fastboot', '-s', self.args.serial, *command], capture_output=True,
                              text=True, check=True, timeout=self.remaining(timeout))

    def present(self):
        result = self.fastboot('devices', timeout=5)
        return any(line.split() and line.split()[0] == self.args.serial for line in result.stdout.splitlines())

    def close(self):
        if self.shell is not None:
            self.shell.close()
            self.shell = None

    def connect(self, end):
        last = 'no USB serial port'
        while time.monotonic() < end:
            if self.report.get('job_pid') and self.present():
                self.report['fastboot'] = True
                raise RuntimeError('reader returned to fastboot before charge log collection')
            ports = [self.args.port] if self.args.port else sorted(glob.glob('/dev/cu.usbmodem*'))
            ports = [port for port in ports if os.path.exists(port)]
            if len(ports) > 1:
                raise RuntimeError('multiple USB serial ports: select the reader with --port')
            if ports:
                try:
                    self.shell = reader.Shell(ports[0], min(end, time.monotonic() + 8))
                    identity = self.shell.run('grep -aq sheng /proc/device-tree/compatible && '
                                              'test -x /bin/sheng-charge-setup && echo SHENG_CHARGE_READER', 5)
                    if b'SHENG_CHARGE_READER' not in identity:
                        self.close()
                        raise RuntimeError('the serial shell is not a sheng charge reader')
                    self.shell.port.deadline = end
                    return ports[0]
                except (OSError, SystemExit, TimeoutError) as error:
                    last = str(error)
                    self.close()
            time.sleep(min(0.5, self.remaining(0.5, end)))
        raise TimeoutError('reader USB shell unavailable: ' + last)

    def collect(self):
        def status():
            for attempt in range(3):
                try:
                    return self.shell.run('if [ -s /run/charge.status ]; then cat /run/charge.status; '
                                          'else echo pending; fi', 5).decode().strip()
                except (OSError, SystemExit, TimeoutError):
                    if attempt == 2:
                        raise
                    self.close()
                    self.report['shell_reconnects'] = self.report.get('shell_reconnects', 0) + 1
                    self.connect(min(self.work_end, time.monotonic() + 8))
        state = status()
        while state == 'pending':
            time.sleep(self.remaining(1, self.work_end))
            state = status()
        self.report['auth_exit'] = int(state)
        self.shell.run('dmesg > /run/charge-dmesg-return.txt', 5)
        for name in ('charge.log', 'charge-console.log', 'charge-dmesg.txt', 'charge-dmesg-return.txt', 'charge.status'):
            exists = self.shell.run(f'test -f /run/{name} && echo present', 5)
            if b'present' not in exists:
                continue
            data = gzip.decompress(self.shell.pull('/run/' + name, timeout=10, retries=3))
            (self.args.out / name).write_bytes(data)
        log_path = self.args.out / 'charge.log'
        log = log_path.read_text(errors='replace') if log_path.exists() else ''
        self.report['auth_completed'] = '[charge auth] state=16 result=' in log
        self.report['source_not_pd'] = '[charge auth] source-not-pd' in log
        self.report['source_changed'] = '[charge auth] source-changed' in log

    def run(self):
        if not self.present():
            raise RuntimeError('the selected tablet must be in fastboot')
        product = self.fastboot('getvar', 'product')
        if not any(line.strip() == 'product: sheng' for line in (product.stdout + product.stderr).splitlines()):
            raise RuntimeError('fastboot product must be sheng')
        self.booted = True
        self.fastboot('boot', str(self.args.reader), timeout=self.remaining(120, self.work_end))
        port = self.connect(min(self.work_end, time.monotonic() + 60))
        self.shell.port.deadline = self.work_end
        window = max(0, int(self.work_end - time.monotonic() - 15))
        if not self.args.snapshot and window < 150:
            raise TimeoutError('reader startup left less than 150 seconds for the charger swap and log collection')
        self.shell.run(detached_command(self.args.wait_pd, self.args.snapshot), 5)
        started = self.shell.run('for i in 1 2 3 4 5; do [ -s /run/charge.started ] && break; '
                                 'sleep 1; done; cat /run/charge.started', 8)
        if not started.isdigit():
            raise RuntimeError('detached authentication did not start')
        self.report['job_pid'] = int(started)
        if self.args.snapshot:
            print('Collecting the connected source, battery and charge-pump readings.', flush=True)
            self.collect()
            return
        window = max(0, int(self.work_end - time.monotonic() - 15))
        print(f'Authentication runs independently of USB. Connect the PD charger now, leave it connected for '
              f'140 seconds, then reconnect this PC within {window} seconds.', flush=True)
        while os.path.exists(port):
            time.sleep(self.remaining(0.5, self.work_end))
        self.close()
        self.report['usb_disconnected'] = True
        print('USB disconnected; waiting for the PC connection to return.', flush=True)
        self.connect(self.work_end)
        print('Reader reconnected; collecting authentication and kernel logs.', flush=True)
        self.collect()

    def finish(self):
        try:
            if not self.booted:
                return
            try:
                self.report['fastboot'] = self.present()
            except subprocess.SubprocessError:
                pass
            if not self.report['fastboot']:
                try:
                    if self.shell is None:
                        self.connect(min(self.end - 5, time.monotonic() + 5))
                    self.shell.port.send('/bin/rebootbl\n')
                except (OSError, SystemExit, TimeoutError) as error:
                    self.report['return_error'] = str(error)
                    self.close()
                    self.connect(min(self.end - 5, time.monotonic() + 5))
                    self.shell.port.send('/bin/rebootbl\n')
                finally:
                    self.close()
            while not self.report['fastboot']:
                self.remaining(1)
                try:
                    self.report['fastboot'] = self.present()
                except subprocess.SubprocessError:
                    pass
                if not self.report['fastboot']:
                    time.sleep(self.remaining(1))
        except (OSError, SystemExit, TimeoutError, RuntimeError, subprocess.SubprocessError) as error:
            self.report['return_error'] = str(error)
        finally:
            self.close()
            self.report['elapsed_seconds'] = round(time.monotonic() - self.started, 1)
            (self.args.out / 'result.json').write_text(json.dumps(self.report, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True, help='fastboot device serial')
    parser.add_argument('--reader', required=True, type=Path, help='image from prepare-linux-charge-reader.py')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/sheng-charge' / time.strftime('%Y%m%d-%H%M%S'))
    parser.add_argument('--port', help='reader USB serial path when several ports are present')
    parser.add_argument('--snapshot', action='store_true', help='collect the connected source without a charger swap')
    parser.add_argument('--wait-pd', type=int, default=90, help='PD connection wait, at most 120 seconds')
    parser.add_argument('--timeout', type=int, default=270, help='total host time including return to fastboot, 180 to 270 seconds')
    args = parser.parse_args()
    args.reader = args.reader.resolve()
    if not args.reader.is_file() or not 1 <= args.wait_pd <= 120 or not 180 <= args.timeout <= 270:
        parser.error('require an existing charge reader, --wait-pd 1..120 and --timeout 180..270')
    args.out.mkdir(parents=True, exist_ok=True)
    hold_awake = None
    if sys.platform == 'darwin':
        hold_awake = subprocess.Popen(['/usr/bin/caffeinate', '-is', '-w', str(os.getpid())])
    task = ChargeTest(args)
    try:
        task.run()
    except (OSError, SystemExit, TimeoutError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        task.report['error'] = str(error)
    finally:
        try:
            task.finish()
        finally:
            if hold_awake is not None:
                hold_awake.terminate()
    print(f'{args.out}: capture exit={task.report["auth_exit"]}, fastboot={task.report["fastboot"]}', flush=True)
    if task.report.get('error'):
        print(task.report['error'], file=sys.stderr)
    if not task.report['fastboot']:
        print('Fastboot was not observed within the host time limit. The reader retains its 300-second rebootbl timer.', file=sys.stderr)
    completed = task.report['auth_exit'] == 0 if args.snapshot else task.report.get('auth_completed')
    return 0 if task.report['fastboot'] and completed and not task.report.get('error') else 1


if __name__ == '__main__':
    sys.exit(main())
