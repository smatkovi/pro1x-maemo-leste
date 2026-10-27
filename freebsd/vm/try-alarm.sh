#!/bin/sh
# try-alarm.sh [seconds]: in the FreeBSD test VM (vm.sh), as root, with
# the packages installed: an alarm through the whole chain. Hildon on a
# virtual screen as try-hildon.sh starts it (alarmd and systemui with the
# session), an alarm set as clock-ui sets one (libalarm, its application
# id, Snooze and Stop) due in SECONDS (30), a screenshot once it is due
# (/root/shots/alarm.xwd: systemui's alarm dialog), what alarmd's queue
# holds before and after; then everything ends and the alarm is deleted.
# RESPOND=snooze or stop: that button tapped (xdotool) once it rings,
# /root/shots/alarm-after.xwd and the queue again.
set -u
SECS=${1:-30}
RESPOND=${RESPOND:-}
SHOTS=/root/shots
mkdir -p "$SHOTS"
service dbus onestatus > /dev/null 2>&1 || service dbus onestart
export DISPLAY=:1 PRO1X_TRY=1
Xvfb :1 -screen 0 2160x1080x24 -noreset -nolisten tcp > /tmp/xvfb.log 2>&1 &
xvfb=$!
sleep 3
/usr/local/bin/pro1x-hidpi theme
daemon -f -o /tmp/session.log /usr/local/bin/hildon-session
sleep ${WAIT:-40}

# An alarm as clock-ui's QNewAlarmDialog makes it, at a time in seconds
cat > /tmp/alarmtest.c <<'EOF'
#include <libalarm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int
main(int argc, char **argv)
{
	cookie_t *list, *c;

	if (argc == 3 && !strcmp(argv[1], "add")) {
		alarm_event_t *e = alarm_event_create();
		alarm_action_t *a;
		cookie_t cookie;

		alarm_event_set_alarm_appid(e, "worldclock_alarmd_id");
		alarm_event_set_message(e, "try-alarm.sh");
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
	/* list: the clock's alarms, when and whether disabled */
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
cc -O -o /tmp/alarmtest /tmp/alarmtest.c -I/usr/local/include/alarmd \
    -L/usr/local/lib -Wl,-rpath,/usr/local/lib -lalarm || exit 1
echo "== running: $(pgrep -x alarmd > /dev/null && echo alarmd) $(pgrep -x systemui > /dev/null && echo systemui)"
cookie=$(/tmp/alarmtest add "$SECS")
echo "== alarm $cookie in ${SECS}s"
/tmp/alarmtest list
sleep $((SECS + ${RINGWAIT:-15}))
xwd -root -silent > "$SHOTS/alarm.xwd"
echo "== after it was due"
/tmp/alarmtest list
# the dialog's buttons at 2160x1080: Snooze left, Stop right, at the bottom
case $RESPOND in
snooze)	xdotool mousemove 540 950 click 1 ;;
stop)	xdotool mousemove 1620 950 click 1 ;;
esac
if [ -n "$RESPOND" ]; then
	sleep 10
	xwd -root -silent > "$SHOTS/alarm-after.xwd"
	echo "== after $RESPOND"
	/tmp/alarmtest list
fi
[ -n "$cookie" ] && /tmp/alarmtest del "$cookie"
# the session and everything it started (by the marker in their environment)
for p in $(ps -axo pid=); do
	if procstat -e "$p" 2> /dev/null | grep -q PRO1X_TRY=1; then
		kill "$p" 2> /dev/null
	fi
done
kill $xvfb 2> /dev/null
tail -20 /tmp/session.log
