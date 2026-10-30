/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-camera for FreeBSD's Hildon (freebsd/camera/README.md): the
 * camera app of Leste (apps/pro1x-camera: Qt, GStreamer, libcamera) is
 * not portable, so this is a small one on pro1x-cam's code and
 * /dev/camss0. As on Fremantle: the viewfinder fills the window, a tap
 * focuses (rear camera: pro1x-camera's contrast sweep), the shutter button
 * or the camera key (or Enter) takes the picture at full size into
 * ~/MyDocs/DCIM, the other button switches cameras.
 *
 * A worker thread owns the camera: the viewfinder is the sensor's smaller
 * mode (rear 1920x1080, front 1632x1224) demosaicked at half size; a
 * picture stops it, runs take_picture() (exposure, the focus found, full
 * size, JPEG) and starts it again. GTK is only touched from the main loop
 * (g_idle_add).
 */

#include <sys/stat.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <hildon/hildon.h>
#include <gdk/gdkkeysyms.h>

#include "cam.h"
#include "sensor_tables.h"

static struct {
	struct cam	c;
	int		s;		/* the camera in use */
	GtkWidget	*window, *image, *status;
	GMutex		lock;
	GdkPixbuf	*pending;	/* the next viewfinder frame */
	volatile int	quit, shot, af, swap;
	int		focus;		/* the lens position (rear) */
	int		lens;
	/* the focus sweep in the viewfinder */
	int		af_pos[24], af_n, af_i, af_best, af_wait;
	double		af_bestv;
	uint16_t	*px;
} app;

static gboolean
show_frame(gpointer unused)
{
	GdkPixbuf *pb;

	g_mutex_lock(&app.lock);
	pb = app.pending;
	app.pending = NULL;
	g_mutex_unlock(&app.lock);
	if (pb != NULL) {
		gtk_image_set_from_pixbuf(GTK_IMAGE(app.image), pb);
		g_object_unref(pb);
	}
	return (FALSE);
}

static gboolean
show_status(gpointer text)
{
	gtk_label_set_text(GTK_LABEL(app.status), text);
	g_free(text);
	return (FALSE);
}

static void
status(const char *fmt, ...)
{
	va_list ap;
	char *t;

	va_start(ap, fmt);
	t = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	g_idle_add(show_status, t);
}

static void
free_rgb(guchar *pixels, gpointer unused)
{
	free(pixels);
}

/* the focus sweep, one step a frame (the move's frame dropped) */
static void
af_step(struct cam *c, int s, struct frame *f)
{
	double v;
	int p, n;

	if (app.af_wait) {
		app.af_wait = 0;
		return;
	}
	if (app.af_i >= 0) {
		v = sharpness(app.px, f->width, f->height);
		if (v > app.af_bestv) {
			app.af_bestv = v;
			app.af_best = app.focus;
		}
	}
	if (++app.af_i == 11) {
		for (p = app.af_best - 80, n = 11; p <= app.af_best + 80; p += 20)
			if (p != app.af_best && p >= 0 && p <= 1023 && n < 24)
				app.af_pos[n++] = p;
		app.af_n = n;
	}
	if (app.af_i < app.af_n) {
		lens_move(c, s, app.focus, app.af_pos[app.af_i]);
		app.focus = app.af_pos[app.af_i];
		app.af_wait = 1;
		return;
	}
	lens_move(c, s, app.focus, app.af_best);
	app.focus = app.af_best;
	app.af = 0;
	status("Focused (%d)", app.focus);
}

static int
viewfinder_frame(struct cam *c, int s, struct frame *f, void *arg)
{
	const struct sensor_ops *o = arg;
	uint32_t w, h;
	uint8_t *rgb;
	GdkPixbuf *pb;

	if (app.quit || app.shot || app.swap)
		return (1);
	raw10_unpack(f, app.px);
	if (app.af && app.lens) {
		if (app.af_n == 0) {
			for (app.af_n = 0; app.af_n < 11; app.af_n++)
				app.af_pos[app.af_n] = 100 * app.af_n;
			app.af_i = -1;
			app.af_bestv = -1;
			app.af_best = app.focus;
			status("Focusing...");
		}
		af_step(c, s, f);
	}
	rgb = debayer(app.px, f->width, f->height, o->bayer, 64, 1, &w, &h);
	if (rgb == NULL)
		return (0);
	rotate_rgb(&rgb, &w, &h, c->info.sensor[s].orientation == 1 ? 180 : 0);
	pb = gdk_pixbuf_new_from_data(rgb, GDK_COLORSPACE_RGB, FALSE, 8, w, h,
	    w * 3, free_rgb, NULL);
	g_mutex_lock(&app.lock);
	if (app.pending != NULL)
		g_object_unref(app.pending);	/* the slower main loop drops */
	else
		g_idle_add(show_frame, NULL);
	app.pending = pb;
	g_mutex_unlock(&app.lock);
	return (0);
}

static void
picture(struct cam *c, int s)
{
	struct shot_opts o;
	char dir[PATH_MAX], path[PATH_MAX];
	const char *home = getenv("HOME");
	time_t now = time(NULL);
	struct tm tm;

	snprintf(dir, sizeof(dir), "%s/MyDocs", home ? home : "/root");
	mkdir(dir, 0755);
	strlcat(dir, "/DCIM", sizeof(dir));
	mkdir(dir, 0755);
	localtime_r(&now, &tm);
	snprintf(path, sizeof(path), "%s/%04d%02d%02d_%02d%02d%02d.jpg", dir,
	    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
	    tm.tm_sec);
	status("Taking a picture...");
	shot_defaults(&o);
	o.out = path;
	o.focus = app.focus;
	o.small = 0;
	if (take_picture(c, s, &o) == 0)
		status("Saved %s", path);
	else
		status("The picture failed");
}

static gpointer
worker(gpointer unused)
{
	struct cam *c = &app.c;
	const struct sensor_ops *o;
	const struct sensor_mode *m;

	while (!app.quit) {
		int s = app.s;

		if ((o = sensor_ops(c, s)) == NULL || (m = sensor_mode(o,
		    strcmp(o->name, "IMX582") == 0 ? "1920x1080" :
		    "1632x1224")) == NULL) {
			status("No driver for this camera");
			return (NULL);
		}
		free(app.px);
		app.px = malloc((size_t)m->width * m->height * 2);
		if (app.px == NULL || sensor_power(c, s, 1) < 0) {
			status("The camera does not come up");
			return (NULL);
		}
		app.lens = c->info.sensor[s].has_lens && lens_power(c, s, 1) == 0;
		if (app.lens) {
			if (app.focus < 0)
				app.focus = 100;
			lens_move(c, s, 0, app.focus);
		}
		status("%s camera", cam_name(c, s));
		if (capture(c, s, m, 2, UINT_MAX - 8, viewfinder_frame,
		    (void *)(uintptr_t)o) < 0 && !app.quit && !app.shot &&
		    !app.swap) {
			status("No frames from the %s camera", cam_name(c, s));
			msleep(2000);
		}
		app.af = 0;
		app.af_n = 0;
		if (app.lens) {
			lens_move(c, s, app.focus, 0);
			lens_power(c, s, 0);
		}
		sensor_power(c, s, 0);
		if (app.shot) {
			picture(c, s);
			app.shot = 0;
		}
		if (app.swap) {
			app.s = (s + 1) % c->info.nsensors;
			app.focus = -1;
			app.swap = 0;
		}
	}
	return (NULL);
}

static gboolean
tapped(GtkWidget *w, GdkEventButton *ev, gpointer unused)
{
	if (app.lens && !app.af)
		app.af = 1;
	return (TRUE);
}

static void
take(GtkWidget *w, gpointer unused)
{
	app.shot = 1;
}

static void
swap(GtkWidget *w, gpointer unused)
{
	if (app.c.info.nsensors > 1)
		app.swap = 1;
}

static gboolean
key(GtkWidget *w, GdkEventKey *ev, gpointer unused)
{
	switch (ev->keyval) {
	case GDK_Return:
	case GDK_KP_Enter:
	case GDK_space:
#ifdef GDK_WebCam
	case GDK_WebCam:		/* the camera key (KEY_CAMERA) */
#endif
		app.shot = 1;
		return (TRUE);
	default:
		return (FALSE);
	}
}

static void
quit(GtkWidget *w, gpointer unused)
{
	app.quit = 1;
	gtk_main_quit();
}

int
main(int argc, char **argv)
{
	GtkWidget *vbox, *hbox, *ebox, *b;
	GThread *t = NULL;

	hildon_gtk_init(&argc, &argv);
	g_mutex_init(&app.lock);
	app.window = hildon_window_new();
	gtk_window_set_title(GTK_WINDOW(app.window), "Camera");
	g_signal_connect(app.window, "destroy", G_CALLBACK(quit), NULL);
	g_signal_connect(app.window, "key-press-event", G_CALLBACK(key), NULL);
	vbox = gtk_vbox_new(FALSE, 0);
	ebox = gtk_event_box_new();
	app.image = gtk_image_new();
	gtk_container_add(GTK_CONTAINER(ebox), app.image);
	g_signal_connect(ebox, "button-release-event", G_CALLBACK(tapped), NULL);
	gtk_box_pack_start(GTK_BOX(vbox), ebox, TRUE, TRUE, 0);
	hbox = gtk_hbox_new(FALSE, 8);
	app.status = gtk_label_new("");
	gtk_box_pack_start(GTK_BOX(hbox), app.status, TRUE, TRUE, 0);
	b = hildon_gtk_button_new(HILDON_SIZE_FINGER_HEIGHT);
	gtk_button_set_label(GTK_BUTTON(b), "Switch camera");
	g_signal_connect(b, "clicked", G_CALLBACK(swap), NULL);
	gtk_box_pack_start(GTK_BOX(hbox), b, FALSE, FALSE, 0);
	b = hildon_gtk_button_new(HILDON_SIZE_FINGER_HEIGHT);
	gtk_button_set_label(GTK_BUTTON(b), "Take picture");
	g_signal_connect(b, "clicked", G_CALLBACK(take), NULL);
	gtk_box_pack_start(GTK_BOX(hbox), b, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(app.window), vbox);
	gtk_widget_show_all(app.window);

	app.focus = -1;
	if (cam_open(&app.c, "/dev/camss0") < 0) {
		gtk_label_set_text(GTK_LABEL(app.status),
		    "No /dev/camss0 (kldload qcom_camss)");
	} else {
		/* the rear camera first */
		app.s = cam_find(&app.c, "rear");
		if (app.s < 0)
			app.s = 0;
		t = g_thread_new("camera", worker, NULL);
	}
	gtk_main();
	/* the worker ends the stream and powers the camera down */
	app.quit = 1;
	if (t != NULL)
		g_thread_join(t);
	return (0);
}
