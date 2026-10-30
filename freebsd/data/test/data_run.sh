#!/bin/sh
# pro1x-modemd's mobile data (data.c) against the simulated modem with WDS,
# WDA and DPM (fakedata.py on freebsd/modem/test's fakemodem.py), on a
# private D-Bus; checked by test_data.py. Scenarios: main (QMAPv4 after
# QMAPv5 refused, dual stack, the network ending it, a wrong APN, stop),
# endpoint (WDA wants the endpoint info). The password is in no log.
# Needs libqmi-glib, libqrtr-glib, python3 with dbus-python and PyGObject.
set -eu
T=$(mktemp -d /tmp/pro1x-data-test.XXXXXX)
D=$(cd "$(dirname "$0")" && pwd)
B=${B:-$T/pro1x-modemd}
trap 'kill $(cat "$T"/*.pid 2>/dev/null) 2>/dev/null || :; rm -rf "$T"' EXIT
[ -x "$B" ] || cc -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
    -fno-omit-frame-pointer -o "$B" "$D"/../../modem/pro1x-modemd/*.c \
    $(pkg-config --cflags --libs qmi-glib qrtr-glib gio-2.0 gio-unix-2.0)
cat > "$T/bus.conf" <<CONF
<busconfig>
  <type>session</type>
  <listen>unix:path=$T/bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF
dbus-daemon --config-file="$T/bus.conf" --nofork --nopidfile > "$T/bus.log" 2>&1 &
echo $! > "$T/bus.pid"
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S "$T/bus" ] && break; sleep 0.2; done
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=$T/bus
PASS=SeCrEt77
umask 077
printf 'apn=internet.test\nusername=user1\npassword=%s\nauth=chap\nprotocol=dual\n' \
    "$PASS" > "$T/data.conf"
cat > "$T/hook" <<HOOK
#!/bin/sh
{ echo "\$1 \$2"; env | grep -E '^(IPV4|IPV6|MTU|INTERFACE)' | sort; } >> "$T/hook.log"
HOOK
chmod +x "$T/hook"
fails=0

scenario() {
	name=$1
	echo "=== $name"
	: > "$T/hook.log"
	python3 "$D/fakedata.py" --pty-link "$T/modem" --ctl "$T/ctl" --log "$T/req.log" \
	    --online > "$T/fake.log" 2>&1 &
	echo $! > "$T/fake.pid"
	for i in 1 2 3 4 5 6 7 8 9 10; do [ -S "$T/ctl" ] && break; sleep 0.2; done
	"$B" -v -d "$T/modem" -s 0 -p "" -c "$T/data.conf" -I "$T/hook" -E 9,1 -X \
	    > "$T/modemd.log" 2>&1 &
	echo $! > "$T/modemd.pid"
	python3 "$D/test_data.py" "$T/ctl" "$T/req.log" "$T/hook.log" "$name" ||
	    { fails=$((fails + 1)); echo "--- pro1x-modemd's log"; cat "$T/modemd.log";
	      echo "--- the requests"; cat "$T/req.log"; }
	kill "$(cat "$T/modemd.pid")" 2>/dev/null || :
	for i in 1 2 3 4 5 6 7 8 9 10; do
		kill -0 "$(cat "$T/modemd.pid")" 2>/dev/null || break; sleep 0.3
	done
	wait "$(cat "$T/modemd.pid")" 2>/dev/null || {
		st=$?; [ $st -eq 0 ] || { echo "FAIL pro1x-modemd ended with $st"; fails=$((fails + 1)); }; }
	rm -f "$T/modemd.pid"
	kill "$(cat "$T/fake.pid")" 2>/dev/null || :
	rm -f "$T/fake.pid" "$T/ctl"
	grep -E "ERROR: (Address|Undefined|Leak)Sanitizer|runtime error" "$T/modemd.log" &&
	    { echo "FAIL sanitizer"; fails=$((fails + 1)); }
	grep -E "Qmi-(WARNING|CRITICAL).*(unread|Cannot read|invalid)" "$T/modemd.log" &&
	    { echo "FAIL libqmi could not decode the simulated modem"; fails=$((fails + 1)); }
	if grep -q "$PASS" "$T/modemd.log"; then
		echo "FAIL the password is in pro1x-modemd's log"; fails=$((fails + 1))
	fi
	grep -E "data:" "$T/modemd.log" | head -40
}

FAKEDATA_WDA=qmapv4 scenario main
FAKEDATA_WDA=qmapv5 FAKEDATA_WDA_EP=1 scenario endpoint

[ $fails -eq 0 ] && echo "data_run.sh: all scenarios passed" || echo "data_run.sh: $fails failed"
[ $fails -eq 0 ]
