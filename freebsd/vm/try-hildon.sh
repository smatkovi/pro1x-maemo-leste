#!/bin/sh
# try-hildon.sh [app.desktop...]: in the FreeBSD test VM (vm.sh), as root,
# with the packages installed: Hildon at 2160x1080 on a virtual screen
# (Xvfb :1), the given apps started from their Hildon menu entries, a
# screenshot of each in /root/shots/<name>.xwd; then everything ends.
# Run with the session's environment as on the phone (hildon-session).
set -u
SHOTS=/root/shots
mkdir -p "$SHOTS"
# Hildon needs the system bus (osso, hildon-home); the Pro1X image has
# dbus_enable=YES
service dbus onestatus > /dev/null 2>&1 || service dbus onestart
export DISPLAY=:1 PRO1X_TRY=1
Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp > /tmp/xvfb.log 2>&1 &
xvfb=$!
sleep 3
/usr/local/bin/pro1x-hidpi theme
daemon -f -o /tmp/session.log /usr/local/bin/hildon-session
sleep ${WAIT:-40}
xwd -root -silent > "$SHOTS/desktop.xwd"
# The session's environment (its D-Bus, HILDON_SCALE, ...) from
# hildon-desktop, for the apps started below
hd=$(pgrep -x hildon-desktop | head -1)
if [ -n "$hd" ]; then
	procstat --libxo=json -e "$hd" | python3.11 -c '
import json, shlex, sys
d = json.load(sys.stdin)["procstat"]["environment"]
for e in list(d.values())[0]["environment"]:
    k, _, v = e.partition("=")
    if k in ("PWD", "SHLVL", "_"): continue
    print("export %s=%s" % (k, shlex.quote(v)))' > /tmp/session.env
else
	echo "hildon-desktop is not running"
fi
for d in "$@"; do
	f=/usr/local/share/applications/hildon/$d
	[ -f "$f" ] || f=$d
	exe=$(sed -n 's/^Exec=//p' "$f" | head -1 | sed 's/ %[a-zA-Z]//g')
	n=$(basename "$d" .desktop)
	echo "== $n: $exe"
	(if [ -f /tmp/session.env ]; then . /tmp/session.env; fi; exec sh -c "exec $exe" > "/tmp/app-$n.log" 2>&1) &
	app=$!
	sleep ${APPWAIT:-25}
	xwd -root -silent > "$SHOTS/$n.xwd"
	if kill -0 $app 2> /dev/null; then echo "running"; else echo "ended"; fi
	tail -5 "/tmp/app-$n.log"
	kill $app 2> /dev/null
	sleep 2
done
# the session and everything it started (by the marker in their environment)
for p in $(ps -axo pid=); do
	if procstat -e "$p" 2> /dev/null | grep -q PRO1X_TRY=1; then
		kill "$p" 2> /dev/null
	fi
done
kill $xvfb 2> /dev/null
tail -20 /tmp/session.log
