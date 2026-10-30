/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The alarm of the Qualcomm PMIC RTC ("qcom,pm8941-rtc", qcom_rtc.c): the
 * registers, the conversion between the real time and the RTC's count,
 * and the register sequences, after Linux's rtc-pm8xxx.c (pm8941_regs).
 * Shared with the host test (freebsd/pm/test/qcom_rtc_test.c) and the
 * QEMU stand-in (qcom_rtc_sim.c).
 *
 * The RTC peripheral (the node's first reg, 0x6000 on the PM6125) has the
 * 32-bit count (READ, four bytes, little endian) and its enable (CTRL bit
 * 7); the alarm peripheral (the second reg, 0x6100) the count it fires at
 * (ALARM_RW), its enable (ALARM_CTRL bit 7) and the status clear
 * (ALARM_CTRL2 bit 0). Its interrupt (<0 0x61 1 IRQ_TYPE_EDGE_RISING>)
 * goes through the SPMI arbiter. On the Pro1X the count cannot be set
 * (Linux's node has no "allow-set-time"), the alarm can: a real time is
 * converted with the difference between the system clock and the count at
 * that moment, as Leste's librtc-realtime does for alarmd
 * (tools/pro1x-rtc/rtc-realtime.c), which with Linux's driver (offset 0)
 * gives the same count as rtc-pm8xxx's set_alarm.
 */
#ifndef _DEV_QCOM_SPMI_QCOM_RTC_ALARM_H_
#define	_DEV_QCOM_SPMI_QCOM_RTC_ALARM_H_

#ifdef _KERNEL
#include <sys/types.h>
#include <sys/errno.h>
#else
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#endif

/* in the RTC peripheral */
#define	QRTC_RTC_WRITE		0x40
#define	QRTC_RTC_CTRL		0x46
#define	 QRTC_RTC_EN		0x80
#define	QRTC_RTC_READ		0x48
/* in either: the peripheral's type and subtype (Qualcomm's qpnp-rtc) */
#define	QRTC_PERPH_TYPE		0x04
#define	QRTC_PERPH_SUBTYPE	0x05
#define	 QRTC_SUBTYPE_RTC	0x01
#define	 QRTC_SUBTYPE_ALARM	0x03
/* in the alarm peripheral */
#define	QRTC_ALARM_RW		0x40
#define	QRTC_ALARM_CTRL		0x46
#define	 QRTC_ALARM_EN		0x80
#define	QRTC_ALARM_CTRL2	0x48
#define	 QRTC_ALARM_CLEAR	0x01

/*
 * An alarm is set at least this many seconds ahead: the count may step
 * between its read and the write, and the alarm fires when the count
 * equals it.
 */
#define	QRTC_ALARM_MIN		2
/* and at most this far: the difference must fit a signed 32-bit number */
#define	QRTC_ALARM_MAX		0x7fffffff

/* Register access, the PMIC's 16-bit addresses (qcom_spmi_read/write) */
struct qrtc_io {
	int	(*rd)(void *arg, uint16_t addr, uint8_t *buf, size_t len);
	int	(*wr)(void *arg, uint16_t addr, const uint8_t *buf, size_t len);
	void	*arg;
};

static inline uint32_t
qrtc_le32(const uint8_t v[4])
{
	return ((uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 |
	    (uint32_t)v[3] << 24);
}

static inline void
qrtc_le32enc(uint8_t v[4], uint32_t x)
{
	v[0] = x & 0xff;
	v[1] = (x >> 8) & 0xff;
	v[2] = (x >> 16) & 0xff;
	v[3] = x >> 24;
}

/*
 * The count for the real time when, the count cnt having been read at the
 * real time now: cnt + (when - now), modulo 2^32 as the hardware's (Linux:
 * rtc_tm_to_time64(alarm) - offset, in 32 bits). EINVAL when when is not
 * in the future (the caller switches the alarm off, as Linux's sysfs
 * wakealarm does), ERANGE when it is more than QRTC_ALARM_MAX seconds
 * ahead; at least QRTC_ALARM_MIN seconds ahead.
 */
static inline int
qrtc_alarm_count(uint32_t cnt, int64_t now, int64_t when, uint32_t *alarm)
{
	uint64_t d;

	if (when <= now)
		return (EINVAL);
	d = (uint64_t)when - (uint64_t)now;
	if (d > QRTC_ALARM_MAX)
		return (ERANGE);
	if (d < QRTC_ALARM_MIN)
		d = QRTC_ALARM_MIN;
	*alarm = cnt + (uint32_t)d;
	return (0);
}

/* The real time of the count alarm, cnt having been read at now */
static inline int64_t
qrtc_alarm_time(uint32_t cnt, int64_t now, uint32_t alarm)
{
	return (now + (int32_t)(alarm - cnt));
}

/* Linux's regmap_update_bits: read, change, written only when it changed */
static inline int
qrtc_update(const struct qrtc_io *io, uint16_t addr, uint8_t mask,
    uint8_t val)
{
	uint8_t v, n;
	int error;

	if ((error = io->rd(io->arg, addr, &v, 1)) != 0)
		return (error);
	n = (v & ~mask) | (val & mask);
	if (n == v)
		return (0);
	return (io->wr(io->arg, addr, &n, 1));
}

/* The count, read again after a carry between its bytes (Linux's
 * pm8xxx_rtc_read_raw) */
static inline int
qrtc_read_count(const struct qrtc_io *io, uint16_t rtc, uint32_t *secs)
{
	uint8_t v[4], lsb;
	int error;

	if ((error = io->rd(io->arg, rtc + QRTC_RTC_READ, v, 4)) != 0)
		return (error);
	if ((error = io->rd(io->arg, rtc + QRTC_RTC_READ, &lsb, 1)) != 0)
		return (error);
	if (lsb < v[0] &&
	    (error = io->rd(io->arg, rtc + QRTC_RTC_READ, v, 4)) != 0)
		return (error);
	*secs = qrtc_le32(v);
	return (0);
}

/* What the alarm is set to */
static inline int
qrtc_alarm_get(const struct qrtc_io *io, uint16_t abase, uint32_t *alarm,
    int *on)
{
	uint8_t v[4], ctrl;
	int error;

	if ((error = io->rd(io->arg, abase + QRTC_ALARM_RW, v, 4)) != 0 ||
	    (error = io->rd(io->arg, abase + QRTC_ALARM_CTRL, &ctrl, 1)) != 0)
		return (error);
	*alarm = qrtc_le32(v);
	*on = (ctrl & QRTC_ALARM_EN) != 0;
	return (0);
}

/*
 * The alarm at the count alarm, as Linux's pm8xxx_rtc_set_alarm: off, the
 * four bytes, on; then read back.
 */
static inline int
qrtc_alarm_set(const struct qrtc_io *io, uint16_t abase, uint32_t alarm)
{
	uint8_t v[4];
	uint32_t got;
	int on, error;

	qrtc_le32enc(v, alarm);
	if ((error = qrtc_update(io, abase + QRTC_ALARM_CTRL, QRTC_ALARM_EN,
	    0)) != 0 ||
	    (error = io->wr(io->arg, abase + QRTC_ALARM_RW, v, 4)) != 0 ||
	    (error = qrtc_update(io, abase + QRTC_ALARM_CTRL, QRTC_ALARM_EN,
	    QRTC_ALARM_EN)) != 0)
		return (error);
	if ((error = qrtc_alarm_get(io, abase, &got, &on)) != 0)
		return (error);
	return (got == alarm && on ? 0 : EIO);
}

/* Off, the count cleared: Linux's pm8xxx_rtc_alarm_irq_enable(0) */
static inline int
qrtc_alarm_off(const struct qrtc_io *io, uint16_t abase)
{
	static const uint8_t zero[4];
	int error;

	if ((error = qrtc_update(io, abase + QRTC_ALARM_CTRL, QRTC_ALARM_EN,
	    0)) != 0)
		return (error);
	return (io->wr(io->arg, abase + QRTC_ALARM_RW, zero, 4));
}

/*
 * After its interrupt, as Linux's pm8xxx_alarm_trigger: off, the status
 * cleared. Linux since 6.3 (Leste's 7.1 on this phone) clears ALARM_CTRL2's
 * bit 0 (regmap_update_bits(..., CLEAR, 0): a write only when it reads
 * 1), so the alarm's switching off is what clears its status there;
 * Linux before 6.3 and Qualcomm's qpnp-rtc write the bit 1. w1: the
 * latter (hw.qcom_rtc.alarm_clear=1).
 */
static inline int
qrtc_alarm_ack(const struct qrtc_io *io, uint16_t abase, int w1)
{
	int error;

	if ((error = qrtc_update(io, abase + QRTC_ALARM_CTRL, QRTC_ALARM_EN,
	    0)) != 0)
		return (error);
	if (w1) {
		uint8_t v;

		if ((error = io->rd(io->arg, abase + QRTC_ALARM_CTRL2, &v,
		    1)) != 0)
			return (error);
		v |= QRTC_ALARM_CLEAR;
		return (io->wr(io->arg, abase + QRTC_ALARM_CTRL2, &v, 1));
	}
	return (qrtc_update(io, abase + QRTC_ALARM_CTRL2, QRTC_ALARM_CLEAR, 0));
}

#endif
