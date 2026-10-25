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
 *     GetSteps () -> (t steps)       steps since the daemon started (boot)
 *     signal StepsChanged (t steps)  at most every COALESCE_S seconds while
 *                                     walking, so listeners wake up rarely
 *
 * Like Android's step counter the count only grows and starts over at boot;
 * clients keep their own day totals from the differences. When the ADSP
 * starts counting from zero again (sensor reopened, ADSP restarted) or the
 * daemon restarts, the steps so far are kept (in /run, which a boot clears).
 *
 * Runs as root (FastRPC/QRTR), from /etc/init.d/pro1x-steps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <libssc-sensor-pedometer.h>

#define NAME "org.pro1x.Steps"
#define PATH "/org/pro1x/Steps"
#define RETRY_S 30
#define COALESCE_S 5
#define STATE "/run/pro1x-steps"

static const char introspection[] =
	"<node>"
	" <interface name='" NAME "'>"
	"  <method name='GetSteps'><arg type='t' name='steps' direction='out'/></method>"
	"  <signal name='StepsChanged'><arg type='t' name='steps'/></signal>"
	" </interface>"
	"</node>";

static GDBusConnection *bus;
static SSCSensorPedometer *sensor;
static guint64 base;	/* steps before the ADSP's counter last started over */
static guint last;	/* the ADSP's counter */
static gint64 emitted_us;	/* when StepsChanged was last sent */
static guint emit_id;	/* pending, coalesced StepsChanged */

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

static void measurement_cb(SSCSensorPedometer *s, guint steps, gpointer data)
{
	gint64 due;

	(void)s, (void)data;
	if (steps < last)
		base += last;
	last = steps;
	/* the ADSP reports every step: pass them on in batches */
	if (emit_id)
		return;
	due = emitted_us + COALESCE_S * G_USEC_PER_SEC - g_get_monotonic_time();
	if (due <= 0)
		emit_cb(NULL);
	else
		emit_id = g_timeout_add((guint)(due / 1000) + 1, emit_cb, NULL);
}

static gboolean open_sensor(gpointer data)
{
	g_autoptr(GError) err = NULL;

	(void)data;
	if (!sensor) {
		sensor = ssc_sensor_pedometer_new_sync(NULL, &err);
		if (sensor)
			g_signal_connect(sensor, "measurement", G_CALLBACK(measurement_cb), NULL);
	}
	if (sensor && ssc_sensor_pedometer_open_sync(sensor, NULL, &err)) {
		g_message("step counter enabled");
		return G_SOURCE_REMOVE;
	}
	/* hexagonrpcd and the ADSP come up late in boot */
	g_warning("step counter: %s, again in %d s", err ? err->message : "?", RETRY_S);
	g_timeout_add_seconds(RETRY_S, open_sensor, NULL);
	return G_SOURCE_REMOVE;
}

static void method_cb(GDBusConnection *conn, const char *sender, const char *path,
		      const char *iface, const char *method, GVariant *params,
		      GDBusMethodInvocation *inv, gpointer data)
{
	(void)conn, (void)sender, (void)path, (void)iface, (void)params, (void)data;
	if (!g_strcmp0(method, "GetSteps"))
		g_dbus_method_invocation_return_value(inv, g_variant_new("(t)", total()));
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
	if (sensor)
		ssc_sensor_pedometer_close_sync(sensor, NULL, NULL);
	return 0;
}
