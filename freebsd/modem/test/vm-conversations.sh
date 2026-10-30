#!/bin/sh
# vm-conversations.sh start|accounts|shot NAME|ctl COMMAND|click X Y|type TEXT|
#     key KEY|db|events on|off|stop:
# Conversations (SMS through telepathy-ring and Mission Control) against
# pro1x-modemd and the simulated modem (fakemodem.py) in the FreeBSD test VM
# (freebsd/vm), as root, on the VM's system bus, in Hildon on a virtual
# screen (Xvfb :1, 2160x1080). Installed in the VM: Hildon and its apps
# (conversations, sphone, pro1x-events), FreeBSD's telepathy-glib and
# telepathy-mission-control, the telepathy-ring part (its files below
# /usr/local, as its package has them). From the PC in /root/convtest:
# pro1x-modemd, lib/ (libqmi-glib, libqrtr-glib, and librtcom-eventlogger
# with patches/rtcom-eventlogger/0001 if the VM's is older), fakemodem.py,
# pro1x-modemd.conf (the D-Bus policy), hildon-session (this repository's,
# which starts Conversations in the background), 50-pro1x-freebsd.ini
# (sphone's settings, calls only) with modules/ (sphone's modules). State
# and logs in /tmp/conv; the event log of before is put aside while the
# test runs (back at stop).
#   start       the policy, the bus, the simulated modem (PIN 7395 in the
#               PIN file; Raw Send answered after 1.5 s, as a network
#               does), pro1x-modemd, the Hildon session, sphone for the
#               calls; DEBUG=1: Mission Control's and telepathy-ring's
#               debug logs in /tmp/conv (mc.log, ring.log; GLib's debug
#               messages on for the whole session)
#   accounts    Mission Control's accounts and ring's connection
#   shot NAME   the screen as /root/shots/NAME.xwd
#   ctl CMD     a command to the simulated modem (fakemodem.py)
#   click, type, key   xdotool on the screen
#   db          the event log's newest events
#   events on|off   the MeeGo-like look (the Events page on the first home
#               view) on or off
#   stop        everything ended, the event log of before back
set -u
T=/root/convtest
F=/tmp/conv
EL=/root/.rtcom-eventlogger
export DISPLAY=:1
senv() { [ -f "$F/session.env" ] && . "$F/session.env"; }
case "${1:-}" in
start)
	mkdir -p "$F" /root/shots
	install -m 644 "$T/pro1x-modemd.conf" /usr/local/etc/dbus-1/system.d/pro1x-modemd.conf
	service dbus onestatus > /dev/null 2>&1 || service dbus onestart
	dbus-send --system --type=method_call --dest=org.freedesktop.DBus / \
	    org.freedesktop.DBus.ReloadConfig
	# a fresh event log for the test, the one of before aside
	if [ -d "$EL" ] && [ ! -d "$F/el-saved" ]; then
		mkdir -p "$F/el-saved"
		mv "$EL"/el-v1.db* "$F/el-saved/" 2> /dev/null
	fi
	Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp > "$F/xvfb.log" 2>&1 &
	echo $! > "$F/xvfb.pid"
	sleep 3
	PRO1X_TRY=1 /usr/local/bin/pro1x-hidpi theme
	# home on its first view (the Events page once the look is on)
	gconftool-2 --direct --config-source=xml:readwrite:$HOME/.config/gconf \
	    -s -t int /apps/osso/hildon-desktop/views/current 1
	# sphone for the calls only (the package's settings) with its modules
	mkdir -p /root/.config/sphone
	[ -f /root/.config/sphone/user.ini ] && [ ! -f "$F/sphone-user.ini" ] &&
	    cp /root/.config/sphone/user.ini "$F/sphone-user.ini"
	cp "$T/50-pro1x-freebsd.ini" /root/.config/sphone/user.ini
	[ -d "$T/modules" ] &&
	    printf '\n[Modules]\nModulePath=%s/modules\n' "$T" >> /root/.config/sphone/user.ini
	umask 077
	echo 7395 > "$F/sim-pin"
	rm -f "$F/pin-tried"
	daemon -f -o "$F/fake.log" -p "$F/fake.pid" python3.11 "$T/fakemodem.py" \
	    --pty-link "$F/modem" --ctl "$F/ctl" --log "$F/req.log" --pin 7395 \
	    --send-delay 1.5
	sleep 3
	LD_LIBRARY_PATH=$T/lib daemon -f -o "$F/modemd.log" -p "$F/modemd.pid" \
	    "$T/pro1x-modemd" -v -d "$F/modem" -p "$F/sim-pin" -P "$F/pin-tried" -s 10
	sleep 15
	umask 022
	# the session (and its bus, so also what the bus starts: Mission
	# Control, telepathy-ring) with the test's libraries
	dbg=
	[ -n "${DEBUG:-}" ] && dbg="G_MESSAGES_DEBUG=all MC_DEBUG=all MC_LOGFILE=$F/mc.log
	    RING_DEBUG=all MODEM_DEBUG=all RING_LOGFILE=$F/ring.log"
	# shellcheck disable=SC2086
	env PRO1X_TRY=1 LD_LIBRARY_PATH=$T/lib $dbg \
	    daemon -f -o "$F/session.log" -p "$F/session.pid" sh "$T/hildon-session"
	sleep "${WAIT:-60}"
	hd=$(pgrep -x hildon-desktop | head -1)
	procstat --libxo=json -e "$hd" | python3.11 -c '
import json, shlex, sys
d = json.load(sys.stdin)["procstat"]["environment"]
for e in list(d.values())[0]["environment"]:
    k, _, v = e.partition("=")
    if k in ("PWD", "SHLVL", "_"): continue
    print("export %s=%s" % (k, shlex.quote(v)))' > "$F/session.env"
	(senv; daemon -f -o "$F/sphone.log" -p "$F/sphone.pid" /usr/local/bin/sphone -v)
	sleep 20
	echo "--- pro1x-modemd"
	cat "$F/modemd.log"
	echo "--- processes"
	ps -axo pid,command | grep -E 'mission-control|telepathy-ring|conversations|sphone' |
	    grep -v grep
	sh "$0" accounts
	;;
accounts)
	senv
	ds() { dbus-send --session --print-reply --dest="$1" "$2" \
	    org.freedesktop.DBus.Properties.Get string:"$3" string:"$4" 2>&1 |
	    sed -n '2,$p' | tr -s ' \n' ' '; echo; }
	AM=org.freedesktop.Telepathy.AccountManager
	A=/org/freedesktop/Telepathy/Account/ring/tel/account0
	echo "ValidAccounts: $(ds $AM /org/freedesktop/Telepathy/AccountManager $AM ValidAccounts)"
	for p in DisplayName Enabled ConnectAutomatically ConnectionStatus Connection; do
		echo "$p: $(ds $AM $A org.freedesktop.Telepathy.Account $p)"
	done
	;;
shot)
	xwd -root -silent > "/root/shots/$2.xwd" && echo "/root/shots/$2.xwd"
	;;
ctl)
	shift
	printf '%s\n' "$*" | python3.11 -c '
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
s.sendall(sys.stdin.read().encode()); print(s.recv(65536).decode().strip())' "$F/ctl"
	;;
click)
	xdotool mousemove "$2" "$3" click 1
	;;
type)
	shift
	xdotool type --delay 80 "$*"
	;;
key)
	xdotool key "$2"
	;;
db)
	sqlite3 "$EL/el-v1.db" "SELECT e.id, s.name, t.name, e.local_uid, e.remote_uid,
	    e.outgoing, e.is_read, e.flags, e.free_text FROM Events e
	    JOIN Services s ON s.id = e.service_id JOIN EventTypes t ON t.id = e.event_type_id
	    ORDER BY e.id DESC LIMIT ${2:-10}"
	echo "--- headers"
	sqlite3 "$EL/el-v1.db" "SELECT event_id, name, value FROM Headers ORDER BY id DESC LIMIT 10"
	;;
events)
	senv
	gconftool-2 -s -t bool /apps/osso/hildon-desktop/meego_pages \
	    "$([ "$2" = on ] && echo true || echo false)"
	;;
stop)
	senv
	gconftool-2 -s -t bool /apps/osso/hildon-desktop/meego_pages false 2> /dev/null
	for p in sphone session modemd fake xvfb; do
		[ -f "$F/$p.pid" ] && kill "$(cat "$F/$p.pid")" 2> /dev/null
	done
	sleep 2
	# what the session and its bus started (the marker in their environment)
	for p in $(ps -axo pid=); do
		[ "$p" = $$ ] && continue
		if procstat -e "$p" 2> /dev/null | grep -q PRO1X_TRY=1; then
			kill "$p" 2> /dev/null
		fi
	done
	sleep 2
	if [ -d "$F/el-saved" ]; then
		mkdir -p "$F/el-test"
		mv "$EL"/el-v1.db* "$F/el-test/" 2> /dev/null
		mv "$F"/el-saved/el-v1.db* "$EL/" 2> /dev/null
		rmdir "$F/el-saved"
	fi
	rm -f /root/.config/sphone/user.ini
	[ -f "$F/sphone-user.ini" ] && mv "$F/sphone-user.ini" /root/.config/sphone/user.ini
	rm -f /usr/local/etc/dbus-1/system.d/pro1x-modemd.conf
	dbus-send --system --type=method_call --dest=org.freedesktop.DBus / \
	    org.freedesktop.DBus.ReloadConfig
	echo stopped
	;;
*)
	echo "usage: $0 start|accounts|shot NAME|ctl COMMAND|click X Y|type TEXT|key KEY|db|events on|off|stop" >&2
	exit 2
	;;
esac
