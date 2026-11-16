#!/bin/sh
# Fixed desktop interaction and manual inspection views.
set -u
if [ -f /private/var/ngpu/test/share/manual-session ]; then
    /bin/echo "manual session boot=$(/usr/sbin/sysctl -n kern.bootsessionuuid)"
    exit 0
fi
/bin/rm -f /Users/Shared/unixnu-test/desktop-180.png /Users/Shared/unixnu-test/desktop-270.png
(
    /bin/sleep 180
    /usr/sbin/screencapture -x -t png /Users/Shared/unixnu-test/desktop-180.png
    /bin/sleep 90
    /usr/sbin/screencapture -x -t png /Users/Shared/unixnu-test/desktop-270.png
) &
/usr/bin/defaults write NSGlobalDomain NSQuitAlwaysKeepsWindows -bool false
/usr/bin/defaults write com.apple.loginwindow TALLogoutSavesState -bool false
/usr/bin/defaults -currentHost write com.apple.loginwindow TALLogoutSavesState -bool false
/usr/bin/defaults write com.apple.finder ApplePersistenceIgnoreState -bool true
/usr/bin/defaults write com.apple.systempreferences ApplePersistenceIgnoreState -bool true
/usr/bin/pkill -u "$(/usr/bin/id -u)" -x KeyboardSetupAssistant 2>/dev/null || :
/usr/bin/pkill -u "$(/usr/bin/id -u)" -x 'System Settings' 2>/dev/null || :
/usr/bin/pkill -u "$(/usr/bin/id -u)" -x Finder 2>/dev/null || :
/bin/rm -rf '/Users/unixnu/Library/Saved Application State/com.apple.finder.savedState' \
    '/Users/unixnu/Library/Saved Application State/com.apple.systempreferences.savedState'
if [ -s /private/var/ngpu/test/share/graphics-comparison ]; then
    /usr/bin/caffeinate -du -t 900 &
    /private/var/ngpu/test/bin/gpu-scene --reset-session
    /bin/sleep 15
    if [ "$(/bin/cat /private/var/ngpu/test/share/graphics-comparison)" = shader-cache ]; then
        /usr/bin/open '/Library/UniXNU/Shader Cache.app'
        /bin/sleep 900
        exit 0
    fi
    if [ "$(/bin/cat /private/var/ngpu/test/share/graphics-comparison)" = view ]; then
        /bin/echo "inspection start boot=$(/usr/sbin/sysctl -n kern.bootsessionuuid) time=$(/bin/date -u '+%Y-%m-%dT%H:%M:%SZ')"
        /usr/bin/open 'x-apple.systempreferences:com.apple.systempreferences.GeneralSettings'
        /bin/sleep 900
        exit 0
    fi
    /private/var/ngpu/test/bin/gpu-scene &
    animation=$!
    /bin/echo "comparison start boot=$(/usr/sbin/sysctl -n kern.bootsessionuuid)"
    for round in 1 2; do
        for pane in com.apple.systempreferences.GeneralSettings com.apple.Network-Settings.extension com.apple.Displays-Settings.extension com.apple.systempreferences.GeneralSettings; do
            /bin/echo "comparison round=$round pane=$pane time=$(/bin/date -u '+%Y-%m-%dT%H:%M:%SZ')"
            /usr/bin/open "x-apple.systempreferences:$pane"
            /bin/sleep 20
        done
    done
    /bin/sleep 30
    /bin/kill -TERM "$animation" 2>/dev/null || :
    /bin/echo "comparison complete time=$(/bin/date -u '+%Y-%m-%dT%H:%M:%SZ')"
    /usr/bin/touch /Users/Shared/unixnu-test/benchmark.done
    exit 0
fi
(
    /bin/sleep 30
    tool=/private/var/ngpu/test/bin/icon-probe
    log=/Users/Shared/unixnu-test/icon-probe.log
    /bin/echo "icon probe boot=$(/usr/sbin/sysctl -n kern.bootsessionuuid)" > "$log"
    /private/var/ngpu/test/bin/gpu-correctness-probe --all >> "$log" 2>&1 &
    probe=$!
    ( /bin/sleep 90; /bin/kill -KILL "$probe" 2>/dev/null ) &
    alarm=$!
    wait "$probe"
    /bin/echo "correctness exit=$?" >> "$log"
    /bin/kill "$alarm" 2>/dev/null || :
    replay=/private/var/ngpu/test/bin/renderbox-alpha-probe
    if [ -x "$replay" ]; then
        inputs=/private/var/ngpu/test/share/renderbox-alpha
        "$replay" "$inputs/vertex.air" "$inputs/fragment.air" "$inputs/globals.bin" "$inputs/gradient.bin" >> "$log" 2>&1 &
        probe=$!
        ( /bin/sleep 30; /bin/kill -KILL "$probe" 2>/dev/null ) &
        alarm=$!
        wait "$probe"
        /bin/echo "RenderBox alpha replay exit=$?" >> "$log"
        /bin/kill "$alarm" 2>/dev/null || :
    fi
    for mps in 1 0; do
        CI_ENABLE_MPS="$mps" "$tool" >> "$log" 2>&1 &
        probe=$!
        ( /bin/sleep 90; /bin/kill -KILL "$probe" 2>/dev/null ) &
        alarm=$!
        wait "$probe"
        /bin/echo "blur mps=$mps exit=$?" >> "$log"
        /bin/kill "$alarm" 2>/dev/null || :
    done
    if [ -s /private/var/ngpu/test/share/icon-probe-path ]; then
        owner=$(/usr/bin/id -u)
        /bin/echo "icon service restart old=$(/usr/bin/pgrep -u "$owner" -x iconservicesagent | /usr/bin/tr '\n' ',')" >> "$log"
        /usr/bin/pkill -KILL -u "$owner" -x iconservicesagent 2>/dev/null || :
        /bin/sleep 1
        single=$(/bin/cat /private/var/ngpu/test/share/icon-probe-path)
        if [ "$single" = settings ]; then
            CI_ENABLE_MPS=0 "$tool" --settings-icons /Users/Shared/unixnu-test/icons.png >> "$log" 2>&1 &
        else
            CI_ENABLE_MPS=0 "$tool" --icon-symbols "$single" /Users/Shared/unixnu-test/icons.png >> "$log" 2>&1 &
        fi
    else
        CI_ENABLE_MPS=0 "$tool" /Users/Shared/unixnu-test/icons.png >> "$log" 2>&1 &
    fi
    probe=$!
    ( /bin/sleep 120; /bin/kill -KILL "$probe" 2>/dev/null ) &
    alarm=$!
    wait "$probe"
    /bin/echo "icons exit=$?" >> "$log"
    /bin/echo "icon service current=$(/usr/bin/pgrep -u "$(/usr/bin/id -u)" -x iconservicesagent | /usr/bin/tr '\n' ',')" >> "$log"
    /bin/kill "$alarm" 2>/dev/null || :
    /usr/bin/open -a 'System Settings'
) &
icons=$!
if [ -s /private/var/ngpu/test/share/icon-probe-path ]; then
    wait "$icons"
    exit $?
fi
cache=/private/var/tmp/NativeAdrenoMetal.cache/prime.log
/bin/date -u
tool=/private/var/ngpu/test
export VK_LAYER_PATH="$tool/etc/vulkan/explicit_layer.d"
export VK_DRIVER_FILES="$tool/etc/vulkan/icd.d/turnip.json"
export DYLD_LIBRARY_PATH="$tool/lib"
"$tool/bin/gpu-acceptance" --loader "$tool/lib/libvulkan.1.dylib" \
    --cache /Users/Shared/unixnu-test/gpu-cache --prime-log "$cache" --mode all &
probe=$!
(
    /bin/sleep 180
    /bin/kill -TERM "$probe" 2>/dev/null
    /bin/sleep 5
    /bin/kill -KILL "$probe" 2>/dev/null
) &
alarm=$!
wait "$probe"
result=$?
/bin/kill -TERM "$alarm" 2>/dev/null
if [ "$result" != 0 ]; then
    /bin/echo "GPU acceptance returned $result"
    /usr/bin/touch /Users/Shared/unixnu-test/reboot-requested
    exit "$result"
fi
/bin/sleep 15
/usr/bin/open -a Finder /Users/unixnu
/bin/sleep 15
/usr/bin/open -a 'System Settings'
"$tool/bin/gpu-scene" &
animation=$!
/bin/sleep 45
/usr/sbin/screencapture -x -t png /Users/Shared/unixnu-test/desktop-180.png
/bin/date -u
/bin/sleep 90
/usr/sbin/screencapture -x -t png /Users/Shared/unixnu-test/desktop-270.png
/bin/date -u
/usr/bin/touch /Users/Shared/unixnu-test/benchmark.done
wait "$animation"
