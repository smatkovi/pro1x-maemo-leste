/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pro1x-autosleep's policy (policy.c): when the phone may suspend, and how
 * long it stays up after a suspend ended. No system calls: the front end
 * (pro1x-autosleep.c) gathers the state, the host test
 * (freebsd/pm/test/autosleep_test.c) makes it up.
 */

#ifndef _PRO1X_AUTOSLEEP_POLICY_H_
#define	_PRO1X_AUTOSLEEP_POLICY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define	AS_MAXWAKE	8

struct as_state {
	uint64_t	now_ms;		/* a monotonic clock */
	int		display;	/* the panel: 1 on, 0 off, -1 unknown */
	int		lpm;		/* the stand-by screen (low power mode) */
	int		acline;		/* external power: 1, 0, -1 unknown */
	bool		audio;		/* a stream, a voice session, FM */
	bool		call;		/* a call (pro1x-callaudio runs) */
	bool		usbnet;		/* the USB gadget's ue0 up */
	bool		inhibit;	/* someone asked (the inhibit file) */
	uint64_t	net_bytes;	/* all interfaces but lo0, in and out */
};

struct as_conf {
	unsigned	delay_s;	/* the display off this long first */
	unsigned	stay_s;		/* up after a wake-up source's interrupt */
	unsigned	settle_s;	/* up after any other interrupt */
	unsigned	net_bps;	/* more than this is a transfer */
	bool		in_standby;	/* also while the stand-by screen shows */
	bool		on_power;	/* also on a charger or a PC's cable */
	unsigned	nwake;
	unsigned	wake[AS_MAXWAKE];	/* the wake-up sources' INTIDs */
};

struct as_track {
	uint64_t	off_since;	/* the display off (or stand-by) since */
	uint64_t	awake_until;	/* the stay after a wake-up */
	uint64_t	net_at, net_bytes;	/* the last sample of the counters */
	unsigned	net_rate;	/* bytes a second between the last two */
};

/* the defaults (Leste's pro1x-autosuspend: Delay 30, StayAwake 45) */
void as_conf_init(struct as_conf *c);
/*
 * 0: suspend now; else the milliseconds until the next look, why says
 * what holds the phone up ("display on", "a call", ...)
 */
int as_decide(struct as_track *t, const struct as_state *s,
    const struct as_conf *c, char *why, size_t len);
/*
 * A suspend ended: what ended it (hw.pro1x_pm.last), the stay that
 * follows. The INTID that ended it in *intid (0: the deadline or a
 * wake-up source's call; -1 in the result: not suspended at all).
 */
enum { AS_END_DEADLINE, AS_END_WAKE, AS_END_OTHER, AS_END_FAILED };
int as_after(struct as_track *t, const char *last, const struct as_conf *c,
    uint64_t now_ms, unsigned *intid, unsigned *secs_ms);

#endif /* _PRO1X_AUTOSLEEP_POLICY_H_ */
