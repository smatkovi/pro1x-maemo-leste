#!/bin/sh
# settings.sh BINARY: pro1x-standby as mce for the display's settings (the
# control panel's Display: get_/set_display_timeout, _brightness,
# _inhibit_timout_mode), on a virtual screen (Xvfb) with a private bus as
# its system bus, where it owns com.nokia.mce. The requests are the
# applet's (dbus-send in its place); each check says ok or FAIL, the exit
# status is the number of failures. Needs Xvfb, dbus-daemon, dbus-send,
# xdotool. Linux or FreeBSD; no backlight there is fine (brightness is
# checked as it is kept and told).
set -u
BIN=${1:?usage: settings.sh BINARY}
T=$(mktemp -d "${TMPDIR:-/tmp}/standby-settings.XXXXXX")
D=:${DISPLAY_NO:-73}
fails=0

ok() { echo "ok   $*"; }
fail() { echo "FAIL $*"; fails=$((fails + 1)); }

cat > "$T/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <listen>unix:path=$T/bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow own="*"/>
    <allow send_destination="*"/>
    <allow receive_sender="*"/>
    <allow eavesdrop="true"/>
  </policy>
</busconfig>
EOF
dbus-daemon --config-file="$T/bus.conf" --nofork --nopidfile & bus=$!
Xvfb $D -screen 0 720x1440x24 -nolisten tcp > /dev/null 2>&1 & x=$!
DBUS_SYSTEM_BUS_ADDRESS=unix:path=$T/bus
export DBUS_SYSTEM_BUS_ADDRESS
unset DBUS_SESSION_BUS_ADDRESS
sleep 2

standby() {
	DISPLAY=$D PRO1X_STANDBY_DEBUG=1 "$BIN" -d 403 -s 30 -A 12s \
	    -C "$T/display" -P "$T/pid" 2>> "$T/log" & pid=$!
	sleep 1
}
stop() { kill $pid 2> /dev/null; wait $pid 2> /dev/null; }
mce() {
	dbus-send --system --print-reply --dest=com.nokia.mce \
	    /com/nokia/mce/request com.nokia.mce.request.$1 ${2:+"$2"} 2>&1
}
get() { mce "get_$1" | sed -n 's/^ *int32 //p'; }
input() { DISPLAY=$D xdotool mousemove_relative 1 1 > /dev/null 2>&1; }
mark() { echo "=== $*" >> "$T/log"; }
# what the log says after the last mark
since() { sed -n '/^=== '"$1"'$/,$p' "$T/log"; }

dbus-monitor --system "interface='com.nokia.mce.signal'" > "$T/signals" 2>&1 &
mon=$!
standby

v=$(get display_timeout)
[ "$v" = 30 ] && ok "time-out -s 30 before any setting" || fail "time-out: '$v', not 30"
v=$(get inhibit_timout_mode)
[ "$v" = 0 ] && ok "inhibit mode 0 at first" || fail "inhibit mode: '$v'"
v=$(get display_brightness)
[ -n "$v" ] && [ "$v" -ge 1 ] && [ "$v" -le 5 ] && ok "brightness level $v (1-5)" ||
    fail "brightness: '$v'"

# a short time-out, at once: dimmed 3 s after input, the stand-by screen
# 3 s later, the panel off at 12 s (-A) after the input
mark short
mce set_display_timeout int32:3 > /dev/null
input
sleep 13
grep -q '^display_dim_timeout 3$' "$T/display" && ok "time-out kept" ||
    fail "time-out not kept: $(cat "$T/display" 2> /dev/null)"
s=$(since short)
echo "$s" | grep -q ' dimmed$' && ok "dimmed after the new time-out" || fail "not dimmed"
echo "$s" | grep -q ' standby$' && ok "stand-by screen after it" || fail "no stand-by screen"
echo "$s" | grep -q ' panel off$' && ok "panel off (-A 12 s)" || fail "panel not off"
grep -q 'sig_display_timeout' "$T/signals" && ok "sig_display_timeout said" ||
    fail "no sig_display_timeout"

# back (the power key, as pro1x-powerkey), then "never"
kill -USR1 $pid
sleep 1
mark never
mce set_display_timeout int32:0 > /dev/null
input
sleep 6
s=$(since never)
echo "$s" | grep -q ' dimmed$' && fail "dimmed with the time-out 0" ||
    ok "time-out 0: never dimmed"

# always on (3), a time-out of 2 s: not dimmed, asked again
mce set_inhibit_timout_mode int32:3 > /dev/null
mce set_display_timeout int32:2 > /dev/null
mark inhibit
input
sleep 6
s=$(since inhibit)
echo "$s" | grep -q ' inhibited$' && ok "inhibit mode 3: held on" || fail "not inhibited"
echo "$s" | grep -q ' dimmed$' && fail "dimmed despite mode 3" || ok "not dimmed in mode 3"
mce set_inhibit_timout_mode int32:0 > /dev/null

# brightness and bad arguments
mce set_display_brightness int32:2 > /dev/null
v=$(get display_brightness)
[ "$v" = 2 ] && ok "brightness level 2" || fail "brightness: '$v'"
mce set_display_brightness int32:9 | grep -q InvalidArgs && ok "brightness 9 refused" ||
    fail "brightness 9 taken"
mce set_display_timeout int32:-1 | grep -q InvalidArgs && ok "time-out -1 refused" ||
    fail "time-out -1 taken"
mce get_something_else | grep -q UnknownMethod && ok "other requests: an error" ||
    fail "other request answered"

# kept over a restart, -s or not
mce set_display_timeout int32:120 > /dev/null
stop
standby
v=$(get display_timeout)
[ "$v" = 120 ] && ok "time-out 120 after a restart (not -s 30)" || fail "after restart: '$v'"
v=$(get display_brightness)
[ "$v" = 2 ] && ok "brightness 2 after a restart" || fail "brightness after restart: '$v'"

stop
kill $mon $x $bus 2> /dev/null
echo "--- kept:"; cat "$T/display"
echo "--- log:"; cat "$T/log"
rm -rf "$T"
echo "$fails failed"
exit $fails
