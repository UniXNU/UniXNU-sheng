#!/bin/sh
# Fixed window sequence for a boot started with data-reset=1.
set -u
/bin/sleep 120
/bin/date -u
/usr/bin/open -a Finder /Users/unixnu
/bin/sleep 15
/usr/bin/open -a 'System Settings'
/bin/sleep 45
/usr/sbin/screencapture -x -t png /Users/Shared/unixnu-test/desktop-180.png
/bin/date -u
/bin/sleep 90
/usr/sbin/screencapture -x -t png /Users/Shared/unixnu-test/desktop-270.png
/bin/date -u
