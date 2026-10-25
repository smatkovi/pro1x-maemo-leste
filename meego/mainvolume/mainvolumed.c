/*
 * PulseAudio's com.Nokia.MainVolume1 for Harmattan, on Leste's PulseAudio.
 * Runs on Leste, as the user, during the MeeGo session.
 *
 * The N9's PulseAudio had a module (module-meego-mainvolume) that offered
 * the main volume in steps on PulseAudio's own D-Bus server; sysuid's
 * volume bar, which the side keys open, talks to nothing else. Leste's
 * PulseAudio has no such module, so this is a peer-to-peer D-Bus server of
 * its own at the address sysuid uses by default (/var/run/pulse/dbus-socket
 * in the Harmattan tree) that maps the steps onto the default sink's
 * volume:
 *
 *   /com/meego/mainvolume1  com.Nokia.MainVolume1
 *     properties  StepCount (u), CurrentStep (u, writable), InterfaceRevision (u)
 *     signal      StepsUpdated (u step_count, u current_step)
 *
 *   mainvolumed <socket path>
 */
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

#define STEPS 20
#define PATH "/com/meego/mainvolume1"
#define IFACE "com.Nokia.MainVolume1"

static const char introspection[] =
	"<node>"
	" <interface name='" IFACE "'>"
	"  <property name='InterfaceRevision' type='u' access='read'/>"
	"  <property name='StepCount' type='u' access='read'/>"
	"  <property name='CurrentStep' type='u' access='readwrite'/>"
	"  <signal name='StepsUpdated'><arg type='u'/><arg type='u'/></signal>"
	" </interface>"
	"</node>";

static GDBusNodeInfo *node;
static GList *peers;
static pa_glib_mainloop *pa_loop;
static pa_context *ctx;
static char *sink;
static pa_channel_map sink_map;
static guint step;

static guint step_of(const pa_cvolume *v)
{
	double s = round((double)pa_cvolume_max(v) * (STEPS - 1) / PA_VOLUME_NORM);
	return s > STEPS - 1 ? STEPS - 1 : (guint)s;
}

static void steps_updated(void)
{
	for (GList *l = peers; l; l = l->next)
		g_dbus_connection_emit_signal(l->data, NULL, PATH, IFACE, "StepsUpdated",
					      g_variant_new("(uu)", STEPS, step), NULL);
}

static void set_step(guint s)
{
	pa_cvolume v;

	step = s < STEPS ? s : STEPS - 1;
	if (!ctx || pa_context_get_state(ctx) != PA_CONTEXT_READY || !sink)
		return;
	pa_cvolume_set(&v, sink_map.channels, (pa_volume_t)lround((double)PA_VOLUME_NORM * step / (STEPS - 1)));
	pa_operation *o = pa_context_set_sink_volume_by_name(ctx, sink, &v, NULL, NULL);
	if (o)
		pa_operation_unref(o);
}

/* --- PulseAudio ---------------------------------------------------------------- */

static void sink_cb(pa_context *c, const pa_sink_info *i, int eol, void *data)
{
	(void)c;
	(void)data;
	if (eol || !i)
		return;
	g_free(sink);
	sink = g_strdup(i->name);
	sink_map = i->channel_map;
	guint s = step_of(&i->volume);
	if (s != step) {
		step = s;
		steps_updated();
	}
}

static void server_cb(pa_context *c, const pa_server_info *i, void *data)
{
	(void)data;
	if (!i || !i->default_sink_name)
		return;
	pa_operation *o = pa_context_get_sink_info_by_name(c, i->default_sink_name, sink_cb, NULL);
	if (o)
		pa_operation_unref(o);
}

static void refresh(pa_context *c)
{
	pa_operation *o = pa_context_get_server_info(c, server_cb, NULL);
	if (o)
		pa_operation_unref(o);
}

static void event_cb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *data)
{
	(void)idx;
	(void)data;
	unsigned facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
	if (facility == PA_SUBSCRIPTION_EVENT_SINK || facility == PA_SUBSCRIPTION_EVENT_SERVER)
		refresh(c);
}

static void pa_connect(void);

static gboolean reconnect(gpointer data)
{
	(void)data;
	if (ctx)
		pa_context_unref(ctx);
	pa_connect();
	return G_SOURCE_REMOVE;
}

static void state_cb(pa_context *c, void *data)
{
	(void)data;
	switch (pa_context_get_state(c)) {
	case PA_CONTEXT_READY:
		pa_context_set_subscribe_callback(c, event_cb, NULL);
		pa_operation_unref(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER,
							NULL, NULL));
		refresh(c);
		break;
	case PA_CONTEXT_FAILED:
	case PA_CONTEXT_TERMINATED:
		/* PulseAudio restarted: start over (outside of its callback) */
		g_timeout_add_seconds(2, reconnect, NULL);
		break;
	default:
		break;
	}
}

static void pa_connect(void)
{
	ctx = pa_context_new(pa_glib_mainloop_get_api(pa_loop), "mainvolumed");
	pa_context_set_state_callback(ctx, state_cb, NULL);
	if (pa_context_connect(ctx, NULL, PA_CONTEXT_NOFAIL, NULL) < 0)
		fprintf(stderr, "pulseaudio: %s\n", pa_strerror(pa_context_errno(ctx)));
}

/* --- D-Bus ----------------------------------------------------------------------- */

static GVariant *get_property(GDBusConnection *conn, const char *sender, const char *path,
			      const char *iface, const char *prop, GError **err, gpointer data)
{
	(void)conn, (void)sender, (void)path, (void)iface, (void)err, (void)data;
	if (!strcmp(prop, "StepCount"))
		return g_variant_new_uint32(STEPS);
	if (!strcmp(prop, "CurrentStep"))
		return g_variant_new_uint32(step);
	return g_variant_new_uint32(1);
}

static gboolean set_property(GDBusConnection *conn, const char *sender, const char *path,
			     const char *iface, const char *prop, GVariant *value, GError **err,
			     gpointer data)
{
	(void)conn, (void)sender, (void)path, (void)iface, (void)data;
	if (strcmp(prop, "CurrentStep") || !g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
		g_set_error(err, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "read-only or wrong type");
		return FALSE;
	}
	set_step(g_variant_get_uint32(value));
	steps_updated();
	return TRUE;
}

static const GDBusInterfaceVTable vtable = { NULL, get_property, set_property, { 0 } };

static void closed_cb(GDBusConnection *conn, gboolean vanished, GError *err, gpointer data)
{
	(void)vanished, (void)err, (void)data;
	peers = g_list_remove(peers, conn);
	g_object_unref(conn);
}

static gboolean new_connection_cb(GDBusServer *server, GDBusConnection *conn, gpointer data)
{
	g_autoptr(GError) err = NULL;

	(void)server, (void)data;
	if (!g_dbus_connection_register_object(conn, PATH, node->interfaces[0], &vtable,
					       NULL, NULL, &err)) {
		fprintf(stderr, "register: %s\n", err->message);
		return FALSE;
	}
	peers = g_list_prepend(peers, g_object_ref(conn));
	g_signal_connect(conn, "closed", G_CALLBACK(closed_cb), NULL);
	return TRUE;
}

static gboolean quit_cb(gpointer loop)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
	g_autoptr(GError) err = NULL;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <socket path>\n", argv[0]);
		return 2;
	}
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	node = g_dbus_node_info_new_for_xml(introspection, NULL);

	unlink(argv[1]);
	umask(0);
	g_autofree char *address = g_strdup_printf("unix:path=%s", argv[1]);
	g_autofree char *guid = g_dbus_generate_guid();
	GDBusServer *server = g_dbus_server_new_sync(address, G_DBUS_SERVER_FLAGS_NONE, guid,
						     NULL, NULL, &err);
	if (!server) {
		fprintf(stderr, "%s: %s\n", argv[1], err->message);
		return 1;
	}
	g_signal_connect(server, "new-connection", G_CALLBACK(new_connection_cb), NULL);
	g_dbus_server_start(server);

	pa_loop = pa_glib_mainloop_new(NULL);
	pa_connect();

	g_unix_signal_add(SIGTERM, quit_cb, loop);
	g_unix_signal_add(SIGINT, quit_cb, loop);
	g_main_loop_run(loop);
	g_dbus_server_stop(server);
	unlink(argv[1]);
	return 0;
}
