#!/bin/sh
# Fixed window sequence for a boot started with data-reset=1.
set -u
/usr/bin/defaults write NSGlobalDomain NSQuitAlwaysKeepsWindows -bool false
/usr/bin/defaults write com.apple.loginwindow TALLogoutSavesState -bool false
/usr/bin/defaults -currentHost write com.apple.loginwindow TALLogoutSavesState -bool false
cache=/private/var/tmp/NativeAdrenoMetal.cache/prime.log
ready=0
for attempt in $(/usr/bin/seq 1 110); do
    /bin/date -u
    if [ -r "$cache" ]; then
        state=$(/usr/bin/grep '^pass ' "$cache" | /usr/bin/tail -n 1)
        /bin/echo "$state"
        case "$state" in *' complete=1 '*) ready=1; break ;; esac
    fi
    /bin/sleep 60
done
if [ "$ready" != 1 ]; then
    /bin/echo 'Priming is incomplete; the comparison scene remains pending.'
    exit 1
fi
/bin/date -u
/usr/bin/open -a Finder /Users/unixnu
/bin/sleep 15
/usr/bin/open -a 'System Settings'
/usr/bin/osascript -l JavaScript <<'JXA' &
ObjC.import('Cocoa');
ObjC.import('QuartzCore');
var app = $.NSApplication.sharedApplication;
app.setActivationPolicy(0);
var window = $.NSWindow.alloc.initWithContentRectStyleMaskBackingDefer($.NSMakeRect(80, 80, 1050, 700), 15, 2, false);
window.title = 'UniXNU GPU frame workload';
window.contentView.wantsLayer = true;
var root = window.contentView.layer;
root.backgroundColor = $.NSColor.colorWithCalibratedRedGreenBlueAlpha(0.08, 0.10, 0.14, 1).CGColor;
for (var index = 0; index < 20; index++) {
    var layer = $.CALayer.layer;
    layer.frame = $.NSMakeRect(30 + (index % 5) * 130, 35 + Math.floor(index / 5) * 145, 100, 100);
    layer.backgroundColor = $.NSColor.colorWithCalibratedRedGreenBlueAlpha(0.15 + index * 0.025, 0.55, 0.75, 0.8).CGColor;
    root.addSublayer(layer);
    var motion = $.CABasicAnimation.animationWithKeyPath('transform.translation.x');
    motion.fromValue = $(0);
    motion.toValue = $(260);
    motion.duration = 2;
    motion.autoreverses = true;
    motion.repeatCount = 1000;
    layer.addAnimationForKey(motion, 'translation');
}
window.makeKeyAndOrderFront(null);
app.activateIgnoringOtherApps(true);
$.NSRunLoop.currentRunLoop.runUntilDate($.NSDate.dateWithTimeIntervalSinceNow(105));
window.close;
JXA
animation=$!
/bin/sleep 45
/usr/sbin/screencapture -x -t png /Users/Shared/unixnu-test/desktop-180.png
/bin/date -u
/bin/sleep 90
/usr/sbin/screencapture -x -t png /Users/Shared/unixnu-test/desktop-270.png
/bin/date -u
wait "$animation"
/usr/bin/touch /Users/Shared/unixnu-test/benchmark.done
