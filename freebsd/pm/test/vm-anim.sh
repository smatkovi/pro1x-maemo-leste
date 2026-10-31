#!/bin/sh
# vm-anim.sh [TRANSITIONS.INI]: what Hildon's animations cost on the CPU
# (FreeBSD has no GPU driver: hildon-desktop draws with Mesa's llvmpipe), in
# the test VM (freebsd/vm, as root, the packages installed): the session as
# try-hildon.sh runs it (Xvfb :1 at the Pro1X's 2160x1080), then the CPU
# time of hildon-desktop and Xvfb idle for 30 s, with an app open and idle,
# and over three rounds of an app opened (leafpad), the task navigator
# (exit_app_view), the app closed, each step on its own.
# With an argument that file stands in for hildon-desktop's transitions.ini
# for the run (put back after it).
set -u
INI=${1:-}
TI=/usr/local/share/hildon-desktop/transitions.ini
service dbus onestatus > /dev/null 2>&1 || service dbus onestart > /dev/null
if [ -n "$INI" ]; then
	cp -p "$TI" /tmp/transitions.ini.pkg
	cp "$INI" "$TI"
fi
export DISPLAY=:1 PRO1X_TRY=1
Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp > /tmp/xvfb.log 2>&1 &
xvfb=$!
sleep 3
/usr/local/bin/pro1x-hidpi theme
daemon -f -o /tmp/session.log /usr/local/bin/hildon-session
sleep "${WAIT:-60}"
hd=$(pgrep -x hildon-desktop | head -1)
procstat --libxo=json -e "$hd" | python3.11 -c '
import json, shlex, sys
d = json.load(sys.stdin)["procstat"]["environment"]
for e in list(d.values())[0]["environment"]:
    k, _, v = e.partition("=")
    if k in ("PWD", "SHLVL", "_"): continue
    print("export %s=%s" % (k, shlex.quote(v)))' > /tmp/session.env
# CPU seconds of a process (ps: [h:]m:ss.ss)
cpu() { ps -o time= -p "$1" | awk -F: '{ s = 0; for (i = 1; i <= NF; i++) s = s * 60 + $i; print s }'; }
now() { date +%s; }
h0=$(cpu "$hd"); x0=$(cpu $xvfb); t0=$(now)
sleep 30
h1=$(cpu "$hd"); x1=$(cpu $xvfb); t1=$(now)
echo "transitions.ini: ${INI:-the package's}"
echo "idle 30 s: hildon-desktop $(echo "$h1 $h0" | awk '{ printf "%.2f", $1 - $2 }') s, Xvfb $(echo "$x1 $x0" | awk '{ printf "%.2f", $1 - $2 }') s"
# CPU seconds of hildon-desktop and Xvfb for a step (the command, then a wait)
step() {
	local w=$1 a b
	shift
	a=$(cpu "$hd"); b=$(cpu $xvfb)
	"$@"
	sleep "$w"
	echo "$(cpu "$hd") $a $(cpu $xvfb) $b" | awk '{ printf "%.2f %.2f\n", $1 - $2, $3 - $4 }'
}
open_app() { (. /tmp/session.env; exec leafpad) > /dev/null 2>&1 & app=$!; }
task_nav() {
	(. /tmp/session.env; dbus-send --session --type=signal \
	    /com/nokia/hildon_desktop com.nokia.hildon_desktop.exit_app_view)
}
close_app() { kill $app 2> /dev/null; }
# (the steps in this shell, not in $(...): close_app needs open_app's $app)
for i in 1 2 3; do
	step 8 open_app > /tmp/anim.1
	step 8 true > /tmp/anim.2
	step 6 task_nav > /tmp/anim.3
	step 6 close_app > /tmp/anim.4
	echo "round $i: open $(cat /tmp/anim.1), idle $(cat /tmp/anim.2)," \
	    "task navigator $(cat /tmp/anim.3), close $(cat /tmp/anim.4)" \
	    "(CPU s: hildon-desktop Xvfb)"
done
rm -f /tmp/anim.1 /tmp/anim.2 /tmp/anim.3 /tmp/anim.4
# the session and everything it started (by the marker in their environment)
for p in $(ps -axo pid=); do
	if procstat -e "$p" 2> /dev/null | grep -q PRO1X_TRY=1; then
		kill "$p" 2> /dev/null
	fi
done
kill $xvfb 2> /dev/null
if [ -n "$INI" ]; then
	cp -p /tmp/transitions.ini.pkg "$TI"
	rm -f /tmp/transitions.ini.pkg
fi
