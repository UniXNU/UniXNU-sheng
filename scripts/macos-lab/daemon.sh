#!/bin/sh
# Capture the memory, swap, storage and charger state of one timed boot.
set -u
if [ -f /private/var/ngpu/test/share/manual-session ]; then
    if [ -d '/Library/UniXNU/Shader Cache.app' ] && [ ! -e '/Applications/Shader Cache.app' ]; then
        /bin/ln -s '/Library/UniXNU/Shader Cache.app' '/Applications/Shader Cache.app'
    fi
    /bin/echo "manual session boot=$(/usr/sbin/sysctl -n kern.bootsessionuuid)"
    exit 0
fi
/bin/mkdir -p /Users/Shared/unixnu-test
/bin/chmod 1777 /Users/Shared/unixnu-test
/bin/rm -f /Users/Shared/unixnu-test/benchmark.done /Users/Shared/unixnu-test/reboot-requested
application='/Library/UniXNU/Shader Cache.app'
if [ -d "$application" ] && [ ! -e '/Applications/Shader Cache.app' ]; then
    /bin/ln -s "$application" '/Applications/Shader Cache.app'
fi
session=/Users/Shared/unixnu-test
for name in icon-probe.log video-probe.log icons.png; do
    if [ -s "$session/$name" ]; then
        /bin/mv -f "$session/$name" "$session/previous-$name"
    fi
done
build=/Library/GPUBundles/NativeAdrenoMetal.bundle/Contents/Resources/gpu-build.json
state=/private/var/db/unixnu-lab
icon_cache=$(/usr/bin/sudo -u unixnu /usr/bin/getconf DARWIN_USER_CACHE_DIR)
icon_command_log=${icon_cache%/}/com.apple.iconservicesagent/com.apple.metal/m2v/command-buffer.log
if [ -s /private/var/ngpu/test/share/icon-probe-path ]; then
    /bin/rm -f "$icon_command_log"
fi
/bin/mkdir -p "$state"
/bin/chmod 700 "$state"
if [ -r "$build" ]; then
    identity=$(/usr/bin/shasum -a 256 "$build" | /usr/bin/awk '{print $1}')
    previous=$(/bin/cat "$state/icon-cache-build" 2>/dev/null || :)
    if [ "$identity" != "$previous" ] || [ -s /private/var/ngpu/test/share/icon-probe-path ]; then
        cache=$(/usr/bin/sudo -u unixnu /usr/bin/getconf DARWIN_USER_CACHE_DIR)
        case "$cache" in
            /var/folders/*/C/|/private/var/folders/*/C/)
                /bin/rm -rf "$cache/com.apple.iconservices" "$cache/com.apple.iconservices.store" ;;
        esac
        /bin/rm -rf /Library/Caches/com.apple.iconservices.store
        /usr/bin/pkill -TERM -x iconservicesagent 2>/dev/null || :
        /usr/bin/pkill -TERM -x iconservicesd 2>/dev/null || :
        /bin/echo "$identity" > "$state/icon-cache-build"
        /bin/echo "Icon cache generation=$identity"
    fi
fi
keyboard=/Library/Preferences/com.apple.keyboardtype.plist
/usr/libexec/PlistBuddy -c 'Add :keyboardtype dict' "$keyboard" 2>/dev/null || :
/usr/libexec/PlistBuddy -c 'Set :keyboardtype:163-5593-0 40' "$keyboard" 2>/dev/null ||
    /usr/libexec/PlistBuddy -c 'Add :keyboardtype:163-5593-0 integer 40' "$keyboard"
boot=$(/usr/sbin/sysctl -n kern.bootsessionuuid)
/bin/echo "graphics capture boot=$boot" > "$session/graphics-events.log"
/bin/rm -f "$session/graphics-events-previous.log"
/bin/echo "graphics state boot=$boot" > "$session/graphics-state.log"
/bin/echo "video probe boot=$boot" > "$session/video-probe.log"
predicate='process == "WindowServer" OR process == "System Settings" OR process == "loginwindow" OR process BEGINSWITH "Wallpaper" OR process CONTAINS[c] "iconservices" OR process == "icon-probe" OR process == "VTDecoderXPCService" OR process == "video-probe" OR subsystem CONTAINS[c] "VideoToolbox" OR subsystem CONTAINS[c] "CoreImage"'
/usr/bin/log stream --style compact --level info --timeout 120m --predicate "$predicate" >> "$session/graphics-events.log" 2>&1 &
graphics_log=$!
trap '/bin/kill "$graphics_log" 2>/dev/null || :' EXIT
video_probe() {
    /bin/sleep 90
    tool=/private/var/ngpu/test
    [ -x "$tool/bin/video-probe" ] || return
    for fixture in "$tool/share/video-probe.mp4" "$tool/share/video-probe-10bit.mp4"; do
        [ -s "$fixture" ] || continue
        /bin/date -u >> "$session/video-probe.log"
        /bin/echo "video fixture=$fixture" >> "$session/video-probe.log"
        "$tool/bin/video-probe" "$fixture" --require-hardware >> "$session/video-probe.log" 2>&1 &
        probe=$!
        ( /bin/sleep 30; /bin/kill -KILL "$probe" 2>/dev/null ) &
        alarm=$!
        wait "$probe"
        result=$?
        /bin/kill "$alarm" 2>/dev/null || :
        /bin/echo "video probe exit=$result" >> "$session/video-probe.log"
    done
}
video_probe &
if [ -s /private/var/ngpu/test/share/graphics-comparison ]; then
    (
        delays='120 90'
        if [ "$(/bin/cat /private/var/ngpu/test/share/graphics-comparison)" = view ]; then
            delays='90 120 120 120'
        fi
        for delay in $delays; do
            /bin/sleep "$delay"
            pid=$(/usr/bin/pgrep -x WindowServer | /usr/bin/head -1)
            [ -n "$pid" ] || continue
            /bin/echo "comparison sample boot=$boot pid=$pid time=$(/bin/date -u '+%Y-%m-%dT%H:%M:%SZ')" >> "$session/graphics-state.log"
            /usr/bin/sample "$pid" 2 10 -file "$session/windowserver-sample.tmp" >/dev/null 2>&1
            /bin/cat "$session/windowserver-sample.tmp" >> "$session/graphics-state.log"
            /bin/rm -f "$session/windowserver-sample.tmp"
        done
    ) &
fi
graphics_state() {
    {
        /bin/date -u
        /bin/echo "boot=$boot"
        /bin/ps -axo pid,ppid,etime,pcpu,rss,comm | /usr/bin/awk 'NR == 1 || /WindowServer|loginwindow|Wallpaper|iconservices|icon-probe|video-probe|VTDecoder|KeyboardSetupAssistant|ShaderCache/'
        if [ -x '/Library/UniXNU/Shader Cache.app/Contents/MacOS/ShaderCache' ]; then
            '/Library/UniXNU/Shader Cache.app/Contents/MacOS/ShaderCache' --status
        fi
        /usr/sbin/ioreg -r -c AppleVideoToolboxParavirtualizationDriver -l
        /usr/sbin/ioreg -r -c NativeUfsBlock -l -d 1
        /sbin/mount | /usr/bin/awk '/Cryptexes\/App|AppCryptex/'
        /bin/ls -ld /Applications/Safari.app /System/Applications/Safari.app /System/Volumes/Preboot/Cryptexes/App/System/Applications/Safari.app
        /bin/launchctl print gui/501/org.unixnu.test.agent
        /bin/launchctl print gui/501/com.apple.iconservices.iconservicesagent
        /bin/ls -lt /Library/Logs/DiagnosticReports /Library/Logs/DiagnosticReports/Retired
    } >> "$session/graphics-state.log" 2>&1
    if [ "$(/usr/bin/stat -f %z "$session/graphics-state.log")" -gt 524288 ]; then
        /usr/bin/tail -c 262144 "$session/graphics-state.log" > "$session/graphics-state.tmp"
        /bin/mv "$session/graphics-state.tmp" "$session/graphics-state.log"
    fi
    if [ "$(/usr/bin/stat -f %z "$session/graphics-events.log")" -gt 2097152 ]; then
        /usr/bin/tail -c 2097152 "$session/graphics-events.log" > "$session/graphics-events-previous.log"
        : > "$session/graphics-events.log"
    fi
    if [ -s "$icon_command_log" ]; then
        /bin/cp "$icon_command_log" "$session/graphics-events-previous.log"
    fi
}
snapshot() {
    /bin/date -u
    /bin/echo "snapshot boot=$boot"
    /usr/bin/sw_vers
    /usr/bin/id unixnu
    /usr/sbin/sysctl hw.memsize hw.ncpu vm.swapusage kern.boot_payload_memory
    /usr/sbin/sysctl machdep.timebase_traps
    /private/var/ngpu/test/bin/clock-probe
    /usr/bin/vm_stat
    /sbin/mount
    /bin/ls -ld /private/var/vm
    /bin/ls -lh /private/var/vm
    /usr/sbin/ioreg -r -c IOPMPowerSource -l
    /bin/ps -axo pid,pcpu,rss,comm
}
snapshot
capture_and_reboot() {
    graphics_state
    snapshot
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
    graphics_state
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
                /bin/echo 'Returning to fastboot at low battery.'
                capture_and_reboot
                exit $?
            fi
        fi
    fi
    /bin/sleep 30
done
capture_and_reboot
