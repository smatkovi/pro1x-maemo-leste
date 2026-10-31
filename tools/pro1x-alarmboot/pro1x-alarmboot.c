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
 *     alarm's text, Snooze and Stop (sized as the N9 clock's style sheet
 *     sizes its alarm notification, in the N9's pixels of 0.1 mm, drawn
 *     with cairo; the N9's own font and button pictures where the owner
 *     installed them, -t, else free ones). A tap answers through the system UI itself: the "turned face down" signal
 *     it listens for while it rings (mce's sig_device_orientation_ind)
 *     snoozes, as turning the phone over does; for Stop, once that dialog
 *     is gone, alarmd gets the Stop button as the answer (it takes the
 *     last one: a snoozed alarm stopped). Should the dialog not go within
 *     3 s the window steps aside for good and the system UI's own dialog
 *     is used; while that dialog is silent (status 6: its sound stopped
 *     after a minute, the face-down signal no longer heard) the window
 *     stays away as well;
 *     stays away as well. Unanswered, it snoozes the alarm itself after
 *     50 s (the N9's dialog snoozed after a minute), as a tap would;
 *   - should no alarm dialog have come 2 minutes after the alarm's time
 *     (alarmd or the system UI missing, the queue lost with FreeBSD's
 *     memory /var), it rings itself: the same N9 alarm screen and a tone
 *     through PulseAudio; Snooze and Stop go to alarmd where it has the
 *     alarm (so that it sets the RTC for the snooze), else Snooze rings
 *     again after 10 minutes; unanswered it snoozes after a minute (3
 *     times, then it stops); not when alarmd answers that it has no such
 *     alarm any more (one deleted after the RTC was set: Leste's alarmd
 *     leaves the RTC alarm then);
 *   - once the alarm is over (stopped or snoozed: alarmd or timed set the
 *     RTC for a snoozed or the next alarm), it switches the phone off
 *     again, as the N9 did: "Switching off" for 10 s, then dsme's shutdown
 *     (FreeBSD: shutdown -p). The phone stays on with a tap on that screen,
 *     a key, the power key (the display going off, to the stand-by screen
 *     or locking: while these screens show they keep the display from
 *     timing out, mce's blanking paused and X's idle time reset for
 *     pro1x-standby, so only the key does that), during a
 *     call, with the charger connected (the N9 stayed in its charging
 *     state then), when it was used meanwhile (any input but on these
 *     screens, XInput's raw events), and when an alarm is still to come but the
 *     RTC's wake-up for it is not set within 15 s. A new alarm ringing
 *     meanwhile takes over. With after-alarm=ask in the configuration
 *     file (or -Q) it asks instead, as the N9's query dialogs look,
 *     whether to switch off ("Switch the phone off again?"; no answer in
 *     60 s keeps it on; its own ringing then rings 5 minutes, no
 *     automatic snoozes).
 *
 * In the MeeGo system (Leste's copy under the N9's own user interface,
 * system key meego, or -T) the N9's own timed and alarm screen ring (and
 * snooze themselves): there is nothing to cover. What tells the alarm's
 * dialog there is timed's next_bootup_event signal (com.nokia.time; 1
 * while a boot-up alarm is in its dialog, after it the next one's time or
 * 0): once that ends, the switch-off (or the question); its own ringing as
 * above should none come in 2 minutes (alarmd is not asked: Leste's
 * alarmd is not that system's).
 *
 * Without a note for this system it ends at once. Once per alarm (a note
 * in the user's runtime directory). Without a display it still rings (the
 * tone alone) and keeps the phone on.
 *
 *   pro1x-alarmboot [-f NOTE] [-s SYSTEM] [-N|-Q] [-T] [-t THEME] [-d DPI] [-v]
 *       -f  the note (default as above), -s  this system's key
 *       (default: /host/pro1x-boot/current; FreeBSD: freebsd),
 *       -N  switch off after the alarm as the N9 did (the default),
 *       -Q  ask instead (after-alarm=ask),
 *       -T  timed's alarms, as in the MeeGo system (its default),
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
 *   after-alarm=switch-off   # the default; or ask (as -Q)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <dirent.h>
#include <fcntl.h>
#include <gdk/gdkx.h>
#include <gio/gio.h>
#ifdef __linux__
#include <gio/gunixfdlist.h>
#endif
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
#include <linux/input.h>
#include <sys/ioctl.h>
#else
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

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
#define RING_FOR 300		/* the own ringing at most this long (asking) */
#define RING_N9 60		/* switching off: then it snoozes itself */
#define AUTO_SNOOZES 3		/* that many times, then it stops */
#define AUTO_SNOOZE 50		/* s of the system UI's dialog: then snoozed */
#define ASK_FOR 60		/* the question at most this long */
#define OFF_IN 10		/* switched off after this long */
#define RTC_WAIT 15		/* s for the RTC's wake-up of an alarm to come */
#define LIFETIME (30 * 60)	/* then give up */
#define STEP_ASIDE 3		/* s for the system UI's dialog to go after a tap */
#define OWN_SNOOZE 600		/* the own ringing's snooze without alarmd's */

/* The alarm screen's buttons, from the top: Snooze (the N9's negative
 * dialog button), Stop (its positive one) */
enum { BTN_SNOOZE, BTN_STOP };

static gboolean verbose, ask_mode, use_timed;
static GDBusConnection *sysbus;
static time_t due;		/* the alarm's real time */
static gboolean have_display, dialog_seen, asked, ringing, own_snoozed;
static guint ask_timer, ring_timer, own_snooze_timer;
static int own_snooze_secs = OWN_SNOOZE, own_autos;
static gint64 t_start, t_own;	/* ms (monotonic): our start, our screens' last input */
static gboolean power_key;	/* pressed (or the display went off) while switching off */
static volatile gint ring_stop;
static GThread *tone;

#define say(...) do { if (verbose) { fprintf(stderr, "pro1x-alarmboot: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* --- texts: our own short words for what the N9's clock and shutdown
 * screen said, in English and German --------------------------------------- */

static const struct texts {
	const char *snooze, *stop, *alarm;
	const char *ask_title, *ask_text, *off, *keep;
	const char *switching, *off_in;
} text_en = {
	"Snooze", "Stop", "Alarm",
	"Switch the phone off again?",
	"It was switched on for the alarm. Without an answer it stays on.",
	"Switch off", "Keep on",
	"Switching off",
	"The phone switches itself off again in %d s. A tap or the power key keeps it on.",
}, text_de = {
	"Später erinnern", "Stoppen", "Weckzeit",
	"Telefon wieder ausschalten?",
	"Es wurde für den Wecker eingeschaltet. Ohne Antwort bleibt es an.",
	"Ausschalten", "Eingeschaltet lassen",
	"Wird ausgeschaltet",
	"Das Telefon schaltet sich in %d s wieder aus. Ein Tippen oder die Ein/Aus-Taste lässt es an.",
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
 * The sizes of the N9 clock's style sheet for its alarm notification and
 * of the query dialog of libmeegotouch's open base theme, in the N9's
 * pixels, which its theme counts as 0.1 mm (251 dpi). The alarm: the time
 * as the title (bold 64), 12 below it the alarm's text (light 32, 42 from
 * the sides, cut short), 40 below that Snooze, 36 below that Stop, both
 * 408 x 64 with bold 32 white labels, Snooze on the dialog's negative
 * (black) button, Stop on its positive (dark blue) one, all centred on
 * black: as in the N9's acting-dead mode, where no compositor ran under
 * the dialog (which label shows what and the buttons' order are ours).
 * The question: the query dialog with its buttons one below the other and
 * not cut to 322 (LargeButtonsQueryDialog): the title bold 32, 17 below it
 * the text light 24, 26 below that the buttons, 16 apart.
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
	gboolean demo, popup;
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

static gint64 now_ms(void)
{
	return g_get_monotonic_time() / 1000;
}

static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer unused)
{
	(void)unused;
	t_own = now_ms();
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
	t_own = now_ms();
	if (scr.busy)
		return TRUE;
	/* "Switching off": a tap anywhere keeps the phone on */
	if (scr.kind == SCR_OFF_SOON && scr.clicked) {
		scr.busy = TRUE;
		scr.clicked(0);
		return TRUE;
	}
	if (i >= 0 && hit(ev->x, ev->y) == i && scr.clicked) {
		scr.busy = TRUE;	/* the button stays pressed meanwhile */
		scr.clicked(i);
		return TRUE;
	}
	scr.pressed = -1;
	gtk_widget_queue_draw(w);
	return TRUE;
}

/* A key on our window: on "Switching off" it keeps the phone on (the
 * power key too where X sees it) */
static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer unused)
{
	(void)w; (void)ev; (void)unused;
	t_own = now_ms();
	if (scr.kind == SCR_OFF_SOON && scr.clicked && !scr.busy) {
		scr.busy = TRUE;
		scr.clicked(0);
	}
	return TRUE;
}

static void set_cardinal(GtkWidget *w, const char *name, gulong value)
{
	gdk_property_change(gtk_widget_get_window(w), gdk_atom_intern(name, FALSE),
			    gdk_atom_intern("CARDINAL", FALSE), 32, GDK_PROP_MODE_REPLACE,
			    (const guchar *)&value, 1);
}

/* The window's size: -g's, else the screen's (also after the screen
 * turned: the override-redirect window resizes itself) */
static void fit_window(GdkScreen *gs, gpointer unused)
{
	(void)unused;
	if (!scr.win || scr.width || !scr.popup)
		return;
	gtk_window_move(GTK_WINDOW(scr.win), 0, 0);
	gtk_window_resize(GTK_WINDOW(scr.win), gdk_screen_get_width(gs),
			  gdk_screen_get_height(gs));
	gtk_widget_set_size_request(scr.area, gdk_screen_get_width(gs),
				    gdk_screen_get_height(gs));
}

/* The screen's window: full screen, above Hildon's dialogs and its screen
 * lock (hildon-desktop's stacking layer 10, the system UI's power key
 * menu's; the lock's is 9), turned with the screen when hildon-desktop
 * turns it to portrait (the keyboard closed, as the MeeGo look does).
 * Managed by hildon-desktop as the system UI's own such windows are; an
 * override-redirect one where hildon-desktop did not size it (seen once in
 * the FreeBSD VM: no answer to its configure request, GTK's drawing area
 * left at 1 x 1), which hildon-desktop then shows blurred under a dialog */
static void screen_create(void)
{
	GdkScreen *gs = gdk_screen_get_default();
	int w = scr.width ? scr.width : gdk_screen_get_width(gs);
	int h = scr.height ? scr.height : gdk_screen_get_height(gs);

	scr.win = gtk_window_new(scr.popup ? GTK_WINDOW_POPUP : GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(scr.win), "Alarm");
	gtk_window_set_decorated(GTK_WINDOW(scr.win), FALSE);
	gtk_window_move(GTK_WINDOW(scr.win), 0, 0);
	gtk_window_set_default_size(GTK_WINDOW(scr.win), w, h);
	g_signal_connect(scr.win, "delete-event", G_CALLBACK(gtk_true), NULL);
	g_signal_connect(scr.win, "key-press-event", G_CALLBACK(on_key), NULL);
	scr.area = gtk_drawing_area_new();
	if (scr.popup)
		gtk_widget_set_size_request(scr.area, w, h);
	gtk_widget_add_events(scr.area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
	g_signal_connect(scr.area, "expose-event", G_CALLBACK(on_expose), NULL);
	g_signal_connect(scr.area, "button-press-event", G_CALLBACK(on_press), NULL);
	g_signal_connect(scr.area, "button-release-event", G_CALLBACK(on_release), NULL);
	gtk_container_add(GTK_CONTAINER(scr.win), scr.area);
	gtk_widget_realize(scr.win);
	set_cardinal(scr.win, "_HILDON_STACKING_LAYER", 10);
	set_cardinal(scr.win, "_HILDON_PORTRAIT_MODE_SUPPORT", 1);
	/* (-g: a window of that size alone, for pictures) */
	if (!scr.popup && !scr.width)
		gtk_window_fullscreen(GTK_WINDOW(scr.win));
}

static guint size_check;

/* 2 s after it was shown: has it the screen's size? */
static gboolean check_size(gpointer unused)
{
	GtkAllocation al;

	(void)unused;
	size_check = 0;
	if (!scr.win || scr.popup || !gtk_widget_get_visible(scr.win))
		return FALSE;
	gtk_widget_get_allocation(scr.area, &al);
	if (al.width >= 100 && al.height >= 100)
		return FALSE;
	say("the window manager did not size the screen (%d x %d): override-redirect",
	    al.width, al.height);
	gtk_widget_destroy(scr.win);
	scr.popup = TRUE;
	screen_create();
	gtk_widget_show_all(scr.win);
	gdk_window_raise(gtk_widget_get_window(scr.win));
	return FALSE;
}

/* While our screens show, nothing times the display out: mce's blanking
 * paused (Leste, MeeGo), X's idle time reset (pro1x-standby's time-outs:
 * FreeBSD's, every 10 s, under its shortest). So a display that goes off
 * or to the stand-by screen meanwhile is the power key (mce_display) */
static void mce_call(const char *method, GVariant *args);
static guint awake_timer;

static gboolean keep_awake_tick(gpointer unused)
{
	(void)unused;
	mce_call("req_display_blanking_pause", NULL);
	XResetScreenSaver(GDK_DISPLAY_XDISPLAY(gdk_display_get_default()));
	XFlush(GDK_DISPLAY_XDISPLAY(gdk_display_get_default()));
	return TRUE;
}

static void keep_awake(gboolean on)
{
	if (on && !awake_timer) {
		say("keeping the display on");
		keep_awake_tick(NULL);
		awake_timer = g_timeout_add_seconds(10, keep_awake_tick, NULL);
	} else if (!on && awake_timer) {
		g_source_remove(awake_timer);
		awake_timer = 0;
	}
}

static void screen_show(enum screen_kind kind, void (*clicked)(int))
{
	if (!have_display)
		return;
	if (!scr.win) {
		screen_create();
		g_signal_connect(gdk_screen_get_default(), "size-changed",
				 G_CALLBACK(fit_window), NULL);
	}
	scr.kind = kind;
	scr.clicked = clicked;
	scr.pressed = -1;
	scr.busy = FALSE;
	gtk_widget_show_all(scr.win);
	if (scr.popup)
		gdk_window_raise(gtk_widget_get_window(scr.win));
	else
		gtk_window_present(GTK_WINDOW(scr.win));
	gtk_widget_queue_draw(scr.area);
	keep_awake(TRUE);
	if (!scr.popup && !scr.width && !size_check)
		size_check = g_timeout_add_seconds(2, check_size, NULL);
}

static void screen_hide(void)
{
	keep_awake(FALSE);
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

/* --- before switching off: what keeps the phone on ---------------------------- */

/* The last input from a device (XInput 2's raw key, button and touch
 * events on the root window; not what resets X's idle time without a
 * device, as the system UI may), 0 none */
static int xi_opcode = -1;
static gint64 t_input;

static GdkFilterReturn raw_input(GdkXEvent *xev, GdkEvent *ev, gpointer unused)
{
	XEvent *e = xev;

	(void)ev; (void)unused;
	if (e->type == GenericEvent && e->xcookie.extension == xi_opcode &&
	    (e->xcookie.evtype == XI_RawKeyPress || e->xcookie.evtype == XI_RawButtonPress ||
	     e->xcookie.evtype == XI_RawTouchBegin))
		t_input = now_ms();
	return GDK_FILTER_CONTINUE;
}

static void input_init(void)
{
	Display *d = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
	unsigned char m[XIMaskLen(XI_LASTEVENT)] = { 0 };
	XIEventMask em = { XIAllMasterDevices, sizeof(m), m };
	int ev, er, maj = 2, min = 2;

	if (!XQueryExtension(d, "XInputExtension", &xi_opcode, &ev, &er) ||
	    XIQueryVersion(d, &maj, &min) != Success) {
		xi_opcode = -1;
		return;
	}
	XISetMask(m, XI_RawKeyPress);
	XISetMask(m, XI_RawButtonPress);
	if (maj > 2 || min >= 2)
		XISetMask(m, XI_RawTouchBegin);
	XISelectEvents(d, DefaultRootWindow(d), &em, 1);
	gdk_window_add_filter(NULL, raw_input, NULL);
}

/* Input that was on no alarm screen: before the first alarm rang (noted
 * then), or after the alarm's end (when t_own moves up to) other than on
 * our screens: the phone in use */
static gboolean used_before, alarm_began;

static void alarm_begins(void)
{
	if (alarm_began)
		return;
	alarm_began = TRUE;
	used_before = t_input > 0;
	if (used_before)
		say("the phone was used before the alarm rang");
}

/* the alarm is over: input until now was the alarm's (any of its screens) */
static void alarm_ends(void)
{
	t_own = now_ms();
}

static gboolean used_meanwhile(void)
{
	return used_before || t_input > t_own + 400;
}

static gboolean has_owner(const char *name)
{
	GVariant *r = g_dbus_connection_call_sync(sysbus, "org.freedesktop.DBus",
						  "/org/freedesktop/DBus", "org.freedesktop.DBus",
						  "NameHasOwner", g_variant_new("(s)", name),
						  G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE,
						  1000, NULL, NULL);
	gboolean has = FALSE;

	if (r) {
		g_variant_get(r, "(b)", &has);
		g_variant_unref(r);
	}
	return has;
}

/* ModemManager (Arch): a call object not yet terminated (MM_CALL_STATE_
 * TERMINATED is 7) */
static gboolean mm_in_call(void)
{
	GVariant *r = g_dbus_connection_call_sync(sysbus, "org.freedesktop.ModemManager1",
						  "/org/freedesktop/ModemManager1",
						  "org.freedesktop.DBus.ObjectManager",
						  "GetManagedObjects", NULL,
						  G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
						  G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
	GVariantIter *objs;
	GVariant *ifaces;
	const gchar *path;
	gboolean call = FALSE;

	if (!r)
		return FALSE;
	g_variant_get(r, "(a{oa{sa{sv}}})", &objs);
	while (!call && g_variant_iter_next(objs, "{&o@a{sa{sv}}}", &path, &ifaces)) {
		GVariant *props = g_variant_lookup_value(ifaces,
							 "org.freedesktop.ModemManager1.Call",
							 G_VARIANT_TYPE("a{sv}"));
		gint32 st = 0;

		if (props && g_variant_lookup(props, "State", "i", &st))
			call = st != 7;
		if (props)
			g_variant_unref(props);
		g_variant_unref(ifaces);
	}
	g_variant_iter_free(objs);
	g_variant_unref(r);
	return call;
}

/* mce: a call ("none" when there is none) */
static gboolean in_call(void)
{
	GVariant *r;
	gboolean call = FALSE;

	r = g_dbus_connection_call_sync(sysbus, "com.nokia.mce", "/com/nokia/mce/request",
					"com.nokia.mce.request", "get_call_state", NULL,
					G_VARIANT_TYPE("(ss)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
	if (r) {
		const gchar *st = NULL;

		g_variant_get(r, "(&s&s)", &st, NULL);
		call = st && strcmp(st, "none");
		g_variant_unref(r);
	}
	if (call)
		return TRUE;
	/* oFono's (Leste's, FreeBSD's pro1x-modemd): any modem's calls */
	r = g_dbus_connection_call_sync(sysbus, "org.ofono", "/", "org.ofono.Manager",
					"GetModems", NULL, G_VARIANT_TYPE("(a(oa{sv}))"),
					G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
	if (r) {
		GVariantIter *it;
		const gchar *modem;

		g_variant_get(r, "(a(oa{sv}))", &it);
		while (!call && g_variant_iter_next(it, "(&o@a{sv})", &modem, NULL)) {
			GVariant *c = g_dbus_connection_call_sync(sysbus, "org.ofono", modem,
								  "org.ofono.VoiceCallManager",
								  "GetCalls", NULL,
								  G_VARIANT_TYPE("(a(oa{sv}))"),
								  G_DBUS_CALL_FLAGS_NO_AUTO_START,
								  1000, NULL, NULL);
			if (c) {
				GVariant *a = g_variant_get_child_value(c, 0);

				call = g_variant_n_children(a) > 0;
				g_variant_unref(a);
				g_variant_unref(c);
			}
		}
		g_variant_iter_free(it);
		g_variant_unref(r);
	}
	return call || mm_in_call();
}

/* While switching off, logind leaves the power key to the session (an
 * inhibitor, Arch): it reaches X, and our window keeps the phone on */
static int key_inhibitor = -1;

static void power_key_hold(gboolean hold)
{
#ifdef __linux__
	GUnixFDList *fds = NULL;
	GVariant *r;

	if (!hold) {
		if (key_inhibitor >= 0)
			close(key_inhibitor);
		key_inhibitor = -1;
		return;
	}
	if (key_inhibitor >= 0 || !sysbus || !has_owner("org.freedesktop.login1"))
		return;
	r = g_dbus_connection_call_with_unix_fd_list_sync(sysbus, "org.freedesktop.login1",
							  "/org/freedesktop/login1",
							  "org.freedesktop.login1.Manager", "Inhibit",
							  g_variant_new("(ssss)", "handle-power-key",
									"pro1x-alarmboot",
									"the power key keeps the phone on",
									"block"),
							  G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE,
							  2000, NULL, &fds, NULL, NULL);
	if (r && fds && g_unix_fd_list_get_length(fds) > 0)
		key_inhibitor = g_unix_fd_list_get(fds, 0, NULL);
	if (r)
		g_variant_unref(r);
	if (fds)
		g_object_unref(fds);
	say("the power key %s logind's", key_inhibitor >= 0 ? "kept from" : "left to");
#else
	(void)hold;
#endif
}

/* The charger connected (the N9 stayed in its charging state then) */
static gboolean on_charger(void)
{
#ifdef __linux__
	DIR *d = opendir("/sys/class/power_supply");
	struct dirent *e;
	gboolean on = FALSE;

	while (d && !on && (e = readdir(d))) {
		gchar *p, *type = NULL, *val = NULL;

		if (e->d_name[0] == '.')
			continue;
		p = g_strdup_printf("/sys/class/power_supply/%s/type", e->d_name);
		g_file_get_contents(p, &type, NULL, NULL);
		g_free(p);
		if (type && (!strncmp(type, "USB", 3) || !strncmp(type, "Mains", 5))) {
			p = g_strdup_printf("/sys/class/power_supply/%s/online", e->d_name);
			on = g_file_get_contents(p, &val, NULL, NULL) && atoi(val) == 1;
		} else if (type && !strncmp(type, "Battery", 7)) {
			p = g_strdup_printf("/sys/class/power_supply/%s/status", e->d_name);
			on = g_file_get_contents(p, &val, NULL, NULL) &&
			     (!strncmp(val, "Charging", 8) || !strncmp(val, "Full", 4));
		} else {
			p = NULL;
		}
		g_free(p);
		g_free(type);
		g_free(val);
	}
	if (d)
		closedir(d);
	return on;
#else
	int v = 0;
	size_t len = sizeof(v);

	return !sysctlbyname("hw.acpi.acline", &v, &len, NULL, 0) && v == 1;
#endif
}

/* s until the RTC's wake-up alarm, <= 0 when it is off or past */
static long rtc_alarm_ahead(void)
{
#ifdef __linux__
	gchar *w = NULL, *n = NULL;
	long ahead = 0;

	/* in the RTC's own time (on the Pro1X not the real time) */
	if (g_file_get_contents("/sys/class/rtc/rtc0/wakealarm", &w, NULL, NULL) &&
	    g_file_get_contents("/sys/class/rtc/rtc0/since_epoch", &n, NULL, NULL) &&
	    *g_strstrip(w))
		ahead = (long)(g_ascii_strtoll(w, NULL, 10) - g_ascii_strtoll(n, NULL, 10));
	g_free(w);
	g_free(n);
	return ahead;
#else
	/* qcom_rtc: the real time, 0 off */
	int64_t v = 0;
	size_t len = sizeof(v);

	if (sysctlbyname("dev.qcom_rtc.0.wakealarm", &v, &len, NULL, 0))
		return 0;
	if (len == sizeof(int32_t))
		v = *(int32_t *)&v;
	return v > 0 ? (long)(v - time(NULL)) : 0;
#endif
}

/* The power key: Linux's input devices that have it (where they can be
 * read); everywhere mce's display going off or locking while switching
 * off, which a press does */
static gboolean switching_soon;

static gboolean power_key_input(GIOChannel *ch, GIOCondition cond, gpointer unused)
{
#ifdef __linux__
	struct input_event ev[16];
	ssize_t n = read(g_io_channel_unix_get_fd(ch), ev, sizeof(ev));

	(void)cond; (void)unused;
	if (n <= 0)
		return FALSE;
	for (int i = 0; i < (int)(n / sizeof(ev[0])); i++)
		if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER && ev[i].value == 1 &&
		    switching_soon) {
			say("the power key");
			power_key = TRUE;
		}
#else
	(void)ch; (void)cond; (void)unused;
#endif
	return TRUE;
}

static void power_key_init(void)
{
#ifdef __linux__
	DIR *d = opendir("/dev/input");
	struct dirent *e;

	while (d && (e = readdir(d))) {
		unsigned long bits[(KEY_MAX + 1) / (8 * sizeof(long)) + 1] = { 0 };
		gchar *p;
		int fd;

		if (strncmp(e->d_name, "event", 5))
			continue;
		p = g_strdup_printf("/dev/input/%s", e->d_name);
		fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		g_free(p);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 &&
		    (bits[KEY_POWER / (8 * sizeof(long))] >> (KEY_POWER % (8 * sizeof(long)))) & 1) {
			GIOChannel *ch = g_io_channel_unix_new(fd);

			g_io_add_watch(ch, G_IO_IN, power_key_input, NULL);
			say("the power key from /dev/input/%s", e->d_name);
		} else {
			close(fd);
		}
	}
	if (d)
		closedir(d);
#endif
}

static void mce_display(GDBusConnection *c, const gchar *sender, const gchar *path,
			const gchar *iface, const gchar *signal, GVariant *params,
			gpointer unused)
{
	const gchar *v = NULL;

	(void)c; (void)sender; (void)path; (void)iface; (void)unused;
	if (!switching_soon || !g_variant_is_of_type(params, G_VARIANT_TYPE("(s)")))
		return;
	g_variant_get(params, "(&s)", &v);
	/* no time-out can blank it while our screens keep it awake (below):
	 * the display going off (Leste's mce), to the stand-by screen
	 * (pro1x-standby's "lpm": FreeBSD's power key, Leste's with mce's
	 * low power mode) or locking is the power key */
	if ((!strcmp(signal, "display_status_ind") && (!strcmp(v, "off") || !strcmp(v, "lpm"))) ||
	    (!strcmp(signal, "tklock_mode_ind") && !strcmp(v, "locked"))) {
		say("%s %s: the power key", signal, v);
		power_key = TRUE;
	}
}

/* Why the phone stays on, or NULL */
static const char *stay_on_why(void)
{
	if (power_key)
		return "the power key";
	if (in_call())
		return "a call";
	if (on_charger())
		return "the charger";
	if (used_meanwhile())
		return "the phone was used";
	return NULL;
}

static void power_off(void)
{
	GError *err = NULL;

	say("switching off");
	if (scr.demo)
		return;
#ifdef __linux__
	/* as the power key menu does: dsme shuts down (Leste, MeeGo; the RTC
	 * alarm of a snoozed or the next alarm stays set); without dsme
	 * (Arch) systemd's logind */
	GVariant *r = has_owner("com.nokia.dsme") ?
		g_dbus_connection_call_sync(sysbus, "com.nokia.dsme", "/com/nokia/dsme/request",
					    "com.nokia.dsme.request", "req_shutdown", NULL, NULL,
					    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err) :
		g_dbus_connection_call_sync(sysbus, "org.freedesktop.login1",
					    "/org/freedesktop/login1",
					    "org.freedesktop.login1.Manager", "PowerOff",
					    g_variant_new("(b)", FALSE), NULL,
					    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
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

static guint answer_timer, rtc_timer;
static int off_in, rtc_tries;
static gboolean off_requested;
static time_t timed_next;		/* MeeGo: timed's next boot-up alarm, 0 none */
static gboolean boot_alarm_pending(void);

/* "Switching off" (as the N9's shutdown screen said) while dsme or
 * shutdown(8) end the session */
static void switching_off(void)
{
	off_requested = TRUE;
	switching_soon = FALSE;
	power_key_hold(FALSE);
	scr.nbtn = 0;
	g_strlcpy(scr.title, tx->switching, sizeof(scr.title));
	screen_show(SCR_SWITCHING, NULL);
	gdk_window_process_all_updates();
	gdk_flush();
	power_off();
	g_timeout_add_seconds(20, quit_later, NULL);
}

/* The phone stays on: the screen away, and the end (unless its own
 * ringing still has a snooze to ring) */
static void stay_on(const char *why)
{
	say("staying on: %s", why);
	switching_soon = FALSE;
	power_key_hold(FALSE);
	if (answer_timer)
		g_source_remove(answer_timer);
	if (rtc_timer)
		g_source_remove(rtc_timer);
	answer_timer = rtc_timer = 0;
	screen_hide();
	if (own_snooze_timer) {
		asked = FALSE;
		return;
	}
	quit();
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
	stay_on("kept on");
}

static gboolean ask_timeout(gpointer unused)
{
	(void)unused;
	answer_timer = 0;
	stay_on("no answer");
	return FALSE;
}

static gboolean rtc_tick(gpointer unused);

/* Off now, unless something keeps it on; an alarm still to come must have
 * its RTC wake-up set first (alarmd and timed set it at once when their
 * queue changes, a snooze too) */
static void try_off(void)
{
	const char *why = stay_on_why();

	if (why) {
		stay_on(why);
		return;
	}
	if (boot_alarm_pending() && rtc_alarm_ahead() <= 0) {
		if (rtc_tries++ < RTC_WAIT) {
			if (rtc_tries == 1)
				say("an alarm is still to come: waiting for its RTC wake-up");
			rtc_timer = g_timeout_add_seconds(1, rtc_tick, NULL);
			return;
		}
		stay_on("the RTC's wake-up for the next alarm is not set");
		return;
	}
	say("the RTC's wake-up: in %ld s", rtc_alarm_ahead());
	switching_off();
}

static gboolean rtc_tick(gpointer unused)
{
	(void)unused;
	rtc_timer = 0;
	try_off();
	return FALSE;
}

static gboolean off_countdown(gpointer unused)
{
	const char *why = stay_on_why();

	(void)unused;
	if (why) {
		answer_timer = 0;
		stay_on(why);
		return FALSE;
	}
	if (--off_in > 0) {
		g_snprintf(scr.text, sizeof(scr.text), tx->off_in, off_in);
		gtk_widget_queue_draw(scr.area);
		return TRUE;
	}
	answer_timer = 0;
	try_off();
	return FALSE;
}

/* After the alarm, as the N9: "Switching off", 10 s to keep the phone on,
 * then off. Asking (-Q): "Switch the phone off again?"; no answer keeps it
 * on */
static gboolean ask(gpointer unused)
{
	(void)unused;
	if (asked || ringing || off_requested || (ask_mode && own_snoozed))
		return FALSE;
	asked = TRUE;
	if (!have_display) {
		say("no display: staying on");
		quit();
		return FALSE;
	}
	if (!ask_mode) {
		const char *why = scr.demo ? NULL : stay_on_why();

		if (why) {
			stay_on(why);
			return FALSE;
		}
		wake_screen();
		say("switching off in %d s unless kept on", OFF_IN);
		power_key = FALSE;
		switching_soon = TRUE;
		power_key_hold(TRUE);
		rtc_tries = 0;
		off_in = OFF_IN;
		g_strlcpy(scr.title, tx->switching, sizeof(scr.title));
		g_snprintf(scr.text, sizeof(scr.text), tx->off_in, off_in);
		scr.btn[0] = (struct button){ .label = tx->keep, .positive = FALSE };
		scr.nbtn = 1;
		screen_show(SCR_OFF_SOON, answered);
		answer_timer = g_timeout_add_seconds(1, off_countdown, NULL);
		return FALSE;
	}
	wake_screen();
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

/* Another alarm rings while switching off (or asking): it takes over */
static void after_cancel(void)
{
	if (!asked || off_requested)
		return;
	say("another alarm rings: not switching off now");
	if (answer_timer)
		g_source_remove(answer_timer);
	if (rtc_timer)
		g_source_remove(rtc_timer);
	answer_timer = rtc_timer = 0;
	switching_soon = FALSE;
	power_key_hold(FALSE);
	asked = FALSE;
	screen_hide();
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
	alarm_ends();
	if (!snoozed)
		ask(NULL);
}

static void own_ring_clicked(int button);

/* Unanswered: asking, it ends after 5 minutes; as the N9, it snoozes after
 * a minute, and after the third time it stops */
static gboolean ring_timeout(gpointer unused)
{
	(void)unused;
	ring_timer = 0;
	if (ask_mode) {
		say("rang for %d s", RING_FOR);
		ring_end(FALSE);
	} else if (own_autos++ < AUTO_SNOOZES) {
		say("no answer: snoozing");
		own_ring_clicked(BTN_SNOOZE);
	} else {
		say("no answer %d times: stopped", AUTO_SNOOZES);
		own_ring_clicked(BTN_STOP);
	}
	return FALSE;
}

static void ring_now(void);

static gboolean own_snooze_over(gpointer unused)
{
	(void)unused;
	own_snooze_timer = 0;
	own_snoozed = FALSE;
	if (!dialog_seen && !asked && !ringing && !off_requested) {
		say("ringing again");
		ring_now();
	}
	return FALSE;
}

struct alarm_info;
static gboolean find_alarm(time_t now, gboolean ringing_only, struct alarm_info *ai);
static gboolean own_answer_alarmd(int button);

/* Its own ringing answered. Where alarmd has the alarm it gets the answer
 * as from its dialog (a snooze then sets the RTC); switching off, a snooze
 * switches off too (and its own ringing comes again after the snooze
 * should the phone stay on) */
static void own_ring_clicked(int button)
{
	gboolean alarmd = own_answer_alarmd(button);

	if (button == BTN_SNOOZE) {
		say("snoozed for %d s", own_snooze_secs);
		own_snoozed = TRUE;
		own_snooze_timer = g_timeout_add_seconds(own_snooze_secs, own_snooze_over, NULL);
		ring_end(TRUE);
		if (!ask_mode && alarmd)
			ask(NULL);
	} else {
		say("stopped");
		ring_end(FALSE);
	}
}

/* --- alarmd ---------------------------------------------------------------- */

/* alarmd's answer for the alarm: 1 there is one, 0 it has none, -1 no answer */
static int alarmd_has_alarm(time_t now)
{
	cookie_t *c;
	int n = 0;

	if (use_timed)
		return -1;
	c = alarmd_event_query(due - 600, now + 900, 0, 0, NULL);
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

static cookie_t stopped;	/* the alarm just stopped (alarmd may not have it done) */

static gboolean find_alarm(time_t now, gboolean ringing_only, struct alarm_info *ai)
{
	cookie_t *c;
	gboolean found = FALSE, best_rings = FALSE;
	time_t best = 0;

	if (use_timed)
		return FALSE;
	c = alarmd_event_query(due - 600, now + 900, 0, 0, NULL);
	if (!c)
		return FALSE;
	for (int i = 0; c[i]; i++) {
		alarm_event_t *e = alarmd_event_get(c[i]);
		time_t t;
		gboolean rings, better;

		if (!e)
			continue;
		if (c[i] == stopped) {
			alarm_event_delete(e);
			continue;
		}
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

/* alarmd's answer for its own ringing's alarm, TRUE where it took it */
static gboolean own_answer_alarmd(int button)
{
	struct alarm_info ai;

	if (!find_alarm(time(NULL), TRUE, &ai) || ai.snooze < 0 || ai.stop < 0)
		return FALSE;
	say("%s for alarm %d through alarmd", button == BTN_SNOOZE ? "snooze" : "stop",
	    (int)ai.cookie);
	return alarmd_ack_dialog(ai.cookie, button == BTN_SNOOZE ? ai.snooze : ai.stop) != -1;
}

/* An alarm still to come that switches the phone on (alarmd's; MeeGo:
 * timed's next boot-up event) */
static gboolean boot_alarm_pending(void)
{
	time_t now = time(NULL);
	cookie_t *c;
	gboolean pending = FALSE;

	if (use_timed)
		return timed_next > now;
	if (!(c = alarmd_event_query(0, 0, 0, 0, NULL)))
		return FALSE;
	for (int i = 0; c[i]; i++) {
		alarm_event_t *e = alarmd_event_get(c[i]);

		if (e && (e->flags & (ALARM_EVENT_ACTDEAD | ALARM_EVENT_BOOT)) &&
		    !(e->flags & ALARM_EVENT_DISABLED) && alarm_event_get_trigger(e) > now)
			pending = TRUE;
		if (e)
			alarm_event_delete(e);
	}
	free(c);
	return pending;
}

/* --- the N9 screen over the system UI's alarm dialog ---------------------- */

enum { NONE, SNOOZING, STOPPING };

static struct {
	gboolean shown, stepped_aside;
	struct alarm_info alarm;
	int pending;			/* the tap being answered */
	guint step_aside, auto_snooze;
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
	if (ov.auto_snooze)
		g_source_remove(ov.auto_snooze);
	ov.step_aside = ov.auto_snooze = 0;
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
		stopped = ov.alarm.cookie;
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
	if (ov.auto_snooze)
		g_source_remove(ov.auto_snooze);
	ov.auto_snooze = 0;
	ov.pending = button == BTN_SNOOZE ? SNOOZING : STOPPING;
	turned_face_down();
	ov.step_aside = g_timeout_add_seconds(STEP_ASIDE, step_aside, NULL);
}

/* As the N9's dialog after a minute: unanswered, snoozed */
static gboolean overlay_auto_snooze(gpointer unused)
{
	(void)unused;
	ov.auto_snooze = 0;
	if (!ov.shown || ov.pending != NONE || scr.busy)
		return FALSE;
	say("no answer: snoozing");
	scr.busy = TRUE;
	scr.pressed = BTN_SNOOZE;
	gtk_widget_queue_draw(scr.area);
	overlay_clicked(BTN_SNOOZE);
	return FALSE;
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
	alarm_begins();
	wake_screen();
	ring_screen(when, ov.alarm.text, overlay_clicked);
	if (ov.auto_snooze)
		g_source_remove(ov.auto_snooze);
	ov.auto_snooze = ask_mode ? 0 : g_timeout_add_seconds(AUTO_SNOOZE, overlay_auto_snooze, NULL);
}

/* --- when to ring ------------------------------------------------------------ */

/* The tone first; then the screen, with the alarm's text and snooze time
 * where alarmd tells them */
static void ring_now(void)
{
	struct alarm_info ai;
	char when[32];

	alarm_begins();
	ringing = TRUE;
	g_atomic_int_set(&ring_stop, 0);
	tone = g_thread_new("tone", tone_thread, NULL);
	ring_timer = g_timeout_add_seconds(ask_mode ? RING_FOR : RING_N9, ring_timeout, NULL);
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
		after_cancel();
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
		alarm_ends();
		/* stopped or snoozed: once it stays gone a moment */
		if (dialog_seen && !asked && !ask_timer)
			ask_timer = g_timeout_add_seconds(ask_mode ? 5 : 2, ask_later, NULL);
	}
}

/* The MeeGo system: timed's next boot-up event, 1 while a boot-up alarm
 * is in its dialog (timed's machine_t::calculate_bootup()) */
static gboolean timed_rings;

static void timed_bootup(GDBusConnection *c, const gchar *sender, const gchar *path,
			 const gchar *iface, const gchar *signal, GVariant *params,
			 gpointer unused)
{
	gint32 v = 0;

	(void)c; (void)sender; (void)path; (void)iface; (void)signal; (void)unused;
	if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(i)")))
		return;
	g_variant_get(params, "(i)", &v);
	say("timed's next boot-up event %d", v);
	if (v == 1)
		after_cancel();
	if (v == 1 && !timed_rings) {
		alarm_begins();
		timed_rings = TRUE;
		dialog_seen = TRUE;
		/* our own ringing gives way to timed's */
		if (own_snooze_timer)
			g_source_remove(own_snooze_timer);
		own_snooze_timer = 0;
		own_snoozed = FALSE;
		if (ringing) {
			asked = TRUE;
			ring_end(FALSE);
			asked = FALSE;
		}
		if (ask_timer)
			g_source_remove(ask_timer);
		ask_timer = 0;
	} else if (v != 1 && timed_rings) {
		/* stopped or snoozed; v: the next boot-up alarm, 0 none */
		timed_rings = FALSE;
		timed_next = v;
		alarm_ends();
		if (!asked && !ask_timer)
			ask_timer = g_timeout_add_seconds(ask_mode ? 5 : 2, ask_later, NULL);
	}
}

static gboolean give_up(gpointer unused)
{
	(void)unused;
	if (ringing || asked || own_snoozed || ov.shown || timed_rings || off_requested)
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
		ask_mode = !strcmp(what, "ask");
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
		if (v && !strcmp(g_strstrip(v), "ask"))
			ask_mode = TRUE;
		g_free(v);
	}
	g_key_file_free(kf);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: pro1x-alarmboot [-f note] [-s system] [-N|-Q] [-T] [-t theme] [-d dpi] [-v]\n"
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
	while ((opt = getopt(argc, argv, "f:s:vNQTD:L:g:t:d:")) != -1) {
		switch (opt) {
		case 'f': note_path = optarg; break;
		case 's': self = optarg; break;
		case 'v': verbose = TRUE; break;
		case 'N': ask_mode = FALSE; break;
		case 'Q': ask_mode = TRUE; break;
		case 'T': use_timed = TRUE; break;
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
	if (!strcmp(self, "meego"))
		use_timed = TRUE;
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
	if (use_timed)
		g_dbus_connection_signal_subscribe(sysbus, NULL, "com.nokia.time",
						   "next_bootup_event", "/com/nokia/time", NULL,
						   G_DBUS_SIGNAL_FLAGS_NONE, timed_bootup, NULL, NULL);
	else
		g_dbus_connection_signal_subscribe(sysbus, NULL, "com.nokia.system_ui.signal",
						   "alarm_dialog_status", "/com/nokia/system_ui/signal",
						   NULL, G_DBUS_SIGNAL_FLAGS_NONE, dialog_status, NULL, NULL);
	/* the power key while switching off: mce's display going off or
	 * locking (what a press does), Linux's input devices with it */
	g_dbus_connection_signal_subscribe(sysbus, NULL, "com.nokia.mce.signal", NULL,
					   "/com/nokia/mce/signal", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
					   mce_display, NULL, NULL);
	power_key_init();
	t_start = now_ms();
	have_display = gtk_init_check(&argc, &argv);
	if (have_display) {
		display_ready(theme);
		input_init();
	} else {
		fprintf(stderr, "pro1x-alarmboot: no display: the tone alone\n");
	}
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
