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
#include <libssc-sensor-accelerometer.h>
#include <libssc-sensor-proximity.h>

#define NAME "ST LIS3LV02DL Accelerometer"
#define MG_PER_MS2 (1000.0 / 9.80665)
#define RANGE 4096
#define LONG_BITS (8 * sizeof(long))
#define LANDSCAPE_START_US (20 * G_USEC_PER_SEC)

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

int main(void)
{
	g_autoptr(GError) err = NULL;
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);

	start_us = g_get_monotonic_time();
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
	if (accel)
		g_signal_connect(accel, "measurement", G_CALLBACK(accel_cb), NULL);
	if (!accel || !ssc_sensor_accelerometer_open_sync(accel, NULL, &err)) {
		fprintf(stderr, "accelerometer: %s\n", err ? err->message : "?");
		return 1;
	}

	SSCSensorProximity *prox = ssc_sensor_proximity_new_sync(NULL, &err);
	if (prox)
		g_signal_connect(prox, "measurement", G_CALLBACK(prox_cb), NULL);
	if (!prox || !ssc_sensor_proximity_open_sync(prox, NULL, &err))
		fprintf(stderr, "proximity: %s\n", err ? err->message : "?");

	g_main_loop_run(loop);
	return 0;
}
