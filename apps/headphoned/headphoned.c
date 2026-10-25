/*
 * headphoned -- pause media players when the headphones are unplugged.
 *
 * A port of Thomas Perl's headphoned for Maemo 5 (1.9, GPL-2.0-or-later)
 * to Maemo Leste. The N900 version woke on a POLLPRI of the gpio-switch
 * sysfs file. Here the kernel's jack input device reports the change
 * (SW_HEADPHONE_INSERT; on the Pro1X "F(x)tec Pro1X Headset Jack"), so the
 * daemon sleeps in poll() until it happens and costs nothing otherwise.
 *
 * On unplugging it pauses every MPRIS2 player on the session bus and Leste's
 * MAFW renderer (the Media Player), and like on Maemo 5 Panucci, FM Radio
 * and MediaBox. During a call the pause waits until the call has ended
 * (mce's call state), as before.
 *
 * Copyright (C) 2009 Thomas Perl (original headphoned)
 * Copyright (C) 2026 the pro1x-maemo-leste authors (this port)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <gio/gio.h>
#include <glib-unix.h>

#define LONG_BITS (8 * sizeof(long))
#define TEST_BIT(bit, a) (((a)[(bit) / LONG_BITS] >> ((bit) % LONG_BITS)) & 1)
#define POST_CALL_PAUSE_DELAY_MS 500

/* service, object, interface, method -- the players of headphoned 1.9 */
static const char *const legacy[][4] = {
	{ "com.nokia.mafw.renderer.Mafw-Gst-Renderer-Plugin.gstrenderer",
	  "/com/nokia/mafw/renderer/gstrenderer", "com.nokia.mafw.renderer", "pause" },
	{ "org.panucci.panucciInterface", "/panucciInterface", "org.panucci.panucciInterface", "pause" },
	{ "de.pycage.fmradio", "/de/pycage/fmradio", "de.pycage.fmradio", "stop" },
	{ "de.pycage.mediabox", "/de/pycage/mediabox/control", "de.pycage.mediabox.control", "stop" },
};

static GDBusConnection *session, *systembus;
static gboolean in_call, pause_after_call;

static void call(const char *name, const char *path, const char *iface, const char *method)
{
	g_dbus_connection_call(session, name, path, iface, method, NULL, NULL,
			       G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, NULL, NULL);
}

static void pause_players(void)
{
	GVariant *r = g_dbus_connection_call_sync(session, "org.freedesktop.DBus", "/org/freedesktop/DBus",
						  "org.freedesktop.DBus", "ListNames", NULL,
						  G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
	GVariantIter *it;
	const char *name;

	if (!r)
		return;
	g_variant_get(r, "(as)", &it);
	while (g_variant_iter_next(it, "&s", &name)) {
		if (g_str_has_prefix(name, "org.mpris.MediaPlayer2."))
			call(name, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", "Pause");
		for (guint i = 0; i < G_N_ELEMENTS(legacy); i++)
			if (!strcmp(name, legacy[i][0]))
				call(legacy[i][0], legacy[i][1], legacy[i][2], legacy[i][3]);
	}
	g_variant_iter_free(it);
	g_variant_unref(r);
}

static gboolean delayed_pause(gpointer data)
{
	(void)data;
	pause_players();
	return G_SOURCE_REMOVE;
}

static void unplugged(void)
{
	if (in_call)
		pause_after_call = TRUE;
	else
		pause_players();
}

static void call_state_cb(GDBusConnection *c, const char *sender, const char *path, const char *iface,
			  const char *signal, GVariant *params, gpointer data)
{
	const char *state = NULL, *type = NULL;

	(void)c, (void)sender, (void)path, (void)iface, (void)signal, (void)data;
	if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(ss)")))
		return;
	g_variant_get(params, "(&s&s)", &state, &type);
	in_call = strcmp(state, "none") != 0;
	if (!in_call && pause_after_call) {
		pause_after_call = FALSE;
		g_timeout_add(POST_CALL_PAUSE_DELAY_MS, delayed_pause, NULL);
	}
}

static gboolean jack_cb(gint fd, GIOCondition cond, gpointer data)
{
	struct input_event ev[16];
	ssize_t n;

	(void)data;
	if (cond & (G_IO_HUP | G_IO_ERR))
		return G_SOURCE_REMOVE;
	while ((n = read(fd, ev, sizeof(ev))) > 0)
		for (ssize_t i = 0; i < n / (ssize_t)sizeof(ev[0]); i++)
			if (ev[i].type == EV_SW && ev[i].code == SW_HEADPHONE_INSERT && ev[i].value == 0)
				unplugged();
	return G_SOURCE_CONTINUE;
}

/* every input device that reports SW_HEADPHONE_INSERT */
static int watch_jacks(void)
{
	char path[64];
	int found = 0;

	for (int i = 0; i < 64; i++) {
		unsigned long ev[EV_MAX / LONG_BITS + 1] = { 0 }, sw[SW_MAX / LONG_BITS + 1] = { 0 };
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		if ((fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0)
			continue;
		if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0 || !TEST_BIT(EV_SW, ev) ||
		    ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sw)), sw) < 0 || !TEST_BIT(SW_HEADPHONE_INSERT, sw)) {
			close(fd);
			continue;
		}
		g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, jack_cb, NULL);
		found++;
	}
	return found;
}

int main(int argc, char **argv)
{
	g_autoptr(GError) err = NULL;
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);

	(void)argc, (void)argv;
	session = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (!session) {
		fprintf(stderr, "headphoned: session bus: %s\n", err->message);
		return 1;
	}
	if (!watch_jacks()) {
		fprintf(stderr, "headphoned: no headphone jack input device (permissions?)\n");
		return 1;
	}
	systembus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
	if (systembus)
		g_dbus_connection_signal_subscribe(systembus, NULL, "com.nokia.mce.signal", "sig_call_state_ind",
						   "/com/nokia/mce/signal", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
						   call_state_cb, NULL, NULL);
	g_main_loop_run(loop);
	return 0;
}
