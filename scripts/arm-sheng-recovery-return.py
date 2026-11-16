#!/usr/bin/env python3
"""Arm or renew a monotonic recovery timer that reboots sheng into fastboot."""
import argparse
from pathlib import Path
import subprocess
import tempfile


SCRIPT = '''#!/system/bin/sh
while sleep 15; do
    read uptime rest < /proc/uptime
    read deadline < /tmp/unixnu-recovery-deadline
    if [ "${uptime%%.*}" -ge "$deadline" ]; then
        reboot bootloader
    fi
done
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--seconds', type=int, default=7200)
    args = parser.parse_args()
    if not 60 <= args.seconds <= 86400:
        parser.error('--seconds must be in 60..86400')
    adb = ['adb', '-s', args.serial]
    state = subprocess.check_output([*adb, 'get-state'], text=True, timeout=15).strip()
    if state != 'recovery':
        parser.error('the selected device must be in ADB recovery')
    with tempfile.TemporaryDirectory(prefix='sheng-recovery-return-') as directory:
        script = Path(directory) / 'return.sh'
        script.write_text(SCRIPT)
        subprocess.run([*adb, 'push', str(script), '/tmp/unixnu-recovery-return.sh'], check=True, timeout=30)
    command = f'''
read uptime rest < /proc/uptime
echo $((${{uptime%%.*}} + {args.seconds})) > /tmp/unixnu-recovery-deadline
pid=$(cat /tmp/unixnu-recovery-return.pid 2>/dev/null)
active=0
if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    case "$(tr '\\000' ' ' < /proc/$pid/cmdline)" in
        */tmp/unixnu-recovery-return.sh*) active=1 ;;
    esac
fi
if [ "$active" = 0 ]; then
    setsid sh /tmp/unixnu-recovery-return.sh </dev/null >/tmp/unixnu-recovery-return.log 2>&1 &
    pid=$!
    echo "$pid" > /tmp/unixnu-recovery-return.pid
fi
kill -0 "$pid" || exit 1
echo "recovery return: seconds={args.seconds} pid=$pid"
'''
    subprocess.run([*adb, 'shell', command], check=True, timeout=15)


if __name__ == '__main__':
    main()
