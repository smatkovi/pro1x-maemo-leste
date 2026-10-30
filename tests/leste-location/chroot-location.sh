#!/bin/bash
# chroot-location.sh start|reset [off|accepted|off-accepted]|ui|basic|
#   methods|create [SECONDS]|startstop|matchrule|closetest PATH|shot NAME|
#   click X Y|log|stop
#
# liblocation's dialogs against location-ui in the Leste chroot on the PC
# (scripts/leste-chroot.sh), as root: a system bus of the chroot's own,
# a session bus (GConf), a fake MCE (fake-mce.py: get_device_mode
# "normal"), Xvfb :5 at the N900's 800x480, dbus-monitor on the system
# bus (fake-mce.py also stands in for location-daemon, so the bus does
# not activate the real one). location-ui is run directly (launch.c loads its
# /usr/libexec/location-ui.launch, as maemo-launcher would, without the
# invoker); it ends by itself 15 s after its last dialog.
#
#   start      the buses, the fake MCE, Xvfb, the monitor
#   reset      GConf's location keys to the schema's defaults (disclaimer
#              not accepted, network positioning off); "off": GPS and
#              network off; "accepted": the disclaimer accepted;
#              "off-accepted": both (the "enable positioning" dialog)
#   ui         location-ui started (unless it runs)
#   basic      liblocation's example (examples/basic.c, method
#              USER_SELECTED) in the background
#   methods    allowed-methods.c: location_gpsd_control_get_allowed_methods()
#   create     create-only.c: a LocationGPSDevice and a LocationGPSDControl
#              created, start() never called; then what that did on the
#              system bus: dialog requests, location-daemon's start,
#              MatchRuleInvalid answers (all 0 with fixed liblocation)
#   startstop  start-stop.c: a control created, started, stopped, started
#              again (run it with "reset accepted"): its signals, the lock
#              location-daemon watches after each step, location-daemon's
#              start calls (2 with fixed liblocation)
#   matchrule  the bus' answer to the Uncertainty match rule without and
#              with its closing quote, and the rule in the installed
#              liblocation.so.0
#   closetest  location-ui's "display" then "close" of PATH by dbus-send,
#              then whether location-ui still runs
#   shot       the screen as $L/NAME.xwd;  click  xdotool's click at X Y
#   log        what the example, location-ui, the fake MCE and the
#              monitor wrote
#   stop       everything started here ended (by its pid)
#
# The sources are next to this script (basic.c from liblocation's
# examples, copied there by the caller); state in /tmp/lt.
UNCERTAINTY="type='signal',interface='com.nokia.Location.Uncertainty"
set -u
T=$(cd "$(dirname "$0")" && pwd)
L=/tmp/lt
GC=/system/nokia/location
export DISPLAY=:5
export LANG=C.UTF-8

session() {
	[ -f "$L/session.addr" ] || { echo "not started: $0 start"; exit 1; }
	DBUS_SESSION_BUS_ADDRESS=$(cat "$L/session.addr") "$@"
}
alive() { [ -f "$L/$1.pid" ] && kill -0 "$(cat "$L/$1.pid")" 2> /dev/null; }
bg() {
	local name=$1
	shift
	session "$@" > "$L/$name.log" 2>&1 &
	echo $! > "$L/$name.pid"
}

case "${1:-}" in
start)
	rm -rf "$L"
	mkdir -p "$L" /run/dbus /run/lock
	dbus-uuidgen --ensure
	if [ ! -S /run/dbus/system_bus_socket ] ||
	    ! dbus-send --system --print-reply --dest=org.freedesktop.DBus / \
	        org.freedesktop.DBus.GetId > /dev/null 2>&1; then
		rm -f /run/dbus/system_bus_socket /run/dbus/pid
		dbus-daemon --system --fork --print-pid=3 3> "$L/sysbus.pid"
	fi
	dbus-daemon --session --fork --print-address=3 --print-pid=4 \
	    3> "$L/session.addr" 4> "$L/sessbus.pid"
	sleep 1
	Xvfb :5 -screen 0 800x480x24 -noreset -nolisten tcp > "$L/xvfb.log" 2>&1 &
	echo $! > "$L/xvfb.pid"
	bg mce python3 -u "$T/fake-mce.py" normal
	bg monitor dbus-monitor --system \
	    "interface='com.nokia.Location.UI.Dialog'" \
	    "interface='org.maemo.LocationDaemon'" "interface='com.nokia.mce.request'" \
	    "type='method_return'" "type='error'"
	sleep 5
	alive xvfb || { echo "Xvfb did not start"; cat "$L/xvfb.log"; exit 1; }
	alive mce || { echo "the fake MCE did not start"; cat "$L/mce.log"; exit 1; }
	echo "started"
	;;
reset)
	for k in disclaimer-accepted gps-disabled network-disabled method; do
		session gconftool-2 --unset "$GC/$k"
	done
	case "${2:-}" in off|off-accepted)
		session gconftool-2 -s -t bool "$GC/gps-disabled" true
		session gconftool-2 -s -t bool "$GC/network-disabled" true ;;
	esac
	case "${2:-}" in accepted|off-accepted)
		session gconftool-2 -s -t bool "$GC/disclaimer-accepted" true ;;
	esac
	for k in disclaimer-accepted gps-disabled network-disabled; do
		printf '%s=%s\n' "$k" "$(session gconftool-2 -g "$GC/$k" 2>&1)"
	done
	;;
ui)
	if alive location-ui; then
		echo "location-ui runs"
	else
		[ ! -f "$L/location-ui.log" ] || cat "$L/location-ui.log" >> "$L/location-ui.old.log"
		[ -x "$L/launch" ] || gcc -Wall -o "$L/launch" "$T/launch.c" -ldl
		bg location-ui env G_MESSAGES_DEBUG=all "$L/launch" /usr/libexec/location-ui.launch
		for _ in $(seq 30); do
			dbus-send --system --print-reply --dest=org.freedesktop.DBus / \
			    org.freedesktop.DBus.NameHasOwner string:com.nokia.Location.UI 2> /dev/null |
			    grep -q "boolean true" && break
			sleep 1
		done
		dbus-send --system --print-reply --dest=org.freedesktop.DBus / \
		    org.freedesktop.DBus.NameHasOwner string:com.nokia.Location.UI | tail -1
	fi
	;;
basic)
	[ -x "$L/basic" ] || gcc -Wall -o "$L/basic" "$T/basic.c" $(pkg-config --cflags --libs liblocation)
	bg basic env G_MESSAGES_DEBUG=all "$L/basic"
	echo "basic: pid $(cat "$L/basic.pid")"
	;;
methods)
	[ -x "$L/allowed-methods" ] ||
	    gcc -Wall -o "$L/allowed-methods" "$T/allowed-methods.c" $(pkg-config --cflags --libs liblocation)
	session "$L/allowed-methods"
	;;
create)
	[ -x "$L/create-only" ] ||
	    gcc -Wall -o "$L/create-only" "$T/create-only.c" $(pkg-config --cflags --libs liblocation)
	n=$(wc -l < "$L/monitor.log")
	m=$(wc -l < "$L/mce.log")
	session "$L/create-only" "${2:-4}" 2>&1 | sed 's/^/  /'
	sleep 1
	tail -n +$((n + 1)) "$L/monitor.log" > "$L/create-monitor.log"
	echo "on the system bus meanwhile ($L/create-monitor.log):"
	echo "  location-ui dialog requests: $(grep -c 'interface=com.nokia.Location.UI.Dialog; member=display' "$L/create-monitor.log")"
	echo "  location-daemon start calls: $(grep -c 'interface=org.maemo.LocationDaemon; member=start' "$L/create-monitor.log")"
	echo "  ... received by the stand-in: $(tail -n +$((m + 1)) "$L/mce.log" | grep -c 'location-daemon: start')"
	echo "  MatchRuleInvalid answers:    $(grep -c 'MatchRuleInvalid' "$L/create-monitor.log")"
	;;
startstop)
	[ -x "$L/start-stop" ] ||
	    gcc -Wall -o "$L/start-stop" "$T/start-stop.c" $(pkg-config --cflags --libs liblocation)
	n=$(wc -l < "$L/monitor.log")
	m=$(wc -l < "$L/mce.log")
	session "$L/start-stop" 2>&1 | grep -v "DEBUG" | sed 's/^/  /'
	sleep 1
	tail -n +$((n + 1)) "$L/monitor.log" > "$L/startstop-monitor.log"
	echo "on the system bus meanwhile ($L/startstop-monitor.log):"
	echo "  location-daemon start calls: $(grep -c 'interface=org.maemo.LocationDaemon; member=start' "$L/startstop-monitor.log")"
	echo "  ... received by the stand-in: $(tail -n +$((m + 1)) "$L/mce.log" | grep -c 'location-daemon: start')"
	echo "  location-ui dialog requests: $(grep -c 'interface=com.nokia.Location.UI.Dialog; member=display' "$L/startstop-monitor.log")"
	;;
matchrule)
	for r in "$UNCERTAINTY" "$UNCERTAINTY'"; do
		echo "AddMatch $r:"
		dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
		    org.freedesktop.DBus.AddMatch "string:$r" 2>&1 | sed 's/^/  /'
	done
	for f in /usr/lib/*/liblocation.so.0; do
		echo "in $(readlink -f "$f"):"
		grep -ao "$UNCERTAINTY'*" "$f" | sed 's/^/  /'
	done
	;;
closetest)
	p=${2:-/com/nokia/location/ui/disclaimer}
	echo "display $p:"
	dbus-send --system --print-reply --dest=com.nokia.Location.UI "$p" \
	    com.nokia.Location.UI.Dialog.display 2>&1 | sed 's/^/  /'
	sleep "${WAIT:-5}"
	echo "close $p:"
	dbus-send --system --print-reply --dest=com.nokia.Location.UI "$p" \
	    com.nokia.Location.UI.Dialog.close 2>&1 | sed 's/^/  /'
	sleep 2
	if alive location-ui; then echo "location-ui still runs"; else
		wait "$(cat "$L/location-ui.pid")" 2> /dev/null
		echo "location-ui is gone"
		tail -3 "$L/location-ui.log"
	fi
	;;
shot)
	xwd -root -silent > "$L/$2.xwd" && echo "$L/$2.xwd"
	;;
click)
	xdotool mousemove "$2" "$3" click 1
	;;
log)
	echo "--- basic"; grep -v "^$" "$L/basic.log" 2> /dev/null | tail -"${N:-15}"
	echo "--- location-ui"; cat "$L/location-ui.old.log" "$L/location-ui.log" 2> /dev/null |
	    grep -v "^$" | tail -"${N:-15}"
	echo "--- fake MCE"; tail -3 "$L/mce.log"
	echo "--- monitor"; grep -v "^ *$" "$L/monitor.log" | grep -A2 -E "Location|mce|LocationDaemon|error|method return" |
	    tail -"${N:-40}"
	;;
stop)
	for p in basic location-ui monitor mce xvfb sessbus sysbus; do
		[ -f "$L/$p.pid" ] && kill "$(cat "$L/$p.pid")" 2> /dev/null
	done
	sleep 1
	echo "stopped"
	;;
*)
	sed -n '2,3p' "$0" >&2
	exit 2
	;;
esac
