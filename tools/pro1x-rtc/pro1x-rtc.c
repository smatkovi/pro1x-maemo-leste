/*
 * pro1x-rtc -- the real time across power off, from the PMIC's RTC.
 *
 * The Pro1X's RTC (PM6125, rtc-pm8xxx) counts from its own start and Linux
 * cannot set it ("allow-set-time" is not given); Android keeps the
 * difference to the real time itself. So does this tool:
 *
 *   pro1x-rtc save      store real time minus RTC time (/var/lib/pro1x-rtc)
 *   pro1x-rtc restore   set the clock to RTC time plus that offset
 *   pro1x-rtc show      both times and the offset
 *
 * /etc/init.d/pro1x-rtc restores at boot, before clockd and alarmd (the RTC
 * keeps counting while the phone is off, so the clock is right even without
 * network; fake-hwclock only brings back the time of the last shutdown),
 * and saves at shutdown. librtc-realtime.so translates the wake alarms that
 * alarmd and Harmattan's timed set through /dev/rtc0.
 */
#include <fcntl.h>
#include <linux/rtc.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define RTC "/dev/rtc0"
#define DIR "/var/lib/pro1x-rtc"
#define OFFSET DIR "/offset"
/* A clock before this has not been set yet (1 Jan 2026 UTC). */
#define PLAUSIBLE 1767225600

static int rtc_now(time_t *t)
{
	struct rtc_time rt;
	struct tm tm;
	int fd = open(RTC, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	if (ioctl(fd, RTC_RD_TIME, &rt) < 0) {
		close(fd);
		return -1;
	}
	close(fd);
	memset(&tm, 0, sizeof(tm));
	tm.tm_sec = rt.tm_sec;
	tm.tm_min = rt.tm_min;
	tm.tm_hour = rt.tm_hour;
	tm.tm_mday = rt.tm_mday;
	tm.tm_mon = rt.tm_mon;
	tm.tm_year = rt.tm_year;
	*t = timegm(&tm);
	return 0;
}

static int read_offset(long long *off)
{
	FILE *f = fopen(OFFSET, "r");
	int ok;

	if (!f)
		return -1;
	ok = fscanf(f, "%lld", off) == 1;
	fclose(f);
	return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "show";
	time_t rtc, now = time(NULL);
	long long off;

	if (rtc_now(&rtc) < 0) {
		perror(RTC);
		return 1;
	}

	if (!strcmp(cmd, "save")) {
		if (now < PLAUSIBLE) {
			fprintf(stderr, "pro1x-rtc: the clock is not set, nothing saved\n");
			return 1;
		}
		mkdir(DIR, 0755);
		FILE *f = fopen(OFFSET ".new", "w");
		if (!f || fprintf(f, "%lld\n", (long long)(now - rtc)) < 0 || fclose(f) != 0 ||
		    rename(OFFSET ".new", OFFSET) < 0) {
			perror(OFFSET);
			return 1;
		}
		return 0;
	}

	if (!strcmp(cmd, "restore")) {
		if (read_offset(&off) < 0) {
			fprintf(stderr, "pro1x-rtc: no saved offset\n");
			return 1;
		}
		struct timeval tv = { .tv_sec = rtc + off };
		if (tv.tv_sec < PLAUSIBLE) {
			fprintf(stderr, "pro1x-rtc: saved offset gives an implausible time\n");
			return 1;
		}
		/* never set the clock back by more than a minute: whatever set
		 * it later than the RTC knows (network time) is better */
		if (tv.tv_sec + 60 < now)
			return 0;
		if (settimeofday(&tv, NULL) < 0) {
			perror("settimeofday");
			return 1;
		}
		return 0;
	}

	if (!strcmp(cmd, "show")) {
		printf("system %lld\nrtc    %lld\n", (long long)now, (long long)rtc);
		if (read_offset(&off) == 0)
			printf("saved offset %lld (rtc + offset = %lld)\n", off, (long long)rtc + off);
		printf("current offset %lld\n", (long long)(now - rtc));
		return 0;
	}

	fprintf(stderr, "usage: pro1x-rtc save|restore|show\n");
	return 2;
}
