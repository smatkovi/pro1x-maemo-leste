#!/bin/sh
# try-meego-look.sh: in the FreeBSD test VM (vm.sh), as root, with the
# packages installed: Hildon at 2160x1080 with the MeeGo-like look on
# (patches/meego), a screenshot after each step in /root/shots/look-N.xwd:
# 1 Home (the thin status bar), 2 the launcher (the icons in the N9's
# shape), 3 an X Terminal (no title bar, below the bar), 4 a tap on the
# bar's left half (its menu, a list), 5 the look off (Hildon's title bar
# again), 6 on again while the terminal is open; with PICKER=<program>
# (a HildonPickerDialog with the time selector, or the date one with the
# argument "date", that prints the chosen value) 7 the time picker's
# wheels, 8 its hours rolled up, 9 the date picker. Then everything ends.
set -u
SHOTS=/root/shots
mkdir -p "$SHOTS"
service dbus onestatus > /dev/null 2>&1 || service dbus onestart
export DISPLAY=:1 PRO1X_TRY=1
Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp > /tmp/xvfb.log 2>&1 &
xvfb=$!
sleep 3
/usr/local/bin/pro1x-hidpi theme
# a session killed before leaves the status menu's stamp, which keeps its
# plugins off
rm -f /tmp/hildon-desktop/status-menu.stamp
# Home on its last view (of four), the launcher one swipe away
gconftool-2 --direct --config-source=xml:readwrite:$HOME/.config/gconf \
    -s -t int /apps/osso/hildon-desktop/views/current ${LASTVIEW:-4}
daemon -f -o /tmp/session.log /usr/local/bin/hildon-session
sleep ${WAIT:-40}
hd=$(pgrep -x hildon-desktop | head -1)
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
look() {
	# through the session's GConf daemon, which hildon-desktop listens to
	gconftool-2 -s -t bool /apps/osso/hildon-desktop/meego_pages "$1"
}
drag() {
	# drag X0 Y0 X1 Y1: in 0.3 s
	xdotool mousemove "$1" "$2" mousedown 1
	for i in 1 2 3 4 5 6 7 8 9 10; do
		xdotool mousemove $(( $1 + ($3 - $1) * i / 10 )) $(( $2 + ($4 - $2) * i / 10 ))
		sleep 0.03
	done
	xdotool mouseup 1
}
tap() { xdotool mousemove "$1" "$2" mousedown 1; sleep 0.15; xdotool mouseup 1; }
shot() {
	sleep ${SHOTWAIT:-8}
	xwd -root -silent > "$SHOTS/look-$1.xwd"
	echo "look-$1: $2"
	xprop -root _HILDON_MEEGO_LOOK
}

look true
shot 1 "Home"
drag 1800 600 300 600;         shot 2 "swipe to the launcher"
osso-xterm > /tmp/xterm.log 2>&1 &
sleep 15
shot 3 "X Terminal"
xwininfo -root -tree | grep -i -E 'osso-xterm|X Terminal|status' | head -5
tap 400 40;                    shot 4 "tap on the bar's left half"
tap 1080 900;                  sleep 3
look false;                    shot 5 "the look off"
xwininfo -root -tree | grep -i -E 'osso-xterm|X Terminal|status' | head -5
look true;                     shot 6 "on again"
xwininfo -root -tree | grep -i -E 'osso-xterm|X Terminal|status' | head -5
if [ -n "${PICKER:-}" ]; then
	"$PICKER" > /tmp/picker.log 2>&1 &
	picker=$!
	sleep 10
	shot 7 "the time picker"
	# the hours: half the column up, slowly enough to roll a few rows
	drag ${HOURS_X:-400} 900 ${HOURS_X:-400} 650
	shot 8 "its hours rolled up"
	cat /tmp/picker.log
	kill $picker 2> /dev/null
	"$PICKER" date > /tmp/picker.log 2>&1 &
	picker=$!
	sleep 10
	shot 9 "the date picker"
	kill $picker 2> /dev/null
fi

look false
# the session and everything it started (by the marker in their environment)
for p in $(ps -axo pid=); do
	[ "$p" = $$ ] && continue
	if procstat -e "$p" 2> /dev/null | grep -q PRO1X_TRY=1; then
		kill "$p" 2> /dev/null
	fi
done
sleep 2
kill $xvfb 2> /dev/null
tail -20 /tmp/session.log
