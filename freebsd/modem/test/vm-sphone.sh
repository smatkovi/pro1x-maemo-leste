#!/bin/sh
# vm-sphone.sh start|shot NAME|ctl COMMAND|click X Y|type TEXT|key KEY|stop:
# sphone, unchanged, against pro1x-modemd and the simulated modem
# (fakemodem.py) in the FreeBSD test VM (freebsd/vm), as root, on the VM's
# system bus, in Hildon on a virtual screen (Xvfb :1, 2160x1080, as
# vm/try-hildon.sh). Its files are in /root/modemtest (copied there by the
# PC: pro1x-modemd, lib/ with libqmi-glib and libqrtr-glib, fakemodem.py,
# pro1x-modemd.conf, the D-Bus policy; optionally modules/ with sphone's
# newer modules and 50-pro1x-freebsd.ini, pro1x-callaudio and
# fakeaudiod.py); its state and logs in /tmp/fm.
#   start      the policy, the bus, Hildon, the simulated modem (PIN 7395,
#              the PIN file), pro1x-modemd, sphone
#   shot NAME  the screen as /root/shots/NAME.xwd
#   ctl CMD    a command to the simulated modem (fakemodem.py)
#   click, type, key   xdotool on the screen
#   stop       everything ended, the policy removed
set -u
T=/root/modemtest
F=/tmp/fm
export DISPLAY=:1
case "${1:-}" in
start)
	mkdir -p "$F" /root/shots
	install -m 644 "$T/pro1x-modemd.conf" /usr/local/etc/dbus-1/system.d/pro1x-modemd.conf
	service dbus onestatus > /dev/null 2>&1 || service dbus onestart
	dbus-send --system --type=method_call --dest=org.freedesktop.DBus / \
	    org.freedesktop.DBus.ReloadConfig
	Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp > "$F/xvfb.log" 2>&1 &
	echo $! > "$F/xvfb.pid"
	sleep 3
	PRO1X_TRY=1 /usr/local/bin/pro1x-hidpi theme
	PRO1X_TRY=1 daemon -f -o "$F/session.log" -p "$F/session.pid" /usr/local/bin/hildon-session
	sleep "${WAIT:-50}"
	hd=$(pgrep -x hildon-desktop | head -1)
	procstat --libxo=json -e "$hd" | python3.11 -c '
import json, shlex, sys
d = json.load(sys.stdin)["procstat"]["environment"]
for e in list(d.values())[0]["environment"]:
    k, _, v = e.partition("=")
    if k in ("PWD", "SHLVL", "_"): continue
    print("export %s=%s" % (k, shlex.quote(v)))' > "$F/session.env"
	# calls in landscape: hildon-desktop turns the screen for sphone's
	# portrait call window by rotating the CRTC, which the VM's X server
	# (and the phone's scfb) cannot: the picture went black
	mkdir -p /root/.config/sphone
	printf '[Gui]\nLandscapeCalls=1\n' > /root/.config/sphone/user.ini
	# a newer sphone's modules (modules/, with route-pro1x) and the
	# package's module list, if they came along
	if [ -d "$T/modules" ] && [ -f "$T/50-pro1x-freebsd.ini" ]; then
		printf '\n[Modules]\nModulePath=%s/modules\n' "$T" >> /root/.config/sphone/user.ini
		grep '^Modules=' "$T/50-pro1x-freebsd.ini" >> /root/.config/sphone/user.ini
	fi
	# pro1x-callaudio per call against a stand-in for pro1x-audiod, if
	# they came along (its pid file where sphone's route-pro1x looks)
	A=
	if [ -x "$T/pro1x-callaudio" ] && [ -f "$T/fakeaudiod.py" ]; then
		daemon -f -o "$F/audiod.log" -p "$F/audiod.pid" python3.11 "$T/fakeaudiod.py" \
		    "$F/audio.sock" "$F/audio.log"
		printf '#!/bin/sh\nexec %s/pro1x-callaudio -v -S %s/audio.sock\n' "$T" "$F" > "$F/callaudio"
		chmod 755 "$F/callaudio"
		A="-A $F/callaudio"
	fi
	umask 077
	echo 7395 > "$F/sim-pin"
	rm -f "$F/pin-tried"
	daemon -f -o "$F/fake.log" -p "$F/fake.pid" python3.11 "$T/fakemodem.py" \
	    --pty-link "$F/modem" --ctl "$F/ctl" --log "$F/req.log" --pin 7395
	sleep 3
	LD_LIBRARY_PATH=$T/lib daemon -f -o "$F/modemd.log" -p "$F/modemd.pid" \
	    "$T/pro1x-modemd" -v -d "$F/modem" -p "$F/sim-pin" -P "$F/pin-tried" -s 10 $A
	sleep 15
	(. "$F/session.env"; daemon -f -o "$F/sphone.log" -p "$F/sphone.pid" \
	    /usr/local/bin/sphone -v)
	sleep 30
	echo "--- pro1x-modemd"
	cat "$F/modemd.log"
	echo "--- sphone"
	tail -40 "$F/sphone.log"
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
stop)
	for p in sphone modemd fake audiod session xvfb; do
		[ -f "$F/$p.pid" ] && kill "$(cat "$F/$p.pid")" 2> /dev/null
	done
	pkill -f /usr/local/bin/sphone 2> /dev/null
	pkill -x hildon-desktop hildon-home hildon-status-menu 2> /dev/null
	sleep 2
	rm -f /usr/local/etc/dbus-1/system.d/pro1x-modemd.conf
	dbus-send --system --type=method_call --dest=org.freedesktop.DBus / \
	    org.freedesktop.DBus.ReloadConfig
	echo stopped
	;;
*)
	echo "usage: $0 start|shot NAME|ctl COMMAND|click X Y|type TEXT|key KEY|stop" >&2
	exit 2
	;;
esac
