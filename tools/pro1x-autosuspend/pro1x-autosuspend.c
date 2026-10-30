/*
 * pro1x-autosuspend -- let the Pro1X sleep while nobody uses it.
 *
 * Android suspends a phone whenever nothing holds it awake. Maemo Leste and
 * the MeeGo session never suspend, so the SoC never reaches its low-power
 * states (qcom_stats' vmin count stays 0), even with the display off. This
 * daemon suspends the system (s2idle, the mainline kernel's only sleep
 * state) once the display has been off for Delay seconds and nothing needs
 * the system awake:
 *
 *   - a call (mce's call state is not "none")
 *   - sound being played or recorded (an ALSA substream RUNNING)
 *   - a USB cable (the gadget not "not attached"): before kernel patch
 *     0032 (pro1x.10) the USB gadget did not come back from a suspend (its
 *     VBUS override was lost with the controller's power domain);
 *     SuspendOnUSB lets it suspend anyway
 *   - a file in /run/pro1x-autosuspend/inhibit
 *   - wake-up events in progress and userspace wake locks
 *     (/sys/power/wake_lock), through the wakeup_count handshake
 *
 * The kernel's wake-up sources resume it: the power key, the keyboard slide
 * and side keys, RTC alarms (alarmd, timed), modem messages and modem data,
 * USB and the charger, Bluetooth. A CLOCK_BOOTTIME_ALARM timer also wakes it
 * every WakeInterval seconds, so that mail and messengers can catch up;
 * after any wake-up it stays up StayAwake seconds before it may sleep again.
 * Each suspend is logged with its length, the wake-up interrupt and how
 * often the SoC reached its sleep states (qcom_stats vlow and vmin).
 *
 * Around each suspend (also with --test):
 *   - StopServices are stopped and started again after it. Not set: only
 *     hexagonrpcd, and only on a kernel without kernel patch 0031
 *     (/sys/module/fastrpc/parameters/freezable_invoke Y): there the
 *     freezer interrupts hexagonrpcd's FastRPC call, the DSP gets it twice
 *     and the ADSP's sensor process crashes at the thaw (and audio stays
 *     broken until a reboot); postmarketOS and Debian stop it too
 *   - the devices in NoWakeup cannot wake the system: the Bluetooth UART,
 *     whose wake-up interrupt woke the first suspend at once, unless
 *     Bluetooth's own wake-up switch is on (the power/wakeup of the serdev
 *     controller on it, /sys/bus/serial/devices/serial0): then it is left
 *     alone, and with kernel patch 0030 the UART wakes the system only
 *     while Bluetooth is up
 *
 *   pro1x-autosuspend             run (/etc/init.d/pro1x-autosuspend)
 *   pro1x-autosuspend --test N    suspend once now with a wake-up after N s,
 *                                 also while USB is connected
 *
 * Settings: /etc/pro1x-autosuspend.conf, group [autosuspend] with Delay,
 * WakeInterval (0: no periodic wake-up) and StayAwake, all in seconds;
 * StopServices (OpenRC services; not set: see above, empty: none) and
 * NoWakeup (platform devices) as lists; SuspendOnUSB (true/false).
 */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <gio/gio.h>
#include <glib-unix.h>

#define CONF "/etc/pro1x-autosuspend.conf"
#define INHIBIT_DIR "/run/pro1x-autosuspend/inhibit"
#define STATS "/sys/kernel/debug/qcom_stats/"
#define FASTRPC_FIXED "/sys/module/fastrpc/parameters/freezable_invoke"
#define BUSY_RECHECK_S 30

static int delay_s = 30, wake_interval_s = 900, stay_awake_s = 45;
static gboolean suspend_on_usb;
/* stop_services NULL: hexagonrpcd only on kernels without patch 0031 */
static char **stop_services, **no_wakeup;
static gboolean display_off, call_active;
static gint64 display_off_us, awake_until_us;
static guint check_id;
static int alarm_fd = -1;
static const char *last_busy;

/* --- the system's state ----------------------------------------------------- */

static gboolean glob_any(const char *pattern, const char *needle)
{
	glob_t g;
	gboolean found = FALSE;

	if (glob(pattern, 0, NULL, &g) != 0)
		return FALSE;
	for (size_t i = 0; i < g.gl_pathc && !found; i++) {
		g_autofree char *s = NULL;
		if (g_file_get_contents(g.gl_pathv[i], &s, NULL, NULL))
			found = strstr(s, needle) != NULL;
	}
	globfree(&g);
	return found;
}

static gboolean inhibited(void)
{
	g_autoptr(GDir) dir = g_dir_open(INHIBIT_DIR, 0, NULL);

	return dir && g_dir_read_name(dir);
}

/*
 * A cable in the USB port: the gadget's state is anything but "not
 * attached", or a USB or mains supply is online (a charger without a host)
 */
static gboolean usb_cable(void)
{
	glob_t g;
	gboolean found = FALSE;

	if (glob("/sys/class/udc/*/state", 0, NULL, &g) == 0) {
		for (size_t i = 0; i < g.gl_pathc && !found; i++) {
			g_autofree char *s = NULL;
			if (g_file_get_contents(g.gl_pathv[i], &s, NULL, NULL))
				found = strcmp(g_strstrip(s), "not attached") != 0;
		}
		globfree(&g);
	}
	if (!found && glob("/sys/class/power_supply/*/type", 0, NULL, &g) == 0) {
		for (size_t i = 0; i < g.gl_pathc && !found; i++) {
			g_autofree char *type = NULL, *online = NULL, *dir = g_path_get_dirname(g.gl_pathv[i]);
			g_autofree char *path = g_build_filename(dir, "online", NULL);

			if (!g_file_get_contents(g.gl_pathv[i], &type, NULL, NULL) ||
			    !g_file_get_contents(path, &online, NULL, NULL))
				continue;
			g_strstrip(type);
			found = (g_str_has_prefix(type, "USB") || !strcmp(type, "Mains")) &&
				!strcmp(g_strstrip(online), "1");
		}
		globfree(&g);
	}
	return found;
}

/* Why the system has to stay up, or NULL */
static const char *busy(void)
{
	if (glob_any("/proc/asound/card*/pcm*/sub*/status", "state: RUNNING"))
		return "sound";
	if (glob_any("/sys/class/udc/*/state", "configured"))
		return "a USB data connection";
	if (!suspend_on_usb && usb_cable())
		return "a USB cable";
	if (inhibited())
		return "a file in " INHIBIT_DIR;
	return NULL;
}

static guint64 stat_count(const char *name)
{
	g_autofree char *path = g_strconcat(STATS, name, NULL);
	g_autofree char *s = NULL;
	const char *c;

	if (!g_file_get_contents(path, &s, NULL, NULL) || !(c = strstr(s, "Count:")))
		return 0;
	return g_ascii_strtoull(c + 6, NULL, 10);
}

/* "irq 204 glink-smem" */
static char *wakeup_irq(void)
{
	g_autofree char *irq = NULL, *path = NULL, *actions = NULL;

	if (!g_file_get_contents("/sys/power/pm_wakeup_irq", &irq, NULL, NULL))
		return g_strdup("no interrupt recorded");
	g_strstrip(irq);
	path = g_strdup_printf("/sys/kernel/irq/%s/actions", irq);
	if (g_file_get_contents(path, &actions, NULL, NULL))
		g_strstrip(actions);
	return g_strdup_printf("irq %s %s", irq, actions ? actions : "");
}

/* --- suspending --------------------------------------------------------------- */

static int write_str(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC), err = 0;

	if (fd < 0)
		return errno;
	if (write(fd, s, strlen(s)) < 0)
		err = errno;
	close(fd);
	return err;
}

/* Blocks while wake-up events are in progress (kernel or wake locks). */
static gboolean read_wakeup_count(char *buf, size_t size)
{
	int fd = open("/sys/power/wakeup_count", O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return FALSE;
	do
		n = read(fd, buf, size - 1);
	while (n < 0 && errno == EINTR);
	close(fd);
	if (n <= 0)
		return FALSE;
	buf[n] = 0;
	g_strstrip(buf);
	return TRUE;
}

static double seconds(const struct timespec *a, const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) + (b->tv_nsec - a->tv_nsec) / 1e9;
}

static void arm_alarm(int s)
{
	struct itimerspec its = { .it_value = { .tv_sec = s } };

	if (alarm_fd >= 0)
		timerfd_settime(alarm_fd, 0, &its, NULL);
}

/* rc-service NAME ACTION; TRUE on success */
static gboolean rc_service(const char *name, const char *action)
{
	const char *argv[] = { "rc-service", "--quiet", name, action, NULL };
	int status = -1;

	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
			  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, NULL))
		return FALSE;
	return g_spawn_check_wait_status(status, NULL);
}

/* Kernel patch 0031: a frozen hexagonrpcd no longer crashes the ADSP */
static gboolean fastrpc_freezes_in_place(void)
{
	g_autofree char *s = NULL;

	return g_file_get_contents(FASTRPC_FIXED, &s, NULL, NULL) && s[0] == 'Y';
}

/*
 * Bluetooth's own wake-up switch for a UART: the power/wakeup of the serdev
 * controller on it (hci_qca asks it whether to set up wake-ups; kernel
 * patch 0030 arms the UART's wake-up interrupt for a suspend only then)
 */
static gboolean serdev_may_wake(const char *dev)
{
	/* the serial core's devices in between: <dev>:0 and <dev>:0.0 */
	g_autofree char *pattern =
		g_strdup_printf("/sys/bus/platform/devices/%s/%s:*/%s:*/serial[0-9]*/power/wakeup",
				dev, dev, dev);

	return glob_any(pattern, "enabled");
}

/*
 * Before a suspend: the services that must not run during it stopped, the
 * devices that must not wake it without their wake-up. stopped and wakeup
 * remember what to give back (restore_after).
 */
static void prepare_suspend(GPtrArray *stopped, GPtrArray *wakeup)
{
	static char *hexagonrpcd[] = { "pro1x-hexagonrpcd", NULL };
	char **services = stop_services;

	if (!services)
		services = fastrpc_freezes_in_place() ? NULL : hexagonrpcd;
	for (char **n = services; n && *n; n++) {
		if (!**n || !rc_service(*n, "status"))
			continue;
		if (rc_service(*n, "stop"))
			g_ptr_array_add(stopped, g_strdup(*n));
		else
			syslog(LOG_WARNING, "could not stop %s", *n);
	}
	for (char **d = no_wakeup; d && *d; d++) {
		g_autofree char *path = g_strdup_printf("/sys/bus/platform/devices/%s/power/wakeup", *d);
		g_autofree char *was = NULL;

		if (!**d || !g_file_get_contents(path, &was, NULL, NULL) || serdev_may_wake(*d))
			continue;
		if (!strcmp(g_strstrip(was), "enabled") && write_str(path, "disabled") == 0)
			g_ptr_array_add(wakeup, g_steal_pointer(&path));
	}
}

static void restore_after(GPtrArray *stopped, GPtrArray *wakeup)
{
	for (guint i = 0; i < wakeup->len; i++)
		write_str(wakeup->pdata[i], "enabled");
	for (guint i = 0; i < stopped->len; i++)
		if (!rc_service(stopped->pdata[i], "start"))
			syslog(LOG_WARNING, "could not start %s again", (char *)stopped->pdata[i]);
}

/*
 * One suspend with a wake-up timer after alarm_s seconds (0: none).
 * Returns TRUE when the system slept.
 */
static gboolean suspend_once(int alarm_s)
{
	char count[32];
	struct timespec boot0, mono0, boot1, mono1;
	guint64 vlow0 = stat_count("vlow"), vmin0 = stat_count("vmin"), expirations;
	g_autoptr(GPtrArray) stopped = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) wakeup = g_ptr_array_new_with_free_func(g_free);
	int err;

	syslog(LOG_DEBUG, "reading wakeup_count");
	if (!read_wakeup_count(count, sizeof(count))) {
		syslog(LOG_WARNING, "cannot read /sys/power/wakeup_count");
		return FALSE;
	}
	/* before the handshake: stopping a service may itself be a wake-up event */
	prepare_suspend(stopped, wakeup);
	if (!read_wakeup_count(count, sizeof(count))) {
		syslog(LOG_WARNING, "cannot read /sys/power/wakeup_count");
		restore_after(stopped, wakeup);
		return FALSE;
	}
	arm_alarm(alarm_s);
	/* fails when a wake-up event came after the read */
	if (write_str("/sys/power/wakeup_count", count) != 0) {
		syslog(LOG_INFO, "wake-up event in progress, not suspending");
		arm_alarm(0);
		restore_after(stopped, wakeup);
		return FALSE;
	}
	syslog(LOG_INFO, "suspending");
	clock_gettime(CLOCK_BOOTTIME, &boot0);
	clock_gettime(CLOCK_MONOTONIC, &mono0);
	err = write_str("/sys/power/state", "mem");
	clock_gettime(CLOCK_BOOTTIME, &boot1);
	clock_gettime(CLOCK_MONOTONIC, &mono1);
	arm_alarm(0);
	if (alarm_fd >= 0)
		while (read(alarm_fd, &expirations, sizeof(expirations)) > 0)
			;
	restore_after(stopped, wakeup);
	if (err) {
		syslog(LOG_WARNING, "suspend failed: %s", strerror(err));
		return FALSE;
	}
	g_autofree char *irq = wakeup_irq();
	/* the monotonic clock stops in suspend, the boot time clock does not */
	syslog(LOG_INFO, "resumed after %.1f s asleep (%s); SoC sleep: vlow +%" G_GUINT64_FORMAT
	       ", vmin +%" G_GUINT64_FORMAT, seconds(&boot0, &boot1) - seconds(&mono0, &mono1), irq,
	       stat_count("vlow") - vlow0, stat_count("vmin") - vmin0);
	return TRUE;
}

/* --- when to suspend ----------------------------------------------------------- */

static gboolean check_cb(gpointer data);

static void schedule_check(gint64 wait_us)
{
	if (check_id)
		g_source_remove(check_id);
	check_id = g_timeout_add_seconds(wait_us > 0 ? (wait_us + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC : 0,
					 check_cb, NULL);
}

static void reschedule(void)
{
	gint64 due;

	if (!display_off || call_active) {
		if (check_id)
			g_source_remove(check_id);
		check_id = 0;
		last_busy = NULL;
		return;
	}
	due = MAX(display_off_us + delay_s * G_USEC_PER_SEC, awake_until_us);
	schedule_check(due - g_get_monotonic_time());
}

static gboolean check_cb(gpointer data)
{
	const char *why;

	(void)data;
	check_id = 0;
	if (!display_off || call_active)
		return G_SOURCE_REMOVE;
	why = busy();
	if (why) {
		if (why != last_busy)
			syslog(LOG_INFO, "staying up for %s", why);
		last_busy = why;
		schedule_check(BUSY_RECHECK_S * G_USEC_PER_SEC);
		return G_SOURCE_REMOVE;
	}
	last_busy = NULL;
	suspend_once(wake_interval_s);
	awake_until_us = g_get_monotonic_time() + stay_awake_s * G_USEC_PER_SEC;
	reschedule();
	return G_SOURCE_REMOVE;
}

/* --- mce ------------------------------------------------------------------------- */

static void display_state(const char *state)
{
	gboolean off = !g_strcmp0(state, "off");

	if (off && !display_off)
		display_off_us = g_get_monotonic_time();
	display_off = off;
	reschedule();
}

static void call_state(const char *state)
{
	call_active = g_strcmp0(state, "none") != 0;
	reschedule();
}

static void mce_sig(GDBusConnection *conn, const char *sender, const char *path, const char *iface,
		    const char *signal, GVariant *params, gpointer data)
{
	const char *state;

	(void)conn, (void)sender, (void)path, (void)iface, (void)data;
	if (!g_strcmp0(signal, "display_status_ind") && g_variant_is_of_type(params, G_VARIANT_TYPE("(s)"))) {
		g_variant_get(params, "(&s)", &state);
		display_state(state);
	} else if (!g_strcmp0(signal, "sig_call_state_ind") &&
		   g_variant_is_of_type(params, G_VARIANT_TYPE("(ss)"))) {
		g_variant_get(params, "(&s&s)", &state, NULL);
		call_state(state);
	}
}

/* Without an answer the display counts as on and the call as active: no suspend. */
static void display_reply(GObject *src, GAsyncResult *res, gpointer data)
{
	g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
	const char *state = "on";

	(void)data;
	if (r)
		g_variant_get(r, "(&s)", &state);
	display_state(state);
}

static void call_reply(GObject *src, GAsyncResult *res, gpointer data)
{
	g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
	const char *state = "unknown";

	(void)data;
	if (r)
		g_variant_get(r, "(&s&s)", &state, NULL);
	call_state(state);
}

static void mce_call(GDBusConnection *bus, const char *method, const char *type, GAsyncReadyCallback cb)
{
	g_dbus_connection_call(bus, "com.nokia.mce", "/com/nokia/mce/request", "com.nokia.mce.request",
			       method, NULL, G_VARIANT_TYPE(type), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, cb, NULL);
}

/* --------------------------------------------------------------------------------- */

static void load_config(void)
{
	g_autoptr(GKeyFile) kf = g_key_file_new();
	g_autoptr(GError) err = NULL;
	int v;

	if (!g_key_file_load_from_file(kf, CONF, G_KEY_FILE_NONE, NULL))
		return;
	v = g_key_file_get_integer(kf, "autosuspend", "Delay", &err);
	if (!err && v >= 0)
		delay_s = v;
	g_clear_error(&err);
	v = g_key_file_get_integer(kf, "autosuspend", "WakeInterval", &err);
	if (!err && v >= 0)
		wake_interval_s = v;
	g_clear_error(&err);
	v = g_key_file_get_integer(kf, "autosuspend", "StayAwake", &err);
	if (!err && v >= 0)
		stay_awake_s = v;
	g_clear_error(&err);
	v = g_key_file_get_boolean(kf, "autosuspend", "SuspendOnUSB", &err);
	if (!err)
		suspend_on_usb = v;
	if (g_key_file_has_key(kf, "autosuspend", "StopServices", NULL)) {
		g_strfreev(stop_services);
		stop_services = g_key_file_get_string_list(kf, "autosuspend", "StopServices", NULL, NULL);
	}
	if (g_key_file_has_key(kf, "autosuspend", "NoWakeup", NULL)) {
		g_strfreev(no_wakeup);
		no_wakeup = g_key_file_get_string_list(kf, "autosuspend", "NoWakeup", NULL, NULL);
	}
}

static gboolean quit_cb(gpointer loop)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
	gboolean test = argc == 3 && !strcmp(argv[1], "--test");
	GMainLoop *loop;
	GDBusConnection *bus;

	if (argc != 1 && !test) {
		fprintf(stderr, "usage: pro1x-autosuspend [--test SECONDS]\n");
		return 2;
	}
	openlog("pro1x-autosuspend", LOG_PID | (test ? LOG_PERROR : 0), LOG_DAEMON);
	alarm_fd = timerfd_create(CLOCK_BOOTTIME_ALARM, TFD_CLOEXEC | TFD_NONBLOCK);
	if (alarm_fd < 0)
		syslog(LOG_WARNING, "no wake-up timer (%m): no periodic wake-ups");
	no_wakeup = g_strsplit("4a8c000.serial", ";", -1);
	load_config();
	if (test)
		return suspend_once(atoi(argv[2])) ? 0 : 1;

	syslog(LOG_INFO, "suspending %d s after the display goes off; wake-ups every %d s, then up for %d s",
	       delay_s, wake_interval_s, stay_awake_s);
	loop = g_main_loop_new(NULL, FALSE);
	bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
	if (!bus) {
		syslog(LOG_ERR, "no system bus");
		return 1;
	}
	g_dbus_connection_signal_subscribe(bus, NULL, "com.nokia.mce.signal", NULL, "/com/nokia/mce/signal",
					   NULL, G_DBUS_SIGNAL_FLAGS_NONE, mce_sig, NULL, NULL);
	mce_call(bus, "get_display_status", "(s)", display_reply);
	mce_call(bus, "get_call_state", "(ss)", call_reply);
	g_unix_signal_add(SIGTERM, quit_cb, loop);
	g_unix_signal_add(SIGINT, quit_cb, loop);
	g_main_loop_run(loop);
	return 0;
}
