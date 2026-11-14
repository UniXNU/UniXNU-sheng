#!/bin/sh
# Capture the memory, swap, storage and charger state of one timed boot.
set -u
/bin/mkdir -p /Users/Shared/unixnu-test
/bin/chmod 1777 /Users/Shared/unixnu-test
for delay in 30 60 90 90; do
    /bin/sleep "$delay"
    /bin/date -u
    /usr/sbin/sysctl hw.memsize hw.ncpu vm.swapusage kern.boot_payload_memory
    /usr/bin/vm_stat
    /sbin/mount
    /usr/sbin/ioreg -r -c IOPMPowerSource -l
    /bin/ps -axo pid,pcpu,rss,comm
done
