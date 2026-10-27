#!/bin/sh
# session.sh start|run CMD...|shot NAME|stop: in the FreeBSD test VM
# (vm.sh), as root, with the packages installed: a Hildon session at
# 2160x1080 on a virtual screen (Xvfb :1) that stays open between calls,
# so that an app can be driven step by step from the PC over ssh, with
# xdotool on DISPLAY=:1 between the screenshots:
#   session.sh start              the session (try-hildon.sh's), its
#                                 environment in /tmp/session.env
#   session.sh run CMD...         CMD in the session's environment, its
#                                 output in /tmp/run.log
#   session.sh shot NAME          a screenshot, /root/shots/NAME.xwd
#   session.sh stop               everything it started ends
# The VM is emulated and slow: wait some seconds after a tap before the
# screenshot.
set -u
case ${1:-} in
start)
	service dbus onestatus > /dev/null 2>&1 || service dbus onestart
	export DISPLAY=:1 PRO1X_TRY=1
	daemon -f -o /tmp/xvfb.log Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp
	sleep 3
	/usr/local/bin/pro1x-hidpi theme > /dev/null
	daemon -f -o /tmp/session.log /usr/local/bin/hildon-session
	sleep "${WAIT:-40}"
	# the session's environment (its D-Bus, HILDON_SCALE, ...) from
	# hildon-desktop, for what runs in it
	hd=$(pgrep -x hildon-desktop | head -1)
	[ -n "$hd" ] || { echo "hildon-desktop is not running"; exit 1; }
	procstat --libxo=json -e "$hd" | python3.11 -c '
import json, shlex, sys
d = json.load(sys.stdin)["procstat"]["environment"]
for e in list(d.values())[0]["environment"]:
    k, _, v = e.partition("=")
    if k in ("PWD", "SHLVL", "_"): continue
    print("export %s=%s" % (k, shlex.quote(v)))' > /tmp/session.env
	echo started
	;;
run)
	shift
	. /tmp/session.env
	daemon -f -o /tmp/run.log sh -c "exec $*"
	;;
shot)
	mkdir -p /root/shots
	DISPLAY=:1 xwd -root -silent > "/root/shots/$2.xwd"
	;;
stop)
	# the session and everything it started (by the marker in their
	# environment)
	for p in $(ps -axo pid=); do
		if procstat -e "$p" 2> /dev/null | grep -q PRO1X_TRY=1; then
			kill "$p" 2> /dev/null
		fi
	done
	;;
*)
	echo "usage: session.sh start|run CMD...|shot NAME|stop" >&2
	exit 2
	;;
esac
