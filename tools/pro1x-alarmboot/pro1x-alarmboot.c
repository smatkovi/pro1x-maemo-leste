/*
 * pro1x-alarmboot -- after the phone switched itself on for an alarm.
 *
 * The boot menu starts the system that set an RTC alarm when that alarm
 * powered the phone on (tools/pro1x-boot alarm-check, docs/alarm-power-on.md)
 * and leaves a note of it: /host/pro1x-boot/alarmboot on Linux (the
 * initramfs writes it at every such start and removes it at every other),
 * /var/run/pro1x-alarmboot on FreeBSD (rc.d/pro1x_alarmboot, from the
 * kernel environment). Started with the X session, this program does what
 * the Nokia N9's "acting dead" mode showed around such an alarm, in the
 * running system and in the N9's look:
 *
 *   - the alarm itself rings through alarmd and the system UI as always
 *     (their sound, snoozes and queue). While the system UI's alarm dialog
 *     rings (com.nokia.system_ui.signal alarm_dialog_status 5), a full
 *     screen window covers it with the N9's alarm screen: the time, the
 *     alarm's text, Snooze and Stop (Harmattan's clock alarm view, in the
 *     N9's pixels of 0.1 mm, drawn with cairo; the N9's own font and button
 *     pictures where the owner installed them, -t, else free ones). A tap
 *     answers through the system UI itself: the "turned face down" signal
 *     it listens for while it rings (mce's sig_device_orientation_ind)
 *     snoozes, as turning the phone over does; for Stop, once that dialog
 *     is gone, alarmd gets the Stop button as the answer (it takes the
 *     last one: a snoozed alarm stopped). Should the dialog not go within
 *     3 s the window steps aside for good and the system UI's own dialog
 *     is used; while that dialog is silent (status 6: its sound stopped
 *     after a minute, the face-down signal no longer heard) the window
 *     stays away as well;
 *   - should no alarm dialog have come 2 minutes after the alarm's time
 *     (alarmd or the system UI missing, the queue lost with FreeBSD's
 *     memory /var), it rings itself: the same N9 alarm screen and a tone
 *     through PulseAudio, 5 minutes at most; Snooze rings again after the
 *     alarm's snooze time (10 minutes where alarmd does not say); not when
 *     alarmd answers that it has no such alarm any more (one deleted after
 *     the RTC was set: Leste's alarmd leaves the RTC alarm then);
 *   - once the alarm is over (stopped or snoozed: a snoozed alarm sets the
 *     RTC again and switches the phone on again), it asks, as the N9's
 *     query dialogs look, whether to switch the phone off again; no answer
 *     in 60 s keeps it on. The N9 itself asked nothing: it switched itself
 *     off after the alarm unless its power key switched it on. With -N (or
 *     after-alarm=switch-off in the configuration file) this does that:
 *     "Switching off", 10 s to keep it on.
 *
 * Without a note for this system it ends at once. Once per alarm (a note
 * in the user's runtime directory). Without a display it still rings (the
 * tone alone) and keeps the phone on.
 *
 *   pro1x-alarmboot [-f NOTE] [-s SYSTEM] [-N] [-t THEME] [-d DPI] [-v]
 *       -f  the note (default as above), -s  this system's key
 *       (default: /host/pro1x-boot/current; FreeBSD: freebsd),
 *       -N  switch off after the alarm as the N9 did (10 s to keep it on),
 *       -t  a "blanco" theme directory (default: /usr/local/share/themes/
 *       blanco or /usr/share/themes/blanco, where installed),
 *       -d  the screen's dots per inch (default: X's), -v  say what it
 *       does on stderr
 *   pro1x-alarmboot -D ring|ask|off|switching [-L en|de] [-g WxH] [-t ...]
 *       one screen alone (no note, no D-Bus), to look at it; prints the
 *       button tapped (0 the upper)
 *
 * Configuration (optional): /etc/pro1x-alarmboot.conf, on FreeBSD
 * /usr/local/etc/pro1x-alarmboot.conf:
 *
 *   [pro1x-alarmboot]
 *   after-alarm=ask          # or switch-off (as -N)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <langinfo.h>
#include <libalarm.h>
#include <locale.h>
#include <math.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#define NOTE_PATH "/host/pro1x-boot/alarmboot"
#define CURRENT_PATH "/host/pro1x-boot/current"
#define CONF_PATH "/etc/pro1x-alarmboot.conf"
#else
#define NOTE_PATH "/var/run/pro1x-alarmboot"
#define CURRENT_PATH NULL
#define CONF_PATH "/usr/local/etc/pro1x-alarmboot.conf"
#endif

/* alarm_dialog_status (systemui/alarm_dialog-dbus-names.h) */
#define DIALOG_ON_SCREEN 5
#define DIALOG_NOT_RINGING 6
#define DIALOG_NOT_ON_SCREEN 7

#define RING_AFTER_DUE 120	/* s after the alarm's time without its dialog */
#define RING_AFTER_START 90	/* and at least this long after our start */
#define RING_FOR 300		/* the own ringing at most this long */
#define ASK_FOR 60		/* the question at most this long */
#define OFF_IN 10		/* -N: switched off after this long */
#define LIFETIME (30 * 60)	/* then give up */
#define STEP_ASIDE 3		/* s for the system UI's dialog to go after a tap */
#define OWN_SNOOZE 600		/* the own ringing's snooze without alarmd's */

/* The alarm screen's buttons, from the top: Snooze (the N9's negative
 * dialog button), Stop (its positive one) */
enum { BTN_SNOOZE, BTN_STOP };

static gboolean verbose, n9_off;
static GDBusConnection *sysbus;
static time_t due;		/* the alarm's real time */
static gboolean have_display, dialog_seen, asked, ringing, own_snoozed;
static guint ask_timer, ring_timer, own_snooze_timer;
static int own_snooze_secs = OWN_SNOOZE;
static volatile gint ring_stop;
static GThread *tone;

#define say(...) do { if (verbose) { fprintf(stderr, "pro1x-alarmboot: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* --- texts: the N9's own where it had them (its clock's and its shutdown
 * screen's catalogues), in English and German ----------------------------- */

static const struct texts {
	const char *snooze, *stop, *alarm;
	const char *ask_title, *ask_text, *off, *keep;
	const char *switching, *off_in;
} text_en = {
	"Snooze", "Stop", "Alarm",
	"Switch the phone off again?",
	"It was switched on for the alarm. Without an answer it stays on.",
	"Switch off", "Keep on",
	"Switching off", "The phone switches itself off again in %d s.",
}, text_de = {
	"Später erinnern", "Stoppen", "Weckzeit",
	"Telefon wieder ausschalten?",
	"Es wurde für den Wecker eingeschaltet. Ohne Antwort bleibt es an.",
	"Ausschalten", "Eingeschaltet lassen",
	"Wird ausgeschaltet", "Das Telefon schaltet sich in %d s wieder aus.",
}, *tx = &text_en;

static void pick_language(const char *lang)
{
	const gchar *const *l;

	if (lang) {
		tx = !strncmp(lang, "de", 2) ? &text_de : &text_en;
		return;
	}
	for (l = g_get_language_names(); *l; l++) {
		if (!strncmp(*l, "de", 2)) {
			tx = &text_de;
			return;
		}
		if (!strncmp(*l, "en", 2))
			return;
	}
}

/* HH:MM, or h:MM am/pm where the locale's time has a 12-hour clock */
static void format_time(time_t t, char *buf, size_t n)
{
	const char *f = nl_langinfo(T_FMT);
	struct tm tm;

	localtime_r(&t, &tm);
	if (f && (strstr(f, "%r") || strstr(f, "%I") || strstr(f, "%l"))) {
		strftime(buf, n, "%l:%M %p", &tm);
		if (buf[0] == ' ')
			memmove(buf, buf + 1, strlen(buf));
	} else {
		strftime(buf, n, "%H:%M", &tm);
	}
}

/* --- the N9 look ----------------------------------------------------------
 *
 * Harmattan's clock alarm view (libclockwidgets' AlarmNotificationView and
 * its style) and query dialog (libmeegotouch's open base theme), in the
 * N9's pixels, which its theme counts as 0.1 mm (251 dpi). The alarm: the
 * time (bold 64), 12 below it the alarm's text (light 32, 42 from the
 * sides, cut short), 40 below that Snooze, 36 below that Stop, both 408 x
 * 64 with bold 32 white labels, Snooze the dialog's negative (black)
 * button, Stop its positive (dark blue) one, all centred on black: what
 * the N9 showed in its acting-dead mode, where no compositor ran under the
 * dialog. The question: the query dialog with its buttons one below the
 * other and not cut to 322 (LargeButtonsQueryDialog): the title bold 32,
 * 17 below it the text light 24, 26 below that the buttons, 16 apart.
 */

#define FONT_BOLD "Nokia Pure Text,Open Sans,Sans Bold"
#define FONT_LIGHT "Nokia Pure Text,Open Sans,Sans Light"
#define DIALOG_PICS "meegotouch/images/theme/basement/meegotouch-dialogs"
#define CORNER 22		/* blanco's CORNER_MARGINS of these pictures */

enum screen_kind { SCR_RING, SCR_ASK, SCR_OFF_SOON, SCR_SWITCHING };

struct button {
	const char *label;
	gboolean positive;
	double x, y, w, h;
};

static struct {
	GtkWidget *win, *area;
	enum screen_kind kind;
	char time[32], text[160], title[160];
	struct button btn[2];
	int nbtn, pressed;
	gboolean busy;			/* a tap is being answered */
	void (*clicked)(int index);
	double dpi;
	int width, height;		/* -g, else the screen's */
	gboolean demo;
	cairo_surface_t *pic[2][2];	/* [positive][pressed] */
} scr = { .pressed = -1 };

static void free_pictures(void)
{
	for (int pos = 0; pos < 2; pos++)
		for (int pr = 0; pr < 2; pr++) {
			if (scr.pic[pos][pr])
				cairo_surface_destroy(scr.pic[pos][pr]);
			scr.pic[pos][pr] = NULL;
		}
}

/* The N9's dialog buttons from an installed blanco theme, all four or none */
static void load_pictures(const char *theme)
{
	static const char *const dirs[] = {
		"/usr/local/share/themes/blanco", "/usr/share/themes/blanco", NULL
	};
	const char *const *d = dirs;

	for (; theme || *d; theme = NULL) {
		const char *dir = theme ? theme : *d++;
		gboolean ok = TRUE;

		for (int pos = 0; pos < 2; pos++)
			for (int pr = 0; pr < 2; pr++) {
				gchar *p = g_strdup_printf("%s/" DIALOG_PICS "/meegotouch-dialog-button-%s%s.png",
							   dir, pos ? "positive" : "negative",
							   pr ? "-pressed" : "");
				cairo_surface_t *s = cairo_image_surface_create_from_png(p);

				if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS ||
				    cairo_image_surface_get_width(s) <= 2 * CORNER ||
				    cairo_image_surface_get_height(s) <= 2 * CORNER) {
					cairo_surface_destroy(s);
					s = NULL;
					ok = FALSE;
				}
				scr.pic[pos][pr] = s;
				g_free(p);
			}
		if (ok) {
			say("the N9's button pictures from %s", dir);
			return;
		}
		free_pictures();
	}
}

/* Screen pixels per N9 pixel: 0.1 mm at the screen's dpi; where that is
 * unknown or absurd, the N9's 480 px (its short side) as three quarters of
 * the short side */
static double scale_for(int w, int h)
{
	double short_side = MIN(w, h), s = scr.dpi > 0 ? scr.dpi / 254.0 : 0;

	if (s * 480 > short_side * 0.95 || s * 480 < short_side * 0.4)
		s = short_side * 0.75 / 480;
	return s;
}

static PangoLayout *text_layout(cairo_t *cr, const char *text, const char *font,
				double px, double width, gboolean wrap)
{
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_from_string(font);

	pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
	pango_layout_set_font_description(l, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(l, text, -1);
	pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
	pango_layout_set_width(l, (int)(width * PANGO_SCALE));
	if (wrap)
		pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
	else
		pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	return l;
}

static double layout_height(PangoLayout *l)
{
	int w, h;

	pango_layout_get_pixel_size(l, &w, &h);
	return h;
}

/* A layout (centred in its width) with its left edge at x, top at y */
static void show_layout(cairo_t *cr, PangoLayout *l, double x, double y, double grey)
{
	cairo_move_to(cr, x, y);
	cairo_set_source_rgb(cr, grey, grey, grey);
	pango_cairo_show_layout(cr, l);
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r)
{
	r = MIN(r, MIN(w, h) / 2);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

/* One part of a picture stretched over a rectangle */
static void pic_part(cairo_t *cr, cairo_surface_t *p, double sx, double sy, double sw,
		     double sh, double dx, double dy, double dw, double dh)
{
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return;
	cairo_save(cr);
	cairo_rectangle(cr, dx, dy, dw, dh);
	cairo_clip(cr);
	cairo_translate(cr, dx, dy);
	cairo_scale(cr, dw / sw, dh / sh);
	cairo_set_source_surface(cr, p, -sx, -sy);
	cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_paint(cr);
	cairo_restore(cr);
}

/* A picture with fixed corners (MeeGo's scalable images), the corners
 * scaled as the rest of the screen */
static void pic_nine(cairo_t *cr, cairo_surface_t *p, double x, double y, double w,
		     double h, double s)
{
	double iw = cairo_image_surface_get_width(p), ih = cairo_image_surface_get_height(p);
	double m = CORNER, d = MIN(CORNER * s, MIN(w, h) / 2);
	double sxs[4] = { 0, m, iw - m, iw }, sys[4] = { 0, m, ih - m, ih };
	double dxs[4] = { x, x + d, x + w - d, x + w }, dys[4] = { y, y + d, y + h - d, y + h };

	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			pic_part(cr, p, sxs[i], sys[j], sxs[i + 1] - sxs[i], sys[j + 1] - sys[j],
				 dxs[i], dys[j], dxs[i + 1] - dxs[i], dys[j + 1] - dys[j]);
}

/* A dialog button: the N9's picture where installed, else drawn alike (a
 * 2 px rim, a gradient to black, or to dark blue for the positive one) */
static void draw_button(cairo_t *cr, const struct button *b, gboolean pressed,
			double s, double font_px)
{
	cairo_surface_t *p = scr.pic[b->positive][pressed];
	PangoLayout *l;

	if (p) {
		pic_nine(cr, p, b->x, b->y, b->w, b->h, s);
	} else {
		static const double rim[2][2][3] = {
			{ { 0.21, 0.21, 0.21 }, { 0.17, 0.17, 0.17 } },
			{ { 0.18, 0.27, 0.34 }, { 0.11, 0.16, 0.20 } },
		}, top[2][2][3] = {
			{ { 0.075, 0.075, 0.075 }, { 0.06, 0.06, 0.06 } },
			{ { 0.086, 0.169, 0.235 }, { 0.051, 0.102, 0.141 } },
		}, bottom[2][2][3] = {
			{ { 0, 0, 0 }, { 0, 0, 0 } },
			{ { 0.012, 0.102, 0.173 }, { 0.008, 0.063, 0.102 } },
		};
		const double *c = rim[b->positive][pressed];
		double r = 19 * s, w = MAX(1, 2 * s);
		cairo_pattern_t *g;

		rounded(cr, b->x, b->y, b->w, b->h, r);
		cairo_set_source_rgb(cr, c[0], c[1], c[2]);
		cairo_fill(cr);
		g = cairo_pattern_create_linear(0, b->y, 0, b->y + b->h);
		c = top[b->positive][pressed];
		cairo_pattern_add_color_stop_rgb(g, 0, c[0], c[1], c[2]);
		c = bottom[b->positive][pressed];
		cairo_pattern_add_color_stop_rgb(g, 0.6, c[0], c[1], c[2]);
		cairo_pattern_add_color_stop_rgb(g, 1, c[0], c[1], c[2]);
		rounded(cr, b->x + w, b->y + w, b->w - 2 * w, b->h - 2 * w, r - w);
		cairo_set_source(cr, g);
		cairo_fill(cr);
		cairo_pattern_destroy(g);
	}
	l = text_layout(cr, b->label, FONT_BOLD, font_px, b->w - 24 * s, FALSE);
	show_layout(cr, l, b->x + 12 * s, b->y + (b->h - layout_height(l)) / 2,
		    pressed ? 0.475 : 1);
	g_object_unref(l);
}

static void draw_screen(cairo_t *cr, int W, int H)
{
	double s = scale_for(W, H), y, total, colw = 408 * s, bh = 64 * s;
	gboolean portrait = H > W;
	double dlgw = ((portrait ? 448 : 544) - 32) * s, textw = W;
	double gap_ab = 0, gap_b = 0, gap_btn = 0, font_btn = 0, ah, bh2 = 0;
	PangoLayout *a, *b = NULL;

	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);

	switch (scr.kind) {
	case SCR_RING:
		textw = ((portrait ? 480 : 544) - 2 * 42) * s;
		a = text_layout(cr, scr.time, FONT_BOLD, 64 * s, W, FALSE);
		b = text_layout(cr, scr.text, FONT_LIGHT, 32 * s, textw, FALSE);
		gap_ab = 12 * s, gap_b = 40 * s, gap_btn = 36 * s, font_btn = 32 * s;
		break;
	case SCR_ASK:
	case SCR_OFF_SOON:
		textw = dlgw;
		a = text_layout(cr, scr.title, FONT_BOLD, 32 * s, dlgw, TRUE);
		b = text_layout(cr, scr.text, FONT_LIGHT, 24 * s, dlgw, TRUE);
		gap_ab = 17 * s, gap_b = 26 * s, gap_btn = 16 * s, font_btn = 28 * s;
		break;
	default:
		a = text_layout(cr, scr.title, FONT_BOLD, 32 * s, dlgw, TRUE);
		break;
	}
	ah = layout_height(a);
	if (b)
		bh2 = layout_height(b);
	total = ah + (b ? gap_ab + bh2 : 0);
	if (scr.nbtn)
		total += gap_b + scr.nbtn * bh + (scr.nbtn - 1) * gap_btn;
	y = (H - total) / 2;

	show_layout(cr, a, (W - pango_layout_get_width(a) / (double)PANGO_SCALE) / 2, y, 1);
	y += ah;
	if (b) {
		y += gap_ab;
		show_layout(cr, b, (W - textw) / 2, y, 1);
		y += bh2;
	}
	y += gap_b;
	for (int i = 0; i < scr.nbtn; i++) {
		struct button *bt = &scr.btn[i];

		bt->x = (W - colw) / 2, bt->y = y, bt->w = colw, bt->h = bh;
		draw_button(cr, bt, scr.pressed == i, s, font_btn);
		y += bh + gap_btn;
	}
	g_object_unref(a);
	if (b)
		g_object_unref(b);
}

static gboolean on_expose(GtkWidget *w, GdkEventExpose *ev, gpointer unused)
{
	GtkAllocation al;
	cairo_t *cr = gdk_cairo_create(gtk_widget_get_window(w));

	(void)ev; (void)unused;
	gtk_widget_get_allocation(w, &al);
	draw_screen(cr, al.width, al.height);
	cairo_destroy(cr);
	return TRUE;
}

static int hit(double x, double y)
{
	for (int i = 0; i < scr.nbtn; i++) {
		const struct button *b = &scr.btn[i];

		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h)
			return i;
	}
	return -1;
}

static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer unused)
{
	(void)unused;
	if (scr.busy || ev->type != GDK_BUTTON_PRESS)
		return TRUE;
	scr.pressed = hit(ev->x, ev->y);
	gtk_widget_queue_draw(w);
	return TRUE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *ev, gpointer unused)
{
	int i = scr.pressed;

	(void)unused;
	if (scr.busy)
		return TRUE;
	if (i >= 0 && hit(ev->x, ev->y) == i && scr.clicked) {
		scr.busy = TRUE;	/* the button stays pressed meanwhile */
		scr.clicked(i);
		return TRUE;
	}
	scr.pressed = -1;
	gtk_widget_queue_draw(w);
	return TRUE;
}

static void set_cardinal(GtkWidget *w, const char *name, gulong value)
{
	gdk_property_change(gtk_widget_get_window(w), gdk_atom_intern(name, FALSE),
			    gdk_atom_intern("CARDINAL", FALSE), 32, GDK_PROP_MODE_REPLACE,
			    (const guchar *)&value, 1);
}

/* The screen's window: full screen, above Hildon's dialogs and its screen
 * lock (hildon-desktop's stacking layer 10, the system UI's power key
 * menu's; the lock's is 9), turned with the screen when hildon-desktop
 * turns it to portrait (the keyboard closed, as the MeeGo look does) */
static void screen_show(enum screen_kind kind, void (*clicked)(int))
{
	GdkScreen *gs;

	if (!have_display)
		return;
	if (!scr.win) {
		gs = gdk_screen_get_default();
		scr.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
		gtk_window_set_title(GTK_WINDOW(scr.win), "Alarm");
		gtk_window_set_decorated(GTK_WINDOW(scr.win), FALSE);
		gtk_window_set_default_size(GTK_WINDOW(scr.win),
					    scr.width ? scr.width : gdk_screen_get_width(gs),
					    scr.height ? scr.height : gdk_screen_get_height(gs));
		gtk_window_move(GTK_WINDOW(scr.win), 0, 0);
		g_signal_connect(scr.win, "delete-event", G_CALLBACK(gtk_true), NULL);
		scr.area = gtk_drawing_area_new();
		gtk_widget_add_events(scr.area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
		g_signal_connect(scr.area, "expose-event", G_CALLBACK(on_expose), NULL);
		g_signal_connect(scr.area, "button-press-event", G_CALLBACK(on_press), NULL);
		g_signal_connect(scr.area, "button-release-event", G_CALLBACK(on_release), NULL);
		gtk_container_add(GTK_CONTAINER(scr.win), scr.area);
		gtk_widget_realize(scr.win);
		if (!scr.demo) {
			set_cardinal(scr.win, "_HILDON_STACKING_LAYER", 10);
			gtk_window_fullscreen(GTK_WINDOW(scr.win));
		}
		set_cardinal(scr.win, "_HILDON_PORTRAIT_MODE_SUPPORT", 1);
	}
	scr.kind = kind;
	scr.clicked = clicked;
	scr.pressed = -1;
	scr.busy = FALSE;
	gtk_widget_show_all(scr.win);
	gtk_window_present(GTK_WINDOW(scr.win));
	gtk_widget_queue_draw(scr.area);
}

static void screen_hide(void)
{
	if (scr.win)
		gtk_widget_hide(scr.win);
	scr.busy = FALSE;
	scr.pressed = -1;
}

static void ring_screen(const char *time_text, const char *text, void (*clicked)(int))
{
	g_strlcpy(scr.time, time_text, sizeof(scr.time));
	g_strlcpy(scr.text, text && *text ? text : tx->alarm, sizeof(scr.text));
	scr.btn[BTN_SNOOZE] = (struct button){ .label = tx->snooze, .positive = FALSE };
	scr.btn[BTN_STOP] = (struct button){ .label = tx->stop, .positive = TRUE };
	scr.nbtn = 2;
	screen_show(SCR_RING, clicked);
}

/* --- the system ------------------------------------------------------------ */

/* key=value lines; value of KEY into buf, FALSE without it */
static gboolean note_get(const char *text, const char *key, char *buf, size_t n)
{
	size_t kl = strlen(key);
	const char *p = text;

	while (p && *p) {
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			const char *v = p + kl + 1, *e = strchr(v, '\n');
			size_t l = e ? (size_t)(e - v) : strlen(v);
			if (l >= n)
				l = n - 1;
			memcpy(buf, v, l);
			buf[l] = 0;
			return TRUE;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return FALSE;
}

static void mce_call(const char *method, GVariant *args)
{
	if (!sysbus) {
		if (args)
			g_variant_unref(g_variant_ref_sink(args));
		return;
	}
	g_dbus_connection_call(sysbus, "com.nokia.mce", "/com/nokia/mce/request",
			       "com.nokia.mce.request", method, args, NULL,
			       G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
}

/* the display on and unlocked, for the screens */
static void wake_screen(void)
{
	mce_call("req_tklock_mode_change", g_variant_new("(s)", "unlocked"));
	mce_call("req_display_state_on", NULL);
}

static void power_off(void)
{
	GError *err = NULL;

	say("switching off");
	if (scr.demo)
		return;
#ifdef __linux__
	/* as the power key menu does: dsme shuts down (the RTC alarm of a
	 * snoozed or the next alarm stays set) */
	GVariant *r = g_dbus_connection_call_sync(sysbus, "com.nokia.dsme",
						  "/com/nokia/dsme/request",
						  "com.nokia.dsme.request",
						  "req_shutdown", NULL, NULL,
						  G_DBUS_CALL_FLAGS_NONE, 5000,
						  NULL, &err);
	if (r)
		g_variant_unref(r);
#else
	g_spawn_command_line_async("/sbin/shutdown -p now", &err);
#endif
	if (err) {
		fprintf(stderr, "pro1x-alarmboot: switching off: %s\n", err->message);
		g_error_free(err);
	}
}

static void quit(void)
{
	if (have_display)
		gtk_main_quit();
	else
		exit(0);
}

static gboolean quit_later(gpointer unused)
{
	(void)unused;
	quit();
	return FALSE;
}

/* --- after the alarm ------------------------------------------------------- */

static guint answer_timer;
static int off_in;

/* "Switching off" (the N9's shutdown screen's words) while dsme or
 * shutdown(8) end the session */
static void switching_off(void)
{
	scr.nbtn = 0;
	g_strlcpy(scr.title, tx->switching, sizeof(scr.title));
	screen_show(SCR_SWITCHING, NULL);
	gdk_window_process_all_updates();
	gdk_flush();
	power_off();
	g_timeout_add_seconds(20, quit_later, NULL);
}

static void answered(int button)
{
	if (answer_timer)
		g_source_remove(answer_timer);
	answer_timer = 0;
	if (button == 0 && scr.kind == SCR_ASK) {
		switching_off();
		return;
	}
	say("staying on");
	screen_hide();
	quit();
}

static gboolean ask_timeout(gpointer unused)
{
	(void)unused;
	answer_timer = 0;
	say("no answer: staying on");
	screen_hide();
	quit();
	return FALSE;
}

static gboolean off_countdown(gpointer unused)
{
	(void)unused;
	if (--off_in > 0) {
		g_snprintf(scr.text, sizeof(scr.text), tx->off_in, off_in);
		gtk_widget_queue_draw(scr.area);
		return TRUE;
	}
	answer_timer = 0;
	switching_off();
	return FALSE;
}

/* "Switch the phone off again?"; no answer keeps it on. With -N as the
 * N9: off in 10 s unless kept on */
static gboolean ask(gpointer unused)
{
	(void)unused;
	if (asked || ringing || own_snoozed)
		return FALSE;
	asked = TRUE;
	if (!have_display) {
		say("no display: staying on");
		quit();
		return FALSE;
	}
	wake_screen();
	if (n9_off) {
		say("switching off in %d s unless kept on", OFF_IN);
		off_in = OFF_IN;
		g_strlcpy(scr.title, tx->switching, sizeof(scr.title));
		g_snprintf(scr.text, sizeof(scr.text), tx->off_in, off_in);
		scr.btn[0] = (struct button){ .label = tx->keep, .positive = FALSE };
		scr.nbtn = 1;
		screen_show(SCR_OFF_SOON, answered);
		answer_timer = g_timeout_add_seconds(1, off_countdown, NULL);
		return FALSE;
	}
	say("asking whether to switch off");
	g_strlcpy(scr.title, tx->ask_title, sizeof(scr.title));
	g_strlcpy(scr.text, tx->ask_text, sizeof(scr.text));
	scr.btn[0] = (struct button){ .label = tx->off, .positive = TRUE };
	scr.btn[1] = (struct button){ .label = tx->keep, .positive = FALSE };
	scr.nbtn = 2;
	screen_show(SCR_ASK, answered);
	answer_timer = g_timeout_add_seconds(ASK_FOR, ask_timeout, NULL);
	return FALSE;
}

static gboolean ask_later(gpointer unused)
{
	(void)unused;
	ask_timer = 0;
	ask(NULL);
	return FALSE;
}

/* --- the own ringing ------------------------------------------------------ */

/* A tone of its own, through PulseAudio: four beeps, a pause, again */
static gpointer tone_thread(gpointer unused)
{
	static const pa_sample_spec ss = { PA_SAMPLE_S16LE, 44100, 1 };
	enum { RATE = 44100, BEEP = RATE * 15 / 100, GAP = RATE / 10, PAUSE = RATE * 8 / 10 };
	static int16_t pattern[4 * (BEEP + GAP) + PAUSE];
	pa_simple *s;
	int err, i, k;

	(void)unused;
	for (k = 0; k < 4; k++)
		for (i = 0; i < BEEP; i++)
			pattern[k * (BEEP + GAP) + i] =
				(int16_t)(24000 * sin(2 * M_PI * 880 * i / RATE));
	s = pa_simple_new(NULL, "pro1x-alarmboot", PA_STREAM_PLAYBACK, NULL,
			  "alarm", &ss, NULL, NULL, &err);
	if (!s) {
		fprintf(stderr, "pro1x-alarmboot: no sound: %s\n", pa_strerror(err));
		return NULL;
	}
	while (!g_atomic_int_get(&ring_stop))
		if (pa_simple_write(s, pattern, sizeof(pattern), &err) < 0)
			break;
	pa_simple_flush(s, NULL);
	pa_simple_free(s);
	return NULL;
}

/* The own ringing ends: stopped (then the question), snoozed (it rings
 * again after the snooze time) or given way to the system UI's dialog */
static void ring_end(gboolean snoozed)
{
	if (!ringing)
		return;
	ringing = FALSE;
	g_atomic_int_set(&ring_stop, 1);
	if (tone) {
		g_thread_join(tone);
		tone = NULL;
	}
	if (ring_timer)
		g_source_remove(ring_timer);
	ring_timer = 0;
	screen_hide();
	if (!snoozed)
		ask(NULL);
}

static gboolean ring_timeout(gpointer unused)
{
	(void)unused;
	ring_timer = 0;
	say("rang for %d s", RING_FOR);
	ring_end(FALSE);
	return FALSE;
}

static void ring_now(void);

static gboolean own_snooze_over(gpointer unused)
{
	(void)unused;
	own_snooze_timer = 0;
	own_snoozed = FALSE;
	if (!dialog_seen && !asked && !ringing) {
		say("ringing again");
		ring_now();
	}
	return FALSE;
}

static void own_ring_clicked(int button)
{
	if (button == BTN_SNOOZE) {
		say("snoozed for %d s", own_snooze_secs);
		own_snoozed = TRUE;
		ring_end(TRUE);
		own_snooze_timer = g_timeout_add_seconds(own_snooze_secs, own_snooze_over, NULL);
	} else {
		say("stopped");
		ring_end(FALSE);
	}
}

/* --- alarmd ---------------------------------------------------------------- */

/* alarmd's answer for the alarm: 1 there is one, 0 it has none, -1 no answer */
static int alarmd_has_alarm(time_t now)
{
	cookie_t *c = alarmd_event_query(due - 600, now + 900, 0, 0, NULL);
	int n = 0;

	if (!c)
		return -1;
	for (int i = 0; c[i]; i++) {
		alarm_event_t *e = alarmd_event_get(c[i]);
		if (e && (e->flags & (ALARM_EVENT_ACTDEAD | ALARM_EVENT_BOOT)))
			n++;
		if (e)
			alarm_event_delete(e);
	}
	free(c);
	return n > 0;
}

/* The alarm that rings now (its trigger passed, not yet answered), or
 * without one the next: its time, text and buttons (their places among
 * its actions are alarmd's answers) */
struct alarm_info {
	cookie_t cookie;
	time_t when;
	char text[128];
	int snooze, stop;
	int snooze_secs;
};

static gboolean find_alarm(time_t now, gboolean ringing_only, struct alarm_info *ai)
{
	cookie_t *c = alarmd_event_query(due - 600, now + 900, 0, 0, NULL);
	gboolean found = FALSE, best_rings = FALSE;
	time_t best = 0;

	if (!c)
		return FALSE;
	for (int i = 0; c[i]; i++) {
		alarm_event_t *e = alarmd_event_get(c[i]);
		time_t t;
		gboolean rings, better;

		if (!e)
			continue;
		t = alarm_event_get_trigger(e);
		rings = t <= now + 5;
		if (!found)
			better = TRUE;
		else if (rings != best_rings)
			better = rings;
		else
			better = rings ? t > best : t < best;
		if ((e->flags & (ALARM_EVENT_ACTDEAD | ALARM_EVENT_BOOT)) &&
		    !(e->flags & ALARM_EVENT_DISABLED) && (rings || !ringing_only) && better) {
			const char *m = alarm_event_get_message(e);

			if (!m || !*m)
				m = alarm_event_get_title(e);
			found = TRUE;
			best = t;
			best_rings = rings;
			ai->cookie = c[i];
			ai->when = t;
			g_strlcpy(ai->text, m ? m : "", sizeof(ai->text));
			ai->snooze = ai->stop = -1;
			for (int k = 0; k < (int)e->action_cnt; k++) {
				alarm_action_t *a = &e->action_tab[k];
				const char *label;

				if (!alarm_action_is_button(a))
					continue;
				label = alarm_action_get_label(a);
				if (a->flags & ALARM_ACTION_TYPE_SNOOZE)
					ai->snooze = k;
				else if (ai->stop < 0 || (label && !strcmp(label, "cloc_bd_stop")))
					ai->stop = k;
			}
			ai->snooze_secs = e->snooze_secs > 0 ? (int)e->snooze_secs : OWN_SNOOZE;
		}
		alarm_event_delete(e);
	}
	free(c);
	return found;
}

/* --- the N9 screen over the system UI's alarm dialog ---------------------- */

enum { NONE, SNOOZING, STOPPING };

static struct {
	gboolean shown, stepped_aside;
	struct alarm_info alarm;
	int pending;			/* the tap being answered */
	guint step_aside;
} ov;

/* What mce says when the phone is turned over: the system UI's ringing
 * alarm dialog snoozes on face_up then face_down */
static void turned_face_down(void)
{
	static const char *const face[2] = { "face_up", "face_down" };
	gchar *o = NULL, *st = NULL, *f = NULL;
	gint32 x = 0, y = 0, z = 0;
	GVariant *r = g_dbus_connection_call_sync(sysbus, "com.nokia.mce", "/com/nokia/mce/request",
						  "com.nokia.mce.request", "get_device_orientation",
						  NULL, G_VARIANT_TYPE("(sssiii)"),
						  G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);

	if (r) {
		g_variant_get(r, "(sssiii)", &o, &st, &f, &x, &y, &z);
		g_variant_unref(r);
	}
	/* the orientation as it is: hildon-desktop turns nothing */
	for (int i = 0; i < 2; i++)
		g_dbus_connection_emit_signal(sysbus, NULL, "/com/nokia/mce/signal",
					      "com.nokia.mce.signal", "sig_device_orientation_ind",
					      g_variant_new("(sssiii)", o ? o : "landscape",
							    st ? st : "off_stand", face[i], x, y, z),
					      NULL);
	g_dbus_connection_flush_sync(sysbus, NULL, NULL);
	g_free(o);
	g_free(st);
	g_free(f);
}

static void overlay_hide(void)
{
	if (ov.step_aside)
		g_source_remove(ov.step_aside);
	ov.step_aside = 0;
	if (ov.shown && !ringing)
		screen_hide();
	ov.shown = FALSE;
}

/* The system UI's dialog went (or shows the next alarm): after a Stop tap
 * its snooze is in, and alarmd now gets the stop */
static void overlay_settle(void)
{
	if (ov.pending == STOPPING) {
		say("stop for alarm %d (button %d)", (int)ov.alarm.cookie, ov.alarm.stop);
		if (alarmd_ack_dialog(ov.alarm.cookie, ov.alarm.stop) == -1)
			fprintf(stderr, "pro1x-alarmboot: alarmd did not take the stop\n");
	}
	ov.pending = NONE;
}

static gboolean step_aside(gpointer unused)
{
	(void)unused;
	ov.step_aside = 0;
	say("the alarm dialog did not go: its own buttons from now on");
	ov.pending = NONE;
	ov.stepped_aside = TRUE;
	overlay_hide();
	return FALSE;
}

static void overlay_clicked(int button)
{
	say("%s tapped", button == BTN_SNOOZE ? "snooze" : "stop");
	ov.pending = button == BTN_SNOOZE ? SNOOZING : STOPPING;
	turned_face_down();
	ov.step_aside = g_timeout_add_seconds(STEP_ASIDE, step_aside, NULL);
}

static void overlay_show(void)
{
	char when[32];

	if (ov.stepped_aside || !have_display)
		return;
	if (!find_alarm(time(NULL), TRUE, &ov.alarm) || ov.alarm.snooze < 0 || ov.alarm.stop < 0) {
		say("no clock alarm with Snooze and Stop rings: the system UI's dialog alone");
		overlay_hide();
		return;
	}
	format_time(ov.alarm.when, when, sizeof(when));
	say("the N9 screen over alarm %d (%s, snooze %d, stop %d)", (int)ov.alarm.cookie,
	    when, ov.alarm.snooze, ov.alarm.stop);
	if (ov.step_aside)
		g_source_remove(ov.step_aside);
	ov.step_aside = 0;
	ov.shown = TRUE;
	wake_screen();
	ring_screen(when, ov.alarm.text, overlay_clicked);
}

/* --- when to ring ------------------------------------------------------------ */

/* The tone first; then the screen, with the alarm's text and snooze time
 * where alarmd tells them */
static void ring_now(void)
{
	struct alarm_info ai;
	char when[32];

	ringing = TRUE;
	g_atomic_int_set(&ring_stop, 0);
	tone = g_thread_new("tone", tone_thread, NULL);
	ring_timer = g_timeout_add_seconds(RING_FOR, ring_timeout, NULL);
	wake_screen();
	format_time(due, when, sizeof(when));
	ring_screen(when, NULL, own_ring_clicked);
	if (find_alarm(time(NULL), FALSE, &ai) && ringing && !ov.shown) {
		own_snooze_secs = ai.snooze_secs;
		format_time(ai.when, when, sizeof(when));
		ring_screen(when, ai.text, own_ring_clicked);
	}
}

static gboolean ring_check(gpointer unused)
{
	time_t now = time(NULL);
	int has;

	(void)unused;
	if (dialog_seen || asked)
		return FALSE;
	has = alarmd_has_alarm(now);
	say("no alarm dialog %ld s after the alarm; alarmd: %s", (long)(now - due),
	    has < 0 ? "no answer" : has ? "has it" : "has none");
	if (has == 0) {
		/* deleted after the RTC was set: nothing to ring for */
		ask(NULL);
		return FALSE;
	}
	say("ringing");
	ring_now();
	return FALSE;
}

static void dialog_status(GDBusConnection *c, const gchar *sender, const gchar *path,
			  const gchar *iface, const gchar *signal, GVariant *params,
			  gpointer unused)
{
	guint32 st = 0;

	(void)c; (void)sender; (void)path; (void)iface; (void)signal; (void)unused;
	if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(u)")))
		return;
	g_variant_get(params, "(u)", &st);
	say("alarm dialog status %u", st);
	if (st == DIALOG_ON_SCREEN) {
		/* (the next alarm, after a tap's) */
		overlay_settle();
		dialog_seen = TRUE;
		/* our own ringing gives way to the real alarm */
		if (own_snooze_timer)
			g_source_remove(own_snooze_timer);
		own_snooze_timer = 0;
		own_snoozed = FALSE;
		if (ringing) {
			asked = TRUE;	/* the real one's end asks */
			ring_end(FALSE);
			asked = FALSE;
		}
		if (ask_timer)
			g_source_remove(ask_timer);
		ask_timer = 0;
		if (!asked)
			overlay_show();
	} else if (st == DIALOG_NOT_RINGING) {
		/* its sound stopped (after a minute): it no longer hears the
		 * face-down signal, its own buttons answer */
		ov.pending = NONE;
		overlay_hide();
	} else if (st == DIALOG_NOT_ON_SCREEN) {
		overlay_settle();
		overlay_hide();
		/* stopped or snoozed: ask once it stays gone a moment */
		if (dialog_seen && !asked && !ask_timer)
			ask_timer = g_timeout_add_seconds(5, ask_later, NULL);
	}
}

static gboolean give_up(gpointer unused)
{
	(void)unused;
	if (ringing || asked || own_snoozed || ov.shown)
		return FALSE;
	say("no alarm dialog in %d min: done", LIFETIME / 60);
	quit();
	return FALSE;
}

/* -D: one screen alone, to look at it */
static void demo_clicked(int button)
{
	printf("%d\n", button);
	fflush(stdout);
	gtk_main_quit();
}

static int demo(const char *what)
{
	char when[32];

	scr.demo = TRUE;
	format_time(time(NULL), when, sizeof(when));
	if (!strcmp(what, "ring")) {
		ring_screen(when, NULL, demo_clicked);
	} else if (!strcmp(what, "ask") || !strcmp(what, "off")) {
		n9_off = !strcmp(what, "off");
		ask(NULL);
		if (answer_timer)
			g_source_remove(answer_timer);
		answer_timer = 0;
		scr.clicked = demo_clicked;
	} else if (!strcmp(what, "switching")) {
		scr.nbtn = 0;
		g_strlcpy(scr.title, tx->switching, sizeof(scr.title));
		screen_show(SCR_SWITCHING, NULL);
	} else {
		fprintf(stderr, "pro1x-alarmboot: -D ring|ask|off|switching\n");
		return 2;
	}
	gtk_main();
	return 0;
}

static void read_conf(void)
{
	GKeyFile *kf = g_key_file_new();
	gchar *v;

	if (g_key_file_load_from_file(kf, CONF_PATH, G_KEY_FILE_NONE, NULL)) {
		v = g_key_file_get_string(kf, "pro1x-alarmboot", "after-alarm", NULL);
		if (v && !strcmp(g_strstrip(v), "switch-off"))
			n9_off = TRUE;
		g_free(v);
	}
	g_key_file_free(kf);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: pro1x-alarmboot [-f note] [-s system] [-N] [-t theme] [-d dpi] [-v]\n"
		"       pro1x-alarmboot -D ring|ask|off|switching [-L en|de] [-g WxH] [-t theme] [-d dpi]\n");
}

/* The display's dots per inch, from its size in mm, unless -d said */
static void display_ready(const char *theme)
{
	GdkScreen *gs = gdk_screen_get_default();

	load_pictures(theme);
	if (scr.dpi <= 0 && gdk_screen_get_width_mm(gs) > 0)
		scr.dpi = gdk_screen_get_width(gs) * 25.4 / gdk_screen_get_width_mm(gs);
}

int main(int argc, char **argv)
{
	const char *note_path = NOTE_PATH, *self = NULL, *demo_what = NULL, *lang = NULL;
	const char *theme = NULL;
	gchar *note = NULL, *done_path, *done = NULL;
	char sys[64], buf[32], rtc[32] = "";
	GError *err = NULL;
	time_t now = time(NULL);
	long late;
	int opt;

	setlocale(LC_ALL, "");
	read_conf();
	while ((opt = getopt(argc, argv, "f:s:vND:L:g:t:d:")) != -1) {
		switch (opt) {
		case 'f': note_path = optarg; break;
		case 's': self = optarg; break;
		case 'v': verbose = TRUE; break;
		case 'N': n9_off = TRUE; break;
		case 'D': demo_what = optarg; break;
		case 'L': lang = optarg; break;
		case 'g':
			if (sscanf(optarg, "%dx%d", &scr.width, &scr.height) != 2) {
				usage();
				return 2;
			}
			break;
		case 't': theme = optarg; break;
		case 'd': scr.dpi = atof(optarg); break;
		default:
			usage();
			return 2;
		}
	}
	pick_language(lang);
	if (demo_what) {
		if (!gtk_init_check(&argc, &argv)) {
			fprintf(stderr, "pro1x-alarmboot: no display\n");
			return 1;
		}
		have_display = TRUE;
		display_ready(theme);
		return demo(demo_what);
	}

	if (!g_file_get_contents(note_path, &note, NULL, NULL) ||
	    !note_get(note, "system", sys, sizeof(sys))) {
		say("not started by an alarm (%s)", note_path);
		return 0;
	}
	if (!self) {
#ifdef __linux__
		gchar *cur = NULL;

		if (g_file_get_contents(CURRENT_PATH, &cur, NULL, NULL))
			g_strstrip(cur);
		self = cur ? cur : "leste";
#else
		self = "freebsd";
#endif
	}
	/* the note names the system the boot menu meant to start (a kexec
	 * that failed falls back to Leste) */
	if (strcmp(sys, self)) {
		say("the alarm was %s's, this is %s", sys, self);
		return 0;
	}
	note_get(note, "rtc_alarm", rtc, sizeof(rtc));
	done_path = g_build_filename(g_get_user_runtime_dir(), "pro1x-alarmboot.done", NULL);
	if (g_file_get_contents(done_path, &done, NULL, NULL) && !strcmp(g_strstrip(done), rtc)) {
		say("this alarm was handled already");
		return 0;
	}
	g_file_set_contents(done_path, rtc, -1, NULL);

	due = note_get(note, "due", buf, sizeof(buf)) ? (time_t)strtoll(buf, NULL, 10) : 0;
	if (due <= 0)
		due = now;

	/* the system bus first: no status of the alarm dialog missed while
	 * the display opens */
	sysbus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!sysbus) {
		fprintf(stderr, "pro1x-alarmboot: system bus: %s\n", err->message);
		return 1;
	}
	g_dbus_connection_signal_subscribe(sysbus, NULL, "com.nokia.system_ui.signal",
					   "alarm_dialog_status", "/com/nokia/system_ui/signal",
					   NULL, G_DBUS_SIGNAL_FLAGS_NONE, dialog_status, NULL, NULL);
	have_display = gtk_init_check(&argc, &argv);
	if (have_display)
		display_ready(theme);
	else
		fprintf(stderr, "pro1x-alarmboot: no display: the tone alone\n");
	late = (long)(now - due);
	say("started by %s's alarm at %ld (%+ld s)", sys, (long)due, late);
	{
		long wait = RING_AFTER_DUE - late;
		if (wait < RING_AFTER_START)
			wait = RING_AFTER_START;
		g_timeout_add_seconds((guint)wait, ring_check, NULL);
	}
	g_timeout_add_seconds(LIFETIME, give_up, NULL);
	if (have_display)
		gtk_main();
	else
		g_main_loop_run(g_main_loop_new(NULL, FALSE));
	return 0;
}
