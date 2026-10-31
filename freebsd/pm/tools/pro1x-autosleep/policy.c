/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pro1x-autosleep's policy, as Android's autosleep and Maemo's mce and
 * dsme decide it: the phone suspends when nobody looks at it (the display
 * off, or showing only the stand-by screen) and nothing needs the CPUs:
 * no sound being played or recorded, no call, no positions wanted (the
 * modem's GNSS engine running: an app records a track), no transfer, no
 * USB network, not on external power, nobody inhibiting it. After a suspend
 * that a wake-up source ended (the power key, the RTC alarm, the modem: a
 * call or a message, a cable) it stays up stay_s seconds so that what
 * woke it can be handled; after any other interrupt settle_s.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "policy.h"

void
as_conf_init(struct as_conf *c)
{
	memset(c, 0, sizeof(*c));
	c->delay_s = 30;
	c->stay_s = 30;
	c->settle_s = 2;
	c->net_bps = 4096;
	c->in_standby = true;
	c->on_power = false;
	/*
	 * the SPMI arbiter (power and volume keys, the RTC alarm, the
	 * charger): 30 s; the modem's GLINK edge and SMP2P (a call, a
	 * message, but also its indications: a cell change, the network's
	 * time): 10 s, a call then holds it up by itself (pro1x-callaudio)
	 */
	(void)as_parse_wake(c, "215:30 100:10 102:10");
}

int
as_parse_wake(struct as_conf *c, const char *s)
{
	unsigned n = 0, intid, secs;
	char *end;

	while (*s != '\0') {
		while (*s == ' ' || *s == ',')
			s++;
		if (*s == '\0')
			break;
		intid = (unsigned)strtoul(s, &end, 10);
		if (end == s || n == AS_MAXWAKE)
			return (-1);
		secs = c->stay_s;
		if (*end == ':') {
			s = end + 1;
			secs = (unsigned)strtoul(s, &end, 10);
			if (end == s)
				return (-1);
		}
		c->wake[n] = intid;
		c->wake_s[n++] = secs;
		s = end;
	}
	c->nwake = n;
	return (0);
}

static int
busy(char *why, size_t len, const char *what, int wait_ms)
{
	snprintf(why, len, "%s", what);
	return (wait_ms);
}

int
as_decide(struct as_track *t, const struct as_state *s,
    const struct as_conf *c, char *why, size_t len)
{
	uint64_t dt, due;
	bool off;

	/* the transfer rate since the last look */
	if (t->net_at != 0 && s->now_ms > t->net_at &&
	    s->net_bytes >= t->net_bytes) {
		dt = s->now_ms - t->net_at;
		t->net_rate = (unsigned)((s->net_bytes - t->net_bytes) * 1000 /
		    dt);
	} else
		t->net_rate = 0;
	t->net_at = s->now_ms;
	t->net_bytes = s->net_bytes;

	off = s->display == 0 || (s->display == 1 && s->lpm == 1 &&
	    c->in_standby);
	if (!off) {
		t->off_since = 0;
		return (busy(why, len, s->display < 0 ? "no display state" :
		    s->lpm == 1 ? "the stand-by screen" : "display on", 10000));
	}
	if (t->off_since == 0)
		t->off_since = s->now_ms;
	if (s->call)
		return (busy(why, len, "a call", 5000));
	if (s->audio)
		return (busy(why, len, "sound", 5000));
	/*
	 * the positions come every second (QMI LOC over GLINK): each would
	 * end a suspend, which then only churns; the N9 stayed up too
	 */
	if (s->gnss)
		return (busy(why, len, "positions", 10000));
	if (s->inhibit)
		return (busy(why, len, "inhibited", 5000));
	if (s->usbnet)
		return (busy(why, len, "USB network", 10000));
	if (s->acline == 1 && !c->on_power)
		return (busy(why, len, "external power", 10000));
	if (t->net_rate > c->net_bps)
		return (busy(why, len, "a transfer", 5000));
	due = t->off_since + (uint64_t)c->delay_s * 1000;
	if (t->awake_until > due)
		due = t->awake_until;
	if (s->now_ms < due) {
		snprintf(why, len, "%s", t->awake_until > t->off_since +
		    (uint64_t)c->delay_s * 1000 ? "up after a suspend" :
		    "the display just went off");
		return ((int)(due - s->now_ms));
	}
	snprintf(why, len, "idle");
	return (0);
}

/*
 * hw.pro1x_pm.last: "suspended 12.345 s in 3 epochs, ended by INTID 215
 * (a device: the power key); ..." or "... ended by a wake-up (the RTC
 * alarm); ..." or "... ended by the deadline 0; ..." or "not suspended:
 * ..."
 */
int
as_after(struct as_track *t, const char *last, const struct as_conf *c,
    uint64_t now_ms, unsigned *intid, unsigned *ms)
{
	const char *p;
	unsigned i, n = 0, sec = 0, frac = 0, stay = c->stay_s;
	int how;

	*intid = 0;
	*ms = 0;
	if (sscanf(last, "suspended %u.%u s", &sec, &frac) == 2)
		*ms = sec * 1000 + frac;
	if (strncmp(last, "suspended", 9) != 0) {
		t->awake_until = now_ms + 60000;
		return (AS_END_FAILED);
	}
	if ((p = strstr(last, "ended by INTID ")) != NULL &&
	    sscanf(p, "ended by INTID %u", &n) == 1) {
		*intid = n;
		how = AS_END_OTHER;
		stay = c->settle_s;
		for (i = 0; i < c->nwake; i++)
			if (c->wake[i] == n) {
				how = AS_END_WAKE;
				stay = c->wake_s[i];
			}
	} else if (strstr(last, "ended by a wake-up") != NULL)
		how = AS_END_WAKE;
	else
		/* our deadline: an alarm's time (pro1x-suspend's) */
		how = AS_END_DEADLINE;
	t->awake_until = now_ms + (uint64_t)stay * 1000;
	return (how);
}
