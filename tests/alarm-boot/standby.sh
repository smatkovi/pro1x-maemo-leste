#!/bin/bash
# pro1x-standby where no mce runs (FreeBSD's way, without -M) on the PC:
# what it takes for the display as mce would, without the phone. An Xvfb
# of its own and a D-Bus daemon of its own as the system bus (their PIDs
# alone are stopped at the end); the time-out 2 s (-s 2: dimmed after 2 s
# without input, the stand-by screen 3 s later).
#
#   bash tests/alarm-boot/standby.sh        (KEEP=dir: its log there)
#
# Checked: it owns com.nokia.mce; the time-out brings the stand-by screen;
# the system UI's alarm_dialog_status 5 brings the desktop back and holds
# off the time-out while that sender's alarm shows; the power key
# (SIGUSR1) still goes to the stand-by screen meanwhile; a second sender's
# 5 (a snoozed alarm ringing again) brings the desktop back from it; the
# senders gone, the time-out again; 7 from the sender ends its hold;
# req_display_state_on brings the desktop back; get_display_status says
# the state; another request gets an error at once; a blanking pause holds
# while its requester stays on the bus, Leste's false ends it.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
T=$(mktemp -d)
XPID= DPID= SPID=
HOLD=()
cleanup() {
  for p in "${HOLD[@]}" $SPID $XPID $DPID; do kill "$p" 2> /dev/null || true; done
  [ -z "${KEEP:-}" ] || cp "$T/sb.log" "$KEEP/" 2> /dev/null || true
  rm -rf "$T"
}
trap cleanup EXIT

# shellcheck disable=SC2046
gcc -O2 -Wall -o "$T/pro1x-standby" "$REPO/tools/pro1x-standby/pro1x-standby.c" \
  $(pkg-config --cflags --libs x11 xext xi cairo pangocairo sqlite3 dbus-1) -lm

# a client that stays on the bus: emits alarm_dialog_status (N, or N,M: M
# 3 s later) or asks for a blanking pause (pause; pause,false: then ends it)
cat > "$T/holder.py" <<'EOF'
import sys, time
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib
bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
for i, what in enumerate(sys.argv[1].split(',')):
    if i:
        time.sleep(3)
    if what == 'pause':
        bus.call_sync('com.nokia.mce', '/com/nokia/mce/request', 'com.nokia.mce.request',
                      'req_display_blanking_pause', None, None, 0, 2000, None)
    elif what == 'false':
        bus.call_sync('com.nokia.mce', '/com/nokia/mce/request', 'com.nokia.mce.request',
                      'req_display_blanking_pause', GLib.Variant('(b)', (False,)), None,
                      0, 2000, None)
    else:
        bus.emit_signal(None, '/com/nokia/system_ui/signal', 'com.nokia.system_ui.signal',
                        'alarm_dialog_status', GLib.Variant('(u)', (int(what),)))
    bus.flush_sync(None)
time.sleep(3600)
EOF

cat > "$T/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>system</type><listen>unix:path=$T/bus</listen>
<policy context="default"><allow user="*"/><allow own="*"/>
<allow send_type="method_call"/><allow send_type="signal"/>
<allow send_type="method_return"/><allow send_type="error"/>
<allow receive_type="signal"/><allow eavesdrop="true"/></policy></busconfig>
EOF
dbus-daemon --config-file="$T/bus.conf" --nofork --nopidfile > /dev/null 2>&1 &
DPID=$!
for _ in $(seq 50); do [ -S "$T/bus" ] && break; sleep 0.1; done
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=$T/bus
unset DBUS_SESSION_BUS_ADDRESS

n=90
while [ -e /tmp/.X11-unix/X$n ] || [ -e /tmp/.X$n-lock ]; do n=$((n + 1)); done
export DISPLAY=:$n
Xvfb "$DISPLAY" -screen 0 1080x2160x24 -nolisten tcp > /dev/null 2>&1 &
XPID=$!
for _ in $(seq 50); do [ -e /tmp/.X11-unix/X$n ] && break; sleep 0.1; done

PRO1X_STANDBY_DEBUG=1 "$T/pro1x-standby" -s 2 -d 403 -P "$T/pid" 2> "$T/sb.log" &
SPID=$!

fails=0
mark=0
L=$T/sb.log
since() { tail -n +$((mark + 1)) "$L"; }
setmark() { mark=$(wc -l < "$L"); }
ok() { echo "ok   $1"; }
fail() { echo "FAIL $1"; fails=$((fails + 1)); }
has() { if since | grep -q -- "$2"; then ok "$1"; else fail "$1 (no '$2')"; fi; }
hasnt() { if since | grep -q -- "$2"; then fail "$1 ('$2')"; else ok "$1"; fi; }
hold() { python3 -I "$T/holder.py" "$1" & HOLD+=($!); }
mce() {
  timeout 3 gdbus call --system --dest com.nokia.mce --object-path /com/nokia/mce/request \
    --method "com.nokia.mce.request.$1" "${@:2}" 2>&1 || true
}

sleep 1
has "it takes com.nokia.mce (no mce runs)" "com.nokia.mce: the display's requests"
sleep 5.5
has "the time-out: the stand-by screen" "lpm"

setmark
hold 5
a1=${HOLD[-1]}
sleep 1.5
has "an alarm rings: the desktop back" "active"
sleep 6
hasnt "while it shows: no time-out" "dimmed"

setmark
kill -USR1 "$SPID"
sleep 1
has "the power key meanwhile: the stand-by screen" "lpm"

setmark
hold 5
a2=${HOLD[-1]}
sleep 1.5
has "a snoozed alarm rings again: the desktop back from the stand-by screen" "active"

setmark
kill "$a1" "$a2"
sleep 7
has "both senders gone: held no more" "a holder left"
has "then the time-out again" "lpm"

setmark
hold 5,7
sleep 1.5
has "another alarm: the desktop back" "active"
sleep 4.5
has "its 7: the time-out again" "dimmed"
kill "${HOLD[-1]}"
sleep 5

setmark
r=$(mce req_display_state_on)
sleep 1
has "req_display_state_on: the desktop back" "active"
r=$(mce get_display_status)
case $r in *"'on'"*) ok "get_display_status: $r" ;; *) fail "get_display_status: $r" ;; esac
t0=$(date +%s%N)
r=$(mce get_call_state)
t1=$(date +%s%N)
case $r in
  *UnknownMethod*) [ $(((t1 - t0) / 1000000)) -lt 1000 ] && ok "another request: an error at once" ||
      fail "another request: the error took $(((t1 - t0) / 1000000)) ms" ;;
  *) fail "another request: $r" ;;
esac

setmark
mce req_display_state_on > /dev/null
hold pause
p1=${HOLD[-1]}
sleep 1.5
has "a blanking pause" "blanking paused"
sleep 6
hasnt "while it lasts: no time-out" "dimmed"
setmark
kill "$p1"
sleep 3
has "its requester gone: the time-out again" "dimmed"
sleep 4

setmark
mce req_display_state_on > /dev/null
hold pause,false
sleep 1.5
has "a pause again" "blanking paused"
sleep 6
has "Leste's false ends it" "blanking pause ended"
has "then the time-out again" "dimmed"

kill -0 "$SPID" 2> /dev/null && ok "pro1x-standby still runs" || fail "pro1x-standby died"
echo ">> $fails failed"
[ "$fails" = 0 ]
