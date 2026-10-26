/*
 * pro1x-proximity -- the Pro1X's proximity sensor on D-Bus, while needed.
 *
 * The sensor is one of the ADSP's (libssc, as iio-sensor-proxy reads it on
 * Leste). Keeping it streaming costs power, so, like the N9's mce, this
 * daemon has it on only while a client watches it:
 *
 *   org.pro1x.Proximity  /org/pro1x/Proximity  org.pro1x.Proximity
 *     Watch ()                      the sensor on for the caller, until
 *                                   Unwatch or the caller leaves the bus
 *     Unwatch ()
 *     GetNear () -> (b near, b valid)   valid: a measurement since Watch
 *     signal Changed (b near)       on every change while watched
 *
 * The stand-by screen (tools/pro1x-standby) watches it while it shows and
 * goes dark while something covers the sensor (a pocket, a table).
 *
 * Runs as root (FastRPC/QRTR), from rc.d/pro1x_proximity on FreeBSD.
 */
#include <stdio.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <libssc-sensor-proximity.h>

#define NAME "org.pro1x.Proximity"
#define PATH "/org/pro1x/Proximity"
#define RETRY_S 30
#define OPEN_TIMEOUT_S 20

static const char introspection[] =
	"<node>"
	" <interface name='" NAME "'>"
	"  <method name='Watch'/>"
	"  <method name='Unwatch'/>"
	"  <method name='GetNear'>"
	"   <arg type='b' name='near' direction='out'/>"
	"   <arg type='b' name='valid' direction='out'/>"
	"  </method>"
	"  <signal name='Changed'><arg type='b' name='near'/></signal>"
	" </interface>"
	"</node>";

static GDBusConnection *bus;
static SSCSensorProximity *sensor;
static gboolean opened;
static gboolean near, valid;
static GHashTable *watchers;	/* unique name -> bus name watch id */

static void measurement_cb(SSCSensorProximity *s, gboolean n, gpointer data)
{
	(void)s, (void)data;
	if (valid && n == near)
		return;
	near = n;
	valid = TRUE;
	if (bus)
		g_dbus_connection_emit_signal(bus, NULL, PATH, NAME, "Changed",
					      g_variant_new("(b)", near), NULL);
}

/*
 * Opening is asynchronous with a time limit, as in pro1x-steps: the ADSP
 * does not always answer. An attempt that runs out of time is dropped; its
 * late callbacks find a newer attempt number and do nothing. Attempts stop
 * when the last watcher leaves.
 */
static guint attempt;
static guint attempt_timeout;
static guint retry_id;

static gboolean open_sensor(gpointer data);

static void drop_sensor(void)
{
	if (attempt_timeout)
		g_source_remove(attempt_timeout);
	attempt_timeout = 0;
	if (retry_id)
		g_source_remove(retry_id);
	retry_id = 0;
	attempt++;
	if (sensor && opened)
		ssc_sensor_proximity_close(sensor, NULL, NULL, NULL);
	g_clear_object(&sensor);
	opened = FALSE;
	valid = FALSE;
}

static void retry(const char *why)
{
	drop_sensor();
	g_warning("proximity: %s, again in %d s", why, RETRY_S);
	retry_id = g_timeout_add_seconds(RETRY_S, open_sensor, NULL);
}

static void opened_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	g_autoptr(GError) err = NULL;
	gboolean ok = ssc_sensor_proximity_open_finish(SSC_SENSOR_PROXIMITY(src),
							res, &err);

	if (GPOINTER_TO_UINT(data) != attempt)
		return;
	if (!ok) {
		retry(err->message);
		return;
	}
	g_source_remove(attempt_timeout);
	attempt_timeout = 0;
	opened = TRUE;
	g_message("proximity sensor on");
}

static void created_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	g_autoptr(GError) err = NULL;
	SSCSensorProximity *s = ssc_sensor_proximity_new_finish(res, &err);

	(void)src;
	if (GPOINTER_TO_UINT(data) != attempt) {
		g_clear_object(&s);
		return;
	}
	if (!s) {
		retry(err->message);
		return;
	}
	sensor = s;
	g_signal_connect(sensor, "measurement", G_CALLBACK(measurement_cb), NULL);
	ssc_sensor_proximity_open(sensor, NULL, opened_cb, data);
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
	retry_id = 0;
	attempt_timeout = g_timeout_add_seconds(OPEN_TIMEOUT_S,
						attempt_timeout_cb, NULL);
	ssc_sensor_proximity_new(NULL, created_cb, GUINT_TO_POINTER(attempt));
	return G_SOURCE_REMOVE;
}

/* --- watchers ---------------------------------------------------------- */

static void unwatch(const char *who)
{
	if (!g_hash_table_remove(watchers, who))
		return;
	if (g_hash_table_size(watchers) == 0) {
		drop_sensor();
		g_message("proximity sensor off");
	}
}

static void vanished_cb(GDBusConnection *conn, const char *name, gpointer data)
{
	(void)conn, (void)data;
	unwatch(name);
}

static void watch(const char *who)
{
	guint id;

	if (g_hash_table_contains(watchers, who))
		return;
	id = g_bus_watch_name_on_connection(bus, who, G_BUS_NAME_WATCHER_FLAGS_NONE,
					    NULL, vanished_cb, NULL, NULL);
	g_hash_table_insert(watchers, g_strdup(who), GUINT_TO_POINTER(id));
	if (g_hash_table_size(watchers) == 1 && !sensor && !attempt_timeout &&
	    !retry_id)
		open_sensor(NULL);
}

static void drop_watch_id(gpointer id)
{
	g_bus_unwatch_name(GPOINTER_TO_UINT(id));
}

static void method_cb(GDBusConnection *conn, const char *sender, const char *path,
		      const char *iface, const char *method, GVariant *params,
		      GDBusMethodInvocation *inv, gpointer data)
{
	(void)conn, (void)path, (void)iface, (void)params, (void)data;
	if (!g_strcmp0(method, "Watch")) {
		watch(sender);
		g_dbus_method_invocation_return_value(inv, NULL);
	} else if (!g_strcmp0(method, "Unwatch")) {
		unwatch(sender);
		g_dbus_method_invocation_return_value(inv, NULL);
	} else if (!g_strcmp0(method, "GetNear")) {
		g_dbus_method_invocation_return_value(inv,
			g_variant_new("(bb)", near, valid));
	} else {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_UNKNOWN_METHOD, "no method %s", method);
	}
}

static const GDBusInterfaceVTable vtable = { method_cb, NULL, NULL, { 0 } };

static void bus_acquired_cb(GDBusConnection *conn, const char *name, gpointer data)
{
	g_autoptr(GError) err = NULL;
	GDBusNodeInfo *node = data;

	(void)name;
	bus = conn;
	if (!g_dbus_connection_register_object(conn, PATH, node->interfaces[0],
					       &vtable, NULL, NULL, &err))
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

	watchers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					 drop_watch_id);
	g_bus_own_name(G_BUS_TYPE_SYSTEM, NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
		       bus_acquired_cb, NULL, name_lost_cb, node, NULL);
	g_unix_signal_add(SIGTERM, quit_cb, loop);
	g_unix_signal_add(SIGINT, quit_cb, loop);
	g_main_loop_run(loop);
	drop_sensor();
	return 0;
}
