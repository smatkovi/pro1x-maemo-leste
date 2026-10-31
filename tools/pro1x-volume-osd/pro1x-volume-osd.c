/*
 * pro1x-volume-osd -- the Nokia N9's volume bar for Hildon's MeeGo-like
 * look (patches/meego): when the volume of PulseAudio's default sink
 * changes (the volume keys, an application), the screen darkens for a
 * moment and the volume rises from the bottom as a lighter area, a
 * loudspeaker at the bottom in the middle; a tap or a drag on it sets the
 * volume there. Only while the look is on (GConf
 * /apps/osso/hildon-desktop/meego_pages), else it waits unseen.
 *
 *   pro1x-volume-osd        runs in the X session (Xsession.post)
 *
 * As the N9's system UI draws it (sysuid's volume extension, its style
 * libsysuid-volume.css): black at 85 % over the screen, the level in the
 * theme's volume-level graphic at half its opacity, the loudspeaker 8 mm
 * with 5 mm below it, 1.5 s on screen after the last change, faded in and
 * out in 250 ms. The graphics are the N9's own where its theme (blanco,
 * Nokia's, not part of this) is installed, else drawn alike.
 *
 * Not while the display is off or shows the stand-by screen (mce's
 * display_status_ind "off" or "lpm"): a volume key in the pocket changes
 * the volume without a window mapped and faded for nobody.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <gtk/gtk.h>
#include <gconf/gconf-client.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

#define KEY_MEEGO "/apps/osso/hildon-desktop/meego_pages"

#define VISIBLE_MS 1500
#define FADE_MS 250
#define FRAME_MS 25
#define BACKGROUND_OPACITY 0.85
#define LEVEL_OPACITY 0.5
#define LEVEL_BORDER 12
#define ICON_MM 8
#define ICON_MARGIN_MM 5

static const char *theme_dirs[] = {
	"/usr/share/themes/blanco/meegotouch",
	"/usr/local/share/themes/blanco/meegotouch",
};
#define LEVEL_IMAGE "images/theme/basement/meegotouch-volumelevel-indicator/meegoui-volume-level.png"
#define ICON "icons/icon-m-music-video-volume.png"
#define ICON_MUTED "icons/icon-m-music-video-volume-off.png"

static GConfClient *gconf;
static GtkWidget *win;
static GdkPixbuf *level_image, *icon, *icon_muted;

static pa_glib_mainloop *pa_loop;
static pa_context *ctx;
static char *sink;
static pa_cvolume sink_volume;
static gboolean known;		/* the first volume is no change */
static gboolean display_on = TRUE;	/* mce's display_status_ind */
static double volume;		/* 0..1 */
static gboolean muted;

static double opacity;		/* of the whole, while fading */
static int fade_dir;		/* +1 in, -1 out, 0 still */
static guint fade_id, hide_id;
static gboolean dragging;

static GdkPixbuf *theme_image(const char *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(theme_dirs); i++) {
		char *path = g_build_filename(theme_dirs[i], name, NULL);
		GdkPixbuf *p = gdk_pixbuf_new_from_file(path, NULL);

		g_free(path);
		if (p)
			return p;
	}
	return NULL;
}

/* Pixels per millimetre of the screen (the X server's, if it is a phone's,
 * else the Pro1X's: 2160 px on 133 mm) */
static double px_per_mm(void)
{
	GdkScreen *s = gdk_screen_get_default();
	int mm = gdk_screen_get_width_mm(s);
	double d = mm > 0 ? (double)gdk_screen_get_width(s) / mm : 0;

	return d >= 8 && d <= 40 ? d : 16;
}

/* @p over x, y, w, h as a border image: its corners (@b of its pixels,
 * those scaled by @f) as they are, its edges and middle stretched */
static void draw_border_image(cairo_t *cr, GdkPixbuf *p, int b, double f,
			      double x, double y, double w, double h, double alpha)
{
	double pw = gdk_pixbuf_get_width(p), ph = gdk_pixbuf_get_height(p);
	double sx[4] = { 0, b, pw - b, pw }, sy[4] = { 0, b, ph - b, ph };
	double bw = MIN(b * f, w / 2), bh = MIN(b * f, h / 2);
	double dx[4] = { x, x + bw, x + w - bw, x + w };
	double dy[4] = { y, y + bh, y + h - bh, y + h };
	int i, j;

	for (j = 0; j < 3; j++)
		for (i = 0; i < 3; i++) {
			if (dx[i + 1] <= dx[i] || dy[j + 1] <= dy[j])
				continue;
			cairo_save(cr);
			cairo_rectangle(cr, dx[i], dy[j], dx[i + 1] - dx[i], dy[j + 1] - dy[j]);
			cairo_clip(cr);
			cairo_translate(cr, dx[i], dy[j]);
			cairo_scale(cr, (dx[i + 1] - dx[i]) / (sx[i + 1] - sx[i]),
				    (dy[j + 1] - dy[j]) / (sy[j + 1] - sy[j]));
			gdk_cairo_set_source_pixbuf(cr, p, -sx[i], -sy[j]);
			cairo_paint_with_alpha(cr, alpha);
			cairo_restore(cr);
		}
}

/* A loudspeaker in white, crossed out when muted */
static void draw_speaker(cairo_t *cr, double x, double y, double s, gboolean off)
{
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, s, s);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_move_to(cr, 0.10, 0.38);
	cairo_line_to(cr, 0.28, 0.38);
	cairo_line_to(cr, 0.50, 0.18);
	cairo_line_to(cr, 0.50, 0.82);
	cairo_line_to(cr, 0.28, 0.62);
	cairo_line_to(cr, 0.10, 0.62);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_set_line_width(cr, 0.06);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	if (off) {
		cairo_move_to(cr, 0.62, 0.36);
		cairo_line_to(cr, 0.90, 0.64);
		cairo_move_to(cr, 0.90, 0.36);
		cairo_line_to(cr, 0.62, 0.64);
	} else {
		cairo_arc(cr, 0.50, 0.50, 0.16, -0.9, 0.9);
		cairo_new_sub_path(cr);
		cairo_arc(cr, 0.50, 0.50, 0.30, -0.9, 0.9);
	}
	cairo_stroke(cr);
	cairo_restore(cr);
}

static gboolean expose_cb(GtkWidget *w, GdkEventExpose *e, gpointer data)
{
	cairo_t *cr = gdk_cairo_create(w->window);
	int width = w->allocation.width, height = w->allocation.height;
	double mm = px_per_mm(), level = height * (muted ? 0 : volume);
	double is = ICON_MM * mm;
	gboolean off = muted || volume <= 0;

	(void)e, (void)data;
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, BACKGROUND_OPACITY * opacity);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	/* the level, from the bottom */
	if (level > 0) {
		if (level_image) {
			/* its border 12 of the N9's pixels (9.9 a millimetre) */
			draw_border_image(cr, level_image, LEVEL_BORDER, mm / 9.9,
					  0, height - level, width, level,
					  LEVEL_OPACITY * opacity);
		} else {
			/* the N9's: its blue at half opacity */
			cairo_set_source_rgba(cr, 16 / 255.0, 128 / 255.0, 221 / 255.0,
					      0.5 * LEVEL_OPACITY * opacity);
			cairo_rectangle(cr, 0, height - level, width, level);
			cairo_fill(cr);
		}
	}

	/* the loudspeaker */
	{
		GdkPixbuf *p = off ? icon_muted : icon;
		double x = (width - is) / 2, y = height - ICON_MARGIN_MM * mm - is;

		if (p) {
			cairo_save(cr);
			cairo_translate(cr, x, y);
			cairo_scale(cr, is / gdk_pixbuf_get_width(p),
				    is / gdk_pixbuf_get_height(p));
			gdk_cairo_set_source_pixbuf(cr, p, 0, 0);
			cairo_paint_with_alpha(cr, opacity);
			cairo_restore(cr);
		} else {
			cairo_push_group(cr);
			draw_speaker(cr, x, y, is, off);
			cairo_pop_group_to_source(cr);
			cairo_paint_with_alpha(cr, opacity);
		}
	}
	cairo_destroy(cr);
	return TRUE;
}

static gboolean fade_step(gpointer data)
{
	(void)data;
	opacity += fade_dir * (double)FRAME_MS / FADE_MS;
	if (opacity >= 1) {
		opacity = 1;
		fade_dir = 0;
	} else if (opacity <= 0) {
		opacity = 0;
		fade_dir = 0;
		gtk_widget_hide(win);
	}
	gtk_widget_queue_draw(win);
	if (fade_dir)
		return TRUE;
	fade_id = 0;
	return FALSE;
}

static void fade(int dir)
{
	fade_dir = dir;
	if (!fade_id)
		fade_id = g_timeout_add(FRAME_MS, fade_step, NULL);
}

static gboolean hide_cb(gpointer data)
{
	(void)data;
	hide_id = 0;
	if (dragging)
		return FALSE;
	fade(-1);
	return FALSE;
}

/* On screen again for VISIBLE_MS from now */
static void show(void)
{
	GdkScreen *s = gdk_screen_get_default();

	if (!GTK_WIDGET_VISIBLE(win)) {
		gtk_window_move(GTK_WINDOW(win), 0, 0);
		gtk_window_resize(GTK_WINDOW(win), gdk_screen_get_width(s),
				  gdk_screen_get_height(s));
		gtk_widget_show(win);
	}
	if (opacity < 1)
		fade(+1);
	gtk_widget_queue_draw(win);
	if (hide_id)
		g_source_remove(hide_id);
	hide_id = g_timeout_add(VISIBLE_MS, hide_cb, NULL);
}

/* PulseAudio */

static void set_volume(double v)
{
	pa_cvolume cv = sink_volume;
	pa_operation *o;

	if (!ctx || pa_context_get_state(ctx) != PA_CONTEXT_READY || !sink ||
	    !cv.channels)
		return;
	v = CLAMP(v, 0, 1);
	pa_cvolume_set(&cv, cv.channels, (pa_volume_t)round(v * PA_VOLUME_NORM));
	o = pa_context_set_sink_volume_by_name(ctx, sink, &cv, NULL, NULL);
	if (o)
		pa_operation_unref(o);
	if (muted && v > 0) {
		o = pa_context_set_sink_mute_by_name(ctx, sink, 0, NULL, NULL);
		if (o)
			pa_operation_unref(o);
	}
}

static void sink_cb(pa_context *c, const pa_sink_info *i, int eol, void *data)
{
	double v;

	(void)c, (void)data;
	if (eol || !i)
		return;
	sink_volume = i->volume;
	v = (double)pa_cvolume_max(&i->volume) / PA_VOLUME_NORM;
	v = CLAMP(v, 0, 1);
	if (known && (fabs(v - volume) > 0.001 || !i->mute != !muted)) {
		volume = v;
		muted = i->mute;
		if (display_on && gconf_client_get_bool(gconf, KEY_MEEGO, NULL))
			show();
	}
	volume = v;
	muted = i->mute;
	known = TRUE;
}

static void server_cb(pa_context *c, const pa_server_info *i, void *data)
{
	pa_operation *o;

	(void)data;
	if (!i || !i->default_sink_name)
		return;
	if (!sink || strcmp(sink, i->default_sink_name)) {
		g_free(sink);
		sink = g_strdup(i->default_sink_name);
		known = FALSE;	/* another sink: its volume is no change */
	}
	o = pa_context_get_sink_info_by_name(c, sink, sink_cb, NULL);
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
	(void)idx, (void)data;
	if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) != PA_SUBSCRIPTION_EVENT_REMOVE)
		refresh(c);
}

static void connect_pulse(void);

static gboolean reconnect(gpointer data)
{
	(void)data;
	connect_pulse();
	return FALSE;
}

static void state_cb(pa_context *c, void *data)
{
	(void)data;
	switch (pa_context_get_state(c)) {
	case PA_CONTEXT_READY:
		pa_context_set_subscribe_callback(c, event_cb, NULL);
		pa_operation_unref(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK |
							PA_SUBSCRIPTION_MASK_SERVER,
							NULL, NULL));
		refresh(c);
		break;
	case PA_CONTEXT_FAILED:
	case PA_CONTEXT_TERMINATED:
		/* PulseAudio gone (restarted): again in a while */
		pa_context_unref(ctx);
		ctx = NULL;
		known = FALSE;
		g_timeout_add_seconds(3, reconnect, NULL);
		break;
	default:
		break;
	}
}

static void connect_pulse(void)
{
	ctx = pa_context_new(pa_glib_mainloop_get_api(pa_loop), "pro1x-volume-osd");
	pa_context_set_state_callback(ctx, state_cb, NULL);
	if (pa_context_connect(ctx, NULL, PA_CONTEXT_NOFAIL, NULL) < 0)
		fprintf(stderr, "pro1x-volume-osd: %s\n", pa_strerror(pa_context_errno(ctx)));
}

/* mce: "on", "dimmed", "off", "lpm" (the stand-by screen, patches/lpm) */
static void mce_signal(GDBusConnection *c, const char *sender, const char *path,
		       const char *iface, const char *signal, GVariant *p, gpointer data)
{
	const char *s;

	(void)c, (void)sender, (void)path, (void)iface, (void)signal, (void)data;
	if (!g_variant_is_of_type(p, G_VARIANT_TYPE("(s)")))
		return;
	g_variant_get(p, "(&s)", &s);
	display_on = strcmp(s, "off") && strcmp(s, "lpm");
	if (!display_on && win && GTK_WIDGET_VISIBLE(win)) {
		/* (nobody sees it fade) */
		if (fade_id)
			g_source_remove(fade_id);
		if (hide_id)
			g_source_remove(hide_id);
		fade_id = hide_id = 0;
		fade_dir = 0;
		opacity = 0;
		dragging = FALSE;
		gtk_widget_hide(win);
	}
}

static void mce_status_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);

	(void)data;
	if (v) {
		mce_signal(NULL, NULL, NULL, NULL, NULL, v, NULL);
		g_variant_unref(v);
	}
}

/* A tap or a drag sets the volume where it is */
static gboolean pointer_cb(GtkWidget *w, GdkEvent *e, gpointer data)
{
	double y;

	(void)data;
	if (e->type == GDK_BUTTON_PRESS)
		dragging = TRUE;
	else if (e->type == GDK_BUTTON_RELEASE)
		dragging = FALSE;
	else if (!dragging)
		return TRUE;
	y = e->type == GDK_MOTION_NOTIFY ? e->motion.y : e->button.y;
	volume = CLAMP(1 - y / w->allocation.height, 0, 1);
	set_volume(volume);
	show();
	return TRUE;
}

int main(int argc, char **argv)
{
	GdkScreen *s;
	GdkColormap *rgba;
	GDBusConnection *sys;

	gtk_init(&argc, &argv);
	gconf = gconf_client_get_default();
	level_image = theme_image(LEVEL_IMAGE);
	icon = theme_image(ICON);
	icon_muted = theme_image(ICON_MUTED);

	win = gtk_window_new(GTK_WINDOW_POPUP);
	s = gtk_widget_get_screen(win);
	rgba = gdk_screen_get_rgba_colormap(s);
	if (rgba)
		gtk_widget_set_colormap(win, rgba);
	gtk_widget_set_app_paintable(win, TRUE);
	gtk_widget_add_events(win, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
			      GDK_POINTER_MOTION_MASK);
	g_signal_connect(win, "expose-event", G_CALLBACK(expose_cb), NULL);
	g_signal_connect(win, "button-press-event", G_CALLBACK(pointer_cb), NULL);
	g_signal_connect(win, "button-release-event", G_CALLBACK(pointer_cb), NULL);
	g_signal_connect(win, "motion-notify-event", G_CALLBACK(pointer_cb), NULL);

	if ((sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL))) {
		/* (any sender: where no mce runs, pro1x-standby says it) */
		g_dbus_connection_signal_subscribe(sys, NULL, "com.nokia.mce.signal",
						   "display_status_ind", "/com/nokia/mce/signal",
						   NULL, G_DBUS_SIGNAL_FLAGS_NONE, mce_signal, NULL,
						   NULL);
		g_dbus_connection_call(sys, "com.nokia.mce", "/com/nokia/mce/request",
				       "com.nokia.mce.request", "get_display_status", NULL,
				       G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000,
				       NULL, mce_status_cb, NULL);
	}
	pa_loop = pa_glib_mainloop_new(NULL);
	connect_pulse();
	gtk_main();
	return 0;
}
