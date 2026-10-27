#!/bin/sh
# try-meego-edges.sh: in the FreeBSD test VM (vm.sh), as root, with the
# packages installed: Hildon at 2160x1080 with the MeeGo-like home on
# (patches/meego) and an X Terminal in front; then the edge gestures with
# xdotool, a screenshot after each in /root/shots/edge-N.xwd: 1 swipe from
# the left edge (to the open applications), 2 a tap on its thumbnail (back),
# 3 a tap on the top edge (its menu), 4 a tap elsewhere (menu closed),
# 5 a swipe down from the top edge (closed). Then everything ends.
set -u
SHOTS=/root/shots
mkdir -p "$SHOTS"
service dbus onestatus > /dev/null 2>&1 || service dbus onestart
export DISPLAY=:1 PRO1X_TRY=1
Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp > /tmp/xvfb.log 2>&1 &
xvfb=$!
sleep 3
/usr/local/bin/pro1x-hidpi theme
daemon -f -o /tmp/session.log /usr/local/bin/hildon-session
sleep ${WAIT:-40}
hd=$(pgrep -x hildon-desktop | head -1)
# an application, for the page of open applications
if [ -n "$hd" ]; then
	procstat --libxo=json -e "$hd" | python3.11 -c '
import json, shlex, sys
d = json.load(sys.stdin)["procstat"]["environment"]
for e in list(d.values())[0]["environment"]:
    k, _, v = e.partition("=")
    if k in ("PWD", "SHLVL", "_"): continue
    print("export %s=%s" % (k, shlex.quote(v)))' > /tmp/session.env
	. /tmp/session.env
fi
# through the session's GConf daemon, which hildon-desktop listens to
gconftool-2 -s -t bool /apps/osso/hildon-desktop/meego_pages true
sleep 2
osso-xterm > /tmp/xterm.log 2>&1 &
sleep 15
xwd -root -silent > "$SHOTS/edge-0.xwd"

drag() {
	# drag X0 Y0 X1 Y1: in 0.3 s
	xdotool mousemove "$1" "$2" mousedown 1
	for i in 1 2 3 4 5 6 7 8 9 10; do
		xdotool mousemove $(( $1 + ($3 - $1) * i / 10 )) $(( $2 + ($4 - $2) * i / 10 ))
		sleep 0.03
	done
	xdotool mouseup 1
}
# a tap held 0.15 s (the task navigator takes no shorter one as a click)
tap() { xdotool mousemove "$1" "$2" mousedown 1; sleep 0.15; xdotool mouseup 1; }
shot() { sleep 6; xwd -root -silent > "$SHOTS/edge-$1.xwd"; echo "edge-$1: $2"; }

drag 5 540 500 540;            shot 1 "swipe from the left edge"
# the VM draws slowly: the zoom into the task navigator takes seconds there,
# and it takes no taps while zooming
sleep 15
tap 1080 600;                  shot 2 "tap on the thumbnail"
tap 1080 10;                   shot 3 "tap on the top edge"
tap 1080 900;                  shot 4 "tap elsewhere"
drag 1080 5 1080 500;          shot 5 "swipe down from the top edge"

gconftool-2 -s -t bool /apps/osso/hildon-desktop/meego_pages false
for p in $(pgrep -f 'hildon|maemo|sapwood|osso-xterm'); do kill "$p" 2> /dev/null; done
sleep 2
kill $xvfb 2> /dev/null
