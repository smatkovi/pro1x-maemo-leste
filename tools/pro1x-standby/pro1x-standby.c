/*
 * pro1x-standby -- the N9's stand-by screen for an X session.
 *
 * When the screen goes blank (X's screen saver: after the timeout, or
 * `xset s activate` from the power key), the AMOLED panel shows what the
 * N9 showed in its low power mode instead of black: the time in large thin
 * digits in the top right corner, and under it a row of small icons for
 * unread notifications, the charging state and a silent or beep profile.
 * Everything else stays black, so the panel lights only those pixels. The
 * content moves every three minutes so that nothing burns in; a touch or a
 * key ends the screen saver as before.
 *
 * Sizes and timings are those of Harmattan's screen lock style
 * (LockScreenLowPowerMode* in libsysuid-screenlock-nokia.css), in mm of the
 * real screen. Nokia's icons (icon-m-low-power-mode-*.png) and the Nokia
 * Pure font are not part of this repository; with them installed in the
 * theme directory (-t) the screen looks like the N9's, without them simple
 * drawn icons and the system's light sans font stand in.
 *
 * Sources: persistent notifications from hildon-home's database
 * (~/.config/hildon-desktop/notifications.db, by their category hint), the
 * battery from sysctl hw.acpi.* (FreeBSD) or /sys/class/power_supply
 * (Linux), the profile from profiled on the session bus.
 *
 *   pro1x-standby [-t themedir] [-d dpi] [-f font] [-2]
 *       registers the stand-by window with X and draws it while the screen
 *       saver is on; run it in the session (and `xset s noblank`, else the
 *       server may blank in hardware instead of mapping the window)
 *   pro1x-standby -p out.png WIDTHxHEIGHT [options]
 *       draws one frame into a PNG and exits (for tests)
 */
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/extensions/scrnsaver.h>
#include <cairo.h>
#include <cairo-xlib.h>
#include <pango/pangocairo.h>
#include <sqlite3.h>
#include <dbus/dbus.h>

#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#define DEFAULT_THEME "/usr/local/share/pro1x/harmattan/standby"
#define DEFAULT_FONT "Nokia Pure Text Light, Open Sans Light, Sans Light"
#define MOVE_S 180		/* animation-interval: 180000 */
#define MAX_ICONS 5		/* NotificationIconAreaStyle max-icons */

/* LockScreenLowPowerMode* styles, in mm */
#define TOP_MM 6.8		/* content-initial-top-margin */
#define RIGHT_MM 1.5		/* content-initial-right-margin */
#define CLOCK_FONT_MM 10.0	/* LockScreenLowPowerModeClockLabel font */
#define CLOCK_HEIGHT_MM 11.4	/* its fixed height */
#define CLOCK_SHIFT_MM 1.7	/* its margin-top -1.7mm, margin-bottom 1.7mm */
#define STATUS_MM 4.0		/* LockScreenLowPowerModeStatusAreaStyle */
#define ICON_MM (40 * 25.4 / 251)	/* the 40 px icons on the N9's 251 dpi */
#define SPACING_MM 0.8		/* icon-spacing */

static const char *theme = DEFAULT_THEME;
static const char *font = DEFAULT_FONT;
static double dpi;
static int twelve_hour;

enum charge { CHARGE_NONE, CHARGE_CHARGING, CHARGE_FULL, CHARGE_FAILED };

struct state {
	char time[16];
	enum charge charge;
	int level;			/* 0-4 */
	const char *profile;		/* "silent", "beep" or NULL */
	const char *notes[MAX_ICONS];	/* icon names, newest first */
	int nnotes;
};

static double mm(double v)
{
	return v * dpi / 25.4;
}

/* --- what to show ------------------------------------------------------ */

/* Maemo's notification categories and icon names to the N9's icons */
static const char *note_icon(const char *category, const char *icon)
{
	static const struct { const char *key, *name; } map[] = {
		{ "missed-call", "missed-call" }, { "missed", "missed-call" },
		{ "voice", "voicemail" },
		{ "sms", "sms" }, { "mms", "mms" },
		{ "email", "email" }, { "mail", "email" },
		{ "chat", "chat" }, { "im.", "chat" }, { "auth-request", "chat" },
		{ "calendar", "calendar" }, { "alarm", "calendar" },
		{ "system-update", "system-update" },
		{ "update", "third-party-update" },
	};
	const char *s[2] = { category, icon };

	for (int k = 0; k < 2; k++) {
		if (!s[k])
			continue;
		for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
			if (strstr(s[k], map[i].key))
				return map[i].name;
	}
	return NULL;
}

static void read_notifications(struct state *st)
{
	char path[1024];
	sqlite3 *db;
	sqlite3_stmt *q;
	const char *home = getenv("HOME");

	st->nnotes = 0;
	if (!home)
		return;
	snprintf(path, sizeof(path), "%s/.config/hildon-desktop/notifications.db",
		 home);
	if (access(path, R_OK) ||
	    sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
		return;
	if (sqlite3_prepare_v2(db,
	    "SELECT h.value, n.icon_name FROM notifications n "
	    "LEFT JOIN hints h ON h.nid = n.id AND h.id = 'category' "
	    "ORDER BY n.id DESC", -1, &q, NULL) == SQLITE_OK) {
		while (st->nnotes < MAX_ICONS && sqlite3_step(q) == SQLITE_ROW) {
			const char *name = note_icon(
			    (const char *)sqlite3_column_text(q, 0),
			    (const char *)sqlite3_column_text(q, 1));
			int dup = !name;

			for (int i = 0; i < st->nnotes && !dup; i++)
				dup = !strcmp(st->notes[i], name);
			if (!dup)
				st->notes[st->nnotes++] = name;
		}
		sqlite3_finalize(q);
	}
	sqlite3_close(db);
}

static void read_battery(struct state *st)
{
	int life = -1, charging = 0, full = 0, online = 0;
#ifdef __FreeBSD__
	int v;
	size_t len = sizeof(v);

	if (!sysctlbyname("hw.acpi.battery.life", &v, &len, NULL, 0))
		life = v;
	len = sizeof(v);
	if (!sysctlbyname("hw.acpi.battery.state", &v, &len, NULL, 0))
		charging = v & 2;	/* ACPI_BATT_STAT_CHARGING */
	len = sizeof(v);
	if (!sysctlbyname("hw.acpi.acline", &v, &len, NULL, 0))
		online = v == 1;
	full = online && !charging && life >= 100;
#else
	static const char *dirs[] = {
		"/sys/class/power_supply/battery",
		"/sys/class/power_supply/BAT0",
		"/sys/class/power_supply/bq27521-0",
	};
	char p[256], buf[32];

	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		FILE *f;

		snprintf(p, sizeof(p), "%s/capacity", dirs[i]);
		if (!(f = fopen(p, "r")))
			continue;
		if (fscanf(f, "%d", &life) != 1)
			life = -1;
		fclose(f);
		snprintf(p, sizeof(p), "%s/status", dirs[i]);
		if ((f = fopen(p, "r"))) {
			if (fgets(buf, sizeof(buf), f)) {
				charging = !strncmp(buf, "Charging", 8);
				full = !strncmp(buf, "Full", 4);
			}
			fclose(f);
		}
		online = charging || full;
		break;
	}
#endif
	st->level = life < 0 ? 0 : life >= 100 ? 4 : life / 25;
	if (charging)
		st->charge = CHARGE_CHARGING;
	else if (full)
		st->charge = CHARGE_FULL;
	else if (online && life >= 0 && life < 100)
		st->charge = CHARGE_FAILED;	/* on the charger, not charging */
	else
		st->charge = CHARGE_NONE;
}

static void read_profile(struct state *st, DBusConnection *bus)
{
	DBusMessage *m, *r;
	const char *name;

	st->profile = NULL;
	if (!bus)
		return;
	m = dbus_message_new_method_call("com.nokia.profiled",
	    "/com/nokia/profiled", "com.nokia.profiled", "get_profile");
	if (!m)
		return;
	r = dbus_connection_send_with_reply_and_block(bus, m, 1000, NULL);
	dbus_message_unref(m);
	if (!r)
		return;
	if (dbus_message_get_args(r, NULL, DBUS_TYPE_STRING, &name,
				  DBUS_TYPE_INVALID)) {
		if (!strcmp(name, "silent"))
			st->profile = "silent";
		else if (!strcmp(name, "beep"))
			st->profile = "beep";
	}
	dbus_message_unref(r);
}

static void read_state(struct state *st, DBusConnection *bus)
{
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(st->time, sizeof(st->time), twelve_hour ? "%l:%M" : "%R", &tm);
	read_notifications(st);
	read_battery(st);
	read_profile(st, bus);
}

/* --- drawing ----------------------------------------------------------- */

/* Nokia's icon from the theme, or NULL */
static cairo_surface_t *theme_icon(const char *name)
{
	char p[1024];
	cairo_surface_t *s;

	snprintf(p, sizeof(p), "%s/icon-m-low-power-mode-%s.png", theme, name);
	s = cairo_image_surface_create_from_png(p);
	if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS)
		return s;
	cairo_surface_destroy(s);
	return NULL;
}

/* Stand-ins in white strokes, in a unit square */
static void fallback_icon(cairo_t *cr, const char *name, int level)
{
	cairo_set_line_width(cr, 0.07);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	if (!strncmp(name, "charging", 8) || !strcmp(name, "remove-charger") ||
	    !strcmp(name, "insufficient-power")) {
		cairo_rectangle(cr, 0.12, 0.3, 0.68, 0.4);
		cairo_rectangle(cr, 0.8, 0.42, 0.08, 0.16);
		cairo_stroke(cr);
		if (!strcmp(name, "insufficient-power")) {
			cairo_move_to(cr, 0.46, 0.36);
			cairo_line_to(cr, 0.46, 0.54);
			cairo_stroke(cr);
			cairo_arc(cr, 0.46, 0.62, 0.035, 0, 2 * M_PI);
			cairo_fill(cr);
		} else {
			int bars = !strcmp(name, "remove-charger") ? 4 : level;

			for (int i = 0; i < bars; i++)
				cairo_rectangle(cr, 0.18 + i * 0.15, 0.36, 0.11, 0.28);
			cairo_fill(cr);
		}
	} else if (!strcmp(name, "silent") || !strcmp(name, "beep")) {
		cairo_move_to(cr, 0.3, 0.68);
		cairo_curve_to(cr, 0.3, 0.2, 0.7, 0.2, 0.7, 0.68);
		cairo_close_path(cr);
		cairo_stroke(cr);
		cairo_arc(cr, 0.5, 0.76, 0.06, 0, 2 * M_PI);
		cairo_fill(cr);
		if (!strcmp(name, "silent")) {
			cairo_move_to(cr, 0.2, 0.2);
			cairo_line_to(cr, 0.8, 0.8);
			cairo_stroke(cr);
		}
	} else if (!strcmp(name, "missed-call") || !strcmp(name, "voicemail")) {
		cairo_move_to(cr, 0.2, 0.62);
		cairo_curve_to(cr, 0.35, 0.4, 0.65, 0.4, 0.8, 0.62);
		cairo_stroke(cr);
		cairo_rectangle(cr, 0.16, 0.6, 0.16, 0.1);
		cairo_rectangle(cr, 0.68, 0.6, 0.16, 0.1);
		cairo_fill(cr);
	} else if (!strcmp(name, "chat") || !strcmp(name, "sms") ||
		   !strcmp(name, "mms")) {
		cairo_move_to(cr, 0.18, 0.25);
		cairo_line_to(cr, 0.82, 0.25);
		cairo_line_to(cr, 0.82, 0.65);
		cairo_line_to(cr, 0.42, 0.65);
		cairo_line_to(cr, 0.25, 0.8);
		cairo_line_to(cr, 0.28, 0.65);
		cairo_line_to(cr, 0.18, 0.65);
		cairo_close_path(cr);
		cairo_stroke(cr);
	} else if (!strcmp(name, "calendar")) {
		cairo_rectangle(cr, 0.2, 0.25, 0.6, 0.55);
		cairo_move_to(cr, 0.2, 0.4);
		cairo_line_to(cr, 0.8, 0.4);
		cairo_stroke(cr);
	} else {
		/* email, updates: an envelope */
		cairo_rectangle(cr, 0.15, 0.28, 0.7, 0.44);
		cairo_move_to(cr, 0.15, 0.28);
		cairo_line_to(cr, 0.5, 0.55);
		cairo_line_to(cr, 0.85, 0.28);
		cairo_stroke(cr);
	}
}

/* One icon whose top right corner is at (x, y); returns its width */
static double draw_icon(cairo_t *cr, const char *name, int level,
			double x, double y)
{
	double size = mm(ICON_MM);
	cairo_surface_t *s = theme_icon(name);

	cairo_save(cr);
	cairo_translate(cr, x - size, y);
	if (s) {
		double w = cairo_image_surface_get_width(s);

		cairo_scale(cr, size / w, size / w);
		cairo_set_source_surface(cr, s, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
		cairo_paint(cr);
		cairo_surface_destroy(s);
	} else {
		cairo_scale(cr, size, size);
		cairo_set_source_rgb(cr, 1, 1, 1);
		fallback_icon(cr, name, level);
	}
	cairo_restore(cr);
	return size;
}

/*
 * The frame: black, the clock right-aligned at the content's top right
 * corner (right, top), the status row under it, right-aligned too.
 */
static void draw(cairo_t *cr, const struct state *st, double right, double top)
{
	PangoLayout *layout;
	PangoFontDescription *fd;
	PangoRectangle ink, logical;
	double x, y;
	char name[32];

	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);

	layout = pango_cairo_create_layout(cr);
	fd = pango_font_description_from_string(font);
	pango_font_description_set_absolute_size(fd, mm(CLOCK_FONT_MM) * PANGO_SCALE);
	pango_layout_set_font_description(layout, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(layout, st->time[0] == ' ' ? st->time + 1 : st->time, -1);
	pango_layout_get_pixel_extents(layout, &ink, &logical);
	cairo_set_source_rgb(cr, 1, 1, 1);
	/* the label box is CLOCK_HEIGHT_MM high, its text moved up 1.7 mm */
	cairo_move_to(cr, right - logical.width,
		      top - mm(CLOCK_SHIFT_MM) +
		      (mm(CLOCK_HEIGHT_MM) - logical.height) / 2);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);

	x = right;
	y = top + mm(CLOCK_HEIGHT_MM) + (mm(STATUS_MM) - mm(ICON_MM)) / 2;
	if (st->profile)
		x -= draw_icon(cr, st->profile, 0, x, y) + mm(SPACING_MM);
	switch (st->charge) {
	case CHARGE_CHARGING:
		snprintf(name, sizeof(name), "charging-%d", st->level);
		x -= draw_icon(cr, name, st->level, x, y) + mm(SPACING_MM);
		break;
	case CHARGE_FULL:
		x -= draw_icon(cr, "remove-charger", 4, x, y) + mm(SPACING_MM);
		break;
	case CHARGE_FAILED:
		x -= draw_icon(cr, "insufficient-power", 0, x, y) + mm(SPACING_MM);
		break;
	case CHARGE_NONE:
		break;
	}
	for (int i = 0; i < st->nnotes; i++)
		x -= draw_icon(cr, st->notes[i], 0, x, y) + mm(SPACING_MM);
}

/*
 * Where the content goes: first the N9's initial margins, then a random
 * place in the upper right part of the screen, a new one every MOVE_S
 */
static void place(int w, int h, int moves, double *right, double *top)
{
	double span_x = w * 0.35, span_y = h * 0.45;

	*right = w - mm(RIGHT_MM);
	*top = mm(TOP_MM);
	if (moves) {
		*right -= span_x * (random() / (double)RAND_MAX);
		*top += span_y * (random() / (double)RAND_MAX);
	}
}

/* --- X ----------------------------------------------------------------- */

static double x_dpi(Display *dpy, int scr)
{
	char *type, *rms = XResourceManagerString(dpy);
	XrmValue val;
	double d = 0;

	XrmInitialize();
	if (rms) {
		XrmDatabase db = XrmGetStringDatabase(rms);

		if (XrmGetResource(db, "pro1x-standby.dpi", "Pro1x-standby.Dpi",
				   &type, &val) && val.addr)
			d = atof(val.addr);
		XrmDestroyDatabase(db);
	}
	if (d <= 0 && DisplayWidthMM(dpy, scr) > 0)
		d = DisplayWidth(dpy, scr) * 25.4 / DisplayWidthMM(dpy, scr);
	return d > 0 ? d : 96;
}

static void set_saver(Display *dpy, int scr, int w, int h)
{
	XSetWindowAttributes a = { .background_pixel = BlackPixel(dpy, scr) };

	XScreenSaverSetAttributes(dpy, RootWindow(dpy, scr), 0, 0, w, h, 0,
				  CopyFromParent, InputOutput,
				  (Visual *)CopyFromParent, CWBackPixel, &a);
}

static int run_x(void)
{
	Display *dpy = XOpenDisplay(NULL);
	DBusConnection *bus;
	int ev_base, err_base, scr, w, h, moves = 0;
	Window root, saver = None;
	time_t shown_min = 0, moved = 0;
	struct state st;
	double right = 0, top = 0;

	if (!dpy) {
		fprintf(stderr, "pro1x-standby: no X display\n");
		return 1;
	}
	if (!XScreenSaverQueryExtension(dpy, &ev_base, &err_base)) {
		fprintf(stderr, "pro1x-standby: no MIT-SCREEN-SAVER extension\n");
		return 1;
	}
	scr = DefaultScreen(dpy);
	root = RootWindow(dpy, scr);
	if (dpi <= 0)
		dpi = x_dpi(dpy, scr);
	w = DisplayWidth(dpy, scr);
	h = DisplayHeight(dpy, scr);
	set_saver(dpy, scr, w, h);
	XScreenSaverSelectInput(dpy, root, ScreenSaverNotifyMask);
	XSelectInput(dpy, root, StructureNotifyMask);
	bus = dbus_bus_get(DBUS_BUS_SESSION, NULL);
	srandom(time(NULL));

	for (;;) {
		int redraw = 0;
		time_t now = time(NULL);

		while (XPending(dpy)) {
			XEvent e;

			XNextEvent(dpy, &e);
			if (e.type == ev_base + ScreenSaverNotify) {
				XScreenSaverNotifyEvent *n = (void *)&e;

				if (n->state == ScreenSaverOn) {
					saver = n->window;
					XSelectInput(dpy, saver, ExposureMask);
					moves = 0;
					moved = now;
					place(w, h, moves, &right, &top);
					redraw = 1;
				} else if (n->state == ScreenSaverOff) {
					saver = None;
				}
			} else if (e.type == ConfigureNotify &&
				   e.xconfigure.window == root) {
				w = e.xconfigure.width;
				h = e.xconfigure.height;
				set_saver(dpy, scr, w, h);
			} else if (e.type == Expose && e.xexpose.count == 0) {
				redraw = 1;
			}
		}
		if (saver != None) {
			if (now - moved >= MOVE_S) {
				moved = now;
				place(w, h, ++moves, &right, &top);
				redraw = 1;
			}
			if (now / 60 != shown_min)
				redraw = 1;
		}
		if (redraw && saver != None) {
			cairo_surface_t *s = cairo_xlib_surface_create(dpy, saver,
			    DefaultVisual(dpy, scr), w, h);
			cairo_t *cr = cairo_create(s);

			read_state(&st, bus);
			draw(cr, &st, right, top);
			cairo_destroy(cr);
			cairo_surface_destroy(s);
			XFlush(dpy);
			shown_min = now / 60;
		}
		/* sleep until the next event or the next minute */
		{
			struct pollfd p = { ConnectionNumber(dpy), POLLIN, 0 };
			int ms = saver != None ? (60 - now % 60) * 1000 : -1;

			if (poll(&p, 1, ms) < 0 && errno != EINTR)
				return 1;
		}
	}
}

/* --- main -------------------------------------------------------------- */

static int run_preview(const char *out, const char *size)
{
	int w, h;
	cairo_surface_t *s;
	cairo_t *cr;
	struct state st;
	double right, top;
	DBusConnection *bus;

	if (sscanf(size, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
		fprintf(stderr, "pro1x-standby: size WIDTHxHEIGHT\n");
		return 2;
	}
	if (dpi <= 0)
		dpi = 96;
	s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	cr = cairo_create(s);
	bus = getenv("DBUS_SESSION_BUS_ADDRESS") ?
	    dbus_bus_get(DBUS_BUS_SESSION, NULL) : NULL;
	read_state(&st, bus);
	/* PRO1X_STANDBY_TEST: icons to show instead of the real state */
	if (getenv("PRO1X_STANDBY_TEST")) {
		static char buf[256];
		char *tok, *save;

		snprintf(buf, sizeof(buf), "%s", getenv("PRO1X_STANDBY_TEST"));
		st.nnotes = 0;
		for (tok = strtok_r(buf, ",", &save); tok;
		     tok = strtok_r(NULL, ",", &save)) {
			if (!strcmp(tok, "silent") || !strcmp(tok, "beep"))
				st.profile = tok;
			else if (!strncmp(tok, "charging", 8)) {
				st.charge = CHARGE_CHARGING;
				st.level = atoi(tok + 8);
			} else if (!strcmp(tok, "full"))
				st.charge = CHARGE_FULL;
			else if (st.nnotes < MAX_ICONS)
				st.notes[st.nnotes++] = tok;
		}
	}
	place(w, h, 0, &right, &top);
	draw(cr, &st, right, top);
	cairo_destroy(cr);
	if (cairo_surface_write_to_png(s, out) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "pro1x-standby: cannot write %s\n", out);
		return 1;
	}
	cairo_surface_destroy(s);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
	    "usage: pro1x-standby [-t themedir] [-d dpi] [-f font] [-2]\n"
	    "       pro1x-standby -p out.png WIDTHxHEIGHT [options]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *preview = NULL;
	int c;

	while ((c = getopt(argc, argv, "t:d:f:p:2h")) != -1) {
		switch (c) {
		case 't': theme = optarg; break;
		case 'd': dpi = atof(optarg); break;
		case 'f': font = optarg; break;
		case 'p': preview = optarg; break;
		case '2': twelve_hour = 1; break;
		default: usage();
		}
	}
	if (preview) {
		if (optind != argc - 1)
			usage();
		return run_preview(preview, argv[optind]);
	}
	if (optind != argc)
		usage();
	return run_x();
}
