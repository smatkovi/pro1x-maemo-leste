/*
 * pro1x-steps -- the Pro1X's hardware step counter on D-Bus.
 *
 * The ADSP counts steps itself from the accelerometer (libssc's "pedometer"
 * sensor, patches/libssc-pedometer.patch), so nothing on the application
 * processor has to watch the accelerometer all day. This daemon keeps that
 * counter enabled and offers it on the system bus, which Maemo Leste and
 * the MeeGo session share:
 *
 *   org.pro1x.Steps  /org/pro1x/Steps  org.pro1x.Steps
 *     GetSteps () -> (t steps)       steps since the daemon started (boot),
 *                                     up to date: the sensor core first
 *                                     hands over what it holds (a flush)
 *     signal StepsChanged (t steps)  at most every COALESCE_S seconds while
 *                                     walking, so listeners wake up rarely
 *
 * The sensor core delivers the count in its power-saving mode
 * (patches/libssc-wakeup-batching.patch, docs/sensors.md):
 *
 *   - non-waking: while the system is suspended the sensor core keeps the
 *     step events and sends them after the resume (with a kernel that
 *     tells the ADSP about suspends, patches 0038/0039); a step never
 *     wakes the phone;
 *   - batched: one report every ACTIVE_BATCH_S seconds of walking while
 *     the display is on (or nobody tells us about the display), one every
 *     IDLE_BATCH_S while it is off (mce's display_status_ind). When the
 *     display comes on again the request is sent again with the short
 *     period, and the sensor core hands over everything it held at once:
 *     the count after a pocketful of walking or a suspend is there when the
 *     screen is.
 *
 * The step events carry the count since the sensor was activated, so a
 * batch or a dropped event loses nothing. Without a step counter in the
 * sensor core (other phones) the daemon counts steps itself from the
 * accelerometer, batched the same way; that keeps the ADSP's accelerometer
 * on all day and counts only while the system is awake.
 *
 * Like Android's step counter the count only grows and starts over at boot;
 * clients keep their own day totals from the differences. When the ADSP
 * starts counting from zero again (sensor reopened, ADSP restarted) or the
 * daemon restarts, the steps so far are kept (in /run, which a boot clears).
 *
 * Runs as root (FastRPC/QRTR), from /etc/init.d/pro1x-steps.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <libssc-sensor-pedometer.h>
#include <libssc-sensor-accelerometer.h>

#define NAME "org.pro1x.Steps"
#define PATH "/org/pro1x/Steps"
#define RETRY_S 30
#define OPEN_TIMEOUT_S 20
#define COALESCE_S 5
#define ACTIVE_BATCH_S 5
#define IDLE_BATCH_S 600
#define FLUSH_WAIT_MS 800	/* GetSteps waits at most this for the sensor core */
#define STATE "/run/pro1x-steps"

static const char introspection[] =
	"<node>"
	" <interface name='" NAME "'>"
	"  <method name='GetSteps'><arg type='t' name='steps' direction='out'/></method>"
	"  <signal name='StepsChanged'><arg type='t' name='steps'/></signal>"
	" </interface>"
	"</node>";

static GDBusConnection *bus;
static SSCSensor *sensor;	/* the step counter, or the accelerometer */
static gboolean counting_self;	/* no step counter: steps from the accelerometer */
static gboolean opened;
static guint64 base;	/* steps before the ADSP's counter last started over */
static guint last;	/* the ADSP's counter (or our own) */
static gint64 emitted_us;	/* when StepsChanged was last sent */
static guint emit_id;	/* pending, coalesced StepsChanged */
static gboolean display_off;	/* mce says so; without mce: on */

static guint64 total(void)
{
	return base + last;
}

static void save(void)
{
	g_autofree char *s = g_strdup_printf("%" G_GUINT64_FORMAT "\n", total());
	g_file_set_contents(STATE, s, -1, NULL);
}

static gboolean emit_cb(gpointer data)
{
	(void)data;
	emit_id = 0;
	emitted_us = g_get_monotonic_time();
	save();
	if (bus)
		g_dbus_connection_emit_signal(bus, NULL, PATH, NAME, "StepsChanged",
					      g_variant_new("(t)", total()), NULL);
	return G_SOURCE_REMOVE;
}

/*
 * A new count: StepsChanged at most every COALESCE_S seconds, and never
 * before the rest of a report is in: libssc hands over the events of one
 * report (a batch) in idle callbacks of default idle priority, the signal
 * comes after them, with the newest count.
 */
static void counted(void)
{
	gint64 due;

	if (emit_id)
		return;
	due = emitted_us + COALESCE_S * G_USEC_PER_SEC - g_get_monotonic_time();
	if (due <= 0)
		emit_id = g_idle_add_full(G_PRIORITY_LOW, emit_cb, NULL, NULL);
	else
		emit_id = g_timeout_add_full(G_PRIORITY_LOW, (guint)(due / 1000) + 1,
					     emit_cb, NULL, NULL);
}

static void measurement_cb(SSCSensorPedometer *s, guint steps, gpointer data)
{
	(void)s, (void)data;
	if (steps < last)
		base += last;
	last = steps;
	counted();
}

/*
 * Steps from the accelerometer, when the sensor core has no step counter:
 * the magnitude of the acceleration, smoothed, against its slow mean
 * (gravity); a step is a rise above the mean by STEP_RISE after a fall
 * below it, at least STEP_GAP_S after the last one. The samples come in
 * batches, so time is counted in samples at the sensor's rate.
 */
#define STEP_RISE 1.0	/* m/s^2 */
#define STEP_GAP_S 0.3
#define STEP_SMOOTH 0.5	/* at 13 Hz about 1.4 Hz: a step's swing (2 Hz) passes */

static double acc_rate = 13;
static double acc_smooth, acc_mean;
static gboolean acc_armed;
static guint64 acc_samples, acc_last_step;

static void accel_cb(SSCSensorAccelerometer *s, gfloat x, gfloat y, gfloat z, gpointer data)
{
	double m = sqrt((double)x * x + (double)y * y + (double)z * z);

	(void)s, (void)data;
	if (acc_samples++ == 0)
		acc_smooth = acc_mean = m;
	acc_smooth += (m - acc_smooth) * STEP_SMOOTH;
	acc_mean += (m - acc_mean) * 0.02;
	if (acc_smooth < acc_mean) {
		acc_armed = TRUE;
	} else if (acc_armed && acc_smooth > acc_mean + STEP_RISE &&
		   acc_samples - acc_last_step >= (guint64)(STEP_GAP_S * acc_rate)) {
		acc_armed = FALSE;
		acc_last_step = acc_samples;
		last++;
		counted();
	}
}

/* --- the sensor core's delivery ---------------------------------------- */

static void set_delivery(void)
{
	guint32 period = (display_off ? IDLE_BATCH_S : ACTIVE_BATCH_S) * G_USEC_PER_SEC;

	/* non-waking; the sensor core keeps at least a batch period of events */
	ssc_sensor_set_delivery(sensor, FALSE, period, 0, FALSE);
}

static void reconfigured_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	g_autoptr(GError) err = NULL;

	(void)data;
	if (!ssc_sensor_reconfigure_finish(SSC_SENSOR(src), res, &err))
		g_warning("step counter: new batch period not taken: %s", err->message);
}

static void display_state(const char *state)
{
	gboolean off = g_strcmp0(state, "off") == 0;

	if (off == display_off)
		return;
	display_off = off;
	g_debug("display %s: batches of up to %d s", state, off ? IDLE_BATCH_S : ACTIVE_BATCH_S);
	if (!off) {
		/* what the sensor core held goes to the listeners at once,
		 * not after the rest of a coalescing pause */
		if (emit_id)
			g_source_remove(emit_id);
		emit_id = 0;
		emitted_us = 0;
	}
	if (!sensor)
		return;
	set_delivery();
	/* the display on again: with the short period the sensor core sends
	 * what it held at once */
	if (opened)
		ssc_sensor_reconfigure(sensor, NULL, reconfigured_cb, NULL);
}

static void display_sig(GDBusConnection *conn, const char *sender, const char *path,
			const char *iface, const char *signal, GVariant *params, gpointer data)
{
	const char *state;

	(void)conn, (void)sender, (void)path, (void)iface, (void)signal, (void)data;
	if (g_variant_is_of_type(params, G_VARIANT_TYPE("(s)"))) {
		g_variant_get(params, "(&s)", &state);
		display_state(state);
	}
}

static void display_status_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
	const char *state;

	(void)data;
	if (!r)
		return;		/* no mce: the display counts as on */
	g_variant_get(r, "(&s)", &state);
	display_state(state);
}

/* --- GetSteps: the count after a flush --------------------------------- */

static GSList *pending;		/* GetSteps calls waiting for the flush */
static guint flush_wait_id;
static gboolean flushing;

/* after the flush's step events: libssc hands them over in idle callbacks
 * of default idle priority, these replies come after them */
static gboolean reply_cb(gpointer data)
{
	GSList *calls = g_slist_reverse(pending);

	(void)data;
	pending = NULL;
	for (GSList *l = calls; l; l = l->next)
		g_dbus_method_invocation_return_value(l->data, g_variant_new("(t)", total()));
	g_slist_free(calls);
	return G_SOURCE_REMOVE;
}

static void reply_pending(void)
{
	if (flush_wait_id)
		g_source_remove(flush_wait_id);
	flush_wait_id = 0;
	g_idle_add_full(G_PRIORITY_LOW, reply_cb, NULL, NULL);
}

static void flushed_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	g_autoptr(GError) err = NULL;

	(void)data;
	flushing = FALSE;
	if (!ssc_sensor_flush_finish(SSC_SENSOR(src), res, &err))
		g_debug("flush: %s", err->message);
	if (pending)
		reply_pending();
}

static gboolean flush_wait_cb(gpointer data)
{
	(void)data;
	flush_wait_id = 0;
	reply_pending();	/* the count as it is; the flush may still come */
	return G_SOURCE_REMOVE;
}

static void get_steps(GDBusMethodInvocation *inv)
{
	if (!sensor || !opened) {
		g_dbus_method_invocation_return_value(inv, g_variant_new("(t)", total()));
		return;
	}
	pending = g_slist_prepend(pending, inv);
	if (!flush_wait_id)
		flush_wait_id = g_timeout_add(FLUSH_WAIT_MS, flush_wait_cb, NULL);
	if (!flushing) {
		flushing = TRUE;
		ssc_sensor_flush(sensor, NULL, flushed_cb, NULL);
	}
}

/*
 * Opening the sensor is asynchronous with a time limit: the ADSP does not
 * always answer (hexagonrpcd and the ADSP come up late in boot, and once an
 * open never got a reply at all), and a waiting daemon must neither hang
 * nor spin. An attempt that runs out of time is dropped, sensor object and
 * all; its late callbacks find a newer attempt number and do nothing.
 */
static guint attempt;		/* the current opening attempt */
static guint attempt_timeout;	/* its time limit */

static gboolean open_sensor(gpointer data);

static void retry(const char *why)
{
	if (attempt_timeout)
		g_source_remove(attempt_timeout);
	attempt_timeout = 0;
	attempt++;
	opened = FALSE;
	g_clear_object(&sensor);
	g_warning("step counter: %s, again in %d s", why, RETRY_S);
	g_timeout_add_seconds(RETRY_S, open_sensor, NULL);
}

static void opened_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	g_autoptr(GError) err = NULL;
	gboolean ok = counting_self
		? ssc_sensor_accelerometer_open_finish(SSC_SENSOR_ACCELEROMETER(src), res, &err)
		: ssc_sensor_pedometer_open_finish(SSC_SENSOR_PEDOMETER(src), res, &err);

	if (GPOINTER_TO_UINT(data) != attempt)
		return;
	if (!ok) {
		retry(err ? err->message : "not opened");
		return;
	}
	g_source_remove(attempt_timeout);
	attempt_timeout = 0;
	opened = TRUE;
	if (counting_self)
		g_message("no step counter in the sensor core: counting from the accelerometer");
	else
		g_message("step counter enabled");
}

static void start(SSCSensor *s, gpointer data)
{
	sensor = s;
	set_delivery();
	if (counting_self) {
		gfloat rate = 0;

		g_object_get(sensor, SSC_SENSOR_SAMPLE_RATE, &rate, NULL);
		if (rate > 0)
			acc_rate = rate;
		g_signal_connect(sensor, "measurement", G_CALLBACK(accel_cb), NULL);
		ssc_sensor_accelerometer_open(SSC_SENSOR_ACCELEROMETER(sensor), NULL, opened_cb, data);
	} else {
		g_signal_connect(sensor, "measurement", G_CALLBACK(measurement_cb), NULL);
		ssc_sensor_pedometer_open(SSC_SENSOR_PEDOMETER(sensor), NULL, opened_cb, data);
	}
}

static void accel_created_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	g_autoptr(GError) err = NULL;
	SSCSensorAccelerometer *s = ssc_sensor_accelerometer_new_finish(res, &err);

	(void)src;
	if (GPOINTER_TO_UINT(data) != attempt) {
		g_clear_object(&s);
		return;
	}
	if (!s) {
		retry(err ? err->message : "neither a step counter nor an accelerometer");
		return;
	}
	start(SSC_SENSOR(s), data);
}

static void created_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	g_autoptr(GError) err = NULL;
	SSCSensorPedometer *s = ssc_sensor_pedometer_new_finish(res, &err);

	(void)src;
	if (GPOINTER_TO_UINT(data) != attempt) {
		g_clear_object(&s);
		return;
	}
	if (!s && !err) {
		/* the sensor core answered: it has no step counter */
		counting_self = TRUE;
		ssc_sensor_accelerometer_new(NULL, accel_created_cb, data);
		return;
	}
	if (!s) {
		retry(err->message);
		return;
	}
	start(SSC_SENSOR(s), data);
}

static gboolean attempt_timeout_cb(gpointer data)
{
	(void)data;
	attempt_timeout = 0;
	retry("no answer from the ADSP");
	return G_SOURCE_REMOVE;
}

static gboolean open_sensor(gpointer data)
{
	(void)data;
	counting_self = FALSE;
	attempt_timeout = g_timeout_add_seconds(OPEN_TIMEOUT_S, attempt_timeout_cb, NULL);
	ssc_sensor_pedometer_new(NULL, created_cb, GUINT_TO_POINTER(attempt));
	return G_SOURCE_REMOVE;
}

static void method_cb(GDBusConnection *conn, const char *sender, const char *path,
		      const char *iface, const char *method, GVariant *params,
		      GDBusMethodInvocation *inv, gpointer data)
{
	(void)conn, (void)sender, (void)path, (void)iface, (void)params, (void)data;
	if (!g_strcmp0(method, "GetSteps"))
		get_steps(inv);
	else
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
						      "no method %s", method);
}

static const GDBusInterfaceVTable vtable = { method_cb, NULL, NULL, { 0 } };

static void bus_acquired_cb(GDBusConnection *conn, const char *name, gpointer data)
{
	g_autoptr(GError) err = NULL;
	GDBusNodeInfo *node = data;

	(void)name;
	bus = conn;
	if (!g_dbus_connection_register_object(conn, PATH, node->interfaces[0], &vtable,
					       NULL, NULL, &err))
		g_warning("register: %s", err->message);
	/* the display's state from mce, where there is one */
	g_dbus_connection_signal_subscribe(conn, NULL, "com.nokia.mce.signal", "display_status_ind",
					   "/com/nokia/mce/signal", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
					   display_sig, NULL, NULL);
	g_dbus_connection_call(conn, "com.nokia.mce", "/com/nokia/mce/request",
			       "com.nokia.mce.request", "get_display_status", NULL,
			       G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL,
			       display_status_cb, NULL);
}

static void name_lost_cb(GDBusConnection *conn, const char *name, gpointer data)
{
	(void)conn, (void)data;
	g_warning("lost the bus name %s", name);
	exit(1);
}

static gboolean quit_cb(gpointer loop)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static void closed_cb(GObject *src, GAsyncResult *res, gpointer loop)
{
	if (counting_self)
		ssc_sensor_accelerometer_close_finish(SSC_SENSOR_ACCELEROMETER(src), res, NULL);
	else
		ssc_sensor_pedometer_close_finish(SSC_SENSOR_PEDOMETER(src), res, NULL);
	g_main_loop_quit(loop);
}

int main(void)
{
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(introspection, NULL);
	g_autofree char *saved = NULL;

	if (g_file_get_contents(STATE, &saved, NULL, NULL))
		base = g_ascii_strtoull(saved, NULL, 10);
	g_bus_own_name(G_BUS_TYPE_SYSTEM, NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
		       bus_acquired_cb, NULL, name_lost_cb, node, NULL);
	open_sensor(NULL);
	g_unix_signal_add(SIGTERM, quit_cb, loop);
	g_unix_signal_add(SIGINT, quit_cb, loop);
	g_main_loop_run(loop);
	if (sensor && opened) {
		/* switch the counter off, without waiting long for the ADSP */
		if (counting_self)
			ssc_sensor_accelerometer_close(SSC_SENSOR_ACCELEROMETER(sensor), NULL, closed_cb, loop);
		else
			ssc_sensor_pedometer_close(SSC_SENSOR_PEDOMETER(sensor), NULL, closed_cb, loop);
		g_timeout_add_seconds(2, quit_cb, loop);
		g_main_loop_run(loop);
	}
	save();
	return 0;
}
