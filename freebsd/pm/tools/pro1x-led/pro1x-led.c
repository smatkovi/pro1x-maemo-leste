/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pro1x-led: the Pro1X's RGB status LED as Maemo 5's notification light,
 * on FreeBSD (freebsd/pm/README.md, stage 6).
 *
 * Maemo's notifications ask MCE for an LED pattern: hildon-home, for a
 * missed call, an SMS, a chat or mail message, a reminder, calls
 * com.nokia.mce.request.req_led_pattern_activate("PatternCommunicationSMS")
 * on the system bus, and req_led_pattern_deactivate once they have been
 * seen. FreeBSD has no MCE: those calls come back as ServiceUnknown, as
 * before. This daemon does not take MCE's name (many programs ask MCE for
 * other things and handle its absence); it watches the bus as a monitor
 * (BecomeMonitor, root) for those calls only, and keeps the LED in the
 * colour of the most important pattern still pending: blue for calls,
 * messages and reminders (Leste's mce.ini, its priorities), and from the
 * battery (pro1x_battery's devd events, as rc.d/pro1x_statusled had them)
 * orange while charging, green when full, red when low. Nothing pending:
 * off. No "device on" pattern, no blinking: led(4)'s blinking is a kernel
 * timer ten times a second, so the colour stays steady and the daemon
 * sleeps until a call or an event comes (the LPG's own slow PWM could
 * blink without the CPU: README).
 *
 * Before a suspend pro1x-suspend switches the LED off and sends SIGUSR1
 * (nothing is shown until SIGUSR2, which it sends afterwards); SIGHUP reads
 * the battery again.
 *
 *   pro1x-led [-d] [-B percent] [-L percent] [-T dir]
 *     -d  in the foreground, the log on stderr too
 *     -B  the brightness of every colour, percent of the pattern's (100)
 *     -L  the battery low below or at this capacity, not on a charger (10)
 *     -T  a test: the LED's duty cycles into dir/red, dir/green, dir/blue,
 *         the battery from dir/status, dir/capacity, dir/usb (SIGHUP), no
 *         devd
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/un.h>

#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define	PIDFILE		"/var/run/pro1x-led.pid"
#define	DEVD_PIPE	"/var/run/devd.seqpacket.pipe"
#define	MCE_REQUEST	"com.nokia.mce.request"
#define	DUTY_MAX	511	/* qcom_lpg's 9-bit PWM */

struct pattern {
	const char	*name;
	int		prio;		/* lower first, as in mce.ini */
	int		rgb[3];
	bool		active;
};

/*
 * Leste's mce.ini [LEDGenericSoftware], its priorities and colours,
 * without PatternDeviceOn, PatternPowerOn/Off, PatternError and the rest
 * that are no notification; PatternBatteryLow is this daemon's own
 */
static struct pattern patterns[] = {
	{ "PatternCommunicationCall",	25, { 0, 0, 255 }, false },
	{ "PatternCommunicationSMS",	26, { 0, 0, 255 }, false },
	{ "PatternCommunicationEmail",	27, { 0, 0, 255 }, false },
	{ "PatternCommunicationIM",	28, { 0, 0, 255 }, false },
	{ "PatternCommonNotification",	30, { 0, 0, 255 }, false },
	{ "PatternBatteryLow",		35, { 255, 0, 0 }, false },
	{ "PatternBatteryFull",		40, { 0, 255, 0 }, false },
	{ "PatternBatteryCharging",	50, { 255, 10, 0 }, false },
};
#define	NPATTERNS	(sizeof(patterns) / sizeof(patterns[0]))

static const char *const colour[3] = { "red", "green", "blue" };
static int duty_now[3] = { -1, -1, -1 };
static const struct pattern *shown;
static bool shown_once, enabled = true, suspended;
static int brightness = 100, low = 10, fg;
static const char *testdir;
static int sigpipe[2] = { -1, -1 };

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
write_file(const char *path, const char *text)
{
	int fd;

	if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
		return;
	(void)write(fd, text, strlen(text));
	close(fd);
}

/* one colour: its duty cycle (qcom_lpg), then on or off (led(4)) */
static void
led_set(int i, int duty)
{
	char path[256], val[16];
	u_int d = (u_int)duty;

	if (duty == duty_now[i])
		return;
	duty_now[i] = duty;
	snprintf(val, sizeof(val), "%d\n", duty);
	if (testdir != NULL) {
		snprintf(path, sizeof(path), "%s/%s", testdir, colour[i]);
		write_file(path, val);
		return;
	}
	snprintf(path, sizeof(path), "dev.qcom_lpg.0.%s", colour[i]);
	(void)sysctlbyname(path, NULL, NULL, &d, sizeof(d));
	snprintf(path, sizeof(path), "/dev/led/status_%s", colour[i]);
	write_file(path, duty > 0 ? "1\n" : "0\n");
}

static void
apply(void)
{
	const struct pattern *best = NULL;
	unsigned i;
	int c;

	if (enabled && !suspended)
		for (i = 0; i < NPATTERNS; i++)
			if (patterns[i].active &&
			    (best == NULL || patterns[i].prio < best->prio))
				best = &patterns[i];
	for (c = 0; c < 3; c++)
		led_set(c, best == NULL ? 0 : best->rgb[c] * DUTY_MAX *
		    brightness / (255 * 100));
	if (!suspended && (best != shown || !shown_once))
		logm(LOG_INFO, best != NULL ? "showing %s" : "off%s",
		    best != NULL ? best->name : "");
	if (!suspended) {
		shown = best;
		shown_once = true;
	}
}

static struct pattern *
pattern_named(const char *name)
{
	unsigned i;

	for (i = 0; i < NPATTERNS; i++)
		if (strcmp(patterns[i].name, name) == 0)
			return (&patterns[i]);
	return (NULL);
}

static void
set_pattern(const char *name, bool on)
{
	struct pattern *p = pattern_named(name);

	if (p != NULL)
		p->active = on;
}

/* the battery's state: status (charging, full, ...), capacity, charger */
static void
battery(const char *status, int capacity, int usb)
{
	bool charging = strcmp(status, "charging") == 0;
	bool full = strcmp(status, "full") == 0;

	set_pattern("PatternBatteryCharging", charging);
	set_pattern("PatternBatteryFull", full);
	set_pattern("PatternBatteryLow", !charging && !full && usb != 1 &&
	    capacity >= 0 && capacity <= low);
}

static bool
read_text(const char *name, char *buf, size_t len)
{
	char path[256];
	ssize_t n;
	int fd;

	buf[0] = '\0';
	if (testdir != NULL) {
		snprintf(path, sizeof(path), "%s/%s", testdir, name);
		if ((fd = open(path, O_RDONLY)) < 0)
			return (false);
		n = read(fd, buf, len - 1);
		close(fd);
		if (n <= 0)
			return (false);
		buf[n] = '\0';
		buf[strcspn(buf, "\n")] = '\0';
		return (true);
	}
	snprintf(path, sizeof(path), "dev.pro1x_battery.0.%s", name);
	if (strcmp(name, "status") == 0)
		return (sysctlbyname(path, buf, &len, NULL, 0) == 0);
	{
		/* an int (capacity) or a bool (usb) */
		union { int i; uint8_t b; } v = { 0 };
		size_t l = sizeof(v);

		if (sysctlbyname(path, &v, &l, NULL, 0) != 0)
			return (false);
		snprintf(buf, len, "%d", l == 1 ? v.b : v.i);
	}
	return (true);
}

static void
battery_now(void)
{
	char st[32], cap[16], usb[16];

	if (!read_text("status", st, sizeof(st)))
		return;
	battery(st, read_text("capacity", cap, sizeof(cap)) ? atoi(cap) : -1,
	    read_text("usb", usb, sizeof(usb)) ? atoi(usb) : -1);
}

/* "!system=PRO1X subsystem=BATTERY type=CHANGE status=X capacity=N usb=U" */
static void
devd_event(char *ev)
{
	char st[32] = "", *p;
	int cap = -1, usb = -1;

	if (strstr(ev, "system=PRO1X") == NULL ||
	    strstr(ev, "subsystem=BATTERY") == NULL)
		return;
	if ((p = strstr(ev, "status=")) != NULL)
		sscanf(p + 7, "%31[a-z]", st);
	if ((p = strstr(ev, "capacity=")) != NULL)
		cap = atoi(p + 9);
	if ((p = strstr(ev, "usb=")) != NULL)
		usb = atoi(p + 4);
	if (st[0] != '\0') {
		battery(st, cap, usb);
		apply();
	}
}

static int
devd_open(void)
{
	struct sockaddr_un sun;
	int s;

	if (testdir != NULL)
		return (-1);
	if ((s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)) < 0)
		return (-1);
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, DEVD_PIPE, sizeof(sun.sun_path));
	if (connect(s, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
		close(s);
		return (-1);
	}
	return (s);
}

/* the system bus as a monitor of MCE's LED requests */
static DBusConnection *
bus_open(void)
{
	static const char *const rules[] = {
		"type='method_call',interface='" MCE_REQUEST "',"
		    "member='req_led_pattern_activate'",
		"type='method_call',interface='" MCE_REQUEST "',"
		    "member='req_led_pattern_deactivate'",
		"type='method_call',interface='" MCE_REQUEST "',"
		    "member='req_led_enable'",
		"type='method_call',interface='" MCE_REQUEST "',"
		    "member='req_led_disable'",
	};
	DBusConnection *c;
	DBusMessage *m, *r;
	DBusMessageIter it, arr;
	DBusError err;
	dbus_uint32_t flags = 0;
	unsigned i;

	dbus_error_init(&err);
	if ((c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err)) == NULL) {
		dbus_error_free(&err);
		return (NULL);
	}
	dbus_connection_set_exit_on_disconnect(c, FALSE);
	m = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
	    "org.freedesktop.DBus.Monitoring", "BecomeMonitor");
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
	for (i = 0; i < sizeof(rules) / sizeof(rules[0]); i++)
		dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING,
		    &rules[i]);
	dbus_message_iter_close_container(&it, &arr);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &flags);
	r = dbus_connection_send_with_reply_and_block(c, m, 5000, &err);
	dbus_message_unref(m);
	if (r == NULL) {
		logm(LOG_WARNING, "BecomeMonitor: %s", err.message);
		dbus_error_free(&err);
		dbus_connection_close(c);
		dbus_connection_unref(c);
		return (NULL);
	}
	dbus_message_unref(r);
	return (c);
}

static void
bus_message(DBusMessage *m)
{
	const char *member, *name = NULL;
	bool on;

	if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL ||
	    !dbus_message_has_interface(m, MCE_REQUEST) ||
	    (member = dbus_message_get_member(m)) == NULL)
		return;
	if (strcmp(member, "req_led_enable") == 0 ||
	    strcmp(member, "req_led_disable") == 0) {
		enabled = strcmp(member, "req_led_enable") == 0;
		logm(LOG_INFO, "LED %s", enabled ? "enabled" : "disabled");
		apply();
		return;
	}
	if (strcmp(member, "req_led_pattern_activate") == 0)
		on = true;
	else if (strcmp(member, "req_led_pattern_deactivate") == 0)
		on = false;
	else
		return;
	if (!dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &name,
	    DBUS_TYPE_INVALID) || name == NULL)
		return;
	if (pattern_named(name) == NULL) {
		logm(LOG_DEBUG, "%s %s: not a pattern of this LED", member, name);
		return;
	}
	set_pattern(name, on);
	apply();
}

static void
on_signal(int sig)
{
	char c = sig == SIGUSR1 ? 'o' : sig == SIGUSR2 ? 'n' :
	    sig == SIGHUP ? 'h' : 'q';
	int e = errno;

	(void)write(sigpipe[1], &c, 1);
	errno = e;
}

static void
usage(void)
{
	fprintf(stderr, "usage: pro1x-led [-d] [-B percent] [-L percent] "
	    "[-T dir]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	DBusConnection *bus = NULL;
	DBusMessage *m;
	struct pollfd pfd[3];
	char ev[1024], c;
	ssize_t n;
	int ch, s = -1, fd, timeout;
	bool quit = false;
	FILE *f;

	while ((ch = getopt(argc, argv, "dB:L:T:")) != -1)
		switch (ch) {
		case 'd': fg = 1; break;
		case 'B': brightness = atoi(optarg); break;
		case 'L': low = atoi(optarg); break;
		case 'T': testdir = optarg; break;
		default: usage();
		}
	if (brightness < 0 || brightness > 100)
		usage();
	openlog("pro1x-led", LOG_PID, LOG_DAEMON);
	if (!fg && daemon(0, 0) != 0)
		return (1);
	if (pipe2(sigpipe, O_CLOEXEC | O_NONBLOCK) != 0)
		return (1);
	signal(SIGUSR1, on_signal);
	signal(SIGUSR2, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	if (testdir == NULL && (f = fopen(PIDFILE, "w")) != NULL) {
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
	}
	battery_now();
	apply();
	logm(LOG_NOTICE, "on: Maemo's LED patterns from the system bus, the "
	    "battery from pro1x_battery (low at %d %%), %d %% brightness",
	    low, brightness);
	while (!quit) {
		if (bus == NULL && (bus = bus_open()) == NULL)
			logm(LOG_WARNING, "no system bus yet: again in 30 s");
		if (s < 0 && testdir == NULL && (s = devd_open()) < 0)
			logm(LOG_WARNING, "no devd yet: again in 30 s");
		/* what libdbus has read already */
		while (bus != NULL && (m = dbus_connection_pop_message(bus)) !=
		    NULL) {
			bus_message(m);
			dbus_message_unref(m);
		}
		pfd[0] = (struct pollfd){ .fd = sigpipe[0], .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = -1, .events = POLLIN };
		pfd[2] = (struct pollfd){ .fd = s, .events = POLLIN };
		if (bus != NULL && dbus_connection_get_unix_fd(bus, &fd))
			pfd[1].fd = fd;
		/* a timer only while the bus or devd is missing */
		timeout = (bus == NULL || (s < 0 && testdir == NULL)) ?
		    30000 : -1;
		if (poll(pfd, 3, timeout) < 0 && errno != EINTR)
			break;
		if (pfd[0].revents & POLLIN)
			while (read(sigpipe[0], &c, 1) == 1)
				switch (c) {
				case 'o':
					/* pro1x-suspend: off until 'n' */
					suspended = true;
					apply();
					break;
				case 'n':
					suspended = false;
					apply();
					break;
				case 'h':
					battery_now();
					apply();
					break;
				default:
					quit = true;
				}
		if (bus != NULL && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
			(void)dbus_connection_read_write(bus, 0);
			if (!dbus_connection_get_is_connected(bus)) {
				logm(LOG_WARNING, "the system bus went away");
				dbus_connection_unref(bus);
				bus = NULL;
			}
		}
		if (s >= 0 && (pfd[2].revents & (POLLIN | POLLHUP | POLLERR))) {
			if ((n = recv(s, ev, sizeof(ev) - 1, 0)) > 0) {
				ev[n] = '\0';
				devd_event(ev);
			} else {
				close(s);
				s = -1;
			}
		}
	}
	suspended = true;
	for (ch = 0; ch < 3; ch++)
		led_set(ch, 0);
	if (testdir == NULL)
		unlink(PIDFILE);
	logm(LOG_NOTICE, "off");
	return (0);
}
