#!/bin/sh
# vm-location.sh start|basic|reset|applet|shot NAME|click X Y|log|stop: the
# location dialogs and settings in the FreeBSD test VM (freebsd/vm), as
# root, in Hildon on a virtual screen (Xvfb :1, 2160x1080): location-ui
# (run as hildon-session runs it, --stay) shows liblocation's dialogs,
# location-control's applet the settings, and the GPS chain of vm-gps.sh
# (location-daemon by D-Bus activation, gpsd, pro1x-gnss on fakemodem.py's
# QMI LOC) gives liblocation's example its position once they are answered.
#   start   the session, location-ui, the files below installed for the run
#   basic   liblocation's example in the session (start, a fix, stop)
#   reset   GConf's location keys back to the schema's defaults; with
#           "off": GPS and network positioning off (the "enable" dialog)
#   applet  the control panel's Location applet
#   shot    the screen as /root/shots/NAME.xwd; click: xdotool
#   log     the example's, location-ui's and location-daemon's output
#   stop    everything ended, the installed files removed
# Its files are in /root/loctest (copied there by the PC: those of vm-gps.sh,
# location-ui, liblocation_applet.so, location-cp.desktop,
# location-cp.schemas, the two D-Bus policies); state in /tmp/lt.
set -u
T=/root/loctest
L=/tmp/lt
export DISPLAY=:1
HOOK=/usr/local/libexec/location-daemon-gpsd
SVC=/usr/local/share/dbus-1/system-services/org.maemo.LocationDaemon.service
POLD=/usr/local/etc/dbus-1/system.d/org.maemo.LocationDaemon.conf
POLU=/usr/local/etc/dbus-1/system.d/com.nokia.Location.UI.conf
APPLET=/usr/local/lib/hildon-control-panel/liblocation_applet.so
DESK=/usr/local/share/applications/hildon-control-panel/location-cp.desktop
GC=/system/nokia/location
in_session() { (. "$L/session.env"; "$@"); }
reload() {
	dbus-send --system --type=method_call --dest=org.freedesktop.DBus / \
	    org.freedesktop.DBus.ReloadConfig
}
case "${1:-}" in
start)
	for f in "$HOOK" "$SVC" "$POLD" "$POLU" "$APPLET" "$DESK"; do
		[ ! -e "$f" ] || { echo "$f is installed already"; exit 1; }
	done
	rm -rf "$L"
	mkdir -p "$L" /root/shots
	service dbus onestatus > /dev/null 2>&1 || service dbus onestart
	install -m 644 "$T/org.maemo.LocationDaemon.conf" "$POLD"
	install -m 644 "$T/com.nokia.Location.UI.conf" "$POLU"
	install -m 644 "$T/liblocation_applet.so" "$APPLET"
	install -m 644 "$T/location-cp.desktop" "$DESK"
	cat > "$HOOK" <<-EOF
	#!/bin/sh
	[ "\$1" = start ] || exit 0
	export LD_LIBRARY_PATH=$T/lib
	$T/gpsd -n -P $L/gpsd.pid udp://127.0.0.1:29470 || exit 1
	/usr/sbin/daemon -f -P $L/gnss.pid -o $L/gnss.log $T/pro1x-gnss -v -d $L/modem -p 29470
	EOF
	chmod 755 "$HOOK"
	printf '#!/bin/sh\nexport LD_LIBRARY_PATH=%s/lib\nexec %s/location-daemon > %s/location-daemon.log 2>&1\n' \
	    "$T" "$T" "$L" > "$L/location-daemon.sh"
	chmod 755 "$L/location-daemon.sh"
	printf '[D-BUS Service]\nName=org.maemo.LocationDaemon\nExec=%s/location-daemon.sh\nUser=root\n' \
	    "$L" > "$SVC"
	reload
	# the schema's defaults where rc.d/hildon_setup puts them at boot
	GCONF_CONFIG_SOURCE=xml:merged:/var/db/gconf/gconf.xml.defaults \
	    gconftool-2 --makefile-install-rule "$T/location-cp.schemas" > /dev/null 2>&1
	daemon -f -o "$L/fake.log" -p "$L/fake.pid" python3.11 "$T/fakemodem.py" \
	    --pty-link "$L/modem" --ctl "$L/ctl" --log "$L/req.log" --online
	Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp > "$L/xvfb.log" 2>&1 &
	echo $! > "$L/xvfb.pid"
	sleep 3
	PRO1X_TRY=1 /usr/local/bin/pro1x-hidpi theme
	PRO1X_TRY=1 daemon -f -o "$L/session.log" -p "$L/session.pid" /usr/local/bin/hildon-session
	sleep "${WAIT:-50}"
	hd=$(pgrep -x hildon-desktop | head -1)
	procstat --libxo=json -e "$hd" | python3.11 -c '
import json, shlex, sys
d = json.load(sys.stdin)["procstat"]["environment"]
for e in list(d.values())[0]["environment"]:
    k, _, v = e.partition("=")
    if k in ("PWD", "SHLVL", "_"): continue
    print("export %s=%s" % (k, shlex.quote(v)))' > "$L/session.env"
	grep -q DBUS_SESSION_BUS_ADDRESS "$L/session.env" || echo "no session bus in the environment"
	sh "$0" reset
	in_session daemon -f -o "$L/location-ui.log" -p "$L/location-ui.pid" \
	    "$T/location-ui" --stay
	sleep 5
	dbus-send --system --print-reply --dest=org.freedesktop.DBus / \
	    org.freedesktop.DBus.NameHasOwner string:com.nokia.Location.UI | tail -1
	;;
reset)
	for k in disclaimer-accepted gps-disabled network-disabled method; do
		in_session gconftool-2 --unset "$GC/$k"
	done
	if [ "${2:-}" = off ]; then
		in_session gconftool-2 -s -t bool "$GC/gps-disabled" true
		in_session gconftool-2 -s -t bool "$GC/network-disabled" true
	fi
	in_session gconftool-2 -R "$GC" 2>&1 | head
	;;
basic)
	in_session env LD_LIBRARY_PATH=$T/lib G_MESSAGES_DEBUG=all \
	    daemon -f -o "$L/basic.log" "$T/basic"
	;;
applet)
	in_session dbus-send --session --type=method_call --dest=com.nokia.controlpanel \
	    /com/nokia/controlpanel com.nokia.controlpanel.run_applet \
	    string:liblocation_applet.so boolean:true > /dev/null 2>&1 &
	;;
shot)
	xwd -root -silent > "/root/shots/$2.xwd" && echo "/root/shots/$2.xwd"
	;;
click)
	xdotool mousemove "$2" "$3" click 1
	;;
log)
	echo "--- basic"; grep -v "^$" "$L/basic.log" 2>/dev/null | tail -12
	echo "--- location-ui"; grep -v "^$" "$L/location-ui.log" | tail -8
	echo "--- location-daemon"; grep -iE "start|gpsd" "$L/location-daemon.log" 2>/dev/null | head -4
	echo "--- GConf"; in_session gconftool-2 -R "$GC" 2>&1 | head
	echo "--- LOC"; grep LOC "$L/req.log" | tail -4
	;;
stop)
	for p in gnss gpsd location-ui fake session xvfb; do
		[ -f "$L/$p.pid" ] && kill "$(cat "$L/$p.pid")" 2> /dev/null
	done
	pkill -x controlpanel
	pkill -f "$T/location-daemon"
	pkill -f "$T/basic"
	pkill -x hildon-desktop hildon-home hildon-status-menu
	pkill -f hildon-session
	sleep 2
	GCONF_CONFIG_SOURCE=xml:merged:/var/db/gconf/gconf.xml.defaults \
	    gconftool-2 --makefile-uninstall-rule "$T/location-cp.schemas" > /dev/null 2>&1
	rm -f "$HOOK" "$SVC" "$POLD" "$POLU" "$APPLET" "$DESK"
	reload
	echo stopped
	;;
*)
	echo "usage: $0 start|basic|reset [off]|applet|shot NAME|click X Y|log|stop" >&2
	exit 2
	;;
esac
