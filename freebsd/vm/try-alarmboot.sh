#!/bin/sh
# try-alarmboot.sh [SRC]: in the FreeBSD test VM (vm.sh), as root, with the
# packages installed: pro1x-alarmboot (built here from SRC, default
# /tmp/pro1x-alarmboot.c, the repository's tools/pro1x-alarmboot) over the
# system UI's real alarm dialog. Hildon on a virtual screen as
# try-alarm.sh starts it; a note as rc.d/pro1x_alarmboot writes one; an
# alarm as clock-ui sets one, due in 30 s, answered on the N9 screen with
# Stop (xdotool); then a second run, its alarm answered with Snooze. Each
# time screenshots (/root/shots/alarmboot-*.xwd: the N9 screen over the
# dialog, then "Switching off", which a tap keeps from switching the VM
# off), the program's log and alarmd's queue. Then everything ends and the
# alarms are deleted. RUNS="snooze stop" (default "stop snooze"): which, in
# order.
set -u
SRC=${1:-/tmp/pro1x-alarmboot.c}
SHOTS=/root/shots
mkdir -p "$SHOTS"
I=/usr/local/include L=/usr/local/lib
cc -O2 -o /tmp/pro1x-alarmboot "$SRC" -I$I/gtk-2.0 -I$L/gtk-2.0/include \
    -I$I/glib-2.0 -I$L/glib-2.0/include -I$I/pango-1.0 -I$I/cairo \
    -I$I/atk-1.0 -I$I/gdk-pixbuf-2.0 -I$I/harfbuzz -I$I/freetype2 \
    -I$I/alarmd -I$I -L$L -Wl,-rpath,$L -lgtk-x11-2.0 -lgdk-x11-2.0 \
    -lpangocairo-1.0 -lpango-1.0 -lcairo -lgio-2.0 -lgobject-2.0 \
    -lglib-2.0 -lintl -lalarm -lpulse-simple -lpulse -lX11 -lXext -lm || exit 1

service dbus onestatus > /dev/null 2>&1 || service dbus onestart
export DISPLAY=:1 PRO1X_TRY=1 XDG_RUNTIME_DIR=/tmp/alarmboot-run
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp > /tmp/xvfb.log 2>&1 &
xvfb=$!
sleep 3
/usr/local/bin/pro1x-hidpi theme
daemon -f -o /tmp/session.log /usr/local/bin/hildon-session
sleep ${WAIT:-40}

# an alarm as clock-ui's makes it (Snooze; Stop switches a single one off)
cat > /tmp/alarmboot-test.c <<'EOF'
#include <libalarm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int
main(int argc, char **argv)
{
	cookie_t *list, *c;

	if (argc == 4 && !strcmp(argv[1], "add")) {
		alarm_event_t *e = alarm_event_create();
		alarm_action_t *a;
		cookie_t cookie;

		alarm_event_set_alarm_appid(e, "worldclock_alarmd_id");
		alarm_event_set_message(e, argv[3]);
		alarm_event_set_attr_string(e, "textdomain", "osso-clock");
		a = alarm_event_add_actions(e, 1);
		alarm_action_set_label(a, "cloc_bd_alarm_notification_snooze");
		a->flags = ALARM_ACTION_WHEN_RESPONDED | ALARM_ACTION_TYPE_SNOOZE;
		a = alarm_event_add_actions(e, 1);
		alarm_action_set_label(a, "cloc_bd_stop");
		a->flags = ALARM_ACTION_WHEN_RESPONDED | ALARM_ACTION_TYPE_DISABLE;
		e->flags |= ALARM_EVENT_ACTDEAD | ALARM_EVENT_SHOW_ICON |
		    ALARM_EVENT_POSTPONE_DELAYED;
		e->alarm_time = time(NULL) + atoi(argv[2]);
		cookie = alarmd_event_add(e);
		alarm_event_delete(e);
		printf("%ld\n", (long)cookie);
		return cookie > 0 ? 0 : 1;
	}
	if (argc == 3 && !strcmp(argv[1], "del"))
		return alarmd_event_del(atol(argv[2])) == 0 ? 0 : 1;
	list = alarmd_event_query(0, 0, 0, 0, "worldclock_alarmd_id");
	for (c = list; c && *c; c++) {
		alarm_event_t *e = alarmd_event_get(*c);
		if (e) {
			printf("%ld at %ld (in %lds)%s\n", (long)*c,
			    (long)e->trigger, (long)(e->trigger - time(NULL)),
			    e->flags & ALARM_EVENT_DISABLED ? " disabled" : "");
			alarm_event_delete(e);
		}
	}
	free(list);
	return 0;
}
EOF
cc -O -o /tmp/alarmboot-test /tmp/alarmboot-test.c -I$I/alarmd -L$L \
    -Wl,-rpath,$L -lalarm || exit 1

# One run: the note, the program, the alarm; at 2160x1080 and 403 dpi the
# N9 screen's Snooze is at y 590, Stop at 750; "Switching off" comes 2 s
# after the dialog went and switches off 10 s later: tapped at once
run() {
	name=$1 y=$2
	now=$(date +%s)
	printf 'system=freebsd\nrtc_alarm=%s\ndue=%s\n' "$now" $((now + 30)) > /tmp/alarmboot-note
	/tmp/pro1x-alarmboot -f /tmp/alarmboot-note -s freebsd -v -d 403 \
	    2> /tmp/alarmboot-$name.log &
	cookie=$(/tmp/alarmboot-test add 30 "try-alarmboot $name")
	cookies="${cookies:-} $cookie"
	echo "== $name: alarm $cookie in 30 s"
	sleep $((30 + ${RINGWAIT:-12}))
	xwd -root -silent > "$SHOTS/alarmboot-$name-ring.xwd"
	xdotool mousemove 1080 "$y" click 1
	sleep ${TAPWAIT:-6}
	xwd -root -silent > "$SHOTS/alarmboot-$name-off.xwd"
	xdotool mousemove 1080 300 click 1
	sleep 2
	echo "== $name: after the tap"
	/tmp/alarmboot-test list
	sleep 2
	echo "== $name: its log"
	cat /tmp/alarmboot-$name.log
}
for r in ${RUNS:-stop snooze}; do
	case $r in
	stop) run stop 750 ;;
	snooze) run snooze 590 ;;
	esac
done

for c in ${cookies:-}; do /tmp/alarmboot-test del "$c"; done
for p in $(ps -axo pid=); do
	if procstat -e "$p" 2> /dev/null | grep -q PRO1X_TRY=1; then
		kill "$p" 2> /dev/null
	fi
done
kill $xvfb 2> /dev/null
tail -20 /tmp/session.log
