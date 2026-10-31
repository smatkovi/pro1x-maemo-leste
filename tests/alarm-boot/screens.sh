#!/bin/bash
# pro1x-alarmboot's screens on the PC, and its answers to the system UI's
# alarm dialog, without the phone: the program built for the PC with
# stub/libalarm-stub.c in place of alarmd, an Xvfb of its own (its PID
# alone is stopped at the end), a D-Bus daemon of its own as the system bus.
#
#   bash tests/alarm-boot/screens.sh shots    the screens as PNGs in OUT
#   bash tests/alarm-boot/screens.sh test     the answers (below)
#   bash tests/alarm-boot/screens.sh          both
#
#   OUT=/dir      where the PNGs go (default ~/ps/pro1x-screens)
#   FONTS="dir.." font directories instead of the PC's (e.g. Leste's
#                 /usr/share/fonts/truetype/open-sans and dejavu, as on the
#                 phone)
#   THEME=dir     a blanco theme (the N9's button pictures), as -t
#   TAG=name      added to the PNGs' names
#
# The test (the N9's way after the alarm, the default): the alarm
# dialog's status 5 (rings) puts the N9 alarm screen over it with the
# alarm's text; a tap on Stop sends mce's face_up and face_down; status 7
# (gone) then gives alarmd the Stop button (stub: "ack <cookie> 1") and
# "Switching off" comes 2 s later; a tap keeps the phone on. A tap on
# Snooze with no status after it: the screen steps aside after 3 s; status
# 6 (silent) takes the screen away, 5 brings it back; with no alarm to come
# it then switches off (dsme is not there: an error). Input elsewhere after
# the alarm, or mce's display going off (the power key), keeps it on.
# MeeGo (-s meego): timed's next_bootup_event 1, then the next alarm's
# time: nothing over timed's own screen, and without that alarm's RTC
# wake-up (the PC's is not set) it stays on after 15 s. Asking (-Q): the
# question, Keep on. With OWN=1 also its own ringing and the automatic
# snooze after 50 s (3 min more). KEEP=dir keeps the logs and PNGs.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
OUT=${OUT:-$HOME/ps/pro1x-screens}
TAG=${TAG:+-$TAG}
what=${1:-all}
T=$(mktemp -d)
XPID= DPID=
cleanup() {
  [ -z "$XPID" ] || kill "$XPID" 2> /dev/null || true
  [ -z "$DPID" ] || kill "$DPID" 2> /dev/null || true
  rm -rf "$T"
}
trap cleanup EXIT

# shellcheck disable=SC2046
gcc -O2 -Wall -Wno-deprecated-declarations -I "$REPO/tests/alarm-boot/stub" \
  -o "$T/pro1x-alarmboot" "$REPO/tools/pro1x-alarmboot/pro1x-alarmboot.c" \
  "$REPO/tests/alarm-boot/stub/libalarm-stub.c" \
  $(pkg-config --cflags --libs gtk+-2.0 gio-2.0 gio-unix-2.0 libpulse-simple x11 xi) -lm

# X's idle time (SYNC's IDLETIME), in ms: what pro1x-standby times out by
cat > "$T/idle.c" <<'EOF'
#include <X11/Xlib.h>
#include <X11/extensions/sync.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
	Display *d = XOpenDisplay(NULL);
	int ev, er, ma, mi, n;
	XSyncSystemCounter *l;
	XSyncValue v;

	if (!d || !XSyncQueryExtension(d, &ev, &er) || !XSyncInitialize(d, &ma, &mi))
		return 1;
	l = XSyncListSystemCounters(d, &n);
	for (int i = 0; i < n; i++)
		if (!strcmp(l[i].name, "IDLETIME") && XSyncQueryCounter(d, l[i].counter, &v))
			printf("%lld\n", ((long long)XSyncValueHigh32(v) << 32) | (unsigned)XSyncValueLow32(v));
	return 0;
}
EOF
gcc -O -o "$T/idle" "$T/idle.c" -lX11 -lXext

if [ -n "${FONTS:-}" ]; then
  {
    echo '<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd"><fontconfig>'
    for d in $FONTS; do echo "<dir>$d</dir>"; done
    echo "<cachedir>$T/fc-cache</cachedir></fontconfig>"
  } > "$T/fonts.conf"
  export FONTCONFIG_FILE=$T/fonts.conf
fi

n=90
while [ -e /tmp/.X11-unix/X$n ] || [ -e /tmp/.X$n-lock ]; do n=$((n + 1)); done
export DISPLAY=:$n
Xvfb "$DISPLAY" -screen 0 2160x2160x24 -nolisten tcp > "$T/xvfb.log" 2>&1 &
XPID=$!
for _ in $(seq 50); do [ -e /tmp/.X11-unix/X$n ] && break; sleep 0.1; done

# the Pro1X: 2160 x 1080, 403 dpi
args=(-d 403 ${THEME:+-t "$THEME"})
snap() { import -window root -crop "$1+0+0" +repage "$2"; }

shots() {
  local s l g o p
  mkdir -p "$OUT"
  for s in ring ask off switching; do
    for l in en de; do
      for g in 2160x1080 1080x2160; do
        o=landscape; [ "$g" = 2160x1080 ] || o=portrait
        "$T/pro1x-alarmboot" -D "$s" -L "$l" -g "$g" "${args[@]}" > /dev/null &
        p=$!
        sleep 1.5
        snap "$g" "$OUT/alarmboot-$s-$l-$o$TAG.png"
        kill "$p" 2> /dev/null || true
        wait "$p" 2> /dev/null || true
      done
    done
  done
  echo ">> screens in $OUT (alarmboot-*$TAG.png)"
}

fails=0
check() {
  if grep -q -- "$2" "$3"; then echo "ok   $1"; else echo "FAIL $1 (no '$2' in $3)"; fails=$((fails + 1)); fi
}
status() {
  gdbus emit --system --object-path /com/nokia/system_ui/signal \
    --signal com.nokia.system_ui.signal.alarm_dialog_status "uint32 $1" > /dev/null
}
# the middle of the upper (0) or lower (1) button of the 2160 x 1080 ring
# screen at 403 dpi (408 x 64 N9 px at 1.587 per px, centred)
tap() { xdotool mousemove 1080 "$1" click 1; }
ring_y=(590 750) ask_y=(585 712)

test_answers() {
  cat > "$T/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>system</type><listen>unix:path=$T/bus</listen>
<policy context="default"><allow user="*"/><allow own="*"/>
<allow send_type="method_call"/><allow send_type="signal"/>
<allow receive_type="signal"/><allow eavesdrop="true"/></policy></busconfig>
EOF
  dbus-daemon --config-file="$T/bus.conf" --nofork --nopidfile > /dev/null 2>&1 &
  DPID=$!
  for _ in $(seq 50); do [ -S "$T/bus" ] && break; sleep 0.1; done
  export DBUS_SYSTEM_BUS_ADDRESS=unix:path=$T/bus
  export XDG_RUNTIME_DIR=$T/run PULSE_SERVER=unix:$T/no-pulse
  mkdir -p "$T/run"
  chmod 700 "$T/run"
  dbus-monitor --system "type='signal',interface='com.nokia.mce.signal'" > "$T/mce.log" 2>&1 &
  local mon=$! p

  # 1: the dialog rings, Stop tapped, the dialog goes, "Switching off", a tap
  printf 'system=test\nrtc_alarm=1001\ndue=%s\n' "$(($(date +%s) - 20))" > "$T/note"
  STUB_ALARMD="77,-10,Wake up" "$T/pro1x-alarmboot" -f "$T/note" -s test -v -g 2160x1080 \
    "${args[@]}" 2> "$T/run1.log" &
  p=$!
  # before the alarm rings, no input: X's idle time kept low already (no
  # stand-by screen before the dialog)
  sleep 5
  idle=$("$T/idle" || echo 99999)
  if [ "${idle:-99999}" -lt 2600 ]; then echo "ok   before the alarm: X's idle time low ($idle ms)"; else echo "FAIL before the alarm: X's idle time $idle ms"; fails=$((fails + 1)); fi
  status 5
  sleep 1.5
  snap 2160x1080 "$T/over.png"
  check "status 5: the N9 screen over the dialog" "the N9 screen over alarm 77" "$T/run1.log"
  tap "${ring_y[1]}"
  sleep 1
  check "Stop: face_up sent" '"face_up"' "$T/mce.log"
  check "Stop: face_down sent" '"face_down"' "$T/mce.log"
  status 7
  sleep 1
  check "status 7: alarmd gets Stop (button 1)" "ack 77 1" "$T/run1.log"
  sleep 2
  check "then switching off, 10 s to keep it on" "switching off in 10 s" "$T/run1.log"
  snap 2160x1080 "$T/off.png"
  tap 300
  sleep 1
  check "a tap keeps it on" "staying on: kept on" "$T/run1.log"
  kill -0 "$p" 2> /dev/null && { echo "FAIL still running"; fails=$((fails + 1)); kill "$p"; }
  wait "$p" 2> /dev/null || true

  # 2: Snooze tapped, the dialog does not go: the screen steps aside;
  # status 6 and 5 before that; then gone: no alarm to come, off
  printf 'system=test\nrtc_alarm=1002\ndue=%s\n' "$(($(date +%s) - 20))" > "$T/note"
  STUB_ALARMD="78,-10," "$T/pro1x-alarmboot" -f "$T/note" -s test -v -g 2160x1080 \
    "${args[@]}" 2> "$T/run2.log" &
  p=$!
  sleep 1.5
  status 5
  sleep 1
  status 6
  sleep 1
  check "status 6: the screen goes" "alarm dialog status 6" "$T/run2.log"
  status 5
  sleep 1
  tap "${ring_y[0]}"
  sleep 4
  check "no status after a tap: steps aside" "its own buttons from now on" "$T/run2.log"
  if grep -q "^ack" "$T/run2.log"; then echo "FAIL snooze answered alarmd itself"; fails=$((fails + 1)); else echo "ok   snooze leaves alarmd to the system UI"; fi
  status 7
  sleep 14
  check "status 7, no alarm to come: off after 10 s" "switching off$" "$T/run2.log"
  kill "$p" 2> /dev/null || true
  wait "$p" 2> /dev/null || true

  # 3: the same alarm a second time: nothing
  printf 'system=test\nrtc_alarm=1002\ndue=%s\n' "$(date +%s)" > "$T/note"
  "$T/pro1x-alarmboot" -f "$T/note" -s test -v 2> "$T/run3.log" || true
  check "the same alarm again: nothing" "handled already" "$T/run3.log"

  # 4: input elsewhere after the alarm (our screen away): stays on
  printf 'system=test\nrtc_alarm=1004\ndue=%s\n' "$(($(date +%s) - 20))" > "$T/note"
  STUB_ALARMD="80,-10,Used" "$T/pro1x-alarmboot" -f "$T/note" -s test -v -g 2160x1080 \
    "${args[@]}" 2> "$T/run4.log" &
  p=$!
  sleep 1.5
  status 5
  sleep 1
  tap "${ring_y[1]}"
  sleep 1
  status 7
  sleep 1
  xdotool mousemove 50 50 click 1
  sleep 2.5
  check "input after the alarm, not on its screens: stays on" "staying on: the phone was used" "$T/run4.log"
  kill "$p" 2> /dev/null || true
  wait "$p" 2> /dev/null || true

  # 5: the power key (mce: the display goes off) while switching off
  printf 'system=test\nrtc_alarm=1005\ndue=%s\n' "$(($(date +%s) - 20))" > "$T/note"
  STUB_ALARMD="81,-10,Power" "$T/pro1x-alarmboot" -f "$T/note" -s test -v -g 2160x1080 \
    "${args[@]}" 2> "$T/run5.log" &
  p=$!
  sleep 1.5
  status 5
  sleep 1
  tap "${ring_y[1]}"
  sleep 1
  status 7
  sleep 3
  # dimmed is no power key; the stand-by screen (pro1x-standby's "lpm",
  # FreeBSD's power key) is, as "off" (Leste's mce)
  gdbus emit --system --object-path /com/nokia/mce/signal \
    --signal com.nokia.mce.signal.display_status_ind "'dimmed'" > /dev/null
  sleep 1
  if grep -q "staying on" "$T/run5.log"; then echo "FAIL dimmed taken for the power key"; fails=$((fails + 1)); else echo "ok   dimmed: still switching off"; fi
  gdbus emit --system --object-path /com/nokia/mce/signal \
    --signal com.nokia.mce.signal.display_status_ind "'lpm'" > /dev/null
  sleep 2
  check "the power key (lpm) while switching off: stays on" "staying on: the power key" "$T/run5.log"
  kill "$p" 2> /dev/null || true
  wait "$p" 2> /dev/null || true

  # 6: the MeeGo system: timed's next_bootup_event 1 (its dialog rings,
  # nothing over it), then the next alarm's time: switching off, but that
  # alarm's RTC wake-up is not set (the PC's): stays on after 15 s
  printf 'system=meego\nrtc_alarm=1006\ndue=%s\n' "$(($(date +%s) - 20))" > "$T/note"
  "$T/pro1x-alarmboot" -f "$T/note" -s meego -v -g 2160x1080 "${args[@]}" 2> "$T/run6.log" &
  p=$!
  sleep 1.5
  gdbus emit --system --object-path /com/nokia/time \
    --signal com.nokia.time.next_bootup_event "int32 1" > /dev/null
  sleep 1
  check "MeeGo: timed's dialog seen" "boot-up event 1" "$T/run6.log"
  if grep -q "N9 screen over" "$T/run6.log"; then echo "FAIL MeeGo: covered timed's own screen"; fails=$((fails + 1)); else echo "ok   MeeGo: timed's own screen not covered"; fi
  gdbus emit --system --object-path /com/nokia/time \
    --signal com.nokia.time.next_bootup_event "int32 $(($(date +%s) + 600))" > /dev/null
  sleep 3
  check "MeeGo: its end, switching off" "switching off in 10 s" "$T/run6.log"
  # no input for 15 s, the screens up: X's idle time kept under 2 s (no
  # stand-by time-out while they show)
  sleep 15
  idle=$("$T/idle" || echo 99999)
  if [ "${idle:-99999}" -lt 2600 ]; then echo "ok   the screens keep X's idle time low ($idle ms)"; else echo "FAIL X's idle time $idle ms with the screens up"; fails=$((fails + 1)); fi
  sleep 11
  check "MeeGo: an alarm to come without its RTC wake-up: stays on" "wake-up for the next alarm is not set" "$T/run6.log"
  kill "$p" 2> /dev/null || true
  wait "$p" 2> /dev/null || true

  # 7: asking (-Q): the question, Keep on
  printf 'system=test\nrtc_alarm=1007\ndue=%s\n' "$(($(date +%s) - 20))" > "$T/note"
  STUB_ALARMD="82,-10,Ask" "$T/pro1x-alarmboot" -Q -f "$T/note" -s test -v -g 2160x1080 \
    "${args[@]}" 2> "$T/run7.log" &
  p=$!
  sleep 1.5
  status 5
  sleep 1
  tap "${ring_y[1]}"
  sleep 1
  status 7
  sleep 6
  check "asking: the question after 5 s" "asking whether to switch off" "$T/run7.log"
  snap 2160x1080 "$T/ask.png"
  tap "${ask_y[1]}"
  sleep 1
  check "asking: Keep on" "staying on" "$T/run7.log"
  kill "$p" 2> /dev/null || true
  wait "$p" 2> /dev/null || true

  # 8 (OWN=1, 1.5 min): no dialog: it rings itself after 90 s with the
  # alarm's text; Snooze goes to alarmd (it has the alarm), then switching
  # off; the system UI's dialog after all takes over; gone: switching off
  if [ -n "${OWN:-}" ]; then
    printf 'system=test\nrtc_alarm=1008\ndue=%s\n' "$(($(date +%s) - 200))" > "$T/note"
    STUB_ALARMD="79,-150,Own ring" "$T/pro1x-alarmboot" -f "$T/note" -s test -v -g 2160x1080 \
      "${args[@]}" 2> "$T/run8.log" &
    p=$!
    sleep 93
    check "no dialog: rings itself" "ringing" "$T/run8.log"
    snap 2160x1080 "$T/own.png"
    tap "${ring_y[0]}"
    sleep 1
    check "own Snooze: to alarmd" "ack 79 0" "$T/run8.log"
    sleep 2
    check "own Snooze: then switching off" "switching off in 10 s" "$T/run8.log"
    status 5
    sleep 1
    check "the dialog after all: switching off waits" "another alarm rings" "$T/run8.log"
    check "the dialog after all: the N9 screen over it" "the N9 screen over alarm 79" "$T/run8.log"
    # unanswered: the automatic snooze after 50 s
    sleep 52
    check "unanswered 50 s: snoozed by itself" "no answer: snoozing" "$T/run8.log"
    status 7
    sleep 3
    check "gone: switching off again" "switching off in 10 s" "$T/run8.log"
    kill "$p" 2> /dev/null || true
    wait "$p" 2> /dev/null || true
  fi
  kill "$mon" 2> /dev/null || true
  [ -z "${KEEP:-}" ] || cp "$T"/*.log "$T"/*.png "$KEEP/"
  echo ">> $fails failed"
  [ "$fails" = 0 ]
}

case $what in
shots) shots ;;
test) test_answers ;;
all) shots; test_answers ;;
*) sed -n '2,/^set /p' "$0" | sed '$d'; exit 1 ;;
esac
