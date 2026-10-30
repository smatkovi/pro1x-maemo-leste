/*
 * Host test of qcom_rtc_alarm.h (freebsd/pm/README.md, the RTC alarm):
 * the PMIC RTC alarm's registers, its conversion from the real time and
 * its register sequences, against Linux 7.1's rtc-pm8xxx.c (pm8941_regs,
 * pm8xxx_rtc_set_alarm, read_alarm, alarm_irq_enable, alarm_trigger) with
 * Leste's librtc-realtime in front of it (tools/pro1x-rtc/rtc-realtime.c:
 * alarmd's RTC_WKALM_SET translated by the offset between the system clock
 * and RTC_RD_TIME), on a model of the PM6125's two peripherals: a count
 * that cannot be set, an alarm that fires when the count equals it.
 *
 *   cc -Wall -I freebsd/pm/sys -o /tmp/qrtct freebsd/pm/test/qcom_rtc_test.c && /tmp/qrtct
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dev/qcom_spmi/qcom_rtc_alarm.h"

static int fails, checks;
#define	EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
	checks++; if (_a != _b) { \
	printf("FAIL %s:%d: %s = %#llx, want %#llx\n", __FILE__, __LINE__, #a, _a, _b); \
	fails++; } } while (0)

/* The Pro1X's tree (pm6125.dtsi): reg = <0x6000>, <0x6100> */
#define	RTC	0x6000
#define	ALARM	0x6100

/* --- the PMIC: the RTC's count and the alarm ------------------------------ */

static struct pmic {
	uint8_t		r[0x10000];
	uint32_t	count;
	int		fired;		/* the alarm's interrupt, latched */
	int		step_after_read; /* the count steps after a 4-byte read */
	struct { uint16_t a; uint8_t v; } log[64];
	int		nlog;
} pm;

static int
pm_rd(void *arg, uint16_t addr, uint8_t *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		uint16_t a = addr + i;

		if (a >= RTC + QRTC_RTC_READ && a < RTC + QRTC_RTC_READ + 4)
			buf[i] = pm.count >> (8 * (a - RTC - QRTC_RTC_READ));
		else
			buf[i] = pm.r[a];
	}
	if (addr == RTC + QRTC_RTC_READ && len == 4 && pm.step_after_read) {
		pm.count++;
		pm.step_after_read = 0;
	}
	return (0);
}

static int
pm_wr(void *arg, uint16_t addr, const uint8_t *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		uint16_t a = addr + i;

		if (pm.nlog < (int)(sizeof(pm.log) / sizeof(pm.log[0]))) {
			pm.log[pm.nlog].a = a;
			pm.log[pm.nlog++].v = buf[i];
		}
		/* the count cannot be set on the Pro1X: RTC_WRITE is ignored */
		if (a >= RTC + QRTC_RTC_WRITE && a < RTC + QRTC_RTC_WRITE + 4)
			continue;
		/* ALARM_CTRL2's CLEAR: write 1 to clear, reads 0 */
		if (a == ALARM + QRTC_ALARM_CTRL2) {
			if (buf[i] & QRTC_ALARM_CLEAR)
				pm.fired = 0;
			continue;
		}
		pm.r[a] = buf[i];
		/* the alarm off clears its status (what Linux 6.3 on relies on) */
		if (a == ALARM + QRTC_ALARM_CTRL && !(buf[i] & QRTC_ALARM_EN))
			pm.fired = 0;
	}
	return (0);
}

static void
pm_tick(void)
{
	pm.count++;
	if ((pm.r[ALARM + QRTC_ALARM_CTRL] & QRTC_ALARM_EN) &&
	    qrtc_le32(&pm.r[ALARM + QRTC_ALARM_RW]) == pm.count)
		pm.fired = 1;
}

static void
pm_reset(uint32_t count)
{
	memset(&pm, 0, sizeof(pm));
	pm.count = count;
	pm.r[RTC + QRTC_RTC_CTRL] = QRTC_RTC_EN;
	pm.r[RTC + QRTC_PERPH_SUBTYPE] = QRTC_SUBTYPE_RTC;
	pm.r[ALARM + QRTC_PERPH_SUBTYPE] = QRTC_SUBTYPE_ALARM;
}

static const struct qrtc_io io = { pm_rd, pm_wr, NULL };

/* --- Linux 7.1's rtc-pm8xxx.c with librtc-realtime in front ------------- */

/* pm8941_regs */
static const struct {
	unsigned ctrl, write, read, alarm_ctrl, alarm_ctrl2, alarm_rw, alarm_en;
} pm8941_regs = { 0x6046, 0x6040, 0x6048, 0x6146, 0x6148, 0x6140, 1u << 7 };

#define	PLAUSIBLE 1767225600	/* rtc-realtime.c: 1 Jan 2026 UTC */

/* rtc_time64_to_tm / rtc_tm_to_time64 and librtc's to_rtc / from_rtc */
static struct tm
to_tm(int64_t t)
{
	time_t tt = (time_t)t;
	struct tm tm;

	gmtime_r(&tt, &tm);
	return (tm);
}

static int64_t
from_tm(struct tm tm)
{
	return ((int64_t)timegm(&tm));
}

/*
 * alarmd's hwrtc_set_alarm (gmtime of the wake-up, RTC_WKALM_SET) through
 * librtc-realtime into pm8xxx_rtc_set_alarm: the four bytes it writes to
 * alarm_rw. The driver's offset is 0 on the Pro1X (no nvmem cell, no UEFI
 * variable), RTC_RD_TIME gives the raw count.
 */
static uint32_t
linux_set_alarm(uint32_t raw, int64_t now, int64_t when, uint8_t v[4])
{
	const uint32_t offset = 0;
	struct tm alarm = to_tm(when);
	int64_t off;
	uint32_t secs;

	if (now >= PLAUSIBLE) {
		/* librtc: offset() = now - from_rtc(RTC_RD_TIME) */
		off = now - from_tm(to_tm((int64_t)raw + offset));
		alarm = to_tm(from_tm(alarm) - off);
	}
	/* pm8xxx_rtc_set_alarm: secs = rtc_tm_to_time64() - offset, u32 */
	secs = (uint32_t)from_tm(alarm);
	secs -= offset;
	v[0] = secs; v[1] = secs >> 8; v[2] = secs >> 16; v[3] = secs >> 24;
	return (secs);
}

/* pm8xxx_rtc_read_alarm through librtc's RTC_WKALM_RD */
static int64_t
linux_read_alarm(uint32_t raw, int64_t now, const uint8_t v[4])
{
	uint32_t secs = (uint32_t)v[0] | (uint32_t)v[1] << 8 |
	    (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
	int64_t t = secs;	/* + offset 0 */

	if (now >= PLAUSIBLE)
		t += now - (int64_t)raw;
	return (t);
}

/* --- the tests ------------------------------------------------------------ */

/* One alarm as alarmd sets it on Leste and as qcom_rtc sets it here */
static void
one(uint32_t raw, int64_t now, int64_t when)
{
	uint8_t lv[4];
	uint32_t ours, theirs, back;
	int on;

	pm_reset(raw);
	theirs = linux_set_alarm(raw, now, when, lv);
	EQ(qrtc_alarm_count(raw, now, when, &ours), 0);
	EQ(ours, theirs);
	EQ(qrtc_alarm_set(&io, ALARM, ours), 0);
	/* the same four bytes at alarm_rw, the enable bit at alarm_ctrl */
	EQ(memcmp(&pm.r[pm8941_regs.alarm_rw], lv, 4), 0);
	EQ(pm.r[pm8941_regs.alarm_ctrl] & pm8941_regs.alarm_en,
	    pm8941_regs.alarm_en);
	/* read back as Linux's read_alarm does */
	EQ(qrtc_alarm_get(&io, ALARM, &back, &on), 0);
	EQ(on, 1);
	EQ(qrtc_alarm_time(raw, now, back),
	    linux_read_alarm(raw, now, &pm.r[pm8941_regs.alarm_rw]));
	EQ(qrtc_alarm_time(raw, now, back), when);
}

int
main(void)
{
	uint32_t c, a;
	int64_t now;
	int i, on, n;

	/* the registers: base + offset as Linux's pm8941_regs */
	EQ(RTC + QRTC_RTC_CTRL, pm8941_regs.ctrl);
	EQ(RTC + QRTC_RTC_WRITE, pm8941_regs.write);
	EQ(RTC + QRTC_RTC_READ, pm8941_regs.read);
	EQ(ALARM + QRTC_ALARM_RW, pm8941_regs.alarm_rw);
	EQ(ALARM + QRTC_ALARM_CTRL, pm8941_regs.alarm_ctrl);
	EQ(ALARM + QRTC_ALARM_CTRL2, pm8941_regs.alarm_ctrl2);
	EQ(QRTC_ALARM_EN, pm8941_regs.alarm_en);
	EQ(QRTC_RTC_EN, 1u << 7);	/* PM8xxx_RTC_ENABLE */
	EQ(QRTC_ALARM_CLEAR, 1u << 0);	/* PM8xxx_RTC_ALARM_CLEAR */

	/*
	 * The conversion, against librtc-realtime + rtc-pm8xxx: a count as
	 * the Pro1X's (some years since the PMIC's first start), the clock
	 * set (2026-10-30), alarms 2 s to a year ahead
	 */
	now = 1793372400;	/* 2026-10-30 14:20 UTC */
	one(0x0a1b2c3d, now, now + 2);
	one(0x0a1b2c3d, now, now + 60);
	one(0x0a1b2c3d, now, now + 120);
	one(0x0a1b2c3d, now, now + 86400);
	one(0x0a1b2c3d, now, now + 366 * 86400);
	one(0, now, now + 240);
	one(1, now, now + 240);
	one(0x7fffffff, now, now + 240);
	one(0x80000000, now, now + 240);
	/* the clock not set (before 2026): Linux's hctosys made it the count;
	 * librtc translates nothing, the same count either way */
	one(123456789, 123456789, 123456789 + 300);
	/* the count's wrap at 2^32 (Linux's RTC core refuses beyond range_max
	 * U32_MAX; ours wraps as the 32-bit compare of the hardware would) */
	EQ(qrtc_alarm_count(0xffffff00, now, now + 0x200, &a), 0);
	EQ(a, 0x100);
	EQ(qrtc_alarm_time(0xffffff00, now, 0x100), now + 0x200);

	/* not in the future: off; too close: QRTC_ALARM_MIN; too far: ERANGE */
	EQ(qrtc_alarm_count(1000, now, now, &a), EINVAL);
	EQ(qrtc_alarm_count(1000, now, now - 5, &a), EINVAL);
	EQ(qrtc_alarm_count(1000, now, 0, &a), EINVAL);
	EQ(qrtc_alarm_count(1000, now, now + 1, &a), 0);
	EQ(a, 1000 + QRTC_ALARM_MIN);
	EQ(qrtc_alarm_count(1000, now, now + 0x7fffffffLL, &a), 0);
	EQ(a, 1000 + 0x7fffffffu);
	EQ(qrtc_alarm_count(1000, now, now + 0x80000000LL, &a), ERANGE);
	EQ(qrtc_alarm_count(1000, -5, INT64_MAX, &a), ERANGE);
	/* a past alarm reads back in the past */
	EQ(qrtc_alarm_time(5000, now, 4990), now - 10);

	/* the order of the writes: off, the four bytes, on (Linux) */
	pm_reset(5000);
	pm.r[ALARM + QRTC_ALARM_CTRL] = QRTC_ALARM_EN;	/* one left on */
	EQ(qrtc_alarm_set(&io, ALARM, 5060), 0);
	EQ(pm.nlog, 6);
	EQ(pm.log[0].a, ALARM + QRTC_ALARM_CTRL); EQ(pm.log[0].v, 0);
	for (i = 0; i < 4; i++) {
		EQ(pm.log[1 + i].a, ALARM + QRTC_ALARM_RW + i);
		EQ(pm.log[1 + i].v, (5060 >> (8 * i)) & 0xff);
	}
	EQ(pm.log[5].a, ALARM + QRTC_ALARM_CTRL); EQ(pm.log[5].v, QRTC_ALARM_EN);
	/* nothing written to the count */
	for (i = 0; i < pm.nlog; i++)
		EQ(pm.log[i].a >= RTC + QRTC_RTC_WRITE &&
		    pm.log[i].a < RTC + QRTC_RTC_WRITE + 4, 0);

	/* it fires when the count reaches it, not before */
	for (n = 0; n < 59; n++)
		pm_tick();
	EQ(pm.fired, 0);
	pm_tick();
	EQ(pm.fired, 1);
	EQ(pm.count, 5060);
	/* the interrupt, Linux 7.1's way: off; CTRL2 not written (reads 0) */
	pm.nlog = 0;
	EQ(qrtc_alarm_ack(&io, ALARM, 0), 0);
	EQ(pm.fired, 0);
	EQ(pm.nlog, 1);
	EQ(pm.log[0].a, ALARM + QRTC_ALARM_CTRL); EQ(pm.log[0].v, 0);
	EQ(qrtc_alarm_get(&io, ALARM, &a, &on), 0);
	EQ(on, 0);
	/* qpnp-rtc's way: CTRL2's CLEAR written 1 */
	pm.r[ALARM + QRTC_ALARM_CTRL] = QRTC_ALARM_EN;
	pm.fired = 1;
	pm.nlog = 0;
	EQ(qrtc_alarm_ack(&io, ALARM, 1), 0);
	EQ(pm.nlog, 2);
	EQ(pm.log[1].a, ALARM + QRTC_ALARM_CTRL2);
	EQ(pm.log[1].v, QRTC_ALARM_CLEAR);
	/* a second alarm fires again */
	EQ(qrtc_alarm_count(pm.count, now, now + 30, &a), 0);
	EQ(qrtc_alarm_set(&io, ALARM, a), 0);
	for (n = 0; n < 30 && !pm.fired; n++)
		pm_tick();
	EQ(pm.fired, 1);
	EQ(n, 30);

	/* off: Linux's alarm_irq_enable(0), the count zeroed */
	EQ(qrtc_alarm_off(&io, ALARM), 0);
	EQ(qrtc_alarm_get(&io, ALARM, &a, &on), 0);
	EQ(on, 0); EQ(a, 0);
	for (n = 0; n < 100; n++)
		pm_tick();
	EQ(pm.fired, 0);

	/* the count's read with a carry between its bytes: read again */
	pm_reset(0x1ff);
	pm.step_after_read = 1;
	EQ(qrtc_read_count(&io, RTC, &c), 0);
	EQ(c, 0x200);
	pm_reset(0x1fe);
	pm.step_after_read = 1;
	EQ(qrtc_read_count(&io, RTC, &c), 0);
	EQ(c, 0x1fe);	/* no carry: the first read stands */

	printf("%s: %d checks, %d failures\n", fails ? "FAILED" : "OK", checks,
	    fails);
	return (fails != 0);
}
