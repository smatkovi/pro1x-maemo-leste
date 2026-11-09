/*
 * pro1x-standby -- the Nokia N9's stand-by screen for an X session.
 *
 * Instead of going black, the screen then shows what the N9 showed in its
 * low power mode: the time in large thin digits in the top right corner,
 * and under it a row of small icons for unread notifications, the charging
 * state and a silent or beep profile. Everything else stays black, so the
 * AMOLED panel lights only those pixels. The content moves every three
 * minutes so that nothing burns in.
 *
 * When, as on the N9 (gconf /system/osso/dsm/display, mce.ini [TKLock]):
 * after the display time-out without input (display_dim_timeout, 30 s,
 * -s) the N9 dims, and display_blank_timeout (3 s) later it blanks and
 * locks, which with use_low_power_mode on is the stand-by screen; the
 * power key goes there at once (SIGUSR1, from pro1x-powerkey, toggles).
 * Single touches then do nothing (DisableTSImmediately=2: only a double
 * tap after blanking), so that the phone stays dark in a pocket: a double
 * tap or the power key wake it up. Not like the N9: no lock screen after.
 *
 * Power, as Nokia saved it: the panel dims to a fifth first; the stand-by
 * screen runs in the panel's low power mode (idle mode, 8 colours at a
 * reduced refresh: the N9 panel driver's "lpm") at -b % brightness; while
 * the proximity sensor is covered (pro1x-proximity, on only while the
 * stand-by screen shows) the panel is off and nothing is drawn; with -R
 * (how X's screen lies on the panel) only the panel rows around the
 * content are driven (partial mode, as the N9's panel had its partial
 * area), following it when it moves. The
 * keyboard light follows the N950's mce: on with key presses, off 30 s
 * (-k) after the last one and when the display dims. The program itself
 * sleeps until something happens: X tells it when the idle time passes a
 * mark (SYNC's IDLETIME alarms), the clock wakes it once a minute only
 * while the stand-by screen is visible, the notifications database is
 * read again only when it changed.
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
 *   pro1x-standby [-s timeout] [-b brightness] [-k kbdtimeout]
 *                 [-O|-A minutes|SECONDSs] [-C settings]
 *                 [-R none|cw|ccw|inverted] [-P pidfile]
 *                 [-t themedir] [-d dpi] [-f font] [-2]
 *       runs in the X session (with X's own screen saver off, xset s off);
 *       -O: the panel off (and the touchscreen asleep, the proximity
 *       sensor unwatched) after that many minutes of the stand-by screen
 *       (-O 87s: seconds; the stand-by screen comes -s + 3 s after the
 *       last input), 0: at once in its place (the power key brings the
 *       desktop back); default never, as on the N9. With the panel off
 *       the SoC can sleep (pro1x-autosleep, freebsd/pm/README.md stage 6).
 *       Where no mce runs (FreeBSD), it says what the display does as
 *       mce would, so that hildon-desktop and the clients stop drawing
 *       while it is off: com.nokia.mce.signal display_status_ind on the
 *       system bus, "on", "dimmed", "lpm" (the stand-by screen, as
 *       patches/lpm's mce) and "off" (the panel off).
 *       There it also takes what mce would for the display: the system
 *       UI's alarm dialog state (com.nokia.system_ui.signal
 *       alarm_dialog_status: 5 ringing, 6 shown, 7 gone; pro1x-alarmboot
 *       says its screens' the same way) and, as com.nokia.mce where no mce
 *       owns that name (freebsd/hildon/extra: its D-Bus policy),
 *       req_display_state_on, req_display_blanking_pause (60 s each, as
 *       mce's; Leste's false or Harmattan's
 *       req_display_cancel_blanking_pause end it) and get_display_status;
 *       other requests get an error at once. While an alarm's screen shows
 *       or a pause runs there is no time-out; an alarm's screen coming
 *       (5, 6) and req_display_state_on bring the desktop back from the
 *       stand-by screen and the panel on, as mce turns the display on.
 *       Each sender's state holds until it changes it or leaves the bus.
 *       The power key works as before.
 *       And it takes the display's settings as Leste's mce does, which
 *       the control panel's Display (osso-applet-display) asks it for:
 *       get_/set_display_timeout (display_dim_timeout, s; 0 never),
 *       get_/set_display_brightness (1-5, 20 % each, as mce's
 *       filter-brightness-simple) and get_/set_inhibit_timout_mode
 *       (inhibit_blank_mode: 1 on and 2 dimmed at most while the charger
 *       is in, 3 and 4 always), each change said as mce's
 *       sig_display_timeout, sig_display_brightness and
 *       sig_display_timeout_mode. They apply at once and are kept in -C
 *       (/var/db/pro1x-display, mce's key names); a time-out kept there
 *       counts instead of -s. -A (as -O, minutes or SECONDSs): the
 *       panel off that long after the last input, whatever the time-out
 *       (the stand-by screen in between, or none when the time-out and
 *       3 s reach that), in place of -O's time of the stand-by screen.
 *   pro1x-standby -M [-R ...] [-t themedir] [-d dpi] [-f font] [-2]
 *       on Leste, with mce's low power mode (patches/lpm): mce decides
 *       (time-outs, proximity, the power key, the panel's lpm and
 *       brightness, the keyboard light); the screen shows while mce's
 *       display_status_ind says "lpm", and a double tap asks mce for the
 *       display (req_display_state_on), which shows the lock screen
 *   pro1x-standby -p out.png WIDTHxHEIGHT [options]
 *       draws one frame into a PNG and exits (for tests)
 */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/extensions/sync.h>
#include <X11/extensions/XInput2.h>
#include <cairo.h>
#include <cairo-xlib.h>
#include <pango/pangocairo.h>
#include <sqlite3.h>
#include <dbus/dbus.h>

#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <sys/backlight.h>
#endif

#define DEFAULT_THEME "/usr/local/share/pro1x/harmattan/standby"
#define DEFAULT_FONT "Nokia Pure Text Light, Open Sans Light, Sans Light"
#define MOVE_S 180		/* animation-interval: 180000 */
#define DIM_TIMEOUT_S 30	/* display_dim_timeout, the N9's default */
#define BLANK_TIMEOUT_S 3	/* display_blank_timeout, after dimming */
#define STANDBY_BL 20		/* % brightness of the stand-by screen */
#define KBD_TIMEOUT_S 30	/* the N950's [KeyPad] BacklightTimeout */
#define DOUBLE_TAP_MS 400	/* two touches this close wake it up */
#define DOUBLE_TAP_MM 10.0	/* and this near each other */
#define DEFAULT_PIDFILE "/var/run/pro1x-standby.pid"
#define DEFAULT_SETTINGS "/var/db/pro1x-display"
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
static int dim_timeout = DIM_TIMEOUT_S;
static int standby_bl = STANDBY_BL;
static int kbd_timeout = KBD_TIMEOUT_S;
/* -O: seconds of the stand-by screen before the panel goes off; -1 never */
static int off_after = -1;
/* -A: seconds after the last input until the panel goes off; -1: -O's */
static int off_total = -1;
/* the control panel's settings, as mce's (where no mce runs): -C */
static const char *settings = DEFAULT_SETTINGS;
static int dim_kept;		/* dim_timeout from there, not -s */
static int bl_level = -1;	/* display_brightness 1-5, -1 never set */
static int inhibit_mode;	/* inhibit_blank_mode 0-4 */
/* how X's screen lies on the panel's rows, for partial mode; NULL: off */
static const char *rotation;
/* -M: follow mce's low power mode instead of timing it here */
static int follow_mce;
/* without -M and without mce: display_status_ind sent from here */
static int announce_display;
static const char *pidfile = DEFAULT_PIDFILE;

enum charge { CHARGE_NONE, CHARGE_CHARGING, CHARGE_FULL, CHARGE_FAILED };

struct state {
	time_t db_mtime;		/* the notifications as read then */
	int db_read;
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

/* Only when the database changed: a stat a minute, no SQLite */
static void read_notifications(struct state *st)
{
	char path[1024];
	struct stat sb;
	sqlite3 *db;
	sqlite3_stmt *q;
	const char *home = getenv("HOME");

	if (!home)
		return;
	snprintf(path, sizeof(path), "%s/.config/hildon-desktop/notifications.db",
		 home);
	if (stat(path, &sb) != 0) {
		st->nnotes = 0;
		return;
	}
	if (st->db_read && sb.st_mtime == st->db_mtime)
		return;
	st->db_read = 1;
	st->db_mtime = sb.st_mtime;
	st->nnotes = 0;
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
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

/* full: the profile too (a D-Bus call; once when the screen comes up) */
static void read_state(struct state *st, DBusConnection *bus, int full)
{
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(st->time, sizeof(st->time), twelve_hour ? "%l:%M" : "%R", &tm);
	read_notifications(st);
	read_battery(st);
	if (full)
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
static void draw(cairo_t *cr, const struct state *st, double right, double top,
		 double box[4])
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

	/* what it lit: x0 y0 x1 y1, for the panel's partial mode */
	if (box) {
		box[0] = x < right - logical.width ? x : right - logical.width;
		box[1] = top - mm(CLOCK_SHIFT_MM);
		box[2] = right;
		box[3] = top + mm(CLOCK_HEIGHT_MM) + mm(STATUS_MM);
	}
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

/* --- the hardware's power: panel, keyboard light -------------------------
 *
 * As the N9 does it: dimmed before the stand-by screen, which runs in the
 * panel's low power mode (idle mode: 8 colours, reduced refresh, as the
 * N9's panel-nokia-dsi "lpm") at a low brightness, the panel off while the
 * proximity sensor is covered; the keyboard light (the N950's mce:
 * [KeyPad] BacklightTimeout=30) on with key presses, off 30 s after the
 * last one and whenever the display dims. FreeBSD's qcom_dpu
 * (patches/0022) and gpioled; elsewhere these do nothing.
 */
#define KBD_LED "/dev/led/kbd-backlight"

#ifdef __FreeBSD__
static int bl_get(void)
{
	struct backlight_props p;
	int fd = open("/dev/backlight/backlight0", O_RDWR), r = -1;

	if (fd >= 0) {
		if (ioctl(fd, BACKLIGHTGETSTATUS, &p) == 0)
			r = p.brightness;
		close(fd);
	}
	return r;
}

static void bl_set(int pct)
{
	struct backlight_props p = { 0 };
	int fd;

	if (pct < 0 || (fd = open("/dev/backlight/backlight0", O_RDWR)) < 0)
		return;
	p.brightness = pct > 100 ? 100 : pct;
	ioctl(fd, BACKLIGHTUPDATESTATUS, &p);
	close(fd);
}

/* dev.qcom_dpu.0.display / .lpm (patches/0022) */
static void panel_set(const char *what, int on)
{
	char name[64];

	snprintf(name, sizeof(name), "dev.qcom_dpu.0.%s", what);
	sysctlbyname(name, NULL, NULL, &on, sizeof(on));
}

/* only rows first..last driven; last 0: all of them */
static void panel_partial(int first, int last)
{
	char v[32];

	if (last > 0)
		snprintf(v, sizeof(v), "%d %d", first, last);
	else
		strlcpy(v, "0", sizeof(v));
	sysctlbyname("dev.qcom_dpu.0.partial", NULL, NULL, v, strlen(v) + 1);
}

/* the touchscreen not scanning (patches/0028), while covered */
static void touch_sleep(int on)
{
	sysctlbyname("dev.goodix.0.sleep", NULL, NULL, &on, sizeof(on));
}
#else
/*
 * Linux: the panel's backlight device (the Pro1X's panel driver with
 * device/kernel/patches/0007: lpm and partial next to it, as the N9 had
 * /sys/class/backlight/display0/device/lpm)
 */
static const char *bl_dir(void)
{
	static char dir[256];
	glob_t g;

	if (!dir[0] && glob("/sys/class/backlight/*", 0, NULL, &g) == 0) {
		snprintf(dir, sizeof(dir), "%s", g.gl_pathv[0]);
		globfree(&g);
	}
	return dir[0] ? dir : NULL;
}

static int sys_read(const char *name)
{
	char p[320];
	FILE *f;
	int v = -1;

	if (!bl_dir())
		return -1;
	snprintf(p, sizeof(p), "%s/%s", bl_dir(), name);
	if ((f = fopen(p, "r"))) {
		if (fscanf(f, "%d", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

static void sys_write(const char *name, const char *v)
{
	char p[320];
	int fd;

	if (!bl_dir())
		return;
	snprintf(p, sizeof(p), "%s/%s", bl_dir(), name);
	if ((fd = open(p, O_WRONLY)) < 0)
		return;
	if (write(fd, v, strlen(v)) < 0) {
		/* the panel may not have it */
	}
	close(fd);
}

static int bl_get(void)
{
	int b = sys_read("brightness"), max = sys_read("max_brightness");

	return b < 0 || max <= 0 ? -1 : (b * 100 + max / 2) / max;
}

static void bl_set(int pct)
{
	int max = sys_read("max_brightness");
	char v[16];

	if (pct < 0 || max <= 0)
		return;
	snprintf(v, sizeof(v), "%d", (pct > 100 ? 100 : pct) * max / 100);
	sys_write("brightness", v);
}

/* display: the backlight's power (the panel driver writes 0 then) */
static void panel_set(const char *what, int on)
{
	if (!strcmp(what, "display"))
		sys_write("bl_power", on ? "0" : "4");
	else
		sys_write("device/lpm", on ? "1" : "0");
}

static void panel_partial(int first, int last)
{
	char v[32];

	if (last > 0)
		snprintf(v, sizeof(v), "%d %d", first, last);
	else
		snprintf(v, sizeof(v), "0");
	sys_write("device/partial", v);
}

/* Leste's mce inhibits the touchscreen itself */
static void touch_sleep(int on)
{
	(void)on;
}
#endif

static void kbd_light(int on)
{
	int fd = open(KBD_LED, O_WRONLY);

	if (fd < 0)
		return;
	if (write(fd, on ? "1" : "0", 1) < 0) {
		/* nothing to do about it */
	}
	close(fd);
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

/* SIGUSR1 (the power key) toggles, through a pipe into the main loop */
static int sigpipe[2];

static void on_usr1(int sig)
{
	int e = errno;

	(void)sig;
	if (write(sigpipe[1], "t", 1) < 0) {
		/* full: a toggle is pending already */
	}
	errno = e;
}

enum mode { ACTIVE, DIMMED, STANDBY };

/* PRO1X_STANDBY_DEBUG: what happens, on stderr */
static void debug(const char *what)
{
	static int on = -1;

	if (on < 0)
		on = getenv("PRO1X_STANDBY_DEBUG") != NULL;
	if (on)
		fprintf(stderr, "%ld %s\n", (long)time(NULL), what);
}

struct standby {
	Display *dpy;
	int scr, w, h;
	Window win;
	DBusConnection *bus;		/* session: the profile */
	DBusConnection *sys;		/* system: the proximity sensor */
	enum mode mode;
	int near;			/* the proximity sensor is covered */
	int saved_bl;			/* brightness before dimming, or -1 */
	int moves;
	time_t moved, shown_min;
	time_t shown_at;		/* the stand-by screen since */
	int panel_off;			/* -O: the panel switched off */
	double right, top;
	int part_first, part_last;	/* partial mode as applied, or 0 */
	struct state st;
	Time tap_time;			/* the last touch, for double taps */
	int tap_x, tap_y;
	/* X's idle time (SYNC's IDLETIME counter): alarms, no polling */
	int sync_ev;
	XSyncCounter idle;
	XSyncAlarm up, down;
	int xi_opcode;			/* key presses (XInput 2), or -1 */
	long long kbd_off_ms;		/* when the keyboard light goes, or 0 */
};

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static long long idle_now(struct standby *sb)
{
	XSyncValue v;

	if (!XSyncQueryCounter(sb->dpy, sb->idle, &v))
		return 0;
	return ((long long)XSyncValueHigh32(v) << 32) |
	    (unsigned)XSyncValueLow32(v);
}

/*
 * up: when the idle time rises past `up` ms; down: when it falls below
 * `down` (input); 0 for either: never. "Never" is a mark 24 days away,
 * not further: the X server turns the distance into a timer, and a larger
 * one overflowed it into a busy loop (seen on Xvfb).
 */
#define NEVER_MS 0x7fffffffLL

static void set_value(XSyncValue *v, long long ms)
{
	XSyncIntsToValue(v, (unsigned)(ms & 0xffffffff), (int)(ms >> 32));
}

static void arm(struct standby *sb, long long up, long long down)
{
	XSyncAlarmAttributes a;
	unsigned long flags = XSyncCAValue | XSyncCATestType | XSyncCAEvents;

	a.events = True;
	a.trigger.test_type = XSyncPositiveTransition;
	set_value(&a.trigger.wait_value, up ? up : NEVER_MS);
	XSyncChangeAlarm(sb->dpy, sb->up, flags, &a);
	if (down) {
		a.trigger.test_type = XSyncNegativeTransition;
		set_value(&a.trigger.wait_value, down);
	} else {
		set_value(&a.trigger.wait_value, NEVER_MS);
	}
	XSyncChangeAlarm(sb->dpy, sb->down, flags, &a);
}

static XSyncAlarm new_alarm(struct standby *sb)
{
	XSyncAlarmAttributes a;

	a.trigger.counter = sb->idle;
	a.trigger.value_type = XSyncAbsolute;
	a.trigger.test_type = XSyncPositiveTransition;
	set_value(&a.trigger.wait_value, NEVER_MS);
	XSyncIntToValue(&a.delta, 0);
	a.events = True;
	return XSyncCreateAlarm(sb->dpy, XSyncCACounter | XSyncCAValueType |
				XSyncCAValue | XSyncCATestType | XSyncCADelta |
				XSyncCAEvents, &a);
}

/*
 * Active: dim after the time-out, counted from now's idle time (after the
 * power key woke it up without input, X's idle time is old); input before
 * then counts from zero again
 */
/*
 * What the display does, as mce's display_status_ind says it (where no mce
 * runs): hildon-desktop stops drawing while it is "off", libosso tells the
 * applications, pro1x-events and pro1x-volume-osd wait; once per change
 */
static const char *said;		/* what announce said last */

static void announce(struct standby *sb, const char *state)
{
	DBusMessage *m;

	if (!announce_display || !sb->sys || (said && !strcmp(said, state)))
		return;
	m = dbus_message_new_signal("/com/nokia/mce/signal", "com.nokia.mce.signal",
	    "display_status_ind");
	if (!m)
		return;
	if (dbus_message_append_args(m, DBUS_TYPE_STRING, &state, DBUS_TYPE_INVALID)) {
		dbus_connection_send(sb->sys, m, NULL);
		dbus_connection_flush(sb->sys);
		said = state;
		debug(state);
	}
	dbus_message_unref(m);
}

/*
 * Where no mce runs: what keeps the display on as mce's would, by sender
 * (its unique bus name): an alarm's screen (alarm_dialog_status 5 or 6)
 * and blanking pauses (mce's BLANK_PREVENT_TIMEOUT, 60 s)
 */
#define HOLDERS 8
#define PAUSE_MS 60000LL
static struct holder {
	char name[64];		/* "" a free one */
	unsigned alarm;		/* its alarm UI state 5 or 6, 0 none */
	long long pause_until;	/* its blanking pause's end (now_ms), 0 none */
} holders[HOLDERS];
static int mce_owned;		/* com.nokia.mce is ours (no mce runs) */

static int held(void)
{
	long long now = now_ms();

	for (int i = 0; i < HOLDERS; i++)
		if (holders[i].name[0] &&
		    (holders[i].alarm || holders[i].pause_until > now))
			return 1;
	return 0;
}

/* NameOwnerChanged for it: a holder that leaves the bus holds no more */
static void name_watch(struct standby *sb, const char *name, int on)
{
	char rule[256];

	snprintf(rule, sizeof(rule), "type='signal',sender='org.freedesktop.DBus',"
	    "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='%.63s'",
	    name);
	if (on)
		dbus_bus_add_match(sb->sys, rule, NULL);
	else
		dbus_bus_remove_match(sb->sys, rule, NULL);
}

static struct holder *holder(struct standby *sb, const char *name, int add)
{
	struct holder *free_one = NULL;

	if (!name || !*name)
		return NULL;
	for (int i = 0; i < HOLDERS; i++) {
		if (!strcmp(holders[i].name, name))
			return &holders[i];
		if (!holders[i].name[0] && !free_one)
			free_one = &holders[i];
	}
	if (!add || !free_one || strlen(name) >= sizeof(free_one->name))
		return NULL;
	snprintf(free_one->name, sizeof(free_one->name), "%s", name);
	free_one->alarm = 0;
	free_one->pause_until = 0;
	name_watch(sb, name, 1);
	return free_one;
}

/* one that holds nothing any more: forgotten */
static void holder_done(struct standby *sb, struct holder *h)
{
	if (h->alarm || h->pause_until > now_ms())
		return;
	name_watch(sb, h->name, 0);
	h->name[0] = 0;
	h->pause_until = 0;
}

static void go_active(struct standby *sb)
{
	long long base;

	sb->mode = ACTIVE;
	debug("active");
	announce(sb, "on");
	if (follow_mce) {
		arm(sb, 0, 0);
		return;
	}
	if (held() || dim_timeout <= 0) {
		/* an alarm's screen or a pause, or the time-out "never" */
		debug(dim_timeout <= 0 ? "no time-out" : "held");
		arm(sb, 0, 0);
		return;
	}
	base = idle_now(sb);
	arm(sb, base + dim_timeout * 1000LL, base > 500 ? base : 0);
}

/*
 * Partial mode: the panel rows under the lit area (and 2 mm more each
 * side). Panel rows run along X's x axis when X is turned by a quarter
 * (cw: row = x, ccw: row = width - 1 - x), along y otherwise.
 */
static void sb_partial(struct standby *sb, const double box[4])
{
	double a, b, n, margin = mm(2);
	int first, last;

	if (!rotation)
		return;
	if (!strcmp(rotation, "cw") || !strcmp(rotation, "ccw")) {
		n = sb->w;
		a = box[0];
		b = box[2];
	} else {
		n = sb->h;
		a = box[1];
		b = box[3];
	}
	if (!strcmp(rotation, "ccw") || !strcmp(rotation, "inverted")) {
		double t = n - 1 - b;

		b = n - 1 - a;
		a = t;
	}
	first = a - margin > 0 ? (int)(a - margin) : 0;
	last = b + margin < n - 1 ? (int)(b + margin) : (int)n - 1;
	if (last <= first || (first == sb->part_first && last == sb->part_last))
		return;
	panel_partial(first, last);
	sb->part_first = first;
	sb->part_last = last;
	{
		char m[48];

		snprintf(m, sizeof(m), "partial rows %d-%d", first, last);
		debug(m);
	}
}

static void sb_draw(struct standby *sb, int full)
{
	cairo_surface_t *s;
	cairo_t *cr;
	double box[4];

	if (sb->near)
		return;
	s = cairo_xlib_surface_create(sb->dpy, sb->win,
	    DefaultVisual(sb->dpy, sb->scr), sb->w, sb->h);
	cr = cairo_create(s);
	read_state(&sb->st, sb->bus, full);
	draw(cr, &sb->st, sb->right, sb->top, box);
	cairo_destroy(cr);
	cairo_surface_destroy(s);
	XFlush(sb->dpy);
	sb->shown_min = time(NULL) / 60;
	sb_partial(sb, box);
}

/* All input to the stand-by window, so that touches and keys reach nothing */
static void sb_grab(struct standby *sb)
{
	XGrabPointer(sb->dpy, sb->win, False, ButtonPressMask,
		     GrabModeAsync, GrabModeAsync, sb->win, None, CurrentTime);
	XGrabKeyboard(sb->dpy, sb->win, False, GrabModeAsync, GrabModeAsync,
		      CurrentTime);
}

static void proximity(struct standby *sb, int near);

/* The proximity sensor on while the stand-by screen shows (pro1x-proximity) */
static void proximity_watch(struct standby *sb, int on)
{
	DBusMessage *m;

	if (!sb->sys)
		return;
	m = dbus_message_new_method_call("org.pro1x.Proximity",
	    "/org/pro1x/Proximity", "org.pro1x.Proximity",
	    on ? "Watch" : "Unwatch");
	if (!m)
		return;
	dbus_message_set_no_reply(m, TRUE);
	dbus_message_set_auto_start(m, FALSE);
	dbus_connection_send(sb->sys, m, NULL);
	dbus_message_unref(m);
	dbus_connection_flush(sb->sys);
}

/*
 * The state now, if the sensor streams already for another watcher
 * (Changed comes only with a change)
 */
static void proximity_query(struct standby *sb)
{
	DBusMessage *m, *r;
	dbus_bool_t near, valid;

	if (!sb->sys)
		return;
	m = dbus_message_new_method_call("org.pro1x.Proximity",
	    "/org/pro1x/Proximity", "org.pro1x.Proximity", "GetNear");
	if (!m)
		return;
	dbus_message_set_auto_start(m, FALSE);
	r = dbus_connection_send_with_reply_and_block(sb->sys, m, 500, NULL);
	dbus_message_unref(m);
	if (!r)
		return;
	if (dbus_message_get_args(r, NULL, DBUS_TYPE_BOOLEAN, &near,
				  DBUS_TYPE_BOOLEAN, &valid, DBUS_TYPE_INVALID) &&
	    valid)
		proximity(sb, near != 0);
	dbus_message_unref(r);
}

static void dim(struct standby *sb)
{
	int cur = bl_get();

	sb->mode = DIMMED;
	debug("dimmed");
	announce(sb, "dimmed");
	sb->saved_bl = cur;
	if (cur > 0)
		bl_set(cur / 5 > 0 ? cur / 5 : 1);
	kbd_light(0);
	sb->kbd_off_ms = 0;
}

static void undim(struct standby *sb)
{
	if (sb->saved_bl >= 0)
		bl_set(sb->saved_bl);
	sb->saved_bl = -1;
}

/*
 * -O: the panel off as while covered, the touchscreen asleep, the
 * proximity sensor no longer watched (it streams over the ADSP, whose
 * messages would wake the CPUs): the power key brings the desktop back
 */
static void panel_sleep(struct standby *sb)
{
	if (sb->panel_off || follow_mce)
		return;
	debug("panel off");
	announce(sb, "off");
	proximity_watch(sb, 0);
	if (!sb->near) {
		panel_set("display", 0);
		touch_sleep(1);
	}
	sb->near = 0;
	sb->panel_off = 1;
}

/* seconds of the stand-by screen before the panel goes off, -1 never: -O,
 * or what -A leaves of its time after the time-out and the 3 s (now) */
static int panel_off_after(void)
{
	int t;

	if (off_total < 0)
		return off_after;
	t = off_total - (dim_timeout > 0 ? dim_timeout : 0) - BLANK_TIMEOUT_S;
	return t > 0 ? t : 0;
}

/*
 * inhibit_blank_mode as mce's: how far the display may go now. 0 all the
 * way, 1 not past on (modes 1 and 3), 2 not past dimmed (2 and 4); 1 and
 * 2 only while the charger is in.
 */
static int inhibit_limit(void)
{
	struct state st;

	switch (inhibit_mode) {
	case 3:
		return 1;
	case 4:
		return 2;
	case 1:
	case 2:
		read_battery(&st);
		return st.charge != CHARGE_NONE ? inhibit_mode : 0;
	}
	return 0;
}

static void sb_show(struct standby *sb)
{
	if (sb->mode == STANDBY)
		return;
	if (sb->mode == ACTIVE)
		sb->saved_bl = bl_get();
	sb->mode = STANDBY;
	debug("standby");
	sb->near = 0;
	arm(sb, 0, 0);
	kbd_light(0);
	sb->kbd_off_ms = 0;
	XMoveResizeWindow(sb->dpy, sb->win, 0, 0, sb->w, sb->h);
	XMapRaised(sb->dpy, sb->win);
	sb_grab(sb);
	sb->moves = 0;
	sb->moved = time(NULL);
	sb->tap_time = 0;
	place(sb->w, sb->h, 0, &sb->right, &sb->top);
	sb->shown_at = time(NULL);
	sb->panel_off = 0;
	if (panel_off_after() == 0 && !follow_mce) {
		panel_sleep(sb);
		return;
	}
	sb_draw(sb, 1);
	if (follow_mce)
		return;
	announce(sb, "lpm");
	panel_set("lpm", 1);
	bl_set(standby_bl);
	proximity_watch(sb, 1);
	proximity_query(sb);
}

static void sb_hide(struct standby *sb)
{
	if (sb->mode != STANDBY)
		return;
	if (!follow_mce)
		proximity_watch(sb, 0);
	if (sb->near || sb->panel_off) {
		touch_sleep(0);
		panel_set("display", 1);
	}
	sb->near = 0;
	sb->panel_off = 0;
	if (sb->part_last) {
		panel_partial(0, 0);
		sb->part_first = sb->part_last = 0;
	}
	if (!follow_mce)
		panel_set("lpm", 0);
	XUngrabPointer(sb->dpy, CurrentTime);
	XUngrabKeyboard(sb->dpy, CurrentTime);
	XUnmapWindow(sb->dpy, sb->win);
	XFlush(sb->dpy);
	undim(sb);
	go_active(sb);
}

/* The display on, as mce's req_display_state_on: the desktop back from
 * the stand-by screen or from dimming, the time-out counted anew */
static void display_on(struct standby *sb)
{
	if (sb->mode == STANDBY) {
		sb_hide(sb);
	} else {
		if (sb->mode == DIMMED)
			undim(sb);
		go_active(sb);
	}
}

/* What holds the display changed: the time-out as that says now */
static void hold_changed(struct standby *sb)
{
	if (sb->mode == ACTIVE)
		go_active(sb);
}

/* alarm_dialog_status from SENDER: its alarm's screen comes (5, 6), the
 * display on for it, or goes (7) */
static void alarm_status(struct standby *sb, const char *sender, unsigned status)
{
	int shown = status == 5 || status == 6;
	struct holder *h = holder(sb, sender, shown);
	char what[96];

	snprintf(what, sizeof(what), "alarm UI %u from %s", status,
	    sender ? sender : "?");
	debug(what);
	if (!h)
		return;
	h->alarm = shown ? status : 0;
	if (shown) {
		display_on(sb);
		return;
	}
	holder_done(sb, h);
	hold_changed(sb);
}

/* req_display_blanking_pause from SENDER (ON 0: ended) */
static void blanking_pause(struct standby *sb, const char *sender, int on)
{
	struct holder *h = holder(sb, sender, on);

	if (!h)
		return;
	debug(on ? "blanking paused" : "blanking pause ended");
	if (!on) {
		h->pause_until = 0;
		holder_done(sb, h);
		hold_changed(sb);
		return;
	}
	h->pause_until = now_ms() + PAUSE_MS;
	if (sb->mode == DIMMED)
		undim(sb);
	if (sb->mode != STANDBY)
		go_active(sb);
}

/* A holder left the bus */
static void holder_gone(struct standby *sb, const char *name)
{
	struct holder *h = holder(sb, name, 0);

	if (!h)
		return;
	debug("a holder left");
	h->alarm = 0;
	h->pause_until = 0;
	holder_done(sb, h);
	hold_changed(sb);
}

/* ms until the next pause ends, -1 none; ended ones forgotten */
static long long pauses_due(struct standby *sb)
{
	long long now = now_ms(), next = -1;
	int ended = 0;

	for (int i = 0; i < HOLDERS; i++) {
		struct holder *h = &holders[i];

		if (!h->name[0] || !h->pause_until)
			continue;
		if (h->pause_until <= now) {
			h->pause_until = 0;
			holder_done(sb, h);
			ended = 1;
		} else if (next < 0 || h->pause_until - now < next) {
			next = h->pause_until - now;
		}
	}
	if (ended) {
		debug("blanking pause over");
		hold_changed(sb);
	}
	return next;
}

/*
 * The display's settings, kept in -C as "key value" lines with mce's key
 * names (its GConf /system/osso/dsm/display), read once at the start
 */
static void settings_read(void)
{
	char line[128], key[64];
	FILE *f;
	int v;

	if (!settings || !*settings || !(f = fopen(settings, "r")))
		return;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%63s %d", key, &v) != 2)
			continue;
		if (!strcmp(key, "display_dim_timeout") && v >= 0) {
			dim_timeout = v;
			dim_kept = 1;
		} else if (!strcmp(key, "display_brightness") && v >= 1 && v <= 5) {
			bl_level = v;
		} else if (!strcmp(key, "inhibit_blank_mode") && v >= 0 && v <= 4) {
			inhibit_mode = v;
		}
	}
	fclose(f);
}

/* what was set, written anew in full (a new file renamed over it) */
static void settings_write(void)
{
	char tmp[512];
	FILE *f;
	int ok;

	if (!settings || !*settings)
		return;
	snprintf(tmp, sizeof(tmp), "%s.new", settings);
	if (!(f = fopen(tmp, "w"))) {
		debug("settings not kept (no file)");
		return;
	}
	fprintf(f, "# pro1x-standby: the display's settings from the control panel\n");
	if (dim_kept)
		fprintf(f, "display_dim_timeout %d\n", dim_timeout);
	if (bl_level > 0)
		fprintf(f, "display_brightness %d\n", bl_level);
	fprintf(f, "inhibit_blank_mode %d\n", inhibit_mode);
	ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	if (fclose(f) != 0 || !ok || rename(tmp, settings) != 0) {
		unlink(tmp);
		debug("settings not kept");
	}
}

/* sig_display_timeout and the like, as mce's */
static void mce_signal_int(struct standby *sb, const char *name, int v)
{
	dbus_int32_t i = v;
	DBusMessage *m;

	if (!(m = dbus_message_new_signal("/com/nokia/mce/signal",
	    "com.nokia.mce.signal", name)))
		return;
	if (dbus_message_append_args(m, DBUS_TYPE_INT32, &i, DBUS_TYPE_INVALID))
		dbus_connection_send(sb->sys, m, NULL);
	dbus_message_unref(m);
}

/* the brightness the desktop has, now or when it is back: 1-5 */
static int brightness_level(struct standby *sb)
{
	int cur = sb->mode == ACTIVE ? bl_get() : sb->saved_bl;

	if (bl_level > 0)
		return bl_level;
	if (cur < 0)
		return 3;
	cur = (cur + 10) / 20;
	return cur < 1 ? 1 : cur > 5 ? 5 : cur;
}

/* set_display_brightness: the level's 20 % each, now on the desktop or
 * when it comes back from dimmed or the stand-by screen */
static void set_brightness(struct standby *sb, int level)
{
	bl_level = level;
	if (sb->mode == ACTIVE)
		bl_set(level * 20);
	else
		sb->saved_bl = level * 20;
}

/* get_ and set_ requests for the display's settings: 1 handled (R the
 * reply, NULL for bad arguments), 0 not one of them */
static int mce_setting(struct standby *sb, DBusMessage *m, const char *member,
    DBusMessage **r)
{
	dbus_int32_t v;
	char what[64];
	int get;

	if (!strncmp(member, "get_", 4))
		get = 1;
	else if (!strncmp(member, "set_", 4))
		get = 0;
	else
		return 0;
	if (strcmp(member + 4, "display_timeout") &&
	    strcmp(member + 4, "display_brightness") &&
	    strcmp(member + 4, "inhibit_timout_mode"))
		return 0;
	if (get) {
		if (!strcmp(member + 4, "display_timeout"))
			v = dim_timeout;
		else if (!strcmp(member + 4, "display_brightness"))
			v = brightness_level(sb);
		else
			v = inhibit_mode;
		if ((*r = dbus_message_new_method_return(m)))
			dbus_message_append_args(*r, DBUS_TYPE_INT32, &v,
						 DBUS_TYPE_INVALID);
		return 1;
	}
	*r = NULL;
	if (!dbus_message_get_args(m, NULL, DBUS_TYPE_INT32, &v,
				   DBUS_TYPE_INVALID))
		return 1;
	snprintf(what, sizeof(what), "%s %d", member, (int)v);
	if (!strcmp(member + 4, "display_timeout")) {
		if (v < 0)
			return 1;
		dim_timeout = v;
		dim_kept = 1;
		mce_signal_int(sb, "sig_display_timeout", v);
		/* counted anew with it (the dim and stand-by steps as they are) */
		if (sb->mode == ACTIVE)
			go_active(sb);
	} else if (!strcmp(member + 4, "display_brightness")) {
		if (v < 1 || v > 5)
			return 1;
		set_brightness(sb, v);
		mce_signal_int(sb, "sig_display_brightness", v);
	} else {
		if (v < 0 || v > 4)
			return 1;
		inhibit_mode = v;
		mce_signal_int(sb, "sig_display_timeout_mode", v);
		if (sb->mode == ACTIVE)
			go_active(sb);
	}
	debug(what);
	settings_write();
	*r = dbus_message_new_method_return(m);
	return 1;
}

/* A request to com.nokia.mce (ours): the display's, others an error */
static void mce_request(struct standby *sb, DBusMessage *m)
{
	const char *member = dbus_message_get_member(m);
	const char *sender = dbus_message_get_sender(m);
	DBusMessage *r = NULL;
	dbus_bool_t on = TRUE;
	int bad = 0;

	if (member && dbus_message_has_interface(m, "com.nokia.mce.request")) {
		if (!strcmp(member, "req_display_state_on")) {
			debug("req_display_state_on");
			display_on(sb);
			r = dbus_message_new_method_return(m);
		} else if (!strcmp(member, "req_display_blanking_pause")) {
			/* Leste's mce: an optional false ends it */
			if (!dbus_message_get_args(m, NULL, DBUS_TYPE_BOOLEAN, &on,
						   DBUS_TYPE_INVALID))
				on = TRUE;
			blanking_pause(sb, sender, on != 0);
			r = dbus_message_new_method_return(m);
		} else if (!strcmp(member, "req_display_cancel_blanking_pause")) {
			blanking_pause(sb, sender, 0);
			r = dbus_message_new_method_return(m);
		} else if (!strcmp(member, "get_display_status")) {
			const char *st = said ? said : "on";

			r = dbus_message_new_method_return(m);
			if (r)
				dbus_message_append_args(r, DBUS_TYPE_STRING, &st,
							 DBUS_TYPE_INVALID);
		} else if (mce_setting(sb, m, member, &r)) {
			bad = !r;
		}
	}
	if (bad)
		r = dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS,
		    "an int32 in the setting's range");
	else if (!r)
		r = dbus_message_new_error(m, DBUS_ERROR_UNKNOWN_METHOD,
		    "pro1x-standby takes only the display's requests");
	if (!r)
		return;
	if (!dbus_message_get_no_reply(m)) {
		dbus_connection_send(sb->sys, r, NULL);
		dbus_connection_flush(sb->sys);
	}
	dbus_message_unref(r);
}

/* mce: the display on, out of low power mode */
static void mce_display_on(struct standby *sb)
{
	DBusMessage *m;

	if (!sb->sys)
		return;
	m = dbus_message_new_method_call("com.nokia.mce", "/com/nokia/mce/request",
	    "com.nokia.mce.request", "req_display_state_on");
	if (!m)
		return;
	dbus_message_set_no_reply(m, TRUE);
	dbus_connection_send(sb->sys, m, NULL);
	dbus_message_unref(m);
	dbus_connection_flush(sb->sys);
	debug("asked mce for the display");
}

/* A touch while shown: the second of two close ones wakes it up */
static void sb_tap(struct standby *sb, XButtonEvent *b)
{
	double dx = b->x - sb->tap_x, dy = b->y - sb->tap_y;
	double r = mm(DOUBLE_TAP_MM);

	if (sb->tap_time && b->time - sb->tap_time <= DOUBLE_TAP_MS &&
	    dx * dx + dy * dy <= r * r) {
		if (follow_mce)
			mce_display_on(sb);	/* "on" from mce hides it */
		else
			sb_hide(sb);
		return;
	}
	sb->tap_time = b->time;
	sb->tap_x = b->x;
	sb->tap_y = b->y;
}

static void proximity(struct standby *sb, int near)
{
	if (sb->mode != STANDBY || sb->panel_off || near == sb->near)
		return;
	sb->near = near;
	debug(near ? "near" : "far");
	if (near) {
		/*
		 * in a pocket: the panel off and the touchscreen asleep,
		 * nothing to draw until it is out
		 */
		panel_set("display", 0);
		touch_sleep(1);
	} else {
		touch_sleep(0);
		panel_set("display", 1);
		sb_draw(sb, 0);
	}
}

static void sys_bus_input(struct standby *sb)
{
	DBusMessage *m;

	dbus_connection_read_write(sb->sys, 0);
	while ((m = dbus_connection_pop_message(sb->sys))) {
		dbus_bool_t near;
		dbus_uint32_t status;
		const char *state, *name, *old, *new_owner;

		if (mce_owned &&
		    dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
			mce_request(sb, m);
		} else if (!follow_mce &&
		    dbus_message_is_signal(m, "com.nokia.system_ui.signal",
					   "alarm_dialog_status") &&
		    dbus_message_get_args(m, NULL, DBUS_TYPE_UINT32, &status,
					  DBUS_TYPE_INVALID)) {
			alarm_status(sb, dbus_message_get_sender(m), status);
		} else if (dbus_message_is_signal(m, "org.freedesktop.DBus",
						  "NameOwnerChanged") &&
		    dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &name,
					  DBUS_TYPE_STRING, &old,
					  DBUS_TYPE_STRING, &new_owner,
					  DBUS_TYPE_INVALID)) {
			if (!*new_owner)
				holder_gone(sb, name);
		} else if (follow_mce &&
		    dbus_message_is_signal(m, "com.nokia.mce.signal",
					   "display_status_ind") &&
		    dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &state,
					  DBUS_TYPE_INVALID)) {
			debug(state);
			if (!strcmp(state, "lpm"))
				sb_show(sb);
			else
				sb_hide(sb);
		} else if (dbus_message_is_signal(m, "org.pro1x.Proximity", "Changed") &&
		    dbus_message_get_args(m, NULL, DBUS_TYPE_BOOLEAN, &near,
					  DBUS_TYPE_INVALID))
			proximity(sb, near != 0);
		dbus_message_unref(m);
	}
}

/* The X server went away (the session ended): no pid file left behind */
static int x_gone(Display *dpy)
{
	(void)dpy;
	if (pidfile && *pidfile)
		unlink(pidfile);
	exit(0);
}

static void write_pidfile(void)
{
	FILE *f;

	if (!pidfile || !*pidfile)
		return;
	if (!(f = fopen(pidfile, "w"))) {
		fprintf(stderr, "pro1x-standby: %s: %s (the power key cannot "
			"reach it)\n", pidfile, strerror(errno));
		return;
	}
	fprintf(f, "%ld\n", (long)getpid());
	fclose(f);
}

static int setup_sync(struct standby *sb)
{
	int err, major, minor, n;
	XSyncSystemCounter *c;

	if (!XSyncQueryExtension(sb->dpy, &sb->sync_ev, &err) ||
	    !XSyncInitialize(sb->dpy, &major, &minor))
		return -1;
	c = XSyncListSystemCounters(sb->dpy, &n);
	for (int i = 0; i < n; i++)
		if (!strcmp(c[i].name, "IDLETIME"))
			sb->idle = c[i].counter;
	XSyncFreeSystemCounterList(c);
	if (!sb->idle)
		return -1;
	sb->up = new_alarm(sb);
	sb->down = new_alarm(sb);
	return 0;
}

/* Key presses anywhere, for the keyboard light (XInput 2 raw events) */
static void setup_keys(struct standby *sb)
{
	int ev, err, major = 2, minor = 0;
	unsigned char bits[XIMaskLen(XI_LASTEVENT)] = { 0 };
	XIEventMask mask = { XIAllMasterDevices, sizeof(bits), bits };

	sb->xi_opcode = -1;
	if (access(KBD_LED, W_OK) != 0 ||
	    !XQueryExtension(sb->dpy, "XInputExtension", &sb->xi_opcode, &ev,
			     &err) ||
	    XIQueryVersion(sb->dpy, &major, &minor) != Success) {
		sb->xi_opcode = -1;
		return;
	}
	XISetMask(bits, XI_RawKeyPress);
	XISelectEvents(sb->dpy, RootWindow(sb->dpy, sb->scr), &mask, 1);
}

static int run_x(void)
{
	struct standby sb = { 0 };
	XSetWindowAttributes a;

	if (!(sb.dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "pro1x-standby: no X display\n");
		return 1;
	}
	XSetIOErrorHandler(x_gone);
	sb.scr = DefaultScreen(sb.dpy);
	sb.w = DisplayWidth(sb.dpy, sb.scr);
	sb.h = DisplayHeight(sb.dpy, sb.scr);
	sb.saved_bl = -1;
	if (dpi <= 0)
		dpi = x_dpi(sb.dpy, sb.scr);
	if (setup_sync(&sb) != 0) {
		fprintf(stderr, "pro1x-standby: no SYNC extension with IDLETIME\n");
		return 1;
	}
	if (!follow_mce)
		setup_keys(&sb);
	else
		sb.xi_opcode = -1;
	a.background_pixel = BlackPixel(sb.dpy, sb.scr);
	a.override_redirect = True;
	a.event_mask = ExposureMask | ButtonPressMask | KeyPressMask |
	    VisibilityChangeMask;
	sb.win = XCreateWindow(sb.dpy, RootWindow(sb.dpy, sb.scr), 0, 0,
			       sb.w, sb.h, 0, CopyFromParent, InputOutput,
			       CopyFromParent,
			       CWBackPixel | CWOverrideRedirect | CWEventMask, &a);
	XStoreName(sb.dpy, sb.win, "pro1x-standby");
	XSelectInput(sb.dpy, RootWindow(sb.dpy, sb.scr), StructureNotifyMask);
	/* only an existing session bus: libdbus would start one otherwise */
	if (getenv("DBUS_SESSION_BUS_ADDRESS"))
		sb.bus = dbus_bus_get(DBUS_BUS_SESSION, NULL);
	if ((sb.sys = dbus_bus_get(DBUS_BUS_SYSTEM, NULL))) {
		dbus_bus_add_match(sb.sys, "type='signal',"
		    "interface='org.pro1x.Proximity',member='Changed'", NULL);
		if (follow_mce)
			dbus_bus_add_match(sb.sys, "type='signal',"
			    "interface='com.nokia.mce.signal',"
			    "member='display_status_ind'", NULL);
		/* (an mce of its own says it, Leste's with -M) */
		announce_display = !follow_mce &&
		    !dbus_bus_name_has_owner(sb.sys, "com.nokia.mce", NULL);
		/* what mce would take for the display (alarms, requests) */
		if (!follow_mce)
			dbus_bus_add_match(sb.sys, "type='signal',"
			    "interface='com.nokia.system_ui.signal',"
			    "member='alarm_dialog_status'", NULL);
		if (announce_display) {
			mce_owned = dbus_bus_request_name(sb.sys, "com.nokia.mce",
			    DBUS_NAME_FLAG_DO_NOT_QUEUE, NULL) ==
			    DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER;
			debug(mce_owned ? "com.nokia.mce: the display's requests" :
			    "com.nokia.mce not ours (its D-Bus policy?)");
		}
	}
#ifdef __linux__
	/* minute ticks and the keyboard light's 30 s: a tenth of a second
	 * late is fine, the kernel may group the wake-ups */
	prctl(PR_SET_TIMERSLACK, 100000000UL, 0UL, 0UL, 0UL);
#endif
	srandom(time(NULL));
	/* the display's settings are ours as mce's: as last set */
	if (mce_owned) {
		settings_read();
		if (bl_level > 0)
			bl_set(bl_level * 20);
	}
	go_active(&sb);

	if (pipe(sigpipe) == 0) {
		fcntl(sigpipe[0], F_SETFL, O_NONBLOCK);
		fcntl(sigpipe[1], F_SETFL, O_NONBLOCK);
		signal(SIGUSR1, on_usr1);
		write_pidfile();
	}

	for (;;) {
		struct pollfd p[3] = {
			{ ConnectionNumber(sb.dpy), POLLIN, 0 },
			{ sigpipe[0], POLLIN, 0 },
			{ -1, POLLIN, 0 },
		};
		long long wait = -1, t;
		time_t now;
		int oa;
		char c;

		while (XPending(sb.dpy)) {
			XEvent e;

			XNextEvent(sb.dpy, &e);
			if (e.type == sb.sync_ev + XSyncAlarmNotify) {
				XSyncAlarmNotifyEvent *n = (void *)&e;

				if (sb.mode == STANDBY)
					continue;
				if (n->alarm == sb.down) {
					/* input: back from dimmed, count anew */
					if (sb.mode == DIMMED)
						undim(&sb);
					go_active(&sb);
				} else if (n->alarm == sb.up) {
					long long base = idle_now(&sb);
					int limit = inhibit_limit();

					/* inhibit_blank_mode (the charger in):
					 * no further, asked again a time-out
					 * later, input as ever */
					if (limit == 1 || (limit == 2 &&
					    sb.mode == DIMMED)) {
						debug("inhibited");
						arm(&sb, base + (dim_timeout > 0 ?
						    dim_timeout : DIM_TIMEOUT_S) *
						    1000LL, base);
					} else if (sb.mode == ACTIVE) {
						dim(&sb);
						arm(&sb, base + BLANK_TIMEOUT_S *
						    1000LL, base);
					} else {
						sb_show(&sb);
					}
				}
				continue;
			}
			if (e.type == GenericEvent && sb.xi_opcode >= 0 &&
			    e.xcookie.extension == sb.xi_opcode) {
				/* a key: the keyboard light for 30 s */
				if (XGetEventData(sb.dpy, &e.xcookie)) {
					if (sb.mode == ACTIVE) {
						if (!sb.kbd_off_ms)
							kbd_light(1);
						sb.kbd_off_ms = now_ms() +
						    kbd_timeout * 1000LL;
					}
					XFreeEventData(sb.dpy, &e.xcookie);
				}
				continue;
			}
			switch (e.type) {
			case ButtonPress:
				if (sb.mode == STANDBY)
					sb_tap(&sb, &e.xbutton);
				break;
			case Expose:
				if (sb.mode == STANDBY && e.xexpose.count == 0)
					sb_draw(&sb, 0);
				break;
			case VisibilityNotify:
				/* something mapped above it: back on top */
				if (sb.mode == STANDBY &&
				    e.xvisibility.state != VisibilityUnobscured) {
					XRaiseWindow(sb.dpy, sb.win);
					sb_grab(&sb);
				}
				break;
			case ConfigureNotify:
				if (e.xconfigure.window ==
				    RootWindow(sb.dpy, sb.scr)) {
					sb.w = e.xconfigure.width;
					sb.h = e.xconfigure.height;
					if (sb.mode == STANDBY) {
						XMoveResizeWindow(sb.dpy, sb.win,
							0, 0, sb.w, sb.h);
						place(sb.w, sb.h, sb.moves,
						      &sb.right, &sb.top);
					}
				}
				break;
			}
		}
		while (read(sigpipe[0], &c, 1) == 1) {
			if (follow_mce)
				continue;	/* mce has the power key */
			if (sb.mode == STANDBY)
				sb_hide(&sb);
			else
				sb_show(&sb);
		}

		/* sleep until the next event; timers only while needed */
		now = time(NULL);
		oa = panel_off_after();
		if (sb.mode == STANDBY && !sb.panel_off && oa > 0 &&
		    now - sb.shown_at >= oa)
			panel_sleep(&sb);
		if (sb.mode == STANDBY && !sb.near && !sb.panel_off) {
			if (now - sb.moved >= MOVE_S) {
				sb.moved = now;
				place(sb.w, sb.h, ++sb.moves, &sb.right, &sb.top);
				sb_draw(&sb, 0);
			} else if (now / 60 != sb.shown_min) {
				sb_draw(&sb, 0);
			}
			wait = (60 - now % 60) * 1000LL;
			if (oa > 0) {
				t = (sb.shown_at + oa - now) * 1000LL;
				if (t < wait)
					wait = t > 0 ? t : 0;
			}
		}
		t = pauses_due(&sb);
		if (t >= 0 && (wait < 0 || t < wait))
			wait = t;
		if (sb.kbd_off_ms) {
			t = sb.kbd_off_ms - now_ms();
			if (t <= 0) {
				kbd_light(0);
				sb.kbd_off_ms = 0;
			} else if (wait < 0 || t < wait) {
				wait = t;
			}
		}
		if (sb.sys) {
			int fd;

			/* what libdbus read already, before sleeping */
			if (dbus_connection_get_dispatch_status(sb.sys) ==
			    DBUS_DISPATCH_DATA_REMAINS)
				sys_bus_input(&sb);
			if (dbus_connection_get_unix_fd(sb.sys, &fd))
				p[2].fd = fd;
		}
		if (poll(p, 3, wait > 0x7fffffff ? 0x7fffffff : (int)wait) < 0 &&
		    errno != EINTR)
			return 1;
		if (p[2].revents)
			sys_bus_input(&sb);
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
	memset(&st, 0, sizeof(st));
	read_state(&st, bus, 1);
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
	draw(cr, &st, right, top, NULL);
	cairo_destroy(cr);
	if (cairo_surface_write_to_png(s, out) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "pro1x-standby: cannot write %s\n", out);
		return 1;
	}
	cairo_surface_destroy(s);
	return 0;
}

/* -O: minutes (as before), or seconds with an s ("87s"), or minutes with an m */
static int off_seconds(const char *a)
{
	char *end;
	long v = strtol(a, &end, 10);

	if (end == a || v < 0 || v > 100000)
		return -2;
	if (!*end || !strcmp(end, "m"))
		return (int)v * 60;
	if (!strcmp(end, "s"))
		return (int)v;
	return -2;
}

static void usage(void)
{
	fprintf(stderr,
	    "usage: pro1x-standby [-s timeout] [-b brightness] [-k kbdtimeout]\n"
	    "                     [-O|-A minutes|SECONDSs] [-C settings]\n"
	    "                     [-R none|cw|ccw|inverted] [-P pidfile]\n"
	    "                     [-t themedir] [-d dpi] [-f font] [-2]\n"
	    "       pro1x-standby -M [-R ...] [-t themedir] [-d dpi] [-f font] [-2]\n"
	    "       pro1x-standby -p out.png WIDTHxHEIGHT [options]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *preview = NULL;
	int c;

	while ((c = getopt(argc, argv, "s:b:k:O:A:C:R:P:t:d:f:p:2Mh")) != -1) {
		switch (c) {
		case 'R':
			if (strcmp(optarg, "none") && strcmp(optarg, "cw") &&
			    strcmp(optarg, "ccw") && strcmp(optarg, "inverted"))
				usage();
			rotation = optarg;
			break;
		case 's': dim_timeout = atoi(optarg); break;
		case 'b': standby_bl = atoi(optarg); break;
		case 'k': kbd_timeout = atoi(optarg); break;
		case 'O':
			if ((off_after = off_seconds(optarg)) < -1)
				usage();
			break;
		case 'A':
			if ((off_total = off_seconds(optarg)) < 0)
				usage();
			break;
		case 'C': settings = optarg; break;
		case 'P': pidfile = optarg; break;
		case 't': theme = optarg; break;
		case 'd': dpi = atof(optarg); break;
		case 'f': font = optarg; break;
		case 'p': preview = optarg; break;
		case '2': twelve_hour = 1; break;
		case 'M': follow_mce = 1; break;
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
