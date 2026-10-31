/*
 * The Pro1X's accelerometer, proximity sensor, keyboard slide switch and
 * side keys as the N9/N950 reported them, for Harmattan's sensord,
 * contextkit and qmkeyd. Runs on Leste, as root, during the MeeGo session.
 *
 * The Pro1X sensors sit behind the ADSP's sensor core (libssc); the N9's
 * were evdev devices, which Harmattan still expects. One uinput device,
 * named like the N9's accelerometer, carries them all:
 *
 *   EV_ABS X/Y/Z in mG     sensord's accelerometeradaptor ("accelerometer"
 *                          in the name)
 *   SW_FRONT_PROXIMITY     sensord's proximityadaptor-evdev
 *                          (proximity-evdev_match_name, sensors/95-pro1x.conf)
 *   SW_KEYPAD_SLIDE        contextkit's kbslider plugin, through the udev
 *                          link /dev/input/gpio-keys: set while the keyboard
 *                          is closed, the N950's sense - the Pro1X's
 *                          gpio-keys sets it while the keyboard is out
 *   KEY_VOLUMEUP/DOWN,     qmkeyd, also through /dev/input/gpio-keys (the
 *   KEY_CAMERA             N9 had them on its gpio-keys); copied from the
 *                          Pro1X's gpio-keys and pm8941_resin
 *
 * Leste's mce skips input devices with that name, so these events neither
 * count as user activity nor as its own keyboard slide; X ignores the
 * device (90-pro1x-n9-input.rules), so it gets the keys only once.
 *
 * Axes: corrected with the Pro1X's mount matrix (x and y flipped, as
 * verified by the NixOS port and set for iio-sensor-proxy in
 * 81-pro1x-sensors.rules), the readings are in the frame of the panel in
 * portrait (x right, y up, z out of the screen). In the landscape screen of
 * the MeeGo session that is x up, y left - the N950's own frame.
 * sensord's acc_trans_matrix and orientation_offset of the N950 rootfs then
 * give Screen.TopEdge "top" with the keyboard towards the user.
 *
 * sensord starts with TopEdge "left" (portrait) and keeps its last value
 * while the device lies flat, so a phone lying on the table would come up
 * in portrait. For the first seconds, while it lies flat, it is reported
 * standing in landscape instead; any real tilt goes through unchanged.
 *
 * The accelerometer streams only while the display is on (mce's
 * display_status_ind): every sample wakes the ADSP link, this daemon and
 * each reader of the device - sensord, qmkeyd and the MeeGo Touch processes
 * - about a hundred wakeups a second in all, for a screen nobody sees.
 * While it streams, the sensor core sends its samples in batches of
 * ACCEL_BATCH_MS (a few samples per report, one wakeup for them all) and
 * never to a suspended system (docs/sensors.md). The proximity sensor
 * (on-change) is on while the display is on and during a call, ringing or
 * active (mce's sig_call_state_ind: the call screen blanks the display
 * while it is covered and needs it to light up again), as mce's own
 * proximity module has it; it is the one sensor that may wake the system.
 * The keys stay on.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <libssc-sensor-accelerometer.h>
#include <libssc-sensor-proximity.h>

#define NAME "ST LIS3LV02DL Accelerometer"
#define MG_PER_MS2 (1000.0 / 9.80665)
#define RANGE 4096
#define LONG_BITS (8 * sizeof(long))
#define LANDSCAPE_START_US (20 * G_USEC_PER_SEC)
#define ACCEL_BATCH_MS 200

static const int side_keys[] = { KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_CAMERA };
static int out_fd = -1;
static gint64 start_us;

static int uinput_open(void)
{
	static const int axes[] = { ABS_X, ABS_Y, ABS_Z };
	struct uinput_setup setup = {
		.id = { .bustype = BUS_VIRTUAL, .vendor = 0x1d50, .product = 0x9001 },
		.name = NAME,
	};
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0) {
		perror("/dev/uinput");
		return -1;
	}
	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	for (int i = 0; i < 3; i++) {
		struct uinput_abs_setup abs = {
			.code = axes[i],
			.absinfo = { .minimum = -RANGE, .maximum = RANGE },
		};
		ioctl(fd, UI_SET_ABSBIT, axes[i]);
		ioctl(fd, UI_ABS_SETUP, &abs);
	}
	ioctl(fd, UI_SET_EVBIT, EV_SW);
	ioctl(fd, UI_SET_SWBIT, SW_FRONT_PROXIMITY);
	ioctl(fd, UI_SET_SWBIT, SW_KEYPAD_SLIDE);
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (unsigned i = 0; i < G_N_ELEMENTS(side_keys); i++)
		ioctl(fd, UI_SET_KEYBIT, side_keys[i]);
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("uinput");
		close(fd);
		return -1;
	}
	return fd;
}

static void emit(int type, int code, int value)
{
	struct input_event ev = { .type = type, .code = code, .value = value };
	if (write(out_fd, &ev, sizeof(ev)) < 0 && errno != EAGAIN)
		perror("uinput write");
}

static int mg(float ms2)
{
	double v = round(ms2 * MG_PER_MS2);
	return v > RANGE ? RANGE : v < -RANGE ? -RANGE : (int)v;
}

static void accel_cb(SSCSensorAccelerometer *s, gfloat x, gfloat y, gfloat z, gpointer data)
{
	(void)s;
	(void)data;
	/* mount matrix: -1 0 0; 0 -1 0; 0 0 1 */
	x = -x;
	y = -y;
	if (g_get_monotonic_time() - start_us < LANDSCAPE_START_US &&
	    fabsf(z) > 0.9f * sqrtf(x * x + y * y + z * z)) {
		x = 9.80665f;
		y = z = 0;
	}
	/* evdev drops values that did not change, and sensord only takes what
	 * comes after it starts reading: a 1 mG dither sends every axis */
	static int dither;
	dither ^= 1;
	emit(EV_ABS, ABS_X, mg(x) + dither);
	emit(EV_ABS, ABS_Y, mg(y) + dither);
	emit(EV_ABS, ABS_Z, mg(z) + dither);
	emit(EV_SYN, SYN_REPORT, 0);
}

static void prox_cb(SSCSensorProximity *s, gboolean near, gpointer data)
{
	(void)s;
	(void)data;
	emit(EV_SW, SW_FRONT_PROXIMITY, near ? 1 : 0);
	emit(EV_SYN, SYN_REPORT, 0);
}

/* An input device of the Pro1X, by name. */
static int open_input(const char *want)
{
	char path[64], name[64];

	for (int i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) > 0 && !strcmp(name, want))
			return fd;
		close(fd);
	}
	fprintf(stderr, "%s not found\n", want);
	return -1;
}

static void copy_side_keys(const struct input_event *ev, ssize_t n)
{
	for (ssize_t i = 0; i < n; i++) {
		if (ev[i].type != EV_KEY)
			continue;
		for (unsigned k = 0; k < G_N_ELEMENTS(side_keys); k++) {
			if (ev[i].code == side_keys[k]) {
				emit(EV_KEY, ev[i].code, ev[i].value);
				emit(EV_SYN, SYN_REPORT, 0);
			}
		}
	}
}

static void slide_emit(int gpio_fd)
{
	unsigned long sw[SW_MAX / LONG_BITS + 1] = { 0 };

	if (ioctl(gpio_fd, EVIOCGSW(sizeof(sw)), sw) < 0)
		return;
	int out = (sw[SW_KEYPAD_SLIDE / LONG_BITS] >> (SW_KEYPAD_SLIDE % LONG_BITS)) & 1;
	emit(EV_SW, SW_KEYPAD_SLIDE, !out);
	emit(EV_SYN, SYN_REPORT, 0);
}

static gboolean gpio_cb(gint fd, GIOCondition cond, gpointer data)
{
	struct input_event ev[16];
	ssize_t n;

	(void)cond;
	(void)data;
	while ((n = read(fd, ev, sizeof(ev))) > 0)
		copy_side_keys(ev, n / (ssize_t)sizeof(ev[0]));
	slide_emit(fd);
	return G_SOURCE_CONTINUE;
}

static gboolean keys_cb(gint fd, GIOCondition cond, gpointer data)
{
	struct input_event ev[16];
	ssize_t n;

	(void)cond;
	(void)data;
	while ((n = read(fd, ev, sizeof(ev))) > 0)
		copy_side_keys(ev, n / (ssize_t)sizeof(ev[0]));
	return G_SOURCE_CONTINUE;
}

/*
 * A sensor that follows a wish: opened while wanted, closed when not, one
 * request at a time; a change of the wish meanwhile is applied when the
 * request is done.
 */
typedef struct {
	const char *name;
	SSCSensor *sensor;
	void (*open)(SSCSensor *, GCancellable *, GAsyncReadyCallback, gpointer);
	gboolean (*open_finish)(SSCSensor *, GAsyncResult *, GError **);
	void (*close)(SSCSensor *, GCancellable *, GAsyncReadyCallback, gpointer);
	gboolean (*close_finish)(SSCSensor *, GAsyncResult *, GError **);
	void (*closed)(void);	/* after closing */
	gboolean want, on, busy;
} Follower;

static void follow(Follower *f);

static void follow_done(GObject *src, GAsyncResult *res, gpointer data)
{
	Follower *f = data;
	g_autoptr(GError) err = NULL;
	gboolean opening = !f->on;
	gboolean ok = opening ? f->open_finish(f->sensor, res, &err)
			      : f->close_finish(f->sensor, res, &err);

	(void)src;
	f->busy = FALSE;
	if (!ok) {
		/* tried again at the next change */
		fprintf(stderr, "%s %s: %s\n", f->name, opening ? "open" : "close",
			err ? err->message : "?");
		return;
	}
	f->on = opening;
	if (!f->on && f->closed)
		f->closed();
	/* the landscape start counts from the first samples, which wait for
	 * the display when the session starts in the dark */
	if (f->on && !strcmp(f->name, "accelerometer") && !start_us)
		start_us = g_get_monotonic_time();
	follow(f);		/* the wish may have changed meanwhile */
}

static void follow(Follower *f)
{
	if (!f->sensor || f->busy || f->want == f->on)
		return;
	f->busy = TRUE;
	if (f->want)
		f->open(f->sensor, NULL, follow_done, f);
	else
		f->close(f->sensor, NULL, follow_done, f);
}

/* while closed the proximity sensor says nothing: "far" for the readers */
static void prox_closed(void)
{
	emit(EV_SW, SW_FRONT_PROXIMITY, 0);
	emit(EV_SYN, SYN_REPORT, 0);
}

/* the sensor types' own open and close (they connect their reports) */
static void accel_open(SSCSensor *s, GCancellable *c, GAsyncReadyCallback cb, gpointer d)
{
	ssc_sensor_accelerometer_open(SSC_SENSOR_ACCELEROMETER(s), c, cb, d);
}
static gboolean accel_open_finish(SSCSensor *s, GAsyncResult *r, GError **e)
{
	return ssc_sensor_accelerometer_open_finish(SSC_SENSOR_ACCELEROMETER(s), r, e);
}
static void accel_close(SSCSensor *s, GCancellable *c, GAsyncReadyCallback cb, gpointer d)
{
	ssc_sensor_accelerometer_close(SSC_SENSOR_ACCELEROMETER(s), c, cb, d);
}
static gboolean accel_close_finish(SSCSensor *s, GAsyncResult *r, GError **e)
{
	return ssc_sensor_accelerometer_close_finish(SSC_SENSOR_ACCELEROMETER(s), r, e);
}
static void prox_open(SSCSensor *s, GCancellable *c, GAsyncReadyCallback cb, gpointer d)
{
	ssc_sensor_proximity_open(SSC_SENSOR_PROXIMITY(s), c, cb, d);
}
static gboolean prox_open_finish(SSCSensor *s, GAsyncResult *r, GError **e)
{
	return ssc_sensor_proximity_open_finish(SSC_SENSOR_PROXIMITY(s), r, e);
}
static void prox_close(SSCSensor *s, GCancellable *c, GAsyncReadyCallback cb, gpointer d)
{
	ssc_sensor_proximity_close(SSC_SENSOR_PROXIMITY(s), c, cb, d);
}
static gboolean prox_close_finish(SSCSensor *s, GAsyncResult *r, GError **e)
{
	return ssc_sensor_proximity_close_finish(SSC_SENSOR_PROXIMITY(s), r, e);
}

static Follower accel_f = {
	"accelerometer", NULL, accel_open, accel_open_finish, accel_close, accel_close_finish,
	NULL, FALSE, FALSE, FALSE,
};
static Follower prox_f = {
	"proximity", NULL, prox_open, prox_open_finish, prox_close, prox_close_finish,
	prox_closed, FALSE, FALSE, FALSE,
};

/* The accelerometer follows the display (on or dimmed), the proximity
 * sensor the display and calls. Without mce: both always on. */
static gboolean display_on = TRUE, in_call;

static void apply(void)
{
	accel_f.want = display_on;
	prox_f.want = display_on || in_call;
	follow(&accel_f);
	follow(&prox_f);
}

static void display_state(const char *state)
{
	display_on = g_strcmp0(state, "off") != 0;
	apply();
}

static void call_state(const char *state)
{
	in_call = !g_strcmp0(state, "ringing") || !g_strcmp0(state, "active");
	apply();
}

static void mce_sig(GDBusConnection *conn, const char *sender, const char *path,
		    const char *iface, const char *signal, GVariant *params, gpointer data)
{
	const char *state;

	(void)conn, (void)sender, (void)path, (void)iface, (void)data;
	if (!strcmp(signal, "display_status_ind") && g_variant_is_of_type(params, G_VARIANT_TYPE("(s)"))) {
		g_variant_get(params, "(&s)", &state);
		display_state(state);
	} else if (!strcmp(signal, "sig_call_state_ind") &&
		   g_variant_is_of_type(params, G_VARIANT_TYPE("(ss)"))) {
		g_variant_get(params, "(&s&s)", &state, NULL);
		call_state(state);
	}
}

static void display_status_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
	const char *state = "on";	/* without mce's answer: always on, as before */

	(void)data;
	if (r)
		g_variant_get(r, "(&s)", &state);
	display_state(state);
}

static void call_status_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
	const char *state;

	(void)data;
	if (r && g_variant_is_of_type(r, G_VARIANT_TYPE("(ss)"))) {
		g_variant_get(r, "(&s&s)", &state, NULL);
		call_state(state);
	}
}

int main(void)
{
	g_autoptr(GError) err = NULL;
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);

	out_fd = uinput_open();
	if (out_fd < 0)
		return 1;

	int gpio_fd = open_input("gpio-keys");
	if (gpio_fd >= 0) {
		slide_emit(gpio_fd);
		g_unix_fd_add(gpio_fd, G_IO_IN, gpio_cb, NULL);
	}
	/* volume down */
	int resin_fd = open_input("pm8941_resin");
	if (resin_fd >= 0)
		g_unix_fd_add(resin_fd, G_IO_IN, keys_cb, NULL);

	SSCSensorAccelerometer *accel = ssc_sensor_accelerometer_new_sync(NULL, &err);
	if (!accel) {
		fprintf(stderr, "accelerometer: %s\n", err ? err->message : "?");
		return 1;
	}
	g_signal_connect(accel, "measurement", G_CALLBACK(accel_cb), NULL);
	/* batched, never to a suspended system */
	ssc_sensor_set_delivery(SSC_SENSOR(accel), FALSE, ACCEL_BATCH_MS * 1000, 0, FALSE);
	accel_f.sensor = SSC_SENSOR(accel);

	g_clear_error(&err);
	SSCSensorProximity *prox = ssc_sensor_proximity_new_sync(NULL, &err);
	if (prox) {
		g_signal_connect(prox, "measurement", G_CALLBACK(prox_cb), NULL);
		/* at once, and it may wake the system (a call) */
		ssc_sensor_set_delivery(SSC_SENSOR(prox), TRUE, 0, 0, FALSE);
		prox_f.sensor = SSC_SENSOR(prox);
	} else {
		fprintf(stderr, "proximity: %s\n", err ? err->message : "?");
	}

	GDBusConnection *sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
	if (sys) {
		g_dbus_connection_signal_subscribe(sys, NULL, "com.nokia.mce.signal", NULL,
						   "/com/nokia/mce/signal", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
						   mce_sig, NULL, NULL);
		g_dbus_connection_call(sys, "com.nokia.mce", "/com/nokia/mce/request", "com.nokia.mce.request",
				       "get_call_state", NULL, G_VARIANT_TYPE("(ss)"),
				       G_DBUS_CALL_FLAGS_NONE, 5000, NULL, call_status_cb, NULL);
		g_dbus_connection_call(sys, "com.nokia.mce", "/com/nokia/mce/request", "com.nokia.mce.request",
				       "get_display_status", NULL, G_VARIANT_TYPE("(s)"),
				       G_DBUS_CALL_FLAGS_NONE, 5000, NULL, display_status_cb, NULL);
	} else {
		display_state("on");
	}

	g_main_loop_run(loop);
	return 0;
}
