#!/bin/sh
# try-meego-portrait.sh: in the FreeBSD test VM (vm.sh), as root, with the
# packages installed: the MeeGo-like look (patches/meego) turned to
# portrait, as Leste turns it when the phone is held upright with the
# keyboard slid in. The screen is Xorg's dummy driver with fake-rotation.so
# (session.sh PORTRAIT=1, beside this script), mce's orientation signal is
# sent by hand (session.sh rotate). Leste's own portrait home (GConf
# scroll_vertical, the views one below the other) is switched on, which the
# look does not follow. With the made-up content of events-fixture.sh
# (served at http://127.0.0.1:8000), a screenshot after each step in
# /root/shots/portrait-N.xwd:
#    1 the Events page in landscape     2 turned: the Events page in portrait
#    3, 4 scrolled up: the feed's pictures (two, a sensitive post's plate,
#      three, a video's still)
#    5 a swipe across: the launcher     6 scrolled up
#    7 a swipe the other way: Events again
#    8 two X Terminals: one in portrait below the status bar
#    9 a tap on the bar's left half: its menu
#   10 the left edge's swipe: the open applications
#   11 a tap on the bar's right half there: the status menu
#   12 turned back: the open applications in landscape
#   13 the launcher in landscape        14 the Events page in landscape
# Then everything ends (HOLD=1: it stays until /tmp/try-stop appears).
set -u
here=$(cd "$(dirname "$0")" && pwd)
SHOTS=/root/shots
mkdir -p "$SHOTS"
gc() { gconftool-2 --direct --config-source=xml:readwrite:$HOME/.config/gconf "$@"; }
gc -s -t int /apps/osso/hildon-desktop/views/current 1
gc -s -t bool /apps/osso/hildon-desktop/scroll_vertical true
rm -rf /tmp/events-fixture
sh "$here/events-fixture.sh" /tmp/events-fixture
(cd /tmp/events-fixture && exec python3.11 -m http.server 8000 --bind 127.0.0.1) \
    > /tmp/fixture.log 2>&1 &
fixture=$!
gc -s -t string /apps/pro1x/events/mastodon_server http://127.0.0.1:8000
gc -s -t string /apps/pro1x/events/mastodon_token try-token
gc -s -t string /apps/pro1x/events/weather_url http://127.0.0.1:8000/v1/forecast
gc -s -t string /apps/pro1x/events/geocoding_url http://127.0.0.1:8000/v1/search
gc -s -t string /apps/pro1x/events/weather_place "Wien"
gc -s -t float /apps/pro1x/events/weather_latitude 48.2085
gc -s -t float /apps/pro1x/events/weather_longitude 16.3721
for k in show_notifications show_weather show_calendar show_mastodon weather_icons \
    calendar_days; do
	gc -u /apps/pro1x/events/$k
done
rm -f "$HOME"/.cache/pro1x-events/weather-*.json "$HOME"/.cache/pro1x-events/media-*
rm -f /tmp/hildon-desktop/status-menu.stamp /tmp/.X1-lock /tmp/.X11-unix/X1
PORTRAIT=1 sh "$here/session.sh" start || exit 1
. /tmp/session.env
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
dragh() { drag "$@"; sleep 0.4; release; }
swipe() { drag "$@"; release; }
tap() { xdotool mousemove "$1" "$2" mousedown 1; sleep 0.15; xdotool mouseup 1; }
shot() {
	sleep ${SHOTWAIT:-6}
	# (a window going away during the capture fails it: again)
	for i in 1 2 3; do
		xwd -root -silent > "$SHOTS/portrait-$1.xwd" 2> /dev/null && break
		sleep 1
	done
	echo "portrait-$1: $2"
}
rotate() { sh "$here/session.sh" rotate "$1"; sleep 15; }
gconftool-2 -s -t bool /apps/osso/hildon-desktop/meego_pages true
sleep 10
# the feed with its pictures (fetched when the look came on)
dragh 1080 200 1080 1000 10; sleep 8
shot 1 "the Events page in landscape"
rotate portrait
xwininfo -root | grep -E 'Width|Height'
shot 2 "turned: the Events page in portrait"
dragh 540 1900 540 300 14; dragh 540 1900 540 500 14
SHOTWAIT=3 shot 3 "scrolled up: the feed's pictures"
# (an emulated VM is slow after a turn: a drag or a swipe during the
# redraws takes longer than a swipe may, and is lost)
sleep 5; dragh 540 1700 540 700 12
SHOTWAIT=3 shot 4 "further: three pictures, the video's still"
# the finger to the right: before the first view the open applications
# (none) and then the launcher
swipe 100 1200 900 1200 10;    SHOTWAIT=14 shot 5 "a swipe across: the launcher"
dragh 540 1800 540 900 12;     SHOTWAIT=3 shot 6 "the launcher scrolled up"
swipe 900 1200 100 1200 10;    SHOTWAIT=14 shot 7 "the other way: Events again"
sh "$here/session.sh" run osso-xterm; sleep 20
sh "$here/session.sh" run osso-xterm; sleep 20
shot 8 "two X Terminals: one in portrait below the status bar"
tap 200 40;                    shot 9 "a tap on the bar's left half: its menu"
tap 540 1500; sleep 4
swipe 5 1200 700 1200 10;      SHOTWAIT=14 shot 10 "the left edge's swipe: the open applications"
tap 900 40;                    SHOTWAIT=8 shot 11 "the bar's right half: the status menu"
tap 540 1500; sleep 12
rotate landscape
shot 12 "turned back: the open applications in landscape"
sleep 10
swipe 300 900 1800 900 10;     SHOTWAIT=14 shot 13 "the launcher in landscape"
# the finger to the left: the open applications, then the first view
sleep 5
swipe 1800 900 300 900 10;     sleep 14
swipe 1800 900 300 900 10;     SHOTWAIT=14 shot 14 "the Events page in landscape"

if [ -n "${HOLD:-}" ]; then
	rm -f /tmp/try-stop
	while [ ! -e /tmp/try-stop ]; do sleep 2; done
fi
gconftool-2 -s -t bool /apps/osso/hildon-desktop/meego_pages false
gconftool-2 -u /apps/osso/hildon-desktop/scroll_vertical
sh "$here/session.sh" stop
kill "$fixture" 2> /dev/null
tail -5 /tmp/session.log
