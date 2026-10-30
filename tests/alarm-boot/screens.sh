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
# The test: the alarm dialog's status 5 (rings) puts the N9 alarm screen
# over it with the alarm's text; a tap on Stop sends mce's face_up and
# face_down; status 7 (gone) then gives alarmd the Stop button (stub: "ack
# <cookie> 1") and the question comes 5 s later; "Keep on" ends it. A tap
# on Snooze with no status after it: the screen steps aside after 3 s.
# Status 6 (silent) takes the screen away; status 5 brings it back. With
# MeeGo (-s meego): timed's next_bootup_event 1 and then another value
# give the question, with nothing over timed's own screen. With OWN=1 also
# its own ringing (1.5 min). KEEP=dir keeps the logs and PNGs.
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
  $(pkg-config --cflags --libs gtk+-2.0 gio-2.0 libpulse-simple) -lm

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

  # 1: the dialog rings, Stop tapped, the dialog goes, the question, Keep on
  printf 'system=test\nrtc_alarm=1001\ndue=%s\n' "$(($(date +%s) - 20))" > "$T/note"
  STUB_ALARMD="77,-10,Wake up" "$T/pro1x-alarmboot" -f "$T/note" -s test -v -g 2160x1080 \
    "${args[@]}" 2> "$T/run1.log" &
  p=$!
  sleep 1.5
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
  sleep 5
  check "the question after 5 s" "asking whether to switch off" "$T/run1.log"
  snap 2160x1080 "$T/ask.png"
  tap "${ask_y[1]}"
  sleep 1
  check "Keep on: stays on" "staying on" "$T/run1.log"
  kill -0 "$p" 2> /dev/null && { echo "FAIL still running"; fails=$((fails + 1)); kill "$p"; }
  wait "$p" 2> /dev/null || true

  # 2: Snooze tapped, the dialog does not go: the screen steps aside;
  # status 6 and 5 before that
  printf 'system=test\nrtc_alarm=1002\ndue=%s\n' "$(($(date +%s) - 20))" > "$T/note"
  : > "$T/mce.log.2"
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
  sleep 6
  check "status 7 after that: the question" "asking whether to switch off" "$T/run2.log"
  tap "${ask_y[0]}"
  sleep 1
  check "Switch off" "switching off" "$T/run2.log"
  kill "$p" 2> /dev/null || true
  wait "$p" 2> /dev/null || true
  kill "$mon" 2> /dev/null || true

  # 3: the same alarm a second time: nothing
  printf 'system=test\nrtc_alarm=1002\ndue=%s\n' "$(date +%s)" > "$T/note"
  "$T/pro1x-alarmboot" -f "$T/note" -s test -v 2> "$T/run3.log" || true
  check "the same alarm again: nothing" "handled already" "$T/run3.log"
  # 5: the MeeGo system: timed's next_bootup_event 1 (its dialog rings,
  # nothing over it), then the next alarm's time: the question
  printf 'system=meego\nrtc_alarm=1005\ndue=%s\n' "$(($(date +%s) - 20))" > "$T/note"
  "$T/pro1x-alarmboot" -f "$T/note" -s meego -v -g 2160x1080 "${args[@]}" 2> "$T/run5.log" &
  p=$!
  sleep 1.5
  gdbus emit --system --object-path /com/nokia/time \
    --signal com.nokia.time.next_bootup_event "int32 1" > /dev/null
  sleep 1
  check "MeeGo: timed's dialog seen" "boot-up event 1" "$T/run5.log"
  if grep -q "N9 screen over" "$T/run5.log"; then echo "FAIL MeeGo: covered timed's own screen"; fails=$((fails + 1)); else echo "ok   MeeGo: timed's own screen not covered"; fi
  gdbus emit --system --object-path /com/nokia/time \
    --signal com.nokia.time.next_bootup_event "int32 $(($(date +%s) + 600))" > /dev/null
  sleep 6
  check "MeeGo: its end, the question" "asking whether to switch off" "$T/run5.log"
  tap "${ask_y[1]}"
  sleep 1
  check "MeeGo: Keep on" "staying on" "$T/run5.log"
  kill "$p" 2> /dev/null || true
  wait "$p" 2> /dev/null || true

  # 4 (OWN=1, 1.5 min): no dialog: it rings itself after 90 s with the
  # alarm's text, Snooze, the system UI's dialog after all (its own ringing
  # gives way), gone: the question
  if [ -n "${OWN:-}" ]; then
    printf 'system=test\nrtc_alarm=1004\ndue=%s\n' "$(($(date +%s) - 200))" > "$T/note"
    STUB_ALARMD="79,-150,Own ring" "$T/pro1x-alarmboot" -f "$T/note" -s test -v -g 2160x1080 \
      "${args[@]}" 2> "$T/run4.log" &
    p=$!
    sleep 93
    check "no dialog: rings itself" "ringing" "$T/run4.log"
    snap 2160x1080 "$T/own.png"
    tap "${ring_y[0]}"
    sleep 1
    check "own Snooze" "snoozed for 600 s" "$T/run4.log"
    status 5
    sleep 1
    check "the dialog after all: the N9 screen over it" "the N9 screen over alarm 79" "$T/run4.log"
    status 7
    sleep 6
    check "gone: the question" "asking whether to switch off" "$T/run4.log"
    kill "$p" 2> /dev/null || true
    wait "$p" 2> /dev/null || true
  fi
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
