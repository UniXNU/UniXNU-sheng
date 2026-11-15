#!/bin/sh
# Capture the memory, swap, storage and charger state of one timed boot.
set -u
/bin/mkdir -p /Users/Shared/unixnu-test
/bin/chmod 1777 /Users/Shared/unixnu-test
/bin/rm -f /Users/Shared/unixnu-test/benchmark.done /Users/Shared/unixnu-test/reboot-requested
snapshot() {
    /bin/date -u
    /usr/bin/sw_vers
    /usr/bin/id unixnu
    /usr/sbin/sysctl hw.memsize hw.ncpu vm.swapusage kern.boot_payload_memory
    /usr/bin/vm_stat
    /sbin/mount
    /bin/ls -ld /private/var/vm
    /bin/ls -lh /private/var/vm
    /usr/sbin/ioreg -r -c IOPMPowerSource -l
    /bin/ps -axo pid,pcpu,rss,comm
}
snapshot
capture_and_reboot() {
    snapshot
    cache=/private/var/tmp/NativeAdrenoMetal.cache
    if [ -d "$cache" ]; then
        /usr/bin/touch "$cache/prime-resume"
        owner=$(/usr/bin/stat -f '%u:%g' "$cache")
        /usr/sbin/chown "$owner" "$cache/prime-resume"
    fi
    /usr/sbin/sysctl -w kern.unixnu_diag_request=1 || return 1
    for wait in $(/usr/bin/seq 1 60); do
        status=$(/usr/sbin/sysctl -n kern.unixnu_diag_status)
        /bin/echo "diagnostic export status=$status"
        if [ "$status" = 2 ]; then
            /bin/sleep 10
            /bin/sync
            /sbin/reboot
            return 0
        fi
        case "$status" in -*) return 1 ;; esac
        /bin/sleep 2
    done
    return 1
}
for attempt in $(/usr/bin/seq 1 230); do
    if [ -f /Users/Shared/unixnu-test/benchmark.done ] || [ -f /Users/Shared/unixnu-test/reboot-requested ]; then
        capture_and_reboot
        exit $?
    fi
    if [ "$attempt" -gt 4 ]; then
        values=$(/usr/sbin/ioreg -r -c IOPMPowerSource -l)
        capacity=$(/bin/echo "$values" | /usr/bin/awk '/"CurrentCapacity" = [0-9]/ {print $NF; exit}')
        maximum=$(/bin/echo "$values" | /usr/bin/awk '/"MaxCapacity" = [0-9]/ {print $NF; exit}')
        if [ -n "$capacity" ] && [ -n "$maximum" ] && [ "$maximum" -gt 0 ]; then
            percent=$((capacity * 100 / maximum))
            /bin/echo "battery=$percent%"
            if [ "$percent" -lt 15 ]; then
                /bin/echo 'Returning to fastboot to recharge before continuing priming.'
                capture_and_reboot
                exit $?
            fi
        fi
    fi
    /bin/sleep 30
done
capture_and_reboot
