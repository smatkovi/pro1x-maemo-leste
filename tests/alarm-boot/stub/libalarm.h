/*
 * The part of alarmd's libalarm.h that pro1x-alarmboot uses, for building
 * it on the PC (tests/alarm-boot/screens.sh): the same names and fields;
 * libalarm-stub.c answers from the environment instead of alarmd.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PRO1X_LIBALARM_STUB_H
#define PRO1X_LIBALARM_STUB_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef int32_t cookie_t;

enum {
	ALARM_ACTION_TYPE_NOP = 0,
	ALARM_ACTION_TYPE_SNOOZE = 1 << 0,
	ALARM_ACTION_TYPE_DISABLE = 1 << 5,
	ALARM_ACTION_WHEN_RESPONDED = 1 << 6,
};

enum {
	ALARM_EVENT_BOOT = 1 << 3,
	ALARM_EVENT_ACTDEAD = 1 << 4,
	ALARM_EVENT_DISABLED = 1 << 11,
};

typedef struct {
	unsigned flags;
	char *label;
} alarm_action_t;

typedef struct {
	cookie_t cookie;
	time_t trigger;
	char *title, *message;
	unsigned flags;
	time_t snooze_secs;
	size_t action_cnt;
	alarm_action_t *action_tab;
} alarm_event_t;

int alarm_action_is_button(const alarm_action_t *self);
const char *alarm_action_get_label(const alarm_action_t *self);
time_t alarm_event_get_trigger(const alarm_event_t *self);
const char *alarm_event_get_title(const alarm_event_t *self);
const char *alarm_event_get_message(const alarm_event_t *self);
void alarm_event_delete(alarm_event_t *self);
alarm_event_t *alarmd_event_get(cookie_t cookie);
cookie_t *alarmd_event_query(const time_t first, const time_t last,
			     int32_t flag_mask, int32_t flags, const char *appid);
int alarmd_ack_dialog(cookie_t cookie, int button);
#endif
