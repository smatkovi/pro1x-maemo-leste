#!/bin/sh
# vm-gps.sh: the GPS chain in the FreeBSD test VM (freebsd/vm), as root,
# as a Hildon app on the phone would use it: liblocation's example client
# (liblocation/examples/basic.c: start, the first fix's latitude and
# longitude, stop) asks location-daemon over the VM's system bus; D-Bus
# activates location-daemon, which finds no gpsd and runs
# /usr/local/libexec/location-daemon-gpsd start; that starts gpsd on
# udp://127.0.0.1:29470 and pro1x-gnss on the simulated modem's QMI LOC
# (fakemodem.py on a pty instead of the modem's QRTR node), which sends the
# positions as NMEA to gpsd.
#
# Its files are in /root/gpstest (copied there by the PC: gpsd, gpspipe,
# pro1x-gnss, location-daemon, basic, lib/ with libgps, libqmi-glib,
# libqrtr-glib and the patched liblocation, fakemodem.py, the D-Bus policy);
# state and logs in /tmp/gt. The hook, the D-Bus service file and the policy
# are installed for the run and removed after it.
set -u
T=/root/gpstest
G=/tmp/gt
HOOK=/usr/local/libexec/location-daemon-gpsd
SVC=/usr/local/share/dbus-1/system-services/org.maemo.LocationDaemon.service
POL=/usr/local/etc/dbus-1/system.d/org.maemo.LocationDaemon.conf
rm -rf "$G"
mkdir -p "$G"
cleanup() {
	for p in gnss gpsd fake; do
		[ -f "$G/$p.pid" ] && kill "$(cat "$G/$p.pid")" 2> /dev/null
	done
	pkill -f "$T/location-daemon" 2> /dev/null
	rm -f "$HOOK" "$SVC" "$POL"
	dbus-send --system --type=method_call --dest=org.freedesktop.DBus / \
	    org.freedesktop.DBus.ReloadConfig
}
trap cleanup EXIT
[ ! -e "$HOOK" ] && [ ! -e "$SVC" ] || { echo "a hook or service file is installed already"; exit 1; }

daemon -f -o "$G/fake.log" -p "$G/fake.pid" python3.11 "$T/fakemodem.py" \
    --pty-link "$G/modem" --ctl "$G/ctl" --log "$G/req.log" --online
sleep 3
# the phone's rc.d/pro1x_gps, with the VM's paths
cat > "$HOOK" <<EOF
#!/bin/sh
[ "\$1" = start ] || exit 0
export LD_LIBRARY_PATH=$T/lib
$T/gpsd -n -P $G/gpsd.pid udp://127.0.0.1:29470 || exit 1
/usr/sbin/daemon -f -P $G/gnss.pid -o $G/gnss.log $T/pro1x-gnss -v -d $G/modem -p 29470
echo "hook: gpsd and pro1x-gnss started" >> $G/hook.log
EOF
chmod 755 "$HOOK"
printf '#!/bin/sh\nexport LD_LIBRARY_PATH=%s/lib G_MESSAGES_DEBUG=all\nexec %s/location-daemon > %s/location-daemon.log 2>&1\n' \
    "$T" "$T" "$G" > "$G/location-daemon.sh"
chmod 755 "$G/location-daemon.sh"
printf '[D-BUS Service]\nName=org.maemo.LocationDaemon\nExec=%s/location-daemon.sh\nUser=root\n' \
    "$G" > "$SVC"
install -m 644 "$T/org.maemo.LocationDaemon.conf" "$POL"
service dbus onestatus > /dev/null 2>&1 || service dbus onestart
dbus-send --system --type=method_call --dest=org.freedesktop.DBus / \
    org.freedesktop.DBus.ReloadConfig
timeout 60 dbus-monitor --system \
    "type='signal',interface='org.maemo.LocationDaemon.Position'" \
    "type='signal',interface='org.maemo.LocationDaemon.Device'" \
    "type='signal',interface='org.maemo.LocationDaemon.Running'" > "$G/monitor.log" 2>&1 &

echo "--- liblocation's example"
# GConf (liblocation reads its settings) needs a session bus
LD_LIBRARY_PATH=$T/lib G_MESSAGES_DEBUG=all timeout 60 dbus-run-session -- "$T/basic" 2>&1 | tail -12
echo "basic: $?"
sleep 2
echo "--- the hook"
cat "$G/hook.log" 2>&1
echo "--- location-daemon"
grep -v "^$" "$G/location-daemon.log" | head -15
echo "--- its signals"
grep -A3 -E "member=(PositionChanged|FixStatusChanged|Running)" "$G/monitor.log" | head -24
echo "--- gpsd (gpspipe -w)"
LD_LIBRARY_PATH=$T/lib timeout 5 "$T/gpspipe" -w -n 8 2>&1 | grep -E '"class":"(TPV|DEVICES)"' | tail -2
echo "--- pro1x-gnss"
head -8 "$G/gnss.log"
echo "--- the simulated modem's LOC requests"
grep "LOC" "$G/req.log"
