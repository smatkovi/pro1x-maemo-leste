/*
 * alarmd for pro1x-alarmboot on the PC (tests/alarm-boot/screens.sh).
 *
 *   STUB_ALARMD=none            alarmd does not answer
 *   STUB_ALARMD=empty           it has no alarms (the default)
 *   STUB_ALARMD=<cookie>,<seconds from now>,<message>
 *                               one clock alarm (Snooze, Stop) at that time
 *
 * alarmd_ack_dialog prints "ack <cookie> <button>" on stderr.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libalarm.h"

static time_t started;

static const char *spec(void)
{
	const char *s = getenv("STUB_ALARMD");

	if (!started)
		started = time(NULL);
	return s ? s : "empty";
}

int alarm_action_is_button(const alarm_action_t *self)
{
	return self && self->label && (self->flags & ALARM_ACTION_WHEN_RESPONDED);
}

const char *alarm_action_get_label(const alarm_action_t *self)
{
	return self ? self->label : NULL;
}

time_t alarm_event_get_trigger(const alarm_event_t *self)
{
	return self->trigger;
}

const char *alarm_event_get_title(const alarm_event_t *self)
{
	return self->title;
}

const char *alarm_event_get_message(const alarm_event_t *self)
{
	return self->message;
}

void alarm_event_delete(alarm_event_t *self)
{
	if (!self)
		return;
	for (size_t i = 0; i < self->action_cnt; i++)
		free(self->action_tab[i].label);
	free(self->action_tab);
	free(self->title);
	free(self->message);
	free(self);
}

alarm_event_t *alarmd_event_get(cookie_t cookie)
{
	const char *s = spec();
	char msg[128] = "";
	long c = 0, off = 0;
	alarm_event_t *e;

	if (sscanf(s, "%ld,%ld,%127[^\n]", &c, &off, msg) < 2 || c != cookie)
		return NULL;
	e = calloc(1, sizeof(*e));
	e->cookie = cookie;
	e->trigger = started + off;
	e->message = strdup(msg);
	e->title = strdup("");
	e->flags = ALARM_EVENT_ACTDEAD;
	e->snooze_secs = 600;
	e->action_cnt = 2;
	e->action_tab = calloc(2, sizeof(alarm_action_t));
	e->action_tab[0].flags = ALARM_ACTION_WHEN_RESPONDED | ALARM_ACTION_TYPE_SNOOZE;
	e->action_tab[0].label = strdup("cloc_bd_alarm_notification_snooze");
	e->action_tab[1].flags = ALARM_ACTION_WHEN_RESPONDED | ALARM_ACTION_TYPE_NOP;
	e->action_tab[1].label = strdup("cloc_bd_stop");
	return e;
}

cookie_t *alarmd_event_query(const time_t first, const time_t last,
			     int32_t flag_mask, int32_t flags, const char *appid)
{
	const char *s = spec();
	cookie_t *v;
	long c = 0, off = 0;

	(void)flag_mask; (void)flags; (void)appid;
	if (!strcmp(s, "none"))
		return NULL;
	v = calloc(2, sizeof(cookie_t));
	if (sscanf(s, "%ld,%ld", &c, &off) == 2 && started + off >= first &&
	    started + off <= last)
		v[0] = (cookie_t)c;
	return v;
}

int alarmd_ack_dialog(cookie_t cookie, int button)
{
	fprintf(stderr, "ack %d %d\n", (int)cookie, button);
	return 0;
}
