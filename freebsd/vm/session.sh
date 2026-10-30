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
#   session.sh rotate portrait|landscape
#                                 the device turned: mce's orientation
#                                 signal (only with PORTRAIT=1)
#   session.sh stop               everything it started ends
# PORTRAIT=1 (start): Xorg with its dummy driver instead of Xvfb, which
# can change the screen's size, and fake-rotation.so (beside this script,
# built when missing) in the session, so that hildon-desktop can turn the
# screen to portrait (1080x2160) when told the device was turned.
# The VM is emulated and slow: wait some seconds after a tap before the
# screenshot.
set -u
here=$(cd "$(dirname "$0")" && pwd)
# Xorg's dummy driver with a landscape and a portrait mode of the phone's
# size, and the rotation that hildon-desktop asks for made a change of mode
# (fake-rotation.c)
portrait_x() {
	so=/tmp/fake-rotation.so
	if [ ! -f "$so" ] || [ "$here/fake-rotation.c" -nt "$so" ]; then
		cc -shared -fPIC -o "$so" "$here/fake-rotation.c" \
		    -I/usr/local/include -L/usr/local/lib -lXrandr -lX11
	fi
	cat > /tmp/pro1x-dummy.conf <<-EOF
	Section "ServerFlags"
		Option "AutoAddDevices" "false"
		Option "AutoAddGPU" "false"
		Option "DontVTSwitch" "true"
	EndSection
	Section "Device"
		Identifier "dummy"
		Driver "dummy"
		VideoRam 65536
	EndSection
	Section "Monitor"
		Identifier "monitor"
		HorizSync 5.0 - 500.0
		VertRefresh 5.0 - 200.0
		Modeline "2160x1080" 196.0 2160 2304 2528 2896 1080 1083 1093 1130
		Modeline "1080x2160" 196.0 1080 1128 1240 1400 2160 2163 2173 2333
	EndSection
	Section "Screen"
		Identifier "screen"
		Device "dummy"
		Monitor "monitor"
		DefaultDepth 24
		SubSection "Display"
			Depth 24
			Modes "2160x1080" "1080x2160"
			Virtual 2160 2160
		EndSubSection
	EndSection
	Section "ServerLayout"
		Identifier "layout"
		Screen "screen"
	EndSection
	EOF
	daemon -f -o /tmp/xvfb.log Xorg :1 -config /tmp/pro1x-dummy.conf -noreset \
	    -nolisten tcp -logfile /tmp/xorg-dummy.log
	sleep 4
	export LD_PRELOAD=$so
}
case ${1:-} in
start)
	service dbus onestatus > /dev/null 2>&1 || service dbus onestart
	export DISPLAY=:1 PRO1X_TRY=1
	if [ -n "${PORTRAIT:-}" ]; then
		portrait_x
	else
		daemon -f -o /tmp/xvfb.log Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp
	fi
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
rotate)
	# mce's device orientation (Leste: its accelerometer module), which
	# hildon-desktop listens to while it may rotate
	dbus-send --system --type=signal /com/nokia/mce/signal \
	    com.nokia.mce.signal.sig_device_orientation_ind string:"$2" \
	    string:off_stand string:face_up int32:0 int32:0 int32:0
	;;
stop)
	# the session and everything it started (by the marker in their
	# environment)
	for p in $(ps -axo pid=); do
		# (not this script nor the one that runs it)
		[ "$p" = $$ ] || [ "$p" = "$PPID" ] && continue
		if procstat -e "$p" 2> /dev/null | grep -q PRO1X_TRY=1; then
			kill "$p" 2> /dev/null
		fi
	done
	;;
*)
	echo "usage: session.sh start|run CMD...|shot NAME|rotate portrait|landscape|stop" >&2
	exit 2
	;;
esac
