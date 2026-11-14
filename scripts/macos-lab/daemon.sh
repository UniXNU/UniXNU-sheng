#!/bin/sh
# Capture the memory, swap, storage and charger state of one timed boot.
set -u
/bin/mkdir -p /Users/Shared/unixnu-test
/bin/chmod 1777 /Users/Shared/unixnu-test
/bin/rm -f /Users/Shared/unixnu-test/benchmark.done
snapshot() {
    /bin/date -u
    /usr/sbin/sysctl hw.memsize hw.ncpu vm.swapusage kern.boot_payload_memory
    /usr/bin/vm_stat
    /sbin/mount
    /usr/sbin/ioreg -r -c IOPMPowerSource -l
    /bin/ps -axo pid,pcpu,rss,comm
}
snapshot
for attempt in $(/usr/bin/seq 1 230); do
    if [ -f /Users/Shared/unixnu-test/benchmark.done ]; then
        snapshot
        /usr/sbin/sysctl -w kern.unixnu_diag_request=1 || exit 1
        for wait in $(/usr/bin/seq 1 60); do
            status=$(/usr/sbin/sysctl -n kern.unixnu_diag_status)
            /bin/echo "diagnostic export status=$status"
            if [ "$status" = 2 ]; then
                /bin/sleep 10
                /bin/sync
                /sbin/reboot
                exit 0
            fi
            case "$status" in -*) exit 1 ;; esac
            /bin/sleep 2
        done
        exit 1
    fi
    /bin/sleep 30
done
