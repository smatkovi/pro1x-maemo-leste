/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pro1x-autosleep: the Pro1X suspends by itself while nobody uses it
 * (freebsd/pm/README.md, "Stage 6"), as Android's autosleep and Maemo's
 * mce/dsme policy do; Leste's counterpart is tools/pro1x-autosuspend.
 *
 * It looks at the phone (policy.c decides): the panel (qcom_dpu's
 * dev.qcom_dpu.0.display and .lpm: off, or the stand-by screen), sound
 * (pro1x-audiod's status: a stream, a voice session, FM), a call
 * (pro1x-callaudio runs), the USB network (ue0 up), external power
 * (hw.acpi.acline), the network's byte counters (a transfer), the inhibit
 * file /var/run/pro1x-autosleep.inhibit. When nothing holds it up it runs
 * pro1x-suspend -k (the panel left as it is; the next alarm as the RTC's
 * alarm, no suspend within 70 s of one) and the CPUs freeze until a device
 * interrupt: the power key, the RTC alarm, the modem (a call, a message:
 * its GLINK and SMP2P interrupts), the keyboard, a cable (devd's
 * pro1x_power.conf: hw.pro1x_pm.wake=1), Wi-Fi traffic. Then it stays up
 * (-a after a wake-up source, -s after anything else) and looks again.
 *
 * Each suspend goes to the system log with what ended it; the totals
 * (how often, how long, by which interrupt, named from hw.intrnames) are
 * in /var/run/pro1x-autosleep.stats, also on SIGUSR1 in the log.
 *
 *   pro1x-autosleep [-dv] [-D delay] [-a stay] [-s settle] [-n bytes/s]
 *                   [-L] [-P] [-w "215:30 100:10 102:10"] [-T dir]
 *     -d  in the foreground, the log on stderr too   -v  every look
 *     -D  seconds the display is off first (30)
 *     -a  seconds up after a wake-up from user space (a cable, devd) or
 *         the deadline (an alarm's time) (30)
 *     -s  seconds up after any other interrupt (2)
 *     -n  a transfer above this many bytes a second keeps it up (4096)
 *     -L  not while the stand-by screen shows (only with the panel off)
 *     -P  also on external power
 *     -w  the wake-up sources' INTIDs and the seconds up after each
 *         (215:30, the PMIC: power key, RTC alarm, charger; 100:10 and
 *         102:10, the modem's GLINK edge and SMP2P); -w after -a
 *     -T  a test's directory: display, lpm, acline from files there, its
 *         ./suspend instead of pro1x-suspend (the VM's test)
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <net/if.h>
#include <net/if_var.h>

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "policy.h"

#define	SUSPEND		"/usr/local/sbin/pro1x-suspend"
#define	AUDIO_SOCK	"/var/run/pro1x-audio.sock"
#define	INHIBIT		"/var/run/pro1x-autosleep.inhibit"
#define	STATS		"/var/run/pro1x-autosleep.stats"
#define	PIDFILE		"/var/run/pro1x-autosleep.pid"
#define	NSRC		32

static struct as_conf conf;
static struct as_track track;
static int fg, verbose;
static const char *testdir;
static volatile sig_atomic_t quit, dump;

static struct {
	unsigned	suspends, failed, refused;
	uint64_t	asleep_ms, since_ms;
	unsigned	deadline, wakeup;
	unsigned	intid[NSRC], count[NSRC], nsrc;
} st;

static void
logm(int pri, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsyslog(pri, fmt, ap);
	va_end(ap);
	if (fg) {
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
		fputc('\n', stderr);
	}
}

static void
chomp(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == ' '))
		s[--n] = '\0';
}

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_UPTIME, &ts);
	return ((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

/* an integer sysctl, or a test's file; -1: there is none */
static int
get_int(const char *name, const char *file)
{
	char path[256], buf[32];
	size_t len;
	int v, fd;
	ssize_t n;

	if (testdir != NULL) {
		snprintf(path, sizeof(path), "%s/%s", testdir, file);
		if ((fd = open(path, O_RDONLY)) < 0)
			return (-1);
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			return (-1);
		buf[n] = '\0';
		return (atoi(buf));
	}
	len = sizeof(v);
	if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
		return (-1);
	return (v);
}

/* pro1x-audiod's status: something playing, recording, a call, FM */
static bool
audio_busy(void)
{
	struct sockaddr_un sun;
	struct timeval tv = { 2, 0 };
	char buf[1024];
	ssize_t n;
	int s;
	bool b = false;

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return (false);
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, AUDIO_SOCK, sizeof(sun.sun_path));
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (connect(s, (struct sockaddr *)&sun, sizeof(sun)) == 0 &&
	    write(s, "status\n", 7) == 7 && (n = read(s, buf,
	    sizeof(buf) - 1)) > 0) {
		buf[n] = '\0';
		b = strstr(buf, "play=running") != NULL ||
		    strstr(buf, "rec=running") != NULL ||
		    strstr(buf, "voice=up") != NULL ||
		    strstr(buf, " fm=1") != NULL;
	}
	close(s);
	return (b);
}

/* a process of that name (its first 19 characters) */
static bool
running(const char *comm)
{
	struct kinfo_proc *kp;
	size_t len = 0;
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC };
	unsigned i, n;
	bool found = false;

	if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0)
		return (false);
	len += len / 8;
	if ((kp = malloc(len)) == NULL)
		return (false);
	if (sysctl(mib, 3, kp, &len, NULL, 0) == 0) {
		n = (unsigned)(len / sizeof(*kp));
		for (i = 0; i < n && !found; i++)
			found = strncmp(kp[i].ki_comm, comm, COMMLEN) == 0;
	}
	free(kp);
	return (found);
}

/* the interfaces' bytes in and out (not lo0); ue0 up */
static uint64_t
net_bytes(bool *usbnet)
{
	struct ifaddrs *ifa, *i;
	struct if_data *d;
	uint64_t sum = 0;

	*usbnet = false;
	if (getifaddrs(&ifa) != 0)
		return (0);
	for (i = ifa; i != NULL; i = i->ifa_next) {
		if (i->ifa_addr == NULL || i->ifa_addr->sa_family != AF_LINK ||
		    i->ifa_data == NULL || (i->ifa_flags & IFF_LOOPBACK))
			continue;
		d = i->ifa_data;
		sum += d->ifi_ibytes + d->ifi_obytes;
		if (strcmp(i->ifa_name, "ue0") == 0 && (i->ifa_flags & IFF_UP) &&
		    (i->ifa_flags & IFF_RUNNING))
			*usbnet = true;
	}
	freeifaddrs(ifa);
	return (sum);
}

static void
look(struct as_state *s)
{
	memset(s, 0, sizeof(*s));
	s->now_ms = now_ms();
	s->display = get_int("dev.qcom_dpu.0.display", "display");
	s->lpm = get_int("dev.qcom_dpu.0.lpm", "lpm");
	s->acline = get_int("hw.acpi.acline", "acline");
	s->call = running("pro1x-callaudio");
	s->audio = audio_busy();
	s->inhibit = access(INHIBIT, F_OK) == 0;
	s->net_bytes = net_bytes(&s->usbnet);
}

/* the interrupt's name: "gic0,s68: qcom_glink0" from hw.intrnames */
static void
intr_name(unsigned intid, char *out, size_t len)
{
	char *names, *p, key[24];
	size_t n = 0;

	snprintf(out, len, "INTID %u", intid);
	if (intid < 32 || sysctlbyname("hw.intrnames", NULL, &n, NULL, 0) != 0 ||
	    (names = malloc(n + 1)) == NULL)
		return;
	if (sysctlbyname("hw.intrnames", names, &n, NULL, 0) == 0) {
		names[n] = '\0';
		snprintf(key, sizeof(key), ",s%u:", intid - 32);
		for (p = names; p < names + n; p += strlen(p) + 1)
			if (strstr(p, key) != NULL) {
				chomp(p);
				snprintf(out, len, "INTID %u (%s)", intid, p);
				break;
			}
	}
	free(names);
}

static void
stats_write(void)
{
	char name[96];
	FILE *f;
	uint64_t up = now_ms() - st.since_ms;
	unsigned i;

	if ((f = fopen(STATS, "w")) == NULL)
		return;
	fprintf(f, "suspends %u (not suspended %u, refused for an alarm %u), "
	    "asleep %ju.%03ju s of %ju s (%ju %%)\n", st.suspends, st.failed,
	    st.refused, (uintmax_t)(st.asleep_ms / 1000),
	    (uintmax_t)(st.asleep_ms % 1000), (uintmax_t)(up / 1000),
	    (uintmax_t)(up ? st.asleep_ms * 100 / up : 0));
	fprintf(f, "ended by the deadline %u, by a wake-up %u\n", st.deadline,
	    st.wakeup);
	for (i = 0; i < st.nsrc; i++) {
		intr_name(st.intid[i], name, sizeof(name));
		fprintf(f, "ended by %s: %u\n", name, st.count[i]);
	}
	fclose(f);
}

static void
stats_add(int how, unsigned intid, unsigned ms)
{
	unsigned i;

	st.suspends++;
	st.asleep_ms += ms;
	if (how == AS_END_FAILED) {
		st.failed++;
		return;
	}
	if (intid == 0) {
		if (how == AS_END_DEADLINE)
			st.deadline++;
		else
			st.wakeup++;
		return;
	}
	for (i = 0; i < st.nsrc; i++)
		if (st.intid[i] == intid)
			break;
	if (i == st.nsrc) {
		if (st.nsrc == NSRC)
			return;
		st.intid[st.nsrc++] = intid;
	}
	st.count[i]++;
}

/* pro1x-suspend -k (or the test's): its exit status, -1 not run */
static int
suspend(void)
{
	char path[256];
	pid_t pid;
	int status;

	if (testdir != NULL)
		snprintf(path, sizeof(path), "%s/suspend", testdir);
	else
		strlcpy(path, SUSPEND, sizeof(path));
	if ((pid = fork()) < 0)
		return (-1);
	if (pid == 0) {
		execl(path, path, "-k", (char *)NULL);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return (-1);
	return (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

static void
last_suspend(char *buf, size_t len)
{
	char path[256];
	FILE *f;

	buf[0] = '\0';
	if (testdir != NULL) {
		snprintf(path, sizeof(path), "%s/last", testdir);
		if ((f = fopen(path, "r")) != NULL) {
			if (fgets(buf, (int)len, f) == NULL)
				buf[0] = '\0';
			fclose(f);
		}
		return;
	}
	if (sysctlbyname("hw.pro1x_pm.last", buf, &len, NULL, 0) != 0)
		buf[0] = '\0';
}


static void
on_signal(int sig)
{
	if (sig == SIGUSR1)
		dump = 1;
	else
		quit = 1;
}

static void
sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };

	/* a signal ends it early: looked at again */
	nanosleep(&ts, NULL);
}

static void
usage(void)
{
	fprintf(stderr, "usage: pro1x-autosleep [-dvLP] [-D delay] [-a stay] "
	    "[-s settle] [-n bytes/s] [-w intids] [-T dir]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	struct as_state s;
	char why[64], last[512], name[96], was[64] = "";
	unsigned intid, ms;
	int c, wait, r, how;
	FILE *f;

	as_conf_init(&conf);
	while ((c = getopt(argc, argv, "dvLPD:a:s:n:w:T:")) != -1)
		switch (c) {
		case 'd': fg = 1; break;
		case 'v': verbose = 1; break;
		case 'L': conf.in_standby = false; break;
		case 'P': conf.on_power = true; break;
		case 'D': conf.delay_s = (unsigned)atoi(optarg); break;
		case 'a': conf.stay_s = (unsigned)atoi(optarg); break;
		case 's': conf.settle_s = (unsigned)atoi(optarg); break;
		case 'n': conf.net_bps = (unsigned)atoi(optarg); break;
		case 'w':
			if (as_parse_wake(&conf, optarg) != 0)
				usage();
			break;
		case 'T': testdir = optarg; break;
		default: usage();
		}
	openlog("pro1x-autosleep", LOG_PID, LOG_DAEMON);
	if (!fg && daemon(0, 0) != 0)
		return (1);
	if ((f = fopen(PIDFILE, "w")) != NULL) {
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
	}
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGUSR1, on_signal);
	st.since_ms = now_ms();
	logm(LOG_NOTICE, "on: after %u s with the display off%s; up %u s after "
	    "a wake-up source, %u s after other interrupts%s", conf.delay_s,
	    conf.in_standby ? " or the stand-by screen" : "", conf.stay_s,
	    conf.settle_s, conf.on_power ? "; also on external power" : "");
	while (!quit) {
		if (dump) {
			dump = 0;
			stats_write();
			if ((f = fopen(STATS, "r")) != NULL) {
				while (fgets(last, sizeof(last), f) != NULL) {
					last[strcspn(last, "\n")] = '\0';
					logm(LOG_NOTICE, "%s", last);
				}
				fclose(f);
			}
		}
		look(&s);
		wait = as_decide(&track, &s, &conf, why, sizeof(why));
		if (wait > 0) {
			if (verbose || strcmp(why, was) != 0)
				logm(verbose ? LOG_INFO : LOG_DEBUG, "up: %s "
				    "(again in %d ms)", why, wait);
			strlcpy(was, why, sizeof(was));
			sleep_ms(wait);
			continue;
		}
		was[0] = '\0';
		r = suspend();
		if (r == 3) {
			/* an alarm within 70 s: after it */
			st.refused++;
			track.awake_until = now_ms() + 30000;
			continue;
		}
		last_suspend(last, sizeof(last));
		chomp(last);
		how = as_after(&track, last, &conf, now_ms(), &intid, &ms);
		stats_add(how, intid, ms);
		if (intid != 0)
			intr_name(intid, name, sizeof(name));
		else
			strlcpy(name, how == AS_END_DEADLINE ? "the deadline" :
			    "a wake-up", sizeof(name));
		logm(LOG_INFO, "%s%s [%s]; up %u s", how == AS_END_FAILED ?
		    "not suspended: " : "", last[0] != '\0' ? last :
		    "(no hw.pro1x_pm.last)", name, (unsigned)
		    ((track.awake_until - now_ms() + 500) / 1000));
		if (r != 0)
			logm(LOG_WARNING, "pro1x-suspend: exit %d", r);
		stats_write();
	}
	stats_write();
	unlink(PIDFILE);
	logm(LOG_NOTICE, "off: %u suspends, %ju s asleep", st.suspends,
	    (uintmax_t)(st.asleep_ms / 1000));
	return (0);
}
