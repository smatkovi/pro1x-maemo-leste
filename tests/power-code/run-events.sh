#!/bin/bash
# tests/power-code/run-events.sh BIN DISPLAYNUM PORT ON_S OFF_S LABEL
# pro1x-events under Xvfb with private session and "system" buses and a
# local server; the wake-ups (context switches of all its threads) and the
# HTTP requests while the display is on, then after mce's "off".
# (docs/power-management.md, "Code-level power fixes")
#
# Everything of the run below RUNS (default /tmp/pro1x-power-code/runs):
# HOME, XDG_* and XDG_RUNTIME_DIR are set before the buses start, so that
# the gconfd they start writes there and not into the PC's own GConf
# (~/.config/gconf); Xvfb -noreset keeps the root window's property when
# xprop disconnects; GLib's netlink network monitor (the fake system bus
# has no NetworkManager); server.py stands in for Mastodon (an ETag, 304)
# and Open-Meteo (weather.json). Two runs side by side need their own
# DISPLAYNUM and PORT.
set -u
BIN=$1 N=$2 PORT=$3 ON=$4 OFF=$5 LABEL=$6
RUNS=${RUNS:-/tmp/pro1x-power-code/runs}; mkdir -p "$RUNS"
RUN=$(mktemp -d "$RUNS/$LABEL.XXXX")
HERE=$(cd "$(dirname "$0")" && pwd)
WEATHER=${WEATHER:-$HERE/weather.json}
H=$RUN/home; mkdir -p "$H/.config/gconf" "$H/.cache"
REQ=$RUN/requests.log; : > "$REQ"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT
mkdir -m 700 -p $RUN/rt; export XDG_RUNTIME_DIR=$RUN/rt HOME=$H XDG_CONFIG_HOME=$H/.config XDG_CACHE_HOME=$H/.cache XDG_DATA_HOME=$H/.local/share
Xvfb :$N -screen 0 540x960x24 -nolisten tcp -noreset > "$RUN/xvfb.log" 2>&1 & PIDS+=($!)
SES=$(dbus-daemon --session --fork --print-address=1 --print-pid=3 3> "$RUN/ses.pid")
PIDS+=($(cat "$RUN/ses.pid"))
SYS=$(dbus-daemon --session --fork --print-address=1 --print-pid=3 3> "$RUN/sys.pid")
PIDS+=($(cat "$RUN/sys.pid"))
export GIO_USE_NETWORK_MONITOR=netlink DISPLAY=:$N DBUS_SESSION_BUS_ADDRESS=$SES DBUS_SYSTEM_BUS_ADDRESS=$SYS
for i in $(seq 50); do xprop -root > /dev/null 2>&1 && break; sleep 0.2; done
xprop -root -f _HILDON_MEEGO_LOOK 32c -set _HILDON_MEEGO_LOOK 1
xprop -root _HILDON_MEEGO_LOOK > "$RUN/look.txt"
g() { gconftool-2 -s -t "$1" "/apps/pro1x/events/$2" "$3"; }
g string mastodon_server "http://127.0.0.1:$PORT"; g string mastodon_token test
g string weather_url "http://127.0.0.1:$PORT/v1/forecast"; g string weather_place Wien
g float weather_latitude 48.2082; g float weather_longitude 16.3738
g bool show_calendar false
python3 -I "$HERE/server.py" "$PORT" "$REQ" "$WEATHER" & PIDS+=($!)
sleep 1
"$BIN" > "$RUN/prog.log" 2>&1 & P=$!; PIDS+=($P)
GC=$(dbus-send --print-reply=literal --dest=org.freedesktop.DBus / org.freedesktop.DBus.GetConnectionUnixProcessID string:org.gnome.GConf 2>/dev/null | awk '{print $2}')
[ -n "$GC" ] && PIDS+=($GC)
cs() { local s=0 v; for f in /proc/$P/task/*/status; do v=$(awk '/^voluntary_ctxt_switches/{print $2}' "$f" 2>/dev/null); s=$((s + ${v:-0})); done; echo $s; }
rq() { awk -v a="$1" -v b="$2" '$1>=a && $1<b' "$REQ" | wc -l; }
bursts() { awk -v a="$1" -v b="$2" '$1>=a && $1<b {if ($1-l>10) n++; l=$1} END{print n+0}' "$REQ"; }
sleep 15
t0=$(date +%s.%N); c0=$(cs); sleep "$ON"; t1=$(date +%s.%N); c1=$(cs)
DBUS_SESSION_BUS_ADDRESS="$SYS" dbus-send --session --type=signal /com/nokia/mce/signal com.nokia.mce.signal.display_status_ind string:off
sleep 5
t2=$(date +%s.%N); c2=$(cs); sleep "$OFF"; t3=$(date +%s.%N); c3=$(cs)
echo "$LABEL on: ${ON}s wakeups=$((c1-c0)) requests=$(rq $t0 $t1) bursts=$(bursts $t0 $t1) | off: ${OFF}s wakeups=$((c3-c2)) requests=$(rq $t2 $t3) bursts=$(bursts $t2 $t3) | run $RUN"
