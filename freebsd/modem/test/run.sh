#!/bin/sh
# pro1x-modemd against the simulated modem (fakemodem.py, QMUX on a pty)
# on a private D-Bus, its clients' view checked by test_ofono.py:
#   main        the PIN from the PIN file, the network, SMS in and out
#               (one part, three parts, class 0), a call out (DTMF, the
#               pro1x-callaudio: the voice up and down at pro1x-audiod's
#               stand-in), a call in, offline
#   wrongpin    a wrong PIN file: one attempt, then none after a restart
#   lowretries  fewer than 3 tries left: no attempt
#   nofile      no PIN file: the PIN over D-Bus (wrong, then right)
# and that the PIN is in no log. Needs libqmi-glib, libqrtr-glib (the
# host's), python3 with dbus-python and PyGObject.
set -eu
T=$(mktemp -d /tmp/pro1x-modem-test.XXXXXX)
D=$(cd "$(dirname "$0")" && pwd)
B=${B:-$T/pro1x-modemd}
CA=${CA:-$T/pro1x-callaudio}
trap 'kill $(cat "$T"/*.pid 2>/dev/null) 2>/dev/null || :; rm -rf "$T"' EXIT
[ -x "$B" ] || cc -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
    -fno-omit-frame-pointer -o "$B" "$D"/../pro1x-modemd/*.c \
    $(pkg-config --cflags --libs qmi-glib qrtr-glib gio-2.0 gio-unix-2.0)
[ -x "$CA" ] || cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o "$CA" \
    "$D"/../pro1x-callaudio/*.c
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
# sphone and pro1x-modemd look for the system bus here
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=$T/bus
# pro1x-callaudio as pro1x-modemd runs it (-A), against a stand-in for
# pro1x-audiod's control socket, which logs its requests in audio.log
python3 "$D/fakeaudiod.py" "$T/audio.sock" "$T/audio.log" > /dev/null 2>&1 &
echo $! > "$T/audiod.pid"
printf '#!/bin/sh\nexec "%s" -v -S "%s"\n' "$CA" "$T/audio.sock" > "$T/callaudio"
chmod +x "$T/callaudio"
PIN=7395
fails=0

# scenario NAME FAKEMODEM-ARGS MODEMD-ARGS
scenario() {
	name=$1 fargs=$2 margs=$3
	echo "=== $name"
	: > "$T/audio.log"
	python3 "$D/fakemodem.py" --pty-link "$T/modem" --ctl "$T/ctl" --log "$T/req.log" \
	    $fargs > "$T/fake.log" 2>&1 &
	echo $! > "$T/fake.pid"
	for i in 1 2 3 4 5 6 7 8 9 10; do [ -S "$T/ctl" ] && break; sleep 0.2; done
	start_modemd "$margs"
	python3 "$D/test_ofono.py" "$T/ctl" "$T/req.log" "$T/audio.log" "$name" ||
	    { fails=$((fails + 1)); echo "--- pro1x-modemd's log"; cat "$T/modemd.log";
	      echo "--- the requests"; cat "$T/req.log"; }
	stop_modemd
	kill "$(cat "$T/fake.pid")" 2>/dev/null || :
	rm -f "$T/fake.pid" "$T/ctl"
	if grep -q "$PIN" "$T/modemd.log"; then
		echo "FAIL the PIN is in pro1x-modemd's log"; fails=$((fails + 1))
	fi
}
start_modemd() {
	"$B" -v -d "$T/modem" -s 2 -A "$T/callaudio" -P "$T/pin-tried" $1 \
	    > "$T/modemd.log" 2>&1 &
	echo $! > "$T/modemd.pid"
}
stop_modemd() {
	kill "$(cat "$T/modemd.pid")" 2>/dev/null || :
	for i in 1 2 3 4 5 6 7 8 9 10; do
		kill -0 "$(cat "$T/modemd.pid")" 2>/dev/null || break; sleep 0.3
	done
	wait "$(cat "$T/modemd.pid")" 2>/dev/null || {
		st=$?; [ $st -eq 0 ] || { echo "FAIL pro1x-modemd ended with $st"; fails=$((fails + 1)); }; }
	rm -f "$T/modemd.pid"
	grep -E "ERROR: (Address|Undefined|Leak)Sanitizer|runtime error" "$T/modemd.log" &&
	    { echo "FAIL sanitizer"; fails=$((fails + 1)); }
	# libqmi's own decoders disagreeing with the simulated modem's TLVs
	grep -E "Qmi-(WARNING|CRITICAL).*(unread|Cannot read|invalid)" "$T/modemd.log" &&
	    { echo "FAIL libqmi could not decode the simulated modem"; fails=$((fails + 1)); }
	return 0
}

umask 077
echo "$PIN" > "$T/sim-pin"
rm -f "$T/pin-tried"
scenario main "--pin $PIN" "-p $T/sim-pin"
grep -E "sim:|nas:|sms:|call|voice:" "$T/modemd.log" | head -40

echo "0000" > "$T/wrong-pin"
rm -f "$T/pin-tried"
scenario wrongpin "--pin $PIN" "-p $T/wrong-pin"
[ -f "$T/pin-tried" ] || { echo "FAIL no mark of the attempt"; fails=$((fails + 1)); }
# the same boot (the mark stays): pro1x-modemd restarted, no second attempt
scenario again "--pin $PIN" "-p $T/wrong-pin"

rm -f "$T/pin-tried"
scenario lowretries "--pin $PIN --retries 2" "-p $T/sim-pin"

rm -f "$T/pin-tried"
scenario nofile "--pin $PIN" "-p $T/no-such-file"

[ $fails -eq 0 ] && echo "run.sh: all scenarios passed" || echo "run.sh: $fails failed"
[ $fails -eq 0 ]
