/*
 * The host test of pro1x-autosleep's policy (pm/tools/pro1x-autosleep):
 *   cc -Wall -I freebsd/pm/tools/pro1x-autosleep -o /tmp/ast \
 *       freebsd/pm/test/autosleep_test.c freebsd/pm/tools/pro1x-autosleep/policy.c && /tmp/ast
 */
#include <stdio.h>
#include <string.h>

#include "policy.h"

static int failures, checks;

#define	CHECK(c) do { checks++; if (!(c)) { failures++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct as_state
idle(uint64_t now)
{
	struct as_state s;

	memset(&s, 0, sizeof(s));
	s.now_ms = now;
	s.display = 0;
	s.lpm = 0;
	s.acline = 0;
	return (s);
}

int
main(void)
{
	struct as_conf c;
	struct as_track t;
	struct as_state s;
	char why[64];
	unsigned intid, ms;
	int w;

	as_conf_init(&c);
	memset(&t, 0, sizeof(t));
	/* the display on: up, looked at every 10 s */
	s = idle(1000);
	s.display = 1;
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) == 10000 &&
	    strcmp(why, "display on") == 0);
	/* off: 30 s later */
	s = idle(2000);
	w = as_decide(&t, &s, &c, why, sizeof(why));
	CHECK(w == 30000 && strcmp(why, "the display just went off") == 0);
	s = idle(31999);
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) == 1);
	s = idle(32000);
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) == 0 &&
	    strcmp(why, "idle") == 0);
	/* the stand-by screen counts as off, unless -L */
	memset(&t, 0, sizeof(t));
	s = idle(0);
	s.now_ms = 100000;
	s.display = 1;
	s.lpm = 1;
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) == 30000);
	s.now_ms = 130000;
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) == 0);
	c.in_standby = false;
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) == 10000 &&
	    strcmp(why, "the stand-by screen") == 0 && t.off_since == 0);
	c.in_standby = true;
	/* no display state (the VM): up */
	s = idle(200000);
	s.display = -1;
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) > 0 &&
	    strcmp(why, "no display state") == 0);
	/* what holds it up, display off long enough */
	memset(&t, 0, sizeof(t));
	s = idle(300000);
	(void)as_decide(&t, &s, &c, why, sizeof(why));
	s.now_ms += 40000;
	s.call = true;
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) == 5000 &&
	    strcmp(why, "a call") == 0);
	s.call = false;
	s.audio = true;
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) > 0 &&
	    strcmp(why, "sound") == 0);
	s.audio = false;
	s.inhibit = true;
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) > 0 &&
	    strcmp(why, "inhibited") == 0);
	s.inhibit = false;
	s.usbnet = true;
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) > 0 &&
	    strcmp(why, "USB network") == 0);
	s.usbnet = false;
	s.acline = 1;
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) > 0 &&
	    strcmp(why, "external power") == 0);
	c.on_power = true;
	s.now_ms += 1000;
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) == 0);
	c.on_power = false;
	s.acline = 0;
	/* a transfer: 100 kB in 5 s; then quiet */
	s.now_ms += 5000;
	s.net_bytes += 100000;
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) == 5000 &&
	    strcmp(why, "a transfer") == 0 && t.net_rate == 20000);
	s.now_ms += 5000;
	s.net_bytes += 1000;
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) == 0 &&
	    t.net_rate == 200);
	/* after a suspend: what ended it, how long it stays up */
	CHECK(as_after(&t, "suspended 12.345 s in 3 epochs, ended by INTID "
	    "215 (a device: the power key); 1 power-downs", &c, s.now_ms,
	    &intid, &ms) == AS_END_WAKE && intid == 215 && ms == 12345);
	CHECK(t.awake_until == s.now_ms + 30000);
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) == 30000 &&
	    strcmp(why, "up after a suspend") == 0);
	CHECK(as_after(&t, "suspended 1.000 s in 0 epochs, ended by INTID 100 "
	    "(a device); x", &c, s.now_ms, &intid, &ms) == AS_END_WAKE &&
	    intid == 100 && t.awake_until == s.now_ms + 10000);
	/* the sources by option */
	CHECK(as_parse_wake(&c, "215:45, 309") == 0 && c.nwake == 2 &&
	    c.wake_s[0] == 45 && c.wake[1] == 309 && c.wake_s[1] == c.stay_s);
	CHECK(as_parse_wake(&c, "x") == -1 && as_parse_wake(&c, "1:") == -1);
	CHECK(as_parse_wake(&c, "215:30 100:10 102:10") == 0 && c.nwake == 3);
	CHECK(as_after(&t, "suspended 4.500 s in 0 epochs, ended by INTID 309 "
	    "(a device); x", &c, s.now_ms, &intid, &ms) == AS_END_OTHER &&
	    intid == 309 && t.awake_until == s.now_ms + 2000);
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) == 2000);
	CHECK(as_after(&t, "suspended 60.000 s in 11 epochs, ended by a "
	    "wake-up (user space); x", &c, s.now_ms, &intid, &ms) ==
	    AS_END_WAKE && intid == 0 && ms == 60000);
	CHECK(as_after(&t, "suspended 197.000 s in 39 epochs, ended by the "
	    "deadline 0; x", &c, s.now_ms, &intid, &ms) == AS_END_DEADLINE);
	CHECK(as_after(&t, "not suspended: 0.000 s in 0 epochs, ended by the "
	    "deadline 0; x", &c, s.now_ms, &intid, &ms) == AS_END_FAILED &&
	    t.awake_until == s.now_ms + 60000);
	CHECK(as_after(&t, "", &c, s.now_ms, &intid, &ms) == AS_END_FAILED);
	/* the display on in between: the delay starts again */
	s.now_ms += 70000;
	s.display = 1;
	(void)as_decide(&t, &s, &c, why, sizeof(why));
	s.now_ms += 1000;
	s.display = 0;
	CHECK(as_decide(&t, &s, &c, why, sizeof(why)) == 30000);
	printf("%d checks, %d failures\n", checks, failures);
	return (failures != 0);
}
