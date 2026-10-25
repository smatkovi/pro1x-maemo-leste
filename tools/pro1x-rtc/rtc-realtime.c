/*
 * librtc-realtime.so -- the Pro1X's RTC as if it kept the real time.
 *
 * The PMIC's RTC counts from its own start and cannot be set (see
 * pro1x-rtc.c). Alarm daemons that wake the phone from power off write the
 * alarm as real time into /dev/rtc0 (alarmd's hwrtc.c, Harmattan's timed),
 * which on this RTC lies decades ahead. Preloaded into them, this library
 * translates the RTC ioctls with the current offset between the system
 * clock and the RTC:
 *
 *   RTC_RD_TIME, RTC_ALM_READ, RTC_WKALM_RD   RTC time + offset
 *   RTC_WKALM_SET, RTC_ALM_SET                real time - offset
 *   RTC_SET_TIME                              ignored (nothing to set)
 *
 * The offset is taken at each call, so it follows the system clock (network
 * time, pro1x-rtc restore). Before the clock is set nothing is translated.
 * All other ioctls go through unchanged.
 *
 * Leste: /etc/conf.d/alarmd preloads it into alarmd. MeeGo: listed in the
 * Harmattan tree's /etc/ld.so.preload (meego/38-hardware-adaptation.sh).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <linux/rtc.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

#define EXPORT __attribute__((visibility("default")))
#define PLAUSIBLE 1767225600	/* 1 Jan 2026 UTC */

typedef int (*ioctl_fn)(int, unsigned long, ...);

static ioctl_fn real_ioctl(void)
{
	static ioctl_fn fn;
	if (!fn)
		fn = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");
	return fn;
}

static time_t from_rtc(const struct rtc_time *r)
{
	struct tm tm;

	memset(&tm, 0, sizeof(tm));
	tm.tm_sec = r->tm_sec;
	tm.tm_min = r->tm_min;
	tm.tm_hour = r->tm_hour;
	tm.tm_mday = r->tm_mday;
	tm.tm_mon = r->tm_mon;
	tm.tm_year = r->tm_year;
	return timegm(&tm);
}

static void to_rtc(time_t t, struct rtc_time *r)
{
	struct tm tm;

	gmtime_r(&t, &tm);
	memset(r, 0, sizeof(*r));
	r->tm_sec = tm.tm_sec;
	r->tm_min = tm.tm_min;
	r->tm_hour = tm.tm_hour;
	r->tm_mday = tm.tm_mday;
	r->tm_mon = tm.tm_mon;
	r->tm_year = tm.tm_year;
	r->tm_wday = tm.tm_wday;
	r->tm_yday = tm.tm_yday;
	r->tm_isdst = -1;
}

/* System clock minus RTC; 0 while the clock is not set. */
static int offset(int fd, time_t *off)
{
	struct rtc_time r;
	time_t now = time(NULL);

	*off = 0;
	if (now < PLAUSIBLE || real_ioctl()(fd, RTC_RD_TIME, &r) < 0)
		return -1;
	*off = now - from_rtc(&r);
	return 0;
}

/* glibc declares ioctl (int, unsigned long int, ...) */
EXPORT int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;
	time_t off;
	int ret;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	switch (req) {
	case RTC_RD_TIME:
	case RTC_ALM_READ:
		ret = real_ioctl()(fd, req, arg);
		if (ret == 0 && offset(fd, &off) == 0)
			to_rtc(from_rtc(arg) + off, arg);
		return ret;
	case RTC_WKALM_RD:
		ret = real_ioctl()(fd, req, arg);
		if (ret == 0 && offset(fd, &off) == 0) {
			struct rtc_wkalrm *w = arg;
			to_rtc(from_rtc(&w->time) + off, &w->time);
		}
		return ret;
	case RTC_WKALM_SET:
		if (offset(fd, &off) == 0) {
			struct rtc_wkalrm w = *(struct rtc_wkalrm *)arg;
			to_rtc(from_rtc(&w.time) - off, &w.time);
			return real_ioctl()(fd, req, &w);
		}
		return real_ioctl()(fd, req, arg);
	case RTC_ALM_SET:
		/* time of day only: within the next 24 hours of real time */
		if (offset(fd, &off) == 0) {
			struct rtc_time r = *(struct rtc_time *)arg;
			time_t now = time(NULL), day = now - now % 86400;
			time_t t = day + r.tm_hour * 3600 + r.tm_min * 60 + r.tm_sec;
			if (t <= now)
				t += 86400;
			to_rtc(t - off, &r);
			return real_ioctl()(fd, req, &r);
		}
		return real_ioctl()(fd, req, arg);
	case RTC_SET_TIME:
		return 0;
	default:
		return real_ioctl()(fd, req, arg);
	}
}
