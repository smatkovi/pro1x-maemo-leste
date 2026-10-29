#!/bin/sh
# try-harmattan.sh [app.desktop...]: in the FreeBSD test VM (vm.sh), as
# root, with the packages installed: the Nokia N9's user interface
# (harmattan-session) at 2160x1080 on a virtual screen (Xvfb :1), the
# given applications started from their menu entries, a screenshot of
# each in /root/shots/harmattan-<name>.xwd; then everything ends.
# The applications run with the session's environment (meegotouchhome's).
set -u
SHOTS=/root/shots
mkdir -p "$SHOTS"
service dbus onestatus > /dev/null 2>&1 || service dbus onestart
export DISPLAY=:1 PRO1X_TRY=1
Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp > /tmp/xvfb.log 2>&1 &
xvfb=$!
sleep 3
daemon -f -o /tmp/harmattan-session.log /usr/local/bin/harmattan-session
sleep ${WAIT:-60}
xwd -root -silent > "$SHOTS/harmattan-desktop.xwd"
for p in mcompositor sysuid meego-im-uiserver meegofeedbackd meegotouchhome; do
	if pgrep -x "$p" > /dev/null; then echo "$p: running"; else echo "$p: not running"; fi
done
home=$(pgrep -x meegotouchhome | head -1)
if [ -n "$home" ]; then
	procstat --libxo=json -e "$home" | python3.11 -c '
import json, shlex, sys
d = json.load(sys.stdin)["procstat"]["environment"]
for e in list(d.values())[0]["environment"]:
    k, _, v = e.partition("=")
    if k in ("PWD", "SHLVL", "_"): continue
    print("export %s=%s" % (k, shlex.quote(v)))' > /tmp/harmattan.env
fi
for d in "$@"; do
	f=/usr/local/share/applications/$d
	[ -f "$f" ] || f=$d
	exe=$(sed -n 's/^Exec=//p' "$f" | head -1 | sed 's/ %[a-zA-Z]//g')
	n=$(basename "$d" .desktop)
	echo "== $n: $exe"
	(if [ -f /tmp/harmattan.env ]; then . /tmp/harmattan.env; fi; exec sh -c "exec $exe" > "/tmp/app-$n.log" 2>&1) &
	app=$!
	sleep ${APPWAIT:-25}
	xwd -root -silent > "$SHOTS/harmattan-$n.xwd"
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
tail -20 /tmp/harmattan-session.log
