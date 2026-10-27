#!/bin/sh
# try-meego-pages.sh: in the FreeBSD test VM (vm.sh), as root, with the
# packages installed: Hildon at 2160x1080 on a virtual screen with the
# MeeGo-like pages on (patches/meego), an X Terminal open and the task
# navigator shown, then swipes with xdotool and a screenshot after each in
# /root/shots/meego-N.xwd (0: before the first);
# then everything ends. Run like try-hildon.sh.
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
# the application's title bar button (top left) opens the task navigator
xdotool mousemove 60 60 click 1
sleep 6
xwd -root -silent > "$SHOTS/meego-0.xwd"

swipe() {
	# swipe FROM_X TO_X: across the middle of the screen in 0.3 s
	xdotool mousemove "$1" 540 mousedown 1
	x=$1
	step=$(( ($2 - $1) / 10 ))
	for i in 1 2 3 4 5 6 7 8 9 10; do
		x=$((x + step))
		xdotool mousemove "$x" 540
		sleep 0.03
	done
	xdotool mouseup 1
}

n=1
for dir in ${SWIPES:-left left left left left left right right}; do
	if [ "$dir" = left ]; then swipe 1700 300; else swipe 300 1700; fi
	sleep 6
	xwd -root -silent > "$SHOTS/meego-$n.xwd"
	echo "meego-$n: swipe $dir"
	n=$((n + 1))
done

gconftool-2 -s -t bool /apps/osso/hildon-desktop/meego_pages false
for p in $(pgrep -f 'hildon|maemo|sapwood|osso-xterm'); do kill "$p" 2> /dev/null; done
sleep 2
kill $xvfb 2> /dev/null
