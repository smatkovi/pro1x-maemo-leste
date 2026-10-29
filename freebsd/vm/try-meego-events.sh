#!/bin/sh
# try-meego-events.sh: in the FreeBSD test VM (vm.sh), as root, with the
# packages installed: the MeeGo-like look's third step (patches/meego) in
# Hildon at 2160x1080, a screenshot after each step in
# /root/shots/events-N.xwd. STEPS chooses (default all of them):
#   volume  the volume bar (pro1x-volume-osd, started by hildon-session) on
#           a null sink: 1 at 60 %, 2 at 25 %
#   bar     with an X Terminal open, the status bar over the task navigator
#           (3) and the launcher (5), a tap on its right half there opens
#           the status menu (4, 6)
#   events  the Events page (pro1x-events) on the first home view (7), its
#           feed pulled down to update (8, while pulled; 9 after), scrolled
#           up (10), the swipe on to the next page (11)
# EVENTS_FIXTURE=1 makes up a Mastodon home timeline and unread calls and
# messages (events-fixture.sh, beside this script), serves the timeline at
# http://127.0.0.1:8000 and points the Events page's Mastodon account
# there. Then everything ends.
set -u
SHOTS=/root/shots
STEPS=${STEPS:-volume bar events}
mkdir -p "$SHOTS"
service dbus onestatus > /dev/null 2>&1 || service dbus onestart
export DISPLAY=:1 PRO1X_TRY=1
Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp > /tmp/xvfb.log 2>&1 &
xvfb=$!
sleep 3
/usr/local/bin/pro1x-hidpi theme
rm -f /tmp/hildon-desktop/status-menu.stamp
gc() { gconftool-2 --direct --config-source=xml:readwrite:$HOME/.config/gconf "$@"; }
# Home on its first view
gc -s -t int /apps/osso/hildon-desktop/views/current 1
fixture=
if [ -n "${EVENTS_FIXTURE:-}" ]; then
	rm -rf /tmp/events-fixture
	sh "$(dirname "$0")/events-fixture.sh" /tmp/events-fixture
	(cd /tmp/events-fixture && exec python3.11 -m http.server 8000 --bind 127.0.0.1) \
	    > /tmp/fixture.log 2>&1 &
	fixture=$!
	gc -s -t string /apps/pro1x/events/mastodon_server http://127.0.0.1:8000
	gc -s -t string /apps/pro1x/events/mastodon_token try-token
fi
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
drag() {
	# drag X0 Y0 X1 Y1 [STEPS]: in STEPS x 30 ms
	n=${5:-10}
	xdotool mousemove "$1" "$2" mousedown 1
	i=1
	while [ $i -le $n ]; do
		xdotool mousemove $(( $1 + ($3 - $1) * i / n )) $(( $2 + ($4 - $2) * i / n ))
		sleep 0.03
		i=$((i + 1))
	done
}
release() { xdotool mouseup 1; }
tap() { xdotool mousemove "$1" "$2" mousedown 1; sleep 0.15; xdotool mouseup 1; }
shot() {
	sleep ${SHOTWAIT:-6}
	xwd -root -silent > "$SHOTS/events-$1.xwd"
	echo "events-$1: $2"
}
# the look on, through the session's GConf daemon, which hildon-desktop
# listens to
gconftool-2 -s -t bool /apps/osso/hildon-desktop/meego_pages true
sleep 8
ps -axo pid,command | grep -E 'pro1x-(volume-osd|events)|pulseaudio' | grep -v grep

case " $STEPS " in *" volume "*)
	pactl load-module module-null-sink sink_name=try > /dev/null
	pactl set-default-sink try
	pactl set-sink-volume try 40%
	sleep 2
	pactl set-sink-volume try 60%
	SHOTWAIT=0.6 shot 1 "the volume bar at 60 %"
	sleep 3
	pactl set-sink-volume try 25%
	SHOTWAIT=0.6 shot 2 "at 25 %"
	sleep 3
	;;
esac

case " $STEPS " in *" bar "*)
	osso-xterm > /tmp/xterm.log 2>&1 &
	sleep 15
	# the left edge's swipe: to the open applications
	drag 5 600 700 600; release; shot 3 "the task navigator with the status bar"
	tap 1900 40;                   shot 4 "a tap on the bar's right half: the status menu"
	# closed: back to the task navigator (its transition takes a while in
	# the VM, a swipe during it is lost)
	tap 1080 900;                  sleep 12
	# on to the launcher (the finger to the right)
	drag 300 600 1800 600; release; shot 5 "the launcher with the status bar"
	tap 1900 40;                   shot 6 "the status menu over the launcher"
	tap 1080 900;                  sleep 12
	# back to the task navigator
	drag 1800 600 300 600; release; sleep 8
	;;
esac

case " $STEPS " in *" events "*)
	# from the task navigator (after the bar's steps) on to the first home
	# view: the Events page
	case " $STEPS " in *" bar "*) drag 1800 600 300 600; release ;; esac
	shot 7 "the Events page"
	xwininfo -root -tree | grep -i -E 'pro1x-events|Events' | head -3
	drag 1080 300 1080 800 15;     SHOTWAIT=5 shot 8 "pulled down"
	release;                       shot 9 "updated"
	drag 1080 900 1080 300 10; release; shot 10 "scrolled up"
	drag 1800 600 300 600; release; shot 11 "swiped on"
	;;
esac

# HOLD=1: the session stays until /tmp/try-stop appears (for looking at
# it by hand, with /tmp/session.env)
if [ -n "${HOLD:-}" ]; then
	rm -f /tmp/try-stop
	while [ ! -e /tmp/try-stop ]; do sleep 2; done
fi
gconftool-2 -s -t bool /apps/osso/hildon-desktop/meego_pages false
for p in $(ps -axo pid=); do
	[ "$p" = $$ ] && continue
	if procstat -e "$p" 2> /dev/null | grep -q PRO1X_TRY=1; then
		kill "$p" 2> /dev/null
	fi
done
[ -n "$fixture" ] && kill "$fixture" 2> /dev/null
sleep 2
kill $xvfb 2> /dev/null
tail -20 /tmp/session.log
