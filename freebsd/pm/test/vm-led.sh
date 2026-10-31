#!/bin/sh
# vm-led.sh BIN: pro1x-led (the FreeBSD/arm64 binary) in the test VM
# (freebsd/vm, as root) in its test mode (-T): the LED's duty cycles and the
# battery's state in files, Maemo's LED requests on the VM's system bus as
# hildon-home sends them to MCE (which is not there: the callers keep
# getting ServiceUnknown, the daemon only watches). Checks: off at the
# start, a notification's colour, its priority over charging, charging
# after it, off and back around a suspend (SIGUSR1, SIGUSR2), the battery
# low, req_led_disable and _enable, an unknown pattern ignored, off at the
# end.
set -u
BIN=${1:-/root/pro1x-led}
T=/tmp/led-test
rm -rf "$T"
mkdir -p "$T"
echo discharging > "$T/status"; echo 50 > "$T/capacity"; echo 0 > "$T/usb"
service dbus onestatus > /dev/null 2>&1 || service dbus onestart > /dev/null
fail=0
led() { echo "$(cat "$T/red" 2> /dev/null) $(cat "$T/green" 2> /dev/null) $(cat "$T/blue" 2> /dev/null)"; }
check() { if [ "$(led)" = "$2" ]; then echo "PASS: $1 ($2)"; else echo "FAIL: $1 ($(led), not $2)"; fail=1; fi; }
mce() {
	dbus-send --system --type=method_call --dest=com.nokia.mce \
	    /com/nokia/mce/request "com.nokia.mce.request.$1" ${2:+string:$2}
	sleep 1
}
"$BIN" -d -T "$T" > "$T/log" 2>&1 &
pid=$!
sleep 3
check "off at the start" "0 0 0"
mce req_led_pattern_activate PatternCommunicationSMS
check "an SMS: blue" "0 0 511"
echo charging > "$T/status"; echo 1 > "$T/usb"; kill -HUP $pid; sleep 1
check "charging under the SMS: still blue" "0 0 511"
mce req_led_pattern_deactivate PatternCommunicationSMS
check "the SMS seen: charging" "511 20 0"
kill -USR1 $pid; sleep 1
check "before a suspend: off" "0 0 0"
mce req_led_pattern_activate PatternCommunicationCall
check "a missed call while suspended: still off" "0 0 0"
kill -USR2 $pid; sleep 1
check "after it: the missed call" "0 0 511"
mce req_led_pattern_deactivate PatternCommunicationCall
echo discharging > "$T/status"; echo 8 > "$T/capacity"; echo 0 > "$T/usb"
kill -HUP $pid; sleep 1
check "the battery low: red" "511 0 0"
mce req_led_disable
check "req_led_disable: off" "0 0 0"
mce req_led_enable
check "req_led_enable: red again" "511 0 0"
echo 50 > "$T/capacity"; kill -HUP $pid; sleep 1
mce req_led_pattern_activate PatternDeviceOn
check "PatternDeviceOn: ignored, off" "0 0 0"
r=$(dbus-send --system --print-reply --dest=com.nokia.mce /com/nokia/mce/request \
    com.nokia.mce.request.req_led_pattern_activate string:PatternCommunicationIM 2>&1)
sleep 1
case $r in
*ServiceUnknown*) echo "PASS: the caller still gets ServiceUnknown (no MCE)" ;;
*) echo "FAIL: the caller got: $r"; fail=1 ;;
esac
check "the chat message: blue" "0 0 511"
kill $pid
wait $pid 2> /dev/null
check "off at the end" "0 0 0"
echo "== log"; cat "$T/log"
exit $fail
