#!/bin/sh
# Fixed window sequence after the current boot's shader priming pass.
set -u
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
(
    /bin/sleep 30
    tool=/private/var/ngpu/test/bin/icon-probe
    log=/Users/Shared/unixnu-test/icon-probe.log
    /bin/echo "icon probe boot=$(/usr/sbin/sysctl -n kern.bootsessionuuid)" > "$log"
    /private/var/ngpu/test/bin/gpu-correctness-probe /private/var/ngpu/test/share/half-compound.air >> "$log" 2>&1 &
    probe=$!
    ( /bin/sleep 90; /bin/kill -KILL "$probe" 2>/dev/null ) &
    alarm=$!
    wait "$probe"
    /bin/echo "correctness exit=$?" >> "$log"
    /bin/kill "$alarm" 2>/dev/null || :
    for mps in 1 0; do
        CI_ENABLE_MPS="$mps" "$tool" >> "$log" 2>&1 &
        probe=$!
        ( /bin/sleep 90; /bin/kill -KILL "$probe" 2>/dev/null ) &
        alarm=$!
        wait "$probe"
        /bin/echo "blur mps=$mps exit=$?" >> "$log"
        /bin/kill "$alarm" 2>/dev/null || :
    done
    CI_ENABLE_MPS=0 "$tool" /Users/Shared/unixnu-test/icons.png >> "$log" 2>&1 &
    probe=$!
    ( /bin/sleep 120; /bin/kill -KILL "$probe" 2>/dev/null ) &
    alarm=$!
    wait "$probe"
    /bin/echo "icons exit=$?" >> "$log"
    /bin/kill "$alarm" 2>/dev/null || :
    /usr/bin/open -a 'System Settings'
) &
cache=/private/var/tmp/NativeAdrenoMetal.cache/prime.log
boot=$(/usr/sbin/sysctl -n kern.bootsessionuuid)
ready=0
for attempt in $(/usr/bin/seq 1 105); do
    /bin/date -u
    if [ -r "$cache" ]; then
        state=$(/usr/bin/awk -v boot="$boot" '
            /^list / {current = index($0, " boot=" boot " ") && index($0, " scope=2 "); state = ""}
            /^pass / && current {state = $0}
            END {print state}
        ' "$cache")
        /bin/echo "$state"
        case "$state" in
            *' complete=1 '*) ready=1; break ;;
            *' complete=2 '*)
                /bin/echo 'Whole-system shader priming finished with failures.'
                /usr/bin/touch /Users/Shared/unixnu-test/reboot-requested
                exit 1 ;;
        esac
    fi
    /bin/sleep 60
done
if [ "$ready" != 1 ]; then
    /bin/echo 'Priming is incomplete; the comparison scene remains pending.'
    /usr/bin/touch /Users/Shared/unixnu-test/reboot-requested
    exit 1
fi
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
