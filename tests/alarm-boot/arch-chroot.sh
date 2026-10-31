#!/bin/bash
# Alarms on Arch, without the phone: in arch/pc-chroot.sh's chroot of Arch
# Linux ARM (qemu) with the Hildon packages and pro1x-alarmboot installed
# (arch/42-meego-look.sh PKGS=, then ONLY=pro1x-alarmboot), what Arch's
# hildon-session starts for alarms: a system bus of its own (the chroot's
# dbus-daemon with Arch's policy files, pro1x-alarm.conf among them),
# alarmd as the user alarm with librtc-realtime, the system UI, and
# pro1x-alarmboot after an alarm's start (a note as the boot menu writes
# one); an alarm as clock-ui sets one, due in 40 s. Then: the system UI's
# dialog rings, the N9 screen covers it, a tap on Stop (xdotool), the
# dialog goes, alarmd has the alarm switched off, "Switching off", a tap
# keeps it on. On an Xvfb of its own on the PC (its PID alone is stopped).
# The PC's rtc0 is /dev/null for the chroot meanwhile (alarmd sets the wake
# alarm there).
#
#   bash tests/alarm-boot/arch-chroot.sh       (sudo -n; the chroot mounted)
#   KEEP=dir  the logs and screenshots there
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
R=$(bash "$REPO/arch/pc-chroot.sh" root)
T=/tmp/pro1x-arch-alarm
H=$R$T
XPID=
fails=0
mounted=
log() { echo ">> $*"; }
check() {
	if grep -q -- "$2" "$3"; then echo "ok   $1"; else echo "FAIL $1 (no '$2' in ${3#"$R"})"; fails=$((fails + 1)); fi
}
# the chroot's processes (all of them: only this test runs some there)
kill_chroot() {
	sudo -n sh -c 'for p in /proc/[0-9]*; do
		[ "$(readlink "$p/root" 2> /dev/null)" = "$1" ] && kill -TERM "${p#/proc/}" 2> /dev/null
	done; true' sh "$R"
}
cleanup() {
	set +e
	kill_chroot
	sleep 1
	[ -z "$XPID" ] || kill "$XPID" 2> /dev/null
	! mountpoint -q "$R/dev/rtc0" || sudo -n umount "$R/dev/rtc0"
	for m in $mounted; do
		! mountpoint -q "$R/$m" || sudo -n umount "$R/$m" || sudo -n umount -l "$R/$m"
	done
}
trap cleanup EXIT

mountpoint -q "$R" || { echo "not mounted: arch/pc-chroot.sh mount"; exit 1; }
for m in proc sys dev dev/pts; do
	mountpoint -q "$R/$m" && continue
	sudo -n mount --bind "/$m" "$R/$m"
	sudo -n mount --make-rslave "$R/$m"
	mounted="$m $mounted"
done
[ ! -e "$R/dev/rtc0" ] || sudo -n mount --bind /dev/null "$R/dev/rtc0"

n=90
while [ -e /tmp/.X11-unix/X$n ] || [ -e /tmp/.X$n-lock ]; do n=$((n + 1)); done
Xvfb ":$n" -screen 0 2160x1080x24 -nolisten tcp > /dev/null 2>&1 &
XPID=$!
export DISPLAY=:$n

sudo -n rm -rf "$H"
sudo -n mkdir -p "$H/run"
sudo -n chown -R 1000:1000 "$H"
sudo -n chmod 700 "$H/run"
# an alarm as clock-ui makes one (Snooze; Stop switches a single one off)
cat > /tmp/pro1x-arch-alarmtest.c <<'EOF'
#include <libalarm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "add")) {
		alarm_event_t *e = alarm_event_create();
		alarm_action_t *a;
		cookie_t c;

		alarm_event_set_alarm_appid(e, "worldclock_alarmd_id");
		alarm_event_set_message(e, "Arch alarm");
		alarm_event_set_attr_string(e, "textdomain", "osso-clock");
		a = alarm_event_add_actions(e, 1);
		alarm_action_set_label(a, "cloc_bd_alarm_notification_snooze");
		a->flags = ALARM_ACTION_WHEN_RESPONDED | ALARM_ACTION_TYPE_SNOOZE;
		a = alarm_event_add_actions(e, 1);
		alarm_action_set_label(a, "cloc_bd_stop");
		a->flags = ALARM_ACTION_WHEN_RESPONDED | ALARM_ACTION_TYPE_DISABLE;
		e->flags |= ALARM_EVENT_ACTDEAD | ALARM_EVENT_SHOW_ICON | ALARM_EVENT_POSTPONE_DELAYED;
		e->alarm_time = time(NULL) + atoi(argv[2]);
		c = alarmd_event_add(e);
		printf("%ld\n", (long)c);
		return c > 0 ? 0 : 1;
	}
	cookie_t *l = alarmd_event_query(0, 0, 0, 0, "worldclock_alarmd_id");
	for (cookie_t *c = l; c && *c; c++) {
		alarm_event_t *e = alarmd_event_get(*c);
		if (e) {
			printf("%ld in %lds%s\n", (long)*c, (long)(e->trigger - time(NULL)),
			       e->flags & ALARM_EVENT_DISABLED ? " disabled" : "");
			alarm_event_delete(e);
		}
	}
	return 0;
}
EOF
sudo -n cp /tmp/pro1x-arch-alarmtest.c "$H/alarmtest.c"
rm -f /tmp/pro1x-arch-alarmtest.c
sudo -n chroot "$R" gcc -O -o "$T/alarmtest" "$T/alarmtest.c" -I/usr/include/alarmd -lalarm

as_alarm() {
	sudo -n chroot --userspec=1000:1000 "$R" /usr/bin/env -i PATH=/usr/bin HOME=/home/alarm \
	    LANG=en_GB.UTF-8 DISPLAY="$DISPLAY" XDG_RUNTIME_DIR="$T/run" \
	    DBUS_SYSTEM_BUS_ADDRESS="unix:path=$T/bus" "$@"
}
# alarmd's queue empty (alarms of an earlier run would ring at once)
sudo -n rm -f "$R"/var/cache/alarmd/alarm_queue.ini*
log "the system bus (Arch's policy), alarmd as alarm, the system UI"
sudo -n chroot "$R" /usr/bin/dbus-daemon --system --nofork --nopidfile \
    --address="unix:path=$T/bus" > "$H/bus.log" 2>&1 &
for _ in $(seq 100); do [ -S "$H/bus" ] && break; sleep 0.2; done
sudo -n chmod 666 "$H/bus"
# (with a session bus, as in the session: alarmd also talks to the status
# bar on it)
as_alarm dbus-run-session -- env LD_PRELOAD=/usr/lib/pro1x/librtc-realtime.so \
    /usr/bin/alarmd -l stderr -L DEBUG > "$H/alarmd.log" 2>&1 &
as_alarm dbus-run-session -- env G_MESSAGES_DEBUG=all /usr/bin/systemui > "$H/systemui.log" 2>&1 &
sleep 25
# the desktop is up (hildon-session sends it once hildon-desktop started):
# alarmd shows alarms only then
as_alarm dbus-send --system --type=signal /com/nokia/HildonDesktop/ready com.nokia.HildonDesktop.ready
as_alarm dbus-send --system --type=signal /com/nokia/startup/signal com.nokia.startup.signal.init_done
as_alarm dbus-send --system --print-reply --dest=org.freedesktop.DBus / \
    org.freedesktop.DBus.ListNames > "$H/names.txt" 2>&1 || true
if as_alarm dbus-send --system --print-reply --dest=org.freedesktop.DBus / \
    org.freedesktop.DBus.NameHasOwner string:com.nokia.alarmd | grep -q "true"; then
	echo "ok   alarmd owns com.nokia.alarmd as the user alarm (pro1x-alarm.conf)"
else
	echo "FAIL alarmd has no name"; fails=$((fails + 1))
fi
now=$(date +%s)
printf 'system=arch\nrtc_alarm=%s\ndue=%s\n' "$now" $((now + 40)) | sudo -n tee "$H/note" > /dev/null
as_alarm /usr/bin/pro1x-alarmboot -f "$T/note" -s arch -v -d 403 > /dev/null 2> "$H/alarmboot.log" &
cookie=$(as_alarm "$T/alarmtest" add 40) || { echo "FAIL no alarm added"; tail -5 "$H/alarmd.log"; exit 1; }
log "alarm $cookie in 40 s"
for _ in $(seq 120); do grep -q "N9 screen over" "$H/alarmboot.log" && break; sleep 1; done
sleep 4
check "the system UI's dialog rings: the N9 screen over it" "the N9 screen over alarm $cookie" "$H/alarmboot.log"
import -window root "$H/ring.png" 2> /dev/null || true
xdotool mousemove 1080 750 click 1
for _ in $(seq 60); do grep -q "switching off in" "$H/alarmboot.log" && break; sleep 1; done
check "Stop: face_down taken by the system UI, its dialog went" "alarm dialog status 7" "$H/alarmboot.log"
check "alarmd got the Stop" "stop for alarm $cookie" "$H/alarmboot.log"
check "then switching off" "switching off in 10 s" "$H/alarmboot.log"
import -window root "$H/off.png" 2> /dev/null || true
xdotool mousemove 1080 300 click 1
sleep 4
check "a tap keeps it on" "staying on: kept on" "$H/alarmboot.log"
as_alarm "$T/alarmtest" list > "$H/queue.txt" 2>&1 || true
check "the alarm switched off in alarmd's queue" "$cookie in .* disabled" "$H/queue.txt"
if [ -n "${KEEP:-}" ]; then
	mkdir -p "$KEEP"
	sudo -n cp "$H"/*.log "$H"/*.png "$H"/*.txt "$KEEP/" 2> /dev/null || true
	sudo -n chown -R "$(id -u):$(id -g)" "$KEEP"
fi
sudo -n rm -rf "$H"
kill_chroot
sudo -n rm -f "$R"/var/cache/alarmd/alarm_queue.ini*
echo ">> $fails failed"
[ "$fails" = 0 ]
