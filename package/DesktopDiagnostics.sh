#!/bin/bash
set -uo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
umask 077
log=/private/var/log/sheng-desktop-diagnostics.log
exec > "$log" 2>&1
printf 'SHENG desktop diagnostics - version 1\n'
for delay in 15 45 60; do
    sleep "$delay"
    printf '\nSnapshot - %s\n' "$(date -u '+%FT%TZ')"
    sysctl hw.memsize vm.swapusage
    vm_stat
    printf '\nVM and cryptex mounts\n'
    mount | grep -E ' on / |Data|VM|Cryptex|Preboot|AppCryptex'
    for path in /System/Volumes/VM /private/var/vm /Applications/Safari.app \
        /System/Applications/Safari.app /System/Cryptexes/App \
        /System/Volumes/Preboot/Cryptexes/App \
        /System/Cryptexes/App/System/Applications/Safari.app/Contents/Info.plist; do
        ls -ld "$path"
    done
    printf '\nLargest resident processes - KiB\n'
    ps -axo pid,uid,rss,comm | sort -nr -k3 | head -16
    printf '\nPower settings and assertions\n'
    pmset -g custom
    pmset -g assertions
    printf '\nWireless interface\n'
    ifconfig en0
done
printf '\nDiagnostics complete\n'
