/*
 * pro1x-events -- the Nokia N9's Events page for Hildon's MeeGo-like look
 * (patches/meego): the first home view shows, below the status bar, the
 * day and the date, the weather of a place (now and the next days), the
 * notifications (missed calls, messages and chats that rtcom-eventlogger
 * has unread, unread e-mail from modest while it runs), the appointments
 * of today and the next days (Hildon's calendar) and the home timeline of
 * a Mastodon account, in one list that scrolls; pulled down at its top it
 * updates. Each of these sections can be switched off. Only while the look
 * is on (the root window's _HILDON_MEEGO_LOOK, which hildon-desktop sets),
 * else it waits unseen.
 *
 *   pro1x-events        runs in the X session (Leste: Xsession.post,
 *                       FreeBSD: hildon-session)
 *
 * The page is a Home widget of its own, as Hildon's operator name is: a
 * _HILDON_WM_WINDOW_TYPE_HOME_APPLET window on the first home view, at the
 * status bar's height, as wide as the view and as high as the rest of it.
 * hildon-desktop gives it the drags up and down
 * (_HILDON_MEEGO_SCROLLING_APPLET, patches/meego/hildon-desktop.patch); the
 * drags across still change the page, a long press edits Home.
 *
 * Mastodon: GET <server>/api/v1/timelines/home with the account's access
 * token (GConf /apps/pro1x/events/mastodon_server and mastodon_token,
 * Settings, Events: apps/pro1x-events), in a thread with libcurl; the
 * avatars are cached in ~/.cache/pro1x-events. A tap opens the post in the
 * browser, a notification its application (through hildon-desktop's
 * launcher), Clear marks the notifications read.
 *
 * Weather: Open-Meteo's forecast API (open-meteo.com: no key, free for
 * non-commercial use, its data CC BY 4.0, credited below it), for the
 * place of GConf /apps/pro1x/events/weather_place, weather_latitude and
 * weather_longitude (the settings applet finds them with Open-Meteo's
 * geocoding), every 30 minutes while the page is seen, the last answer
 * kept in ~/.cache/pro1x-events for the time without a connection;
 * weather_url takes another Open-Meteo server (it is free software). The
 * icons are MeeCast's set (weather_icons, default "Meecast") where MeeCast
 * is installed, else drawn here. A tap opens a weather application, on the
 * credit line Open-Meteo's site.
 *
 * Calendar: the appointments of calendar-backend, the library of Hildon's
 * calendar qalendar, through its API (calendar.cpp), from today on for
 * calendar_days days (default 7), updated on its dbChange signal; a tap
 * opens the appointment in qalendar.
 *
 * Sections: show_notifications, show_weather, show_calendar, show_mastodon
 * (all on by default).
 *
 * Sizes and colours as the N9's Events view (its home screen's style,
 * meegotouchhome.css, in the N9's millimetres of 10 of its pixels, which
 * HILDON_SCALE makes ours, as for the status bar): the day in light 4 mm,
 * the date in light 3.2 mm grey, the items' titles bold 2.2 mm, their text
 * 2.2 mm (5 lines at most), their time 1.6 mm grey, icons 6.4 mm, on black.
 * The N9's font where it is installed. No Nokia graphics.
 */
#include <libintl.h>
#include <locale.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gio/gio.h>
#include <gconf/gconf-client.h>
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <rtcom-eventlogger/eventlogger.h>
#include <X11/Xatom.h>
#ifndef PRO1X_NO_CALENDAR
#include "calendar.h"
#endif

#define DOMAIN "pro1x-events"
#define _(s) dgettext(DOMAIN, s)
#define N_(s) (s)
#ifndef LOCALEDIR
#define LOCALEDIR "/usr/share/locale"
#endif

#define KEY_MEEGO "/apps/osso/hildon-desktop/meego_pages"
#define KEY_DIR "/apps/pro1x/events"
#define KEY_SERVER KEY_DIR "/mastodon_server"
#define KEY_TOKEN KEY_DIR "/mastodon_token"
#define KEY_CLEARED KEY_DIR "/mail_cleared"
#define KEY_SHOW_NOTES KEY_DIR "/show_notifications"
#define KEY_SHOW_WEATHER KEY_DIR "/show_weather"
#define KEY_SHOW_CALENDAR KEY_DIR "/show_calendar"
#define KEY_SHOW_FEED KEY_DIR "/show_mastodon"
#define KEY_PLACE KEY_DIR "/weather_place"
#define KEY_LAT KEY_DIR "/weather_latitude"
#define KEY_LON KEY_DIR "/weather_longitude"
#define KEY_WEATHER_URL KEY_DIR "/weather_url"
#define KEY_WEATHER_ICONS KEY_DIR "/weather_icons"
#define KEY_CAL_DAYS KEY_DIR "/calendar_days"
#define APPLET_ID "pro1x-events"
#define APPLET_KEY "/apps/osso/hildon-desktop/applets/" APPLET_ID

#define FEED_LIMIT 30		/* posts (the API gives 40 at most) */
#define FEED_EVERY (15 * 60)	/* s between updates while seen */
#define MAIL_PER_ACCOUNT 3
#define AVATAR_MAX (2 << 20)	/* bytes */
#define AVATAR_DAYS 7
#define WEATHER_URL "https://api.open-meteo.com/v1/forecast"
#define WEATHER_SITE "https://open-meteo.com/"
#define WEATHER_EVERY (30 * 60)	/* s between updates while seen */
#define WEATHER_RETRY (5 * 60)	/* after one that failed */
#define WEATHER_DAYS 5		/* today and four more */
#define CAL_DAYS 7
#define CAL_LIMIT 12		/* appointments at most */

/* the N9's pixels to ours (HILDON_SCALE, as hildon-desktop's), its
 * millimetre (10 of its pixels) */
static double scale = 1;
#define PX(n9) ((n9) * scale)
#define MM(n9mm) ((n9mm) * 10 * scale)
static int bar_height;		/* the status bar: 36 of the N9's pixels */

static GConfClient *gconf;
static GtkWidget *win;
static int width, height;	/* of the page */
static gboolean look_on, closed;
static RTComEl *el;
static GDBusConnection *bus;

/* --- the items ---------------------------------------------------------- */

enum kind { K_DATE, K_SECTION, K_NOTE, K_FEED, K_HINT, K_WEATHER, K_CAL };
enum action { A_NONE, A_APP, A_URL, A_SETTINGS, A_CALENDAR, A_WEATHER };

typedef struct {
	int kind, action;
	char *title, *body, *footer;
	GdkPixbuf *icon;
	gint64 time;
	char *target;		/* application ids (space separated), a URL or an
				 * appointment's id */
	GArray *events;		/* rtcom-eventlogger's ids of a notification */
	gboolean clear;		/* a section with the Clear button */
	int colour, cal_id, cal_kind;	/* an appointment's calendar, kind */
	/* laid out */
	int y, h;
	PangoLayout *lt, *lb, *lf;
	/* the weather's: the temperature, then each day's name and degrees */
	PangoLayout *lx[1 + 2 * WEATHER_DAYS];
	int wtemp_x, wtext_x, wtext_y, wday_x, wday_y, wday_w, wdays, wlink_y;
} Item;

static GPtrArray *notes;	/* calls and messages (rtcom-eventlogger) */
static GPtrArray *mails;	/* unread e-mail (modest) */
static GPtrArray *feed;		/* the Mastodon posts */
static char *feed_hint;		/* instead of them: why there are none */
static gboolean feed_hint_settings;	/* a tap on the hint opens the settings */
static GPtrArray *items;	/* what the page shows, in its order */
static GPtrArray *chrome;	/* the date and the sections (owned) */
static GPtrArray *appointments;	/* the calendar's */
static int content_height;

/* the sections shown (Settings, Events) */
static gboolean show_notes = TRUE, show_weather = TRUE, show_calendar = TRUE,
		show_feed = TRUE;
static int cal_days = CAL_DAYS;

/* the weather, as Open-Meteo answered */
typedef struct {
	double temp, feels, wind;
	int code, is_day;
	int days;
	struct {
		int year, month, mday;
		int code, pop;	/* pop: the chance of rain in %, -1 unknown */
		double tmax, tmin;
	} day[WEATHER_DAYS];
	gint64 fetched;		/* Unix time */
} Weather;

static Weather *weather;
static char *weather_hint;	/* why it is not (or not newly) there */
static gboolean weather_busy;
static gint64 weather_next;	/* monotonic time of the next update */
static char *place_name;
static double place_lat, place_lon;

static void item_free(gpointer p)
{
	Item *it = p;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(it->lx); i++)
		if (it->lx[i])
			g_object_unref(it->lx[i]);
	g_free(it->title);
	g_free(it->body);
	g_free(it->footer);
	g_free(it->target);
	if (it->icon)
		g_object_unref(it->icon);
	if (it->events)
		g_array_free(it->events, TRUE);
	if (it->lt)
		g_object_unref(it->lt);
	if (it->lb)
		g_object_unref(it->lb);
	if (it->lf)
		g_object_unref(it->lf);
	g_free(it);
}

static Item *item_new(int kind, const char *title, const char *body)
{
	Item *it = g_new0(Item, 1);

	it->kind = kind;
	it->title = g_strdup(title);
	it->body = g_strdup(body);
	return it;
}

/* --- scrolling state ---------------------------------------------------- */

static double offset;		/* scrolled down by, in pixels */
static double pull;		/* pulled down at the top by */
static gboolean pressed, dragging, refreshing;
static double press_y, press_cy, press_offset, last_y;
static gint64 last_t;
static double velocity;		/* px/s, of the content upwards */
static guint anim_id;
static Item *pressed_item;
static gboolean pressed_clear;
static gint64 moved_at;		/* the scroll indicator's last movement */
static double spin;		/* the update spinner's angle */
static gint64 feed_time;	/* the last update of the feed */
static gboolean feed_busy;

#define TAP_SLOP MM(1.0)
#define PULL_UPDATE MM(8.0)	/* pulled this far, it updates */

static double max_offset(void)
{
	return MAX(0, content_height - height);
}

static void redraw(void)
{
	if (win && GTK_WIDGET_REALIZED(win))
		gtk_widget_queue_draw(win);
}

/* --- fonts and layout --------------------------------------------------- */

static PangoFontDescription *font(gboolean light, gboolean bold, double px)
{
	PangoFontDescription *d = pango_font_description_from_string(
	    light ? "Nokia Pure Text Light,Nokia Pure Text,Sans" : "Nokia Pure Text,Sans");

	if (light && !bold)
		pango_font_description_set_weight(d, PANGO_WEIGHT_LIGHT);
	if (bold)
		pango_font_description_set_weight(d, PANGO_WEIGHT_BOLD);
	pango_font_description_set_absolute_size(d, px * PANGO_SCALE);
	return d;
}

static PangoLayout *layout_text(const char *text, PangoFontDescription *d,
				int w, int lines)
{
	PangoLayout *l = gtk_widget_create_pango_layout(win, NULL);

	pango_layout_set_font_description(l, d);
	pango_layout_set_text(l, text ? text : "", -1);
	pango_layout_set_width(l, w * PANGO_SCALE);
	pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
	if (lines > 0) {
		pango_layout_set_height(l, -lines);
		pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	}
	return l;
}

static int layout_h(PangoLayout *l)
{
	int w, h;

	if (!l)
		return 0;
	pango_layout_get_pixel_size(l, &w, &h);
	return h;
}

static int margin(void)
{
	return MM(1.6);
}

static int icon_size(void)
{
	return MM(6.4);
}

/* the items' x for their text */
static int text_x(const Item *it)
{
	return margin() + (it->kind == K_NOTE || it->kind == K_FEED || it->kind == K_CAL ?
			   MM(0.2) + icon_size() + MM(1.6) : 0);
}

/* the weather's sizes: its icon now, a day's icon, a day's column */
#define WICON MM(8.0)
#define WDAY_ICON MM(4.4)
#define WDAY_W MM(7.8)

static void temp_text(char *buf, gsize n, double t)
{
	long v = lround(t);

	g_snprintf(buf, n, "%ld°", v == 0 ? 0 : v);	/* (no "-0°") */
}

static void layout_weather(Item *it)
{
	PangoFontDescription *d;
	int tw, th, textw, dayh = 0, n = weather->days, i, h1, texth;
	gboolean one_row;
	char buf[64];

	d = font(TRUE, FALSE, MM(5.6));
	temp_text(buf, sizeof buf, weather->temp);
	it->lx[0] = layout_text(buf, d, MM(30), 1);
	pango_font_description_free(d);
	pango_layout_get_pixel_size(it->lx[0], &tw, &th);
	it->wtemp_x = margin() + WICON + MM(1.2);
	it->wtext_x = it->wtemp_x + tw + MM(1.8);
	textw = width - margin() - it->wtext_x;
	/* the days beside it where the page is wide enough (landscape),
	 * else below */
	one_row = n > 0 && textw - MM(3) - n * WDAY_W >= MM(24);
	if (one_row)
		textw -= MM(3) + n * WDAY_W;
	d = font(FALSE, TRUE, MM(2.2));
	it->lt = layout_text(it->title, d, textw, 1);
	pango_font_description_free(d);
	d = font(FALSE, FALSE, MM(2.2));
	it->lb = layout_text(it->body, d, textw, 2);
	pango_font_description_free(d);
	d = font(FALSE, FALSE, MM(1.6));
	it->lf = layout_text(it->footer, d, textw, 2);
	pango_font_description_free(d);
	texth = layout_h(it->lt) + layout_h(it->lb) + MM(0.4) + layout_h(it->lf);
	h1 = MAX(WICON, texth);
	it->wtext_y = MM(0.8) + (h1 - texth) / 2;
	it->wlink_y = it->wtext_y + layout_h(it->lt) + layout_h(it->lb) + MM(0.4);

	it->wdays = n;
	it->wday_w = one_row ? WDAY_W : MIN(MM(13), (width - 2 * margin()) / MAX(n, 1));
	for (i = 0; i < n; i++) {
		GDateTime *now = g_date_time_new_now_local();
		GDateTime *day = g_date_time_new_local(weather->day[i].year, weather->day[i].month,
						       weather->day[i].mday, 12, 0, 0);
		char *name, *max, *min, *m;

		if (day && g_date_time_get_year(day) == g_date_time_get_year(now) &&
		    g_date_time_get_day_of_year(day) == g_date_time_get_day_of_year(now))
			name = g_strdup(_("Today"));
		else
			name = day ? g_date_time_format(day, "%a") : g_strdup("");
		if (day)
			g_date_time_unref(day);
		g_date_time_unref(now);
		d = font(FALSE, FALSE, MM(1.8));
		it->lx[1 + 2 * i] = layout_text(name, d, it->wday_w, 1);
		pango_layout_set_alignment(it->lx[1 + 2 * i], PANGO_ALIGN_CENTER);
		g_free(name);
		temp_text(buf, sizeof buf, weather->day[i].tmax);
		max = g_markup_escape_text(buf, -1);
		temp_text(buf, sizeof buf, weather->day[i].tmin);
		min = g_markup_escape_text(buf, -1);
		m = g_strdup_printf("%s <span foreground=\"#808080\">%s</span>", max, min);
		it->lx[2 + 2 * i] = layout_text("", d, it->wday_w, 1);
		pango_layout_set_markup(it->lx[2 + 2 * i], m, -1);
		pango_layout_set_alignment(it->lx[2 + 2 * i], PANGO_ALIGN_CENTER);
		g_free(max), g_free(min), g_free(m);
		pango_font_description_free(d);
		dayh = layout_h(it->lx[1 + 2 * i]) + MM(0.3) + WDAY_ICON + MM(0.3) +
		       layout_h(it->lx[2 + 2 * i]);
	}
	if (one_row) {
		it->wday_x = width - margin() - n * WDAY_W;
		it->wday_y = MM(0.8) + MAX(0, (h1 - dayh) / 2);
		it->h = MM(0.8) + MAX(h1, dayh) + MM(1.0);
	} else {
		it->wday_x = margin();
		it->wday_y = MM(0.8) + h1 + MM(1.0);
		it->h = it->wday_y + dayh + (n ? MM(1.0) : 0);
	}
}

static void layout_item(Item *it)
{
	PangoFontDescription *d;
	int w = width - text_x(it) - margin();
	guint i;

	if (it->lt)
		g_object_unref(it->lt);
	if (it->lb)
		g_object_unref(it->lb);
	if (it->lf)
		g_object_unref(it->lf);
	it->lt = it->lb = it->lf = NULL;
	for (i = 0; i < G_N_ELEMENTS(it->lx); i++)
		if (it->lx[i]) {
			g_object_unref(it->lx[i]);
			it->lx[i] = NULL;
		}
	switch (it->kind) {
	case K_WEATHER:
		layout_weather(it);
		break;
	case K_DATE:
		d = font(TRUE, FALSE, MM(4.0));
		it->lt = layout_text(it->title, d, w, 1);
		pango_font_description_free(d);
		d = font(TRUE, FALSE, MM(3.2));
		it->lb = layout_text(it->body, d, w, 1);
		pango_font_description_free(d);
		it->h = MM(1.2) + layout_h(it->lt) + layout_h(it->lb) + MM(0.8);
		break;
	case K_SECTION:
		d = font(FALSE, TRUE, MM(2.4));
		it->lt = layout_text(it->title, d, w - MM(16) - MM(1.4), 1);
		pango_font_description_free(d);
		it->h = MM(1.2) + MAX(layout_h(it->lt), MM(3.1)) + MM(1.2);
		break;
	case K_HINT:
		d = font(FALSE, FALSE, MM(2.2));
		it->lt = layout_text(it->title, d, w, 0);
		pango_font_description_free(d);
		it->h = MM(0.8) + layout_h(it->lt) + MM(1.6);
		break;
	default:
		d = font(FALSE, TRUE, MM(2.2));
		it->lt = layout_text(it->title, d, w, 1);
		pango_font_description_free(d);
		d = font(FALSE, FALSE, MM(2.2));
		if (it->body && *it->body)
			it->lb = layout_text(it->body, d, w, 5);
		pango_font_description_free(d);
		d = font(FALSE, FALSE, MM(1.6));
		if (it->footer && *it->footer)
			it->lf = layout_text(it->footer, d, w, 1);
		pango_font_description_free(d);
		it->h = MM(0.8) + layout_h(it->lt) + layout_h(it->lb) +
			(it->lf ? MM(0.4) + layout_h(it->lf) : 0) + MM(0.8);
		it->h = MAX(it->h, MM(0.8) + icon_size() + MM(0.8));
		break;
	}
}

static void layout_all(void)
{
	guint i;
	int y = 0;

	if (!win || !items)
		return;
	for (i = 0; i < items->len; i++) {
		Item *it = g_ptr_array_index(items, i);

		layout_item(it);
		it->y = y;
		y += it->h;
	}
	content_height = y + MM(2.4);
	offset = CLAMP(offset, 0, max_offset());
}

/* --- time --------------------------------------------------------------- */

/* 12:34 today, else Yesterday, else 29 Oct */
static char *when(gint64 t)
{
	GDateTime *now = g_date_time_new_now_local();
	GDateTime *d = g_date_time_new_from_unix_local(t);
	char *s;
	int days;

	if (!d) {
		g_date_time_unref(now);
		return g_strdup("");
	}
	days = g_date_time_get_day_of_year(now) - g_date_time_get_day_of_year(d) +
	       (g_date_time_get_year(now) - g_date_time_get_year(d)) * 365;
	if (days <= 0)
		s = g_date_time_format(d, "%H:%M");
	else if (days == 1)
		s = g_strdup(_("Yesterday"));
	else {
		s = g_date_time_format(d, "%e %b");
		g_strstrip(s);
	}
	g_date_time_unref(d);
	g_date_time_unref(now);
	return s;
}

/* --- icons -------------------------------------------------------------- */

/* the N9's icons where its theme (blanco, Nokia's, not part of this) is
 * installed, else Hildon's */
static const char *n9_dirs[] = {
	"/usr/share/themes/blanco/meegotouch/icons",
	"/usr/local/share/themes/blanco/meegotouch/icons",
};

static GdkPixbuf *load_icon(const char *n9, const char *const *hildon)
{
	GtkIconTheme *t = gtk_icon_theme_get_default();
	int s = icon_size();
	guint i;

	for (i = 0; n9 && i < G_N_ELEMENTS(n9_dirs); i++) {
		char *p = g_strdup_printf("%s/%s.png", n9_dirs[i], n9);
		GdkPixbuf *b = gdk_pixbuf_new_from_file_at_size(p, s, s, NULL);

		g_free(p);
		if (b)
			return b;
	}
	for (; hildon && *hildon; hildon++) {
		GdkPixbuf *b = gtk_icon_theme_load_icon(t, *hildon, s, 0, NULL);

		if (b) {
			if (gdk_pixbuf_get_width(b) != s) {
				GdkPixbuf *c = gdk_pixbuf_scale_simple(b, s, s, GDK_INTERP_BILINEAR);

				g_object_unref(b);
				b = c;
			}
			return b;
		}
	}
	return NULL;
}

/* --- notifications: rtcom-eventlogger ------------------------------------ */

typedef struct {
	char *service, *remote, *name, *text;
	int count;
	gint64 time;
	GArray *ids;
} Group;

static const char *const call_apps = "sphone-dialer rtcom-call-ui";
static const char *const sms_apps = "conversations sphone-sms-history rtcom-messaging-ui";

static void notes_reload(void)
{
	static const char *services[] = {
		"RTCOM_EL_SERVICE_CALL", "RTCOM_EL_SERVICE_SMS", "RTCOM_EL_SERVICE_CHAT", NULL,
	};
	RTComElQuery *q;
	RTComElIter *iter;
	GHashTable *groups;
	GList *list, *l;

	g_ptr_array_set_size(notes, 0);
	if (!el)
		return;
	q = rtcom_el_query_new(el);
	rtcom_el_query_set_limit(q, 300);
	if (!rtcom_el_query_prepare(q, "is-read", GINT_TO_POINTER(FALSE), RTCOM_EL_OP_EQUAL,
				    "outgoing", GINT_TO_POINTER(FALSE), RTCOM_EL_OP_EQUAL,
				    "service", services, RTCOM_EL_OP_IN_STRV, NULL)) {
		g_object_unref(q);
		return;
	}
	iter = rtcom_el_get_events(el, q);
	g_object_unref(q);
	groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	if (iter && rtcom_el_iter_first(iter)) {
		do {
			char *service = NULL, *type = NULL, *ruid = NULL, *rname = NULL, *text = NULL;
			char *key;
			gint id = 0;
			time_t start = 0;
			Group *g;

			if (!rtcom_el_iter_get_values(iter, "id", &id, "service", &service,
						      "event-type", &type, "remote-uid", &ruid,
						      "remote-name", &rname, "free-text", &text,
						      "start-time", &start, NULL))
				continue;
			/* the calls missed, the messages (no joins, topics) */
			if ((!strcmp(service, "RTCOM_EL_SERVICE_CALL") &&
			     g_strcmp0(type, "RTCOM_EL_EVENTTYPE_CALL_MISSED")) ||
			    (strcmp(service, "RTCOM_EL_SERVICE_CALL") &&
			     !g_str_has_suffix(type ? type : "", "_MESSAGE"))) {
				g_free(service), g_free(type), g_free(ruid), g_free(rname), g_free(text);
				continue;
			}
			key = g_strconcat(service, "|", ruid ? ruid : "", NULL);
			g = g_hash_table_lookup(groups, key);
			if (!g) {
				g = g_new0(Group, 1);
				g->service = service;
				g->remote = ruid;
				g->name = rname;
				g->text = text;	/* the newest first */
				g->time = start;
				g->ids = g_array_new(FALSE, FALSE, sizeof(gint));
				g_hash_table_insert(groups, key, g);
				service = ruid = rname = text = NULL;
			} else
				g_free(key);
			g->count++;
			g_array_append_val(g->ids, id);
			g_free(service), g_free(type), g_free(ruid), g_free(rname), g_free(text);
		} while (rtcom_el_iter_next(iter));
	}
	if (iter)
		g_object_unref(iter);

	list = g_hash_table_get_values(groups);
	for (l = list; l; l = l->next) {
		static const char *const call_icons[] = { "general_missed", "general_call", NULL };
		static const char *const sms_icons[] = { "general_sms", "general_chat", NULL };
		static const char *const chat_icons[] = { "general_chat", "general_sms", NULL };
		Group *g = l->data;
		gboolean call = !strcmp(g->service, "RTCOM_EL_SERVICE_CALL");
		gboolean sms = !strcmp(g->service, "RTCOM_EL_SERVICE_SMS");
		const char *who = g->name && *g->name ? g->name :
				  g->remote && *g->remote ? g->remote : _("Unknown");
		Item *it;
		char *body;

		if (call)
			body = g_strdup_printf(dngettext(DOMAIN, "Missed call", "%d missed calls",
							 g->count), g->count);
		else if (g->count > 1)
			body = g_strdup_printf(_("%d messages: %s"), g->count, g->text ? g->text : "");
		else
			body = g_strdup(g->text ? g->text : "");
		it = item_new(K_NOTE, who, body);
		g_free(body);
		it->time = g->time;
		it->footer = when(g->time);
		/* the applications' icons, as the N9's notifications have them */
		it->icon = load_icon(call ? "icon-l-telephony" : "icon-l-conversation",
				     call ? call_icons : sms ? sms_icons : chat_icons);
		it->action = A_APP;
		it->target = g_strdup(call ? call_apps : sms_apps);
		it->events = g->ids;
		g_ptr_array_add(notes, it);
		g_free(g->service), g_free(g->remote), g_free(g->name), g_free(g->text);
		g_free(g);
	}
	g_list_free(list);
	g_hash_table_destroy(groups);
}

/* --- notifications: modest ---------------------------------------------- */

static void rebuild(void);
static void calendar_reload(void);
static void busy_check(void);
static void weather_update(void);

static void mails_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
	GVariantIter *accounts, *hits;
	const char *id, *name, *proto, *subject;
	gint64 unread, ts, cleared;
	static const char *const mail_icons[] = { "general_email", "general_mail", NULL };

	(void)data;
	g_ptr_array_set_size(mails, 0);
	if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE("(a(sssxa(xs)))"))) {
		if (v)
			g_variant_unref(v);
		rebuild();
		return;
	}
	cleared = gconf_client_get_int(gconf, KEY_CLEARED, NULL);
	g_variant_get(v, "(a(sssxa(xs)))", &accounts);
	while (g_variant_iter_loop(accounts, "(&s&s&sxa(xs))", &id, &name, &proto,
				   &unread, &hits)) {
		GString *subjects = g_string_new(NULL);
		gint64 newest = 0;
		Item *it;
		char *n, *f;

		while (g_variant_iter_loop(hits, "(x&s)", &ts, &subject)) {
			if (subjects->len)
				g_string_append_c(subjects, '\n');
			g_string_append(subjects, subject && *subject ? subject : _("(no subject)"));
			newest = MAX(newest, ts);
		}
		if (unread <= 0 || newest <= cleared) {
			g_string_free(subjects, TRUE);
			continue;
		}
		it = item_new(K_NOTE, name && *name ? name : _("E-mail"), subjects->str);
		g_string_free(subjects, TRUE);
		it->time = newest;
		n = g_strdup_printf(dngettext(DOMAIN, "%d unread", "%d unread", unread), (int)unread);
		f = when(newest);
		it->footer = g_strconcat(f, " • ", n, NULL);
		g_free(n), g_free(f);
		it->icon = load_icon("icon-l-email", mail_icons);
		it->action = A_APP;
		it->target = g_strdup("modest");
		g_ptr_array_add(mails, it);
	}
	g_variant_iter_free(accounts);
	g_variant_unref(v);
	rebuild();
}

/* only while modest runs (its call would start it, and it answers late) */
static void mails_owner_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
	gboolean owned = FALSE;

	(void)data;
	if (v) {
		g_variant_get(v, "(b)", &owned);
		g_variant_unref(v);
	}
	if (!owned) {
		if (mails->len) {
			g_ptr_array_set_size(mails, 0);
			rebuild();
		}
		return;
	}
	g_dbus_connection_call(bus, "com.nokia.modest", "/com/nokia/modest", "com.nokia.modest",
			       "GetUnreadMessages", g_variant_new("(i)", MAIL_PER_ACCOUNT),
			       NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 30000, NULL, mails_cb, NULL);
}

static void mails_reload(void)
{
	if (!bus)
		return;
	g_dbus_connection_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			       "org.freedesktop.DBus", "NameHasOwner",
			       g_variant_new("(s)", "com.nokia.modest"), G_VARIANT_TYPE("(b)"),
			       G_DBUS_CALL_FLAGS_NONE, 5000, NULL, mails_owner_cb, NULL);
}

/* changes come in bursts: one reload after them */
static guint reload_id;

static gboolean reload_cb(gpointer data)
{
	(void)data;
	reload_id = 0;
	notes_reload();
	mails_reload();
	calendar_reload();
	rebuild();
	return FALSE;
}

static void reload_soon(void)
{
	if (!reload_id)
		reload_id = g_timeout_add(500, reload_cb, NULL);
}

static void el_changed(gpointer data)
{
	(void)data;
	reload_soon();
}

static void modest_signal(GDBusConnection *c, const char *sender, const char *path,
			  const char *iface, const char *signal, GVariant *p, gpointer data)
{
	(void)c, (void)sender, (void)path, (void)iface, (void)signal, (void)p, (void)data;
	reload_soon();
}

static void modest_owner(GDBusConnection *c, const char *name, const char *owner,
			 gpointer data)
{
	(void)c, (void)name, (void)owner, (void)data;
	reload_soon();
}

/* --- Mastodon ----------------------------------------------------------- */

typedef struct {
	char *server, *token, *cache;
	int icon;
	GPtrArray *items;	/* Item, with their icons */
	char *error;
	gboolean settings;	/* the error is the settings' */
} Fetch;

static size_t curl_write(void *p, size_t size, size_t n, void *data)
{
	GByteArray *b = data;

	if (b->len + size * n > 8 << 20)
		return 0;
	g_byte_array_append(b, p, size * n);
	return size * n;
}

/* the body of a GET (NULL on no answer), its HTTP status */
static GByteArray *http_get(const char *url, const char *token, long *status, char **error)
{
	CURL *c = curl_easy_init();
	GByteArray *b = g_byte_array_new();
	struct curl_slist *h = NULL;
	char errbuf[CURL_ERROR_SIZE] = "";
	CURLcode r;

	*status = 0;
	if (!c) {
		g_byte_array_free(b, TRUE);
		return NULL;
	}
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 4L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(c, CURLOPT_USERAGENT, "pro1x-events/1.0");
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
	if (token) {
		char *a = g_strconcat("Authorization: Bearer ", token, NULL);

		h = curl_slist_append(h, a);
		h = curl_slist_append(h, "Accept: application/json");
		g_free(a);
		curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	}
	r = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
	curl_easy_cleanup(c);
	curl_slist_free_all(h);
	if (r != CURLE_OK) {
		if (error)
			*error = g_strdup(*errbuf ? errbuf : curl_easy_strerror(r));
		g_byte_array_free(b, TRUE);
		return NULL;
	}
	return b;
}

static const char *jstr(JsonObject *o, const char *name)
{
	JsonNode *n;

	if (!o || !json_object_has_member(o, name))
		return NULL;
	n = json_object_get_member(o, name);
	if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE ||
	    json_node_get_value_type(n) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(n);
}

static JsonObject *jobj(JsonObject *o, const char *name)
{
	JsonNode *n;

	if (!o || !json_object_has_member(o, name))
		return NULL;
	n = json_object_get_member(o, name);
	return n && JSON_NODE_TYPE(n) == JSON_NODE_OBJECT ? json_node_get_object(n) : NULL;
}

/* A post's HTML as text: paragraphs and line breaks kept, links' hidden
 * parts (Mastodon's class="invisible") left out, "ellipsis" ones ended
 * with an ellipsis, entities decoded */
static char *html_text(const char *html)
{
	GString *s = g_string_new(NULL);
	const char *p = html ? html : "";
	int hide = 0, ellipsis = 0;

	while (*p) {
		if (*p == '<') {
			const char *e = strchr(p, '>');
			char *tag;

			if (!e)
				break;
			tag = g_ascii_strdown(p + 1, e - p - 1);
			if (!strncmp(tag, "br", 2))
				g_string_append_c(s, '\n');
			else if (!strcmp(tag, "/p"))
				g_string_append(s, "\n\n");
			else if (!strncmp(tag, "span", 4) && strstr(tag, "invisible"))
				hide++;
			else if (!strncmp(tag, "span", 4) && strstr(tag, "ellipsis"))
				ellipsis++;
			else if (!strcmp(tag, "/span")) {
				if (hide)
					hide--;
				else if (ellipsis) {
					ellipsis--;
					g_string_append(s, "…");
				}
			}
			g_free(tag);
			p = e + 1;
			continue;
		}
		if (hide) {
			p++;
			continue;
		}
		if (*p == '&') {
			const char *e = strchr(p, ';');
			static const struct { const char *name, *text; } ents[] = {
				{ "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" },
				{ "apos", "'" }, { "nbsp", " " },
			};
			gboolean done = FALSE;
			guint i;

			if (e && e - p < 10) {
				char *name = g_strndup(p + 1, e - p - 1);

				if (name[0] == '#') {
					gunichar c = name[1] == 'x' || name[1] == 'X' ?
						     g_ascii_strtoull(name + 2, NULL, 16) :
						     g_ascii_strtoull(name + 1, NULL, 10);

					if (c && g_unichar_validate(c)) {
						g_string_append_unichar(s, c);
						done = TRUE;
					}
				}
				for (i = 0; !done && i < G_N_ELEMENTS(ents); i++)
					if (!strcmp(name, ents[i].name)) {
						g_string_append(s, ents[i].text);
						done = TRUE;
					}
				g_free(name);
			}
			if (done) {
				p = e + 1;
				continue;
			}
		}
		g_string_append_c(s, *p++);
	}
	while (s->len && (s->str[s->len - 1] == '\n' || s->str[s->len - 1] == ' '))
		g_string_truncate(s, s->len - 1);
	return g_string_free(s, FALSE);
}

/* An avatar from the cache (fetched anew after AVATAR_DAYS), rounded */
static GdkPixbuf *avatar(Fetch *f, const char *url)
{
	char *sum, *path;
	GStatBuf st;
	GdkPixbuf *p = NULL;

	if (!url || !g_str_has_prefix(url, "http"))
		return NULL;
	sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, url, -1);
	path = g_build_filename(f->cache, sum, NULL);
	g_free(sum);
	if (g_stat(path, &st) || time(NULL) - st.st_mtime > AVATAR_DAYS * 86400) {
		long status;
		GByteArray *b = http_get(url, NULL, &status, NULL);

		if (b && status == 200 && b->len < AVATAR_MAX)
			g_file_set_contents(path, (char *)b->data, b->len, NULL);
		if (b)
			g_byte_array_free(b, TRUE);
	}
	p = gdk_pixbuf_new_from_file_at_scale(path, f->icon, f->icon, FALSE, NULL);
	g_free(path);
	return p;
}

static gint by_time(gconstpointer a, gconstpointer b)
{
	const Item *x = *(Item *const *)a, *y = *(Item *const *)b;

	return x->time < y->time ? 1 : x->time > y->time ? -1 : 0;
}

static void parse_feed(Fetch *f, const char *data, gsize len)
{
	JsonParser *jp = json_parser_new();
	JsonNode *root;
	JsonArray *a;
	GHashTable *icons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	guint i;

	if (!json_parser_load_from_data(jp, data, len, NULL) ||
	    !(root = json_parser_get_root(jp)) || JSON_NODE_TYPE(root) != JSON_NODE_ARRAY) {
		f->error = g_strdup(_("Mastodon's answer was not understood."));
		g_object_unref(jp);
		g_hash_table_destroy(icons);
		return;
	}
	a = json_node_get_array(root);
	for (i = 0; i < json_array_get_length(a); i++) {
		JsonObject *st = json_array_get_object_element(a, i);
		JsonObject *shown = jobj(st, "reblog") ? jobj(st, "reblog") : st;
		JsonObject *acc = jobj(shown, "account"), *booster = shown != st ? jobj(st, "account") : NULL;
		const char *name = jstr(acc, "display_name"), *acct = jstr(acc, "acct");
		const char *pic = jstr(acc, "avatar_static"), *created = jstr(shown, "created_at");
		const char *spoiler = jstr(shown, "spoiler_text");
		const char *url = jstr(shown, "url") ? jstr(shown, "url") : jstr(shown, "uri");
		JsonNode *media = json_object_has_member(shown, "media_attachments") ?
				  json_object_get_member(shown, "media_attachments") : NULL;
		guint nmedia = media && JSON_NODE_TYPE(media) == JSON_NODE_ARRAY ?
			       json_array_get_length(json_node_get_array(media)) : 0;
		GDateTime *d = created ? g_date_time_new_from_iso8601(created, NULL) : NULL;
		GString *footer = g_string_new(NULL);
		char *text, *w;
		Item *it;

		if (!acc)
			continue;
		if (spoiler && *spoiler)
			text = g_strconcat(spoiler, " …", NULL);
		else
			text = html_text(jstr(shown, "content"));
		it = item_new(K_FEED, name && *name ? name : acct ? acct : "", text);
		g_free(text);
		it->time = d ? g_date_time_to_unix(d) : 0;
		if (d)
			g_date_time_unref(d);
		w = when(it->time);
		g_string_append(footer, w);
		g_free(w);
		if (acct)
			g_string_append_printf(footer, " • @%s", acct);
		if (booster) {
			const char *b = jstr(booster, "display_name");

			g_string_append(footer, " • ");
			g_string_append_printf(footer, _("boosted by %s"),
					       b && *b ? b : jstr(booster, "acct") ? jstr(booster, "acct") : "");
		}
		if (nmedia) {
			g_string_append(footer, " • ");
			g_string_append_printf(footer, dngettext(DOMAIN, "%u attachment",
								 "%u attachments", nmedia), nmedia);
		}
		it->footer = g_string_free(footer, FALSE);
		if (pic) {
			GdkPixbuf *p = g_hash_table_lookup(icons, pic);

			if (!p && (p = avatar(f, pic)))
				g_hash_table_insert(icons, g_strdup(pic), p);
			if (p)
				it->icon = g_object_ref(p);
		}
		if (url && g_str_has_prefix(url, "http")) {
			it->action = A_URL;
			it->target = g_strdup(url);
		}
		g_ptr_array_add(f->items, it);
	}
	g_ptr_array_sort(f->items, by_time);
	g_hash_table_destroy(icons);
	g_object_unref(jp);
}

static gboolean fetch_done(gpointer data)
{
	Fetch *f = data;

	feed_busy = FALSE;
	busy_check();
	feed_time = g_get_monotonic_time();
	g_free(feed_hint);
	feed_hint = f->error;
	feed_hint_settings = f->settings;
	if (!f->error) {
		g_ptr_array_unref(feed);
		feed = f->items;
		f->items = NULL;
		if (!feed->len)
			feed_hint = g_strdup(_("Nothing new in the home timeline."));
	}
	if (f->items)
		g_ptr_array_unref(f->items);
	g_free(f->server), g_free(f->token), g_free(f->cache);
	g_free(f);
	notes_reload();
	mails_reload();
	rebuild();
	return FALSE;
}

static gpointer fetch_thread(gpointer data)
{
	Fetch *f = data;
	char *url = g_strdup_printf("%s/api/v1/timelines/home?limit=%d", f->server, FEED_LIMIT);
	char *error = NULL;
	long status;
	GByteArray *b = http_get(url, f->token, &status, &error);

	if (!b) {
		f->error = g_strdup_printf(_("No connection to %s (%s)."), f->server,
					   error ? error : "?");
	} else if (status == 401 || status == 403) {
		f->error = g_strdup(_("Mastodon does not take the access token: "
				      "set it anew in Settings, Events."));
		f->settings = TRUE;
	} else if (status != 200 && status != 206) {
		f->error = g_strdup_printf(_("%s answered %ld."), f->server, status);
	} else
		parse_feed(f, (const char *)b->data, b->len);
	if (b)
		g_byte_array_free(b, TRUE);
	g_free(error);
	g_free(url);
	/* (at the timers' priority: the spinner's frames and their redraws
	 * keep a slow machine busy, an idle callback would wait for ever) */
	g_idle_add_full(G_PRIORITY_DEFAULT, fetch_done, f, NULL);
	return NULL;
}

/* "mastodon.social" or "https://mastodon.social/" -> https://mastodon.social */
static char *server_url(const char *s)
{
	char *u, *t;

	if (!s || !*s)
		return NULL;
	t = g_strstrip(g_strdup(s));
	u = strstr(t, "://") ? g_strdup(t) : g_strconcat("https://", t, NULL);
	g_free(t);
	while (*u && u[strlen(u) - 1] == '/')
		u[strlen(u) - 1] = 0;
	return u;
}

static void feed_update(void)
{
	char *server, *token;
	Fetch *f;
	GThread *t;

	if (feed_busy || !show_feed)
		return;
	server = gconf_client_get_string(gconf, KEY_SERVER, NULL);
	token = gconf_client_get_string(gconf, KEY_TOKEN, NULL);
	if (!server || !*server || !token || !*token) {
		g_free(server), g_free(token);
		g_ptr_array_set_size(feed, 0);
		g_free(feed_hint);
		feed_hint = g_strdup(_("Add a Mastodon account in Settings, Events, "
				       "and its home timeline shows here."));
		feed_hint_settings = TRUE;
		busy_check();
		notes_reload();
		mails_reload();
		rebuild();
		return;
	}
	f = g_new0(Fetch, 1);
	f->server = server_url(server);
	f->token = g_strstrip(token);
	f->cache = g_build_filename(g_get_user_cache_dir(), "pro1x-events", NULL);
	f->icon = icon_size();
	f->items = g_ptr_array_new_with_free_func(item_free);
	g_mkdir_with_parents(f->cache, 0700);
	g_free(server);
	feed_busy = TRUE;
	t = g_thread_try_new("mastodon", fetch_thread, f, NULL);
	if (t)
		g_thread_unref(t);
	else {
		feed_busy = FALSE;
		g_ptr_array_unref(f->items);
		g_free(f->server), g_free(f->token), g_free(f->cache), g_free(f);
	}
	rebuild();
}

/* --- weather: Open-Meteo ------------------------------------------------- */

/* The WMO weather codes of Open-Meteo: their text, MeeCast's icons (the
 * weather.com numbers its sets use) by day and by night, and the glyph
 * drawn without them */
enum glyph { G_CLEAR, G_FAIR, G_PARTLY, G_CLOUDY, G_FOG, G_DRIZZLE, G_RAIN, G_SHOWER,
	     G_SLEET, G_SNOW, G_SNOWSHOWER, G_THUNDER };

static const struct wmo {
	int code;
	const char *text;
	int day, night, glyph;
} wmo[] = {
	{ 0, N_("Clear sky"), 32, 31, G_CLEAR },
	{ 1, N_("Mainly clear"), 34, 33, G_FAIR },
	{ 2, N_("Partly cloudy"), 30, 29, G_PARTLY },
	{ 3, N_("Overcast"), 26, 26, G_CLOUDY },
	{ 45, N_("Fog"), 20, 20, G_FOG },
	{ 48, N_("Freezing fog"), 20, 20, G_FOG },
	{ 51, N_("Light drizzle"), 9, 9, G_DRIZZLE },
	{ 53, N_("Drizzle"), 9, 9, G_DRIZZLE },
	{ 55, N_("Dense drizzle"), 9, 9, G_DRIZZLE },
	{ 56, N_("Freezing drizzle"), 8, 8, G_SLEET },
	{ 57, N_("Freezing drizzle"), 8, 8, G_SLEET },
	{ 61, N_("Light rain"), 11, 11, G_RAIN },
	{ 63, N_("Rain"), 12, 12, G_RAIN },
	{ 65, N_("Heavy rain"), 12, 12, G_RAIN },
	{ 66, N_("Freezing rain"), 10, 10, G_SLEET },
	{ 67, N_("Freezing rain"), 10, 10, G_SLEET },
	{ 71, N_("Light snow"), 14, 14, G_SNOW },
	{ 73, N_("Snow"), 16, 16, G_SNOW },
	{ 75, N_("Heavy snow"), 43, 43, G_SNOW },
	{ 77, N_("Snow grains"), 13, 13, G_SNOW },
	{ 80, N_("Light showers"), 39, 45, G_SHOWER },
	{ 81, N_("Showers"), 39, 45, G_SHOWER },
	{ 82, N_("Heavy showers"), 39, 45, G_SHOWER },
	{ 85, N_("Snow showers"), 41, 46, G_SNOWSHOWER },
	{ 86, N_("Heavy snow showers"), 41, 46, G_SNOWSHOWER },
	{ 95, N_("Thunderstorm"), 4, 4, G_THUNDER },
	{ 96, N_("Thunderstorm with hail"), 3, 3, G_THUNDER },
	{ 99, N_("Thunderstorm with hail"), 3, 3, G_THUNDER },
};

static const struct wmo *wmo_find(int code)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(wmo); i++)
		if (wmo[i].code == code)
			return &wmo[i];
	return NULL;
}

static double jnum(JsonNode *n, double def)
{
	GType t;

	if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE)
		return def;
	t = json_node_get_value_type(n);
	if (t == G_TYPE_DOUBLE || t == G_TYPE_FLOAT)
		return json_node_get_double(n);
	if (t == G_TYPE_INT64 || t == G_TYPE_INT)
		return json_node_get_int(n);
	return def;
}

static double jmember(JsonObject *o, const char *name, double def)
{
	return o && json_object_has_member(o, name) ?
	       jnum(json_object_get_member(o, name), def) : def;
}

static JsonArray *jarray(JsonObject *o, const char *name)
{
	JsonNode *n;

	if (!o || !json_object_has_member(o, name))
		return NULL;
	n = json_object_get_member(o, name);
	return n && JSON_NODE_TYPE(n) == JSON_NODE_ARRAY ? json_node_get_array(n) : NULL;
}

static double jat(JsonArray *a, guint i, double def)
{
	return a && i < json_array_get_length(a) ? jnum(json_array_get_element(a, i), def) : def;
}

/* Open-Meteo's answer (current=..., daily=...), NULL if it is none */
static Weather *parse_weather(const char *data, gsize len)
{
	JsonParser *jp = json_parser_new();
	JsonNode *root;
	JsonObject *o, *cur, *daily;
	JsonArray *days, *codes, *tmax, *tmin, *pop;
	Weather *w = NULL;
	guint i;

	if (!json_parser_load_from_data(jp, data, len, NULL) ||
	    !(root = json_parser_get_root(jp)) || JSON_NODE_TYPE(root) != JSON_NODE_OBJECT)
		goto out;
	o = json_node_get_object(root);
	cur = jobj(o, "current");
	if (!cur || jmember(cur, "temperature_2m", G_MAXDOUBLE) == G_MAXDOUBLE)
		goto out;
	w = g_new0(Weather, 1);
	w->temp = jmember(cur, "temperature_2m", 0);
	w->feels = jmember(cur, "apparent_temperature", G_MAXDOUBLE);
	w->wind = jmember(cur, "wind_speed_10m", -1);
	w->code = (int)jmember(cur, "weather_code", -1);
	w->is_day = (int)jmember(cur, "is_day", 1);
	daily = jobj(o, "daily");
	days = jarray(daily, "time");
	codes = jarray(daily, "weather_code");
	tmax = jarray(daily, "temperature_2m_max");
	tmin = jarray(daily, "temperature_2m_min");
	pop = jarray(daily, "precipitation_probability_max");
	for (i = 0; days && i < json_array_get_length(days) && w->days < WEATHER_DAYS; i++) {
		const char *s = json_array_get_string_element(days, i);
		int y, m, d;

		if (!s || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3 ||
		    jat(tmax, i, G_MAXDOUBLE) == G_MAXDOUBLE || jat(tmin, i, G_MAXDOUBLE) == G_MAXDOUBLE)
			continue;
		w->day[w->days].year = y;
		w->day[w->days].month = m;
		w->day[w->days].mday = d;
		w->day[w->days].code = (int)jat(codes, i, -1);
		w->day[w->days].tmax = jat(tmax, i, 0);
		w->day[w->days].tmin = jat(tmin, i, 0);
		w->day[w->days].pop = (int)jat(pop, i, -1);
		w->days++;
	}
out:
	g_object_unref(jp);
	return w;
}

/* the days of before (a cached answer) left out */
static void weather_drop_past(Weather *w)
{
	GDateTime *now = g_date_time_new_now_local();
	int today = g_date_time_get_year(now) * 10000 + g_date_time_get_month(now) * 100 +
		    g_date_time_get_day_of_month(now);
	int i, j = 0;

	g_date_time_unref(now);
	for (i = 0; i < w->days; i++)
		if (w->day[i].year * 10000 + w->day[i].month * 100 + w->day[i].mday >= today)
			w->day[j++] = w->day[i];
	w->days = j;
}

static gboolean place_set(void)
{
	return place_name && *place_name && place_lat >= -90 && place_lat <= 90 &&
	       place_lon >= -180 && place_lon <= 180;
}

/* the cache of a place's last answer */
static char *weather_cache(void)
{
	char la[G_ASCII_DTOSTR_BUF_SIZE], lo[G_ASCII_DTOSTR_BUF_SIZE], *name, *p;

	g_ascii_formatd(la, sizeof la, "%.3f", place_lat);
	g_ascii_formatd(lo, sizeof lo, "%.3f", place_lon);
	name = g_strdup_printf("weather-%s_%s.json", la, lo);
	p = g_build_filename(g_get_user_cache_dir(), "pro1x-events", name, NULL);
	g_free(name);
	return p;
}

static void weather_load_cache(void)
{
	char *path, *data = NULL;
	gsize len;
	GStatBuf st;
	Weather *w;

	g_free(weather);
	weather = NULL;
	if (!place_set())
		return;
	path = weather_cache();
	/* (a day old, its days are gone) */
	if (!g_stat(path, &st) && time(NULL) - st.st_mtime < 2 * 86400 &&
	    g_file_get_contents(path, &data, &len, NULL) && (w = parse_weather(data, len))) {
		w->fetched = st.st_mtime;
		weather_drop_past(w);
		weather = w;
	}
	g_free(data);
	g_free(path);
}

typedef struct {
	char *url, *cache;
	double lat, lon;	/* of the place asked for */
	Weather *w;
	char *error;
} WeatherFetch;

static void busy_check(void)
{
	if (!feed_busy && !weather_busy)
		refreshing = FALSE;
}

static gboolean weather_done(gpointer data)
{
	WeatherFetch *f = data;

	weather_busy = FALSE;
	if (f->lat != place_lat || f->lon != place_lon) {
		/* another place meanwhile: its weather */
		g_free(f->w), g_free(f->error);
		g_free(f->url), g_free(f->cache), g_free(f);
		weather_next = 0;
		weather_update();
		busy_check();
		return FALSE;
	}
	busy_check();
	g_free(weather_hint);
	weather_hint = f->error;
	if (f->w) {
		g_free(weather);
		weather = f->w;
		weather_drop_past(weather);
	}
	weather_next = g_get_monotonic_time() +
		       (gint64)(f->w ? WEATHER_EVERY : WEATHER_RETRY) * G_USEC_PER_SEC;
	g_free(f->url), g_free(f->cache), g_free(f);
	rebuild();
	return FALSE;
}

static gpointer weather_thread(gpointer data)
{
	WeatherFetch *f = data;
	char *error = NULL;
	long status;
	GByteArray *b = http_get(f->url, NULL, &status, &error);

	if (!b)
		f->error = g_strdup_printf(_("No connection to Open-Meteo (%s)."), error ? error : "?");
	else if (status != 200)
		f->error = g_strdup_printf(_("Open-Meteo answered %ld."), status);
	else if (!(f->w = parse_weather((const char *)b->data, b->len)))
		f->error = g_strdup(_("Open-Meteo's answer was not understood."));
	else {
		f->w->fetched = time(NULL);
		g_file_set_contents(f->cache, (const char *)b->data, b->len, NULL);
	}
	if (b)
		g_byte_array_free(b, TRUE);
	g_free(error);
	g_idle_add_full(G_PRIORITY_DEFAULT, weather_done, f, NULL);
	return NULL;
}

static void weather_update(void)
{
	char la[G_ASCII_DTOSTR_BUF_SIZE], lo[G_ASCII_DTOSTR_BUF_SIZE], *base, *dir;
	WeatherFetch *f;
	GThread *t;

	if (weather_busy || !show_weather || !place_set())
		return;
	base = gconf_client_get_string(gconf, KEY_WEATHER_URL, NULL);
	if (base)
		g_strstrip(base);
	g_ascii_formatd(la, sizeof la, "%.4f", place_lat);
	g_ascii_formatd(lo, sizeof lo, "%.4f", place_lon);
	f = g_new0(WeatherFetch, 1);
	f->url = g_strdup_printf("%s?latitude=%s&longitude=%s"
				 "&current=temperature_2m,apparent_temperature,weather_code,"
				 "is_day,wind_speed_10m"
				 "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
				 "precipitation_probability_max"
				 "&timezone=auto&forecast_days=%d",
				 base && *base ? base : WEATHER_URL, la, lo, WEATHER_DAYS);
	g_free(base);
	f->cache = weather_cache();
	f->lat = place_lat;
	f->lon = place_lon;
	dir = g_path_get_dirname(f->cache);
	g_mkdir_with_parents(dir, 0700);
	g_free(dir);
	weather_busy = TRUE;
	t = g_thread_try_new("weather", weather_thread, f, NULL);
	if (t)
		g_thread_unref(t);
	else {
		weather_busy = FALSE;
		g_free(f->url), g_free(f->cache), g_free(f);
	}
	rebuild();
}

/* MeeCast's icon sets (GPL, not part of this; where MeeCast is installed):
 * weather_icons names one (default "Meecast") or a directory, "none"
 * draws them here */
static const char *iconset_dirs[] = {
	"/usr/local/lib/meecast/share/iconsets",	/* FreeBSD's MeeCast */
	"/opt/com.meecast.omweather/share/iconsets",	/* its own place */
	"/usr/share/omweather/icons",			/* Maemo 5's */
};
static char *iconset;		/* the directory, "" none; NULL: not looked for */
static GHashTable *weather_icons;	/* "number size" -> GdkPixbuf */

static void iconset_reset(void)
{
	g_free(iconset);
	iconset = NULL;
	if (weather_icons)
		g_hash_table_remove_all(weather_icons);
}

static const char *iconset_dir(void)
{
	char *set;
	guint i;

	if (iconset)
		return iconset;
	set = gconf_client_get_string(gconf, KEY_WEATHER_ICONS, NULL);
	if (!set)
		set = g_strdup("Meecast");
	g_strstrip(set);
	iconset = g_strdup("");
	if (*set && strcmp(set, "none")) {
		if (g_path_is_absolute(set) && g_file_test(set, G_FILE_TEST_IS_DIR)) {
			g_free(iconset);
			iconset = g_strdup(set);
		}
		for (i = 0; !*iconset && i < G_N_ELEMENTS(iconset_dirs); i++) {
			char *p = g_build_filename(iconset_dirs[i], set, "32.png", NULL);

			if (g_file_test(p, G_FILE_TEST_EXISTS)) {
				g_free(iconset);
				iconset = g_path_get_dirname(p);
			}
			g_free(p);
		}
	}
	g_free(set);
	return iconset;
}

static GdkPixbuf *iconset_icon(int number, int size)
{
	char key[32], *p;
	GdkPixbuf *b;

	if (!*iconset_dir())
		return NULL;
	if (!weather_icons)
		weather_icons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
						      g_object_unref);
	g_snprintf(key, sizeof key, "%d %d", number, size);
	if ((b = g_hash_table_lookup(weather_icons, key)))
		return b;
	p = g_strdup_printf("%s/%d.png", iconset, number);
	b = gdk_pixbuf_new_from_file_at_size(p, size, size, NULL);
	g_free(p);
	if (b)
		g_hash_table_insert(weather_icons, g_strdup(key), b);
	return b;
}

/* --- the calendar -------------------------------------------------------- */

/* "Today", "Tomorrow" or "Mon 3 Nov" */
static char *day_name(GDateTime *d)
{
	GDateTime *now = g_date_time_new_now_local();
	GDateTime *today = g_date_time_new_local(g_date_time_get_year(now), g_date_time_get_month(now),
						 g_date_time_get_day_of_month(now), 0, 0, 0);
	GTimeSpan diff = g_date_time_difference(d, today);
	char *s;

	if (diff >= 0 && diff < G_TIME_SPAN_DAY)
		s = g_strdup(_("Today"));
	else if (diff >= G_TIME_SPAN_DAY && diff < 2 * G_TIME_SPAN_DAY)
		s = g_strdup(_("Tomorrow"));
	else
		/* TRANSLATORS: g_date_time_format()'s, a day of the next week */
		s = g_date_time_format(d, _("%a %-d %b"));
	g_date_time_unref(today);
	g_date_time_unref(now);
	return s;
}

#ifndef PRO1X_NO_CALENDAR
/* When an appointment is: "Today, 14:00–15:30", "Tomorrow, all day",
 * "Now, until 15:30" */
static char *cal_when(const CalEntry *c, gint64 now)
{
	GDateTime *s = g_date_time_new_from_unix_local(c->start);
	GDateTime *e = g_date_time_new_from_unix_local(c->end);
	char *day = day_name(s), *r, *a, *b;

	if (c->kind == CAL_TODO)
		r = g_strdup_printf(_("%s, task"), day);
	else if (c->kind == CAL_BIRTHDAY)
		r = g_strdup_printf(_("%s, birthday"), day);
	else if (c->allday)
		r = g_strdup_printf(_("%s, all day"), day);
	else if (c->start <= now && now < c->end) {
		a = g_date_time_format(e, "%H:%M");
		r = g_strdup_printf(_("Now, until %s"), a);
		g_free(a);
	} else if (c->end <= c->start) {
		a = g_date_time_format(s, "%H:%M");
		r = g_strdup_printf("%s, %s", day, a);
		g_free(a);
	} else if (g_date_time_get_day_of_year(s) == g_date_time_get_day_of_year(e) &&
		   g_date_time_get_year(s) == g_date_time_get_year(e)) {
		a = g_date_time_format(s, "%H:%M");
		b = g_date_time_format(e, "%H:%M");
		r = g_strdup_printf("%s, %s–%s", day, a, b);
		g_free(a), g_free(b);
	} else {
		char *eday = day_name(e);

		a = g_date_time_format(s, "%H:%M");
		b = g_date_time_format(e, "%H:%M");
		r = g_strdup_printf("%s, %s – %s, %s", day, a, eday, b);
		g_free(a), g_free(b), g_free(eday);
	}
	g_free(day);
	g_date_time_unref(s);
	g_date_time_unref(e);
	return r;
}
#endif

/* when the next shown appointment begins or ends (its line changes) */
static gint64 cal_next = G_MAXINT64;

/* today's and the next days' appointments, those over left out */
static void calendar_reload(void)
{
	cal_next = G_MAXINT64;
#ifndef PRO1X_NO_CALENDAR
	GDateTime *now, *today, *end;
	GPtrArray *all;
	gint64 t = time(NULL);
	guint i;

	g_ptr_array_set_size(appointments, 0);
	if (!show_calendar)
		return;
	now = g_date_time_new_now_local();
	today = g_date_time_new_local(g_date_time_get_year(now), g_date_time_get_month(now),
				      g_date_time_get_day_of_month(now), 0, 0, 0);
	end = g_date_time_add_days(today, cal_days);
	all = pro1x_calendar_load(g_date_time_to_unix(today), g_date_time_to_unix(end) - 1, 0);
	for (i = 0; i < all->len && appointments->len < CAL_LIMIT; i++) {
		CalEntry *c = g_ptr_array_index(all, i);
		Item *it;

		if (!c->allday && MAX(c->end, c->start) <= t)
			continue;
		if (!c->allday)
			cal_next = MIN(cal_next, c->start > t ? c->start : c->end);
		it = item_new(K_CAL, c->summary && *c->summary ? c->summary : _("(no title)"),
			      c->location);
		if (c->kind == CAL_BIRTHDAY && c->born) {
			GDateTime *s = g_date_time_new_from_unix_local(c->start);

			g_free(it->title);
			it->title = g_strdup_printf("%s (%d)", c->summary ? c->summary : "",
						    g_date_time_get_year(s) - c->born);
			g_date_time_unref(s);
		}
		it->footer = cal_when(c, t);
		it->time = c->start;
		it->colour = c->colour;
		it->cal_kind = c->kind;
		it->cal_id = c->calendar_id;
		it->target = g_strdup(c->id);
		it->action = A_CALENDAR;
		g_ptr_array_add(appointments, it);
	}
	g_ptr_array_unref(all);
	g_date_time_unref(end);
	g_date_time_unref(today);
	g_date_time_unref(now);
#endif
}

static void calendar_signal(GDBusConnection *c, const char *sender, const char *path,
			    const char *iface, const char *signal, GVariant *p, gpointer data)
{
	(void)c, (void)sender, (void)path, (void)iface, (void)signal, (void)p, (void)data;
	reload_soon();
}

/* --- updates ------------------------------------------------------------- */

static gboolean feed_timer(gpointer data)
{
	gint64 now = g_get_monotonic_time();

	(void)data;
	if (look_on && !closed) {
		if (show_feed && now - feed_time > (gint64)FEED_EVERY * G_USEC_PER_SEC)
			feed_update();
		if (show_weather && now >= weather_next)
			weather_update();
	}
	return TRUE;
}

/* --- the page's items --------------------------------------------------- */

/* the weather under the date: now and the next days, or why not */
static void add_weather(void)
{
	Item *it;

	if (weather) {
		const struct wmo *w = wmo_find(weather->code);
		GString *b = g_string_new(w ? _(w->text) : "");
		char t[32], *at, *as_of;

		if (weather->feels != G_MAXDOUBLE) {
			temp_text(t, sizeof t, weather->feels);
			if (b->len)
				g_string_append(b, " · ");
			g_string_append_printf(b, _("feels like %s"), t);
		}
		if (weather->wind >= 0) {
			char *w = g_strdup_printf(_("wind %ld km/h"), lround(weather->wind)), *p;

			if (b->len)
				g_string_append(b, " · ");
			/* "10 km/h" kept on one line: a no-break space before the
			 * unit, a word joiner after its slash */
			for (p = w; *p; p++) {
				if (*p == ' ' && g_str_has_prefix(p + 1, "km/h")) {
					g_string_append(b, "\u00a0km/\u2060h");
					p += 4;
				} else
					g_string_append_c(b, *p);
			}
			g_free(w);
		}
		it = item_new(K_WEATHER, place_name, b->str);
		g_string_free(b, TRUE);
		at = when(weather->fetched);
		as_of = g_strdup_printf(weather_hint ? _("As of %s, not updated") : _("As of %s"), at);
		/* the credit Open-Meteo's licence (CC BY 4.0) asks for */
		it->footer = g_strconcat(as_of, "\n", _("Weather data by Open-Meteo.com"), NULL);
		g_free(at), g_free(as_of);
		it->action = A_WEATHER;
	} else if (!place_set()) {
		it = item_new(K_HINT, _("Choose a place in Settings, Events, and its weather "
					"shows here."), NULL);
		it->action = A_SETTINGS;
	} else {
		char *s = g_strdup_printf("%s: %s", place_name, weather_hint ? weather_hint :
					  _("Updating…"));

		it = item_new(K_HINT, s, NULL);
		g_free(s);
	}
	g_ptr_array_add(chrome, it);
	g_ptr_array_add(items, it);
}

static void rebuild(void)
{
	GDateTime *now = g_date_time_new_now_local();
	char *day = g_date_time_format(now, "%A"), *date = g_date_time_format(now, "%e %B %Y");
	GPtrArray *n = g_ptr_array_new();
	Item *it;
	guint i;

	g_date_time_unref(now);
	g_ptr_array_set_size(chrome, 0);
	g_ptr_array_set_size(items, 0);
	pressed_item = NULL;

	it = item_new(K_DATE, day, g_strstrip(date));
	g_ptr_array_add(chrome, it);
	g_ptr_array_add(items, it);
	g_free(day), g_free(date);

	if (show_weather)
		add_weather();

	for (i = 0; show_notes && i < notes->len; i++)
		g_ptr_array_add(n, g_ptr_array_index(notes, i));
	for (i = 0; show_notes && i < mails->len; i++)
		g_ptr_array_add(n, g_ptr_array_index(mails, i));
	g_ptr_array_sort(n, by_time);
	if (n->len) {
		it = item_new(K_SECTION, _("Notifications"), NULL);
		it->clear = TRUE;
		g_ptr_array_add(chrome, it);
		g_ptr_array_add(items, it);
		for (i = 0; i < n->len; i++)
			g_ptr_array_add(items, g_ptr_array_index(n, i));
	}
	g_ptr_array_free(n, TRUE);

#ifndef PRO1X_NO_CALENDAR
	if (show_calendar) {
		it = item_new(K_SECTION, _("Calendar"), NULL);
		g_ptr_array_add(chrome, it);
		g_ptr_array_add(items, it);
		for (i = 0; i < appointments->len; i++)
			g_ptr_array_add(items, g_ptr_array_index(appointments, i));
		if (!appointments->len) {
			char *s = cal_days <= 1 ? g_strdup(_("No appointments today.")) :
				  g_strdup_printf(_("No appointments in the next %d days."), cal_days);

			it = item_new(K_HINT, s, NULL);
			g_free(s);
			it->action = A_CALENDAR;
			g_ptr_array_add(chrome, it);
			g_ptr_array_add(items, it);
		}
	}
#endif

	if (show_feed) {
		it = item_new(K_SECTION, "Mastodon", NULL);
		g_ptr_array_add(chrome, it);
		g_ptr_array_add(items, it);
		if (feed->len && !feed_hint)
			for (i = 0; i < feed->len; i++)
				g_ptr_array_add(items, g_ptr_array_index(feed, i));
		else {
			it = item_new(K_HINT, feed_hint ? feed_hint :
				      feed_busy ? _("Updating…") : "", NULL);
			it->action = feed_hint_settings ? A_SETTINGS : A_NONE;
			g_ptr_array_add(chrome, it);
			g_ptr_array_add(items, it);
			/* the posts of before, below the reason there are no new ones */
			if (!feed_hint_settings)
				for (i = 0; i < feed->len; i++)
					g_ptr_array_add(items, g_ptr_array_index(feed, i));
		}
	}
	layout_all();
	redraw();
}

/* the Clear button's place in a section item (in content coordinates) */
static void clear_rect(const Item *it, GdkRectangle *r)
{
	r->width = MM(16);
	r->height = MM(3.1);
	r->x = width - MM(1.4) - r->width;
	r->y = it->y + (it->h - r->height) / 2;
}

/* --- drawing ------------------------------------------------------------ */

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r)
{
	r = MIN(r, MIN(w, h) / 2);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
	cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
	cairo_close_path(cr);
}

static void text_at(cairo_t *cr, PangoLayout *l, double x, double y, guint32 rgb)
{
	if (!l)
		return;
	cairo_set_source_rgb(cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0,
			     (rgb & 255) / 255.0);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, l);
}

static void set_rgb(cairo_t *cr, guint32 rgb)
{
	cairo_set_source_rgb(cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0,
			     (rgb & 255) / 255.0);
}

/* The weather's glyphs where no icon set of MeeCast's is there: white
 * outlines on the black page as MeeCast's own set has them, the sun and
 * the moon in colour */
static void cloud(cairo_t *cr, double x, double y, double w, double lw)
{
	/* (w wide, 0.6 w high) its parts' union, outlined: stroked twice as
	 * wide, then filled black over the inner half */
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + 0.25 * w, y + 0.40 * w, 0.20 * w, 0, 2 * G_PI);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + 0.48 * w, y + 0.28 * w, 0.24 * w, 0, 2 * G_PI);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + 0.75 * w, y + 0.40 * w, 0.20 * w, 0, 2 * G_PI);
	cairo_rectangle(cr, x + 0.25 * w, y + 0.30 * w, 0.50 * w, 0.30 * w);
	cairo_set_line_width(cr, 2 * lw);
	set_rgb(cr, 0xffffff);
	cairo_stroke_preserve(cr);
	set_rgb(cr, 0x000000);
	cairo_fill(cr);
}

static void sun(cairo_t *cr, double cx, double cy, double r)
{
	int i;

	set_rgb(cr, 0xf5c518);
	cairo_arc(cr, cx, cy, r * 0.6, 0, 2 * G_PI);
	cairo_fill(cr);
	cairo_set_line_width(cr, r * 0.14);
	for (i = 0; i < 8; i++) {
		double a = i * G_PI / 4;

		cairo_move_to(cr, cx + cos(a) * r * 0.78, cy + sin(a) * r * 0.78);
		cairo_line_to(cr, cx + cos(a) * r, cy + sin(a) * r);
	}
	cairo_stroke(cr);
}

static void moon(cairo_t *cr, double cx, double cy, double r)
{
	cairo_push_group(cr);
	set_rgb(cr, 0xfaeb9c);
	cairo_arc(cr, cx, cy, r * 0.8, 0, 2 * G_PI);
	cairo_fill(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_arc(cr, cx + r * 0.45, cy - r * 0.3, r * 0.68, 0, 2 * G_PI);
	cairo_fill(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_pop_group_to_source(cr);
	cairo_paint(cr);
}

static void sky(cairo_t *cr, gboolean day, double cx, double cy, double r)
{
	if (day)
		sun(cr, cx, cy, r);
	else
		moon(cr, cx, cy, r);
}

static void drops(cairo_t *cr, double x, double y, double w, int n, double len, double lw)
{
	int i;

	set_rgb(cr, 0xffffff);
	cairo_set_line_width(cr, lw * 1.3);
	for (i = 0; i < n; i++) {
		double xi = x + w * (i + 0.5) / n + len * 0.15;

		cairo_move_to(cr, xi, y + (i % 2) * len * 0.3);
		cairo_line_to(cr, xi - len * 0.3, y + (i % 2) * len * 0.3 + len);
	}
	cairo_stroke(cr);
}

static void flakes(cairo_t *cr, double x, double y, double w, int n, double r)
{
	int i;

	set_rgb(cr, 0xffffff);
	for (i = 0; i < n; i++) {
		cairo_new_sub_path(cr);
		cairo_arc(cr, x + w * (i + 0.5) / n, y + r + (i % 2) * r * 2.2, r, 0, 2 * G_PI);
	}
	cairo_fill(cr);
}

static void bolt(cairo_t *cr, double x, double y, double h)
{
	set_rgb(cr, 0xf5c518);
	cairo_move_to(cr, x + 0.30 * h, y);
	cairo_line_to(cr, x + 0.02 * h, y + 0.56 * h);
	cairo_line_to(cr, x + 0.22 * h, y + 0.56 * h);
	cairo_line_to(cr, x + 0.08 * h, y + h);
	cairo_line_to(cr, x + 0.46 * h, y + 0.40 * h);
	cairo_line_to(cr, x + 0.26 * h, y + 0.40 * h);
	cairo_line_to(cr, x + 0.40 * h, y);
	cairo_close_path(cr);
	cairo_fill(cr);
}

static void fog(cairo_t *cr, double x, double y, double w, double lw)
{
	int i;

	set_rgb(cr, 0xffffff);
	cairo_set_line_width(cr, lw * 1.3);
	for (i = 0; i < 3; i++) {
		double yi = y + i * w * 0.13;

		cairo_move_to(cr, x, yi);
		cairo_curve_to(cr, x + w * 0.25, yi - w * 0.06, x + w * 0.25, yi + w * 0.06,
			       x + w * 0.5, yi);
		cairo_curve_to(cr, x + w * 0.75, yi - w * 0.06, x + w * 0.75, yi + w * 0.06,
			       x + w, yi);
	}
	cairo_stroke(cr);
}

static void draw_glyph(cairo_t *cr, int g, gboolean day, double x, double y, double s)
{
	double lw = MAX(1, s * 0.045);
	double top = y + s * 0.04, low = y + s * 0.64;	/* a cloud above, rain below */

	cairo_save(cr);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	switch (g) {
	case G_CLEAR:
		sky(cr, day, x + s / 2, y + s / 2, s * 0.44);
		break;
	case G_FAIR:
		sky(cr, day, x + s * 0.42, y + s * 0.40, s * 0.38);
		cloud(cr, x + s * 0.42, y + s * 0.54, s * 0.52, lw);
		break;
	case G_PARTLY:
		sky(cr, day, x + s * 0.36, y + s * 0.34, s * 0.32);
		cloud(cr, x + s * 0.18, y + s * 0.36, s * 0.78, lw);
		break;
	case G_CLOUDY:
		cloud(cr, x + s * 0.06, y + s * 0.20, s * 0.88, lw);
		break;
	case G_FOG:
		cloud(cr, x + s * 0.12, y + s * 0.04, s * 0.76, lw);
		fog(cr, x + s * 0.14, y + s * 0.66, s * 0.72, lw);
		break;
	default:
		if (g == G_SHOWER || g == G_SNOWSHOWER)
			sky(cr, day, x + s * 0.72, y + s * 0.22, s * 0.24);
		cloud(cr, x + s * 0.08, top, s * 0.84, lw);
		if (g == G_DRIZZLE)
			drops(cr, x + s * 0.2, low, s * 0.6, 3, s * 0.14, lw);
		else if (g == G_RAIN || g == G_SHOWER)
			drops(cr, x + s * 0.16, low, s * 0.68, 4, s * 0.24, lw);
		else if (g == G_SLEET) {
			drops(cr, x + s * 0.16, low, s * 0.34, 2, s * 0.22, lw);
			flakes(cr, x + s * 0.52, low, s * 0.34, 2, s * 0.05);
		} else if (g == G_SNOW || g == G_SNOWSHOWER)
			flakes(cr, x + s * 0.16, low, s * 0.68, 4, s * 0.055);
		else if (g == G_THUNDER) {
			bolt(cr, x + s * 0.38, y + s * 0.5, s * 0.46);
			drops(cr, x + s * 0.12, low, s * 0.24, 1, s * 0.22, lw);
			drops(cr, x + s * 0.66, low, s * 0.24, 1, s * 0.22, lw);
		}
		break;
	}
	cairo_restore(cr);
}

/* a weather code's icon: MeeCast's where its set is there, else a glyph */
static void draw_weather_icon(cairo_t *cr, int code, gboolean day, double x, double y, int s)
{
	const struct wmo *w = wmo_find(code);
	GdkPixbuf *b = w ? iconset_icon(day ? w->day : w->night, s) : NULL;

	if (b) {
		gdk_cairo_set_source_pixbuf(cr, b, x + (s - gdk_pixbuf_get_width(b)) / 2,
					    y + (s - gdk_pixbuf_get_height(b)) / 2);
		cairo_paint(cr);
	} else
		draw_glyph(cr, w ? w->glyph : G_CLOUDY, day, x, y, s);
}

/* An appointment's icon: its day on a plate of its calendar's colour
 * (calendar-backend's ten, as Hildon's calendar shows them) */
static const guint32 cal_colours[] = {
	0x2b5cb0, 0x2f8a3c, 0xa5302b, 0xee8a1e, 0x8c4db6,
	0xe3c21a, 0xd6d6d6, 0x3ea2ea, 0xe5413a, 0x7cc63b,
};

static void draw_plate(cairo_t *cr, Item *it, double x, double y)
{
	int s = icon_size(), w, h;
	guint32 c = cal_colours[it->colour >= 0 && it->colour < (int)G_N_ELEMENTS(cal_colours) ?
				it->colour : 0];
	gboolean light = it->colour == 5 || it->colour == 6;	/* yellow, white */
	GDateTime *d = g_date_time_new_from_unix_local(it->time);
	char *wd = d ? g_date_time_format(d, "%a") : g_strdup(""), num[8];
	PangoFontDescription *f;
	PangoLayout *l;

	g_snprintf(num, sizeof num, "%d", d ? g_date_time_get_day_of_month(d) : 0);
	if (d)
		g_date_time_unref(d);
	cairo_save(cr);
	rounded(cr, x, y, s, s, s * 0.18);
	cairo_clip(cr);
	set_rgb(cr, c);
	cairo_paint(cr);
	/* the darker band of its weekday */
	cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
	cairo_rectangle(cr, x, y, s, s * 0.32);
	cairo_fill(cr);
	f = font(FALSE, TRUE, s * 0.2);
	l = layout_text(wd, f, s, 1);
	pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
	pango_layout_get_pixel_size(l, &w, &h);
	text_at(cr, l, x, y + (s * 0.32 - h) / 2, light ? 0x202020 : 0xffffff);
	g_object_unref(l);
	pango_font_description_free(f);
	f = font(FALSE, TRUE, s * 0.4);
	l = layout_text(num, f, s, 1);
	pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
	pango_layout_get_pixel_size(l, &w, &h);
	text_at(cr, l, x, y + s * 0.32 + (s * 0.68 - h) / 2, light ? 0x202020 : 0xffffff);
	g_object_unref(l);
	pango_font_description_free(f);
	cairo_restore(cr);
	g_free(wd);
}

/* an icon in a rounded square (the avatars, as the N9 masked its feeds'
 * icons), or a letter on grey for none */
static void draw_icon(cairo_t *cr, Item *it, double x, double y)
{
	int s = icon_size();

	if (it->kind == K_CAL) {
		draw_plate(cr, it, x, y);
		return;
	}
	cairo_save(cr);
	rounded(cr, x, y, s, s, s * 0.18);
	if (it->icon) {
		cairo_clip(cr);
		gdk_cairo_set_source_pixbuf(cr, it->icon, x + (s - gdk_pixbuf_get_width(it->icon)) / 2,
					    y + (s - gdk_pixbuf_get_height(it->icon)) / 2);
		cairo_paint(cr);
	} else {
		PangoLayout *l;
		PangoFontDescription *d = font(FALSE, TRUE, s * 0.5);
		gunichar c = it->title && *it->title ? g_unichar_toupper(g_utf8_get_char(it->title)) : '?';
		char buf[8] = "";
		int w, h;

		cairo_set_source_rgb(cr, 0.27, 0.27, 0.27);
		cairo_fill(cr);
		buf[g_unichar_to_utf8(c, buf)] = 0;
		l = layout_text(buf, d, s, 1);
		pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
		pango_layout_get_pixel_size(l, &w, &h);
		text_at(cr, l, x, y + (s - h) / 2, 0xffffff);
		g_object_unref(l);
		pango_font_description_free(d);
	}
	cairo_restore(cr);
}

static void draw_item(cairo_t *cr, Item *it)
{
	int x = text_x(it), y = it->y;

	if (it == pressed_item && !pressed_clear && it->action != A_NONE) {
		/* the N9's pressed banner: a dark grey rounded plate */
		rounded(cr, MM(0.6), y, width - 2 * MM(0.6), it->h, MM(0.8));
		cairo_set_source_rgb(cr, 0.17, 0.17, 0.17);
		cairo_fill(cr);
	}
	switch (it->kind) {
	case K_DATE:
		text_at(cr, it->lt, x, y + MM(1.2), 0xffffff);
		text_at(cr, it->lb, x, y + MM(1.2) + layout_h(it->lt), 0x808080);
		break;
	case K_SECTION:
		text_at(cr, it->lt, x, y + (it->h - layout_h(it->lt)) / 2, 0x646464);
		if (it->clear) {
			GdkRectangle r;
			PangoFontDescription *d = font(FALSE, TRUE, MM(2.0));
			PangoLayout *l = layout_text(_("Clear"), d, MM(16), 1);
			gboolean down = it == pressed_item && pressed_clear;

			clear_rect(it, &r);
			rounded(cr, r.x, r.y, r.width, r.height, r.height / 2);
			cairo_set_source_rgb(cr, down ? 0.12 : 0.22, down ? 0.12 : 0.22,
					     down ? 0.12 : 0.22);
			cairo_fill_preserve(cr);
			cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
			cairo_set_line_width(cr, MAX(1, PX(1)));
			cairo_stroke(cr);
			pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
			text_at(cr, l, r.x, r.y + (r.height - layout_h(l)) / 2,
				down ? 0x797979 : 0xffffff);
			g_object_unref(l);
			pango_font_description_free(d);
		}
		break;
	case K_HINT:
		text_at(cr, it->lt, x, y + MM(0.8), it->action != A_NONE ? 0xc8c8c8 : 0x808080);
		break;
	case K_WEATHER: {
		int i, n = weather ? MIN(it->wdays, weather->days) : 0;

		if (!weather)
			break;
		draw_weather_icon(cr, weather->code, weather->is_day, margin(), y + MM(0.8), WICON);
		text_at(cr, it->lx[0], it->wtemp_x, y + MM(0.8) + (WICON - layout_h(it->lx[0])) / 2,
			0xffffff);
		text_at(cr, it->lt, it->wtext_x, y + it->wtext_y, 0xffffff);
		text_at(cr, it->lb, it->wtext_x, y + it->wtext_y + layout_h(it->lt), 0xffffff);
		text_at(cr, it->lf, it->wtext_x, y + it->wlink_y, 0x808080);
		for (i = 0; i < n; i++) {
			int dx = it->wday_x + i * it->wday_w, dy = y + it->wday_y;

			text_at(cr, it->lx[1 + 2 * i], dx, dy, i ? 0x808080 : 0xc8c8c8);
			dy += layout_h(it->lx[1 + 2 * i]) + MM(0.3);
			draw_weather_icon(cr, weather->day[i].code, TRUE,
					  dx + (it->wday_w - WDAY_ICON) / 2, dy, WDAY_ICON);
			dy += WDAY_ICON + MM(0.3);
			text_at(cr, it->lx[2 + 2 * i], dx, dy, 0xffffff);
		}
		cairo_set_source_rgb(cr, 0.16, 0.16, 0.16);
		cairo_rectangle(cr, margin(), y + it->h - MAX(1, MM(0.1)), width - 2 * margin(),
				MAX(1, MM(0.1)));
		cairo_fill(cr);
		break;
	}
	default:
		draw_icon(cr, it, margin() + MM(0.2), y + MM(0.8));
		text_at(cr, it->lt, x, y + MM(0.8), 0xffffff);
		text_at(cr, it->lb, x, y + MM(0.8) + layout_h(it->lt), 0xffffff);
		text_at(cr, it->lf, x, y + MM(0.8) + layout_h(it->lt) + layout_h(it->lb) + MM(0.4),
			0x808080);
		/* the separator below, as the N9's feed has */
		cairo_set_source_rgb(cr, 0.16, 0.16, 0.16);
		cairo_rectangle(cr, x, y + it->h - MAX(1, MM(0.1)), width - x - margin(),
				MAX(1, MM(0.1)));
		cairo_fill(cr);
		break;
	}
}

/* what shows above the list while it is pulled down: the arrow and "Pull
 * down to update", "Release to update", or a spinner while it updates */
static void draw_pull(cairo_t *cr, double top)
{
	PangoFontDescription *d = font(FALSE, FALSE, MM(2.0));
	const char *t = refreshing ? _("Updating…") :
			top >= PULL_UPDATE ? _("Release to update") : _("Pull down to update");
	PangoLayout *l = layout_text(t, d, width, 1);
	double cy = top - PULL_UPDATE / 2, r = MM(1.2), cx;
	int w, h;

	pango_layout_get_pixel_size(l, &w, &h);
	cx = (width - w) / 2 - MM(1.2) - r;
	cairo_save(cr);
	cairo_rectangle(cr, 0, 0, width, top);
	cairo_clip(cr);
	text_at(cr, l, (width - w) / 2 + r, cy - h / 2, 0x808080);
	cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
	cairo_set_line_width(cr, MAX(2, MM(0.3)));
	if (refreshing) {
		cairo_arc(cr, cx, cy, r, spin, spin + 1.5 * G_PI);
		cairo_stroke(cr);
	} else {
		/* an arrow, down until it updates on release, then up */
		double dir = top >= PULL_UPDATE ? -1 : 1;

		cairo_move_to(cr, cx, cy - dir * r);
		cairo_line_to(cr, cx, cy + dir * r);
		cairo_move_to(cr, cx - r * 0.6, cy + dir * r * 0.4);
		cairo_line_to(cr, cx, cy + dir * r);
		cairo_line_to(cr, cx + r * 0.6, cy + dir * r * 0.4);
		cairo_stroke(cr);
	}
	cairo_restore(cr);
	g_object_unref(l);
	pango_font_description_free(d);
}

static gboolean expose_cb(GtkWidget *w, GdkEventExpose *e, gpointer data)
{
	cairo_t *cr = gdk_cairo_create(w->window);
	double top = pull > 0 ? pull : 0;
	gint64 since;
	guint i;

	(void)data;
	gdk_cairo_region(cr, e->region);
	cairo_clip(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	if (top > 0)
		draw_pull(cr, top);
	cairo_save(cr);
	cairo_translate(cr, 0, top - offset);
	for (i = 0; items && i < items->len; i++) {
		Item *it = g_ptr_array_index(items, i);

		if (it->y + it->h < offset - top || it->y > offset - top + height)
			continue;
		draw_item(cr, it);
	}
	cairo_restore(cr);
	/* the scroll indicator, while it moves and a little after */
	since = g_get_monotonic_time() - moved_at;
	if (content_height > height && since < 600000) {
		double a = since < 300000 ? 1 : 1 - (since - 300000) / 300000.0;
		double bh = MAX(MM(3), (double)height * height / content_height);
		double by = (height - bh) * (max_offset() > 0 ? offset / max_offset() : 0);

		rounded(cr, width - MM(0.8), by, MM(0.5), bh, MM(0.25));
		cairo_set_source_rgba(cr, 0.6, 0.6, 0.6, 0.8 * a);
		cairo_fill(cr);
	}
	cairo_destroy(cr);
	return TRUE;
}

/* --- motion ------------------------------------------------------------- */

static gboolean anim_cb(gpointer data);

static void anim_start(void)
{
	if (!anim_id)
		anim_id = g_timeout_add(16, anim_cb, NULL);
}

/* kinetic scrolling, the pull and the overscroll springing back, the
 * spinner, the scroll indicator fading */
static gboolean anim_cb(gpointer data)
{
	static gint64 prev;
	gint64 now = g_get_monotonic_time();
	/* the time since the last step (a slow machine steps less often) */
	double dt = prev ? MIN(now - prev, 100000) / 1e6 : 0.016;
	gboolean more = FALSE;

	(void)data;
	prev = now;
	if (!pressed) {
		if (fabs(velocity) > MM(2)) {
			offset += velocity * dt;
			velocity *= exp(-dt / 0.35);
			moved_at = now;
			if (offset < 0 || offset > max_offset()) {
				velocity *= exp(-dt / 0.05);
			}
			more = TRUE;
		} else
			velocity = 0;
		if (offset < 0) {
			offset += (0 - offset) * MIN(1, dt * 12);
			if (offset > -0.5)
				offset = 0;
			more = TRUE;
		} else if (offset > max_offset()) {
			offset += (max_offset() - offset) * MIN(1, dt * 12);
			if (offset - max_offset() < 0.5)
				offset = max_offset();
			more = TRUE;
		}
		{
			double rest = refreshing ? PULL_UPDATE : 0;

			if (fabs(pull - rest) > 0.5) {
				pull += (rest - pull) * MIN(1, dt * 10);
				more = TRUE;
			} else
				pull = rest;
		}
	}
	if (refreshing) {
		spin += dt * 2 * G_PI;
		more = TRUE;
	}
	if (now - moved_at < 600000)
		more = TRUE;
	redraw();
	if (!more) {
		anim_id = 0;
		prev = 0;
		return FALSE;
	}
	return TRUE;
}

static Item *item_at(double y, gboolean *on_clear)
{
	double cy = y - (pull > 0 ? pull : 0) + offset;
	guint i;

	*on_clear = FALSE;
	for (i = 0; items && i < items->len; i++) {
		Item *it = g_ptr_array_index(items, i);

		if (cy >= it->y && cy < it->y + it->h) {
			if (it->kind == K_SECTION && it->clear)
				*on_clear = TRUE;
			return it;
		}
	}
	return NULL;
}

/* an application through hildon-desktop's launcher (its loading screen, a
 * running one brought up): the first of @ids that has a menu entry */
static gboolean launch(const char *ids)
{
	char **v = g_strsplit(ids, " ", 0), **id;
	const char *const *dirs = g_get_system_data_dirs();
	int i;

	for (id = v; *id; id++)
		for (i = 0; dirs[i]; i++) {
			char *p = g_strdup_printf("%s/applications/hildon/%s.desktop", dirs[i], *id);
			gboolean there = g_file_test(p, G_FILE_TEST_EXISTS);

			g_free(p);
			if (there && bus) {
				g_dbus_connection_call(bus, "com.nokia.HildonDesktop.AppMgr",
						       "/com/nokia/HildonDesktop/AppMgr",
						       "com.nokia.HildonDesktop.AppMgr",
						       "LaunchApplication", g_variant_new("(s)", *id),
						       NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
				g_strfreev(v);
				return TRUE;
			}
		}
	g_strfreev(v);
	return FALSE;
}

static void open_url(const char *url)
{
	char *argv[] = { "xdg-open", (char *)url, NULL };

	if (!g_app_info_launch_default_for_uri(url, NULL, NULL))
		g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

/* Settings, Events (apps/pro1x-events): the control panel's run_applet,
 * on libosso's path for the program "controlpanel" */
static void open_settings(void)
{
	if (bus)
		g_dbus_connection_call(bus, "com.nokia.controlpanel", "/com/nokia/controlpanel",
				       "com.nokia.controlpanel", "run_applet",
				       g_variant_new("(sb)", "libpro1x-events.so", TRUE), NULL,
				       G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* qalendar not there for its D-Bus service: through the launcher */
static void calendar_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	GError *e = NULL;
	GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);

	(void)data;
	if (v)
		g_variant_unref(v);
	else {
		g_error_free(e);
		launch("qalendar calendar");
	}
}

/* Hildon's calendar (qalendar's com.nokia.calendar, Maemo's calendar
 * interface): an appointment, else the agenda of today */
static void open_calendar(Item *it)
{
	guint type = 3;		/* the agenda */

	if (!bus) {
		launch("qalendar calendar");
		return;
	}
#ifndef PRO1X_NO_CALENDAR
	if (it && it->target && *it->target)
		type = it->cal_kind == CAL_TODO ? 5 : 4;
#endif
	g_dbus_connection_call(bus, "com.nokia.calendar", "/com/nokia/calendar",
			       "com.nokia.calendar", "launch_view",
			       g_variant_new("(uisi)", type, (gint32)(it && it->time ? it->time :
								      time(NULL)),
					     type == 3 ? "" : it->target, type == 3 ? 0 : it->cal_id),
			       NULL, G_DBUS_CALL_FLAGS_NONE, 60000, NULL, calendar_cb, NULL);
}

/* Clear: the calls and messages read, the mails until new ones come */
static void clear_notes(void)
{
	guint i, j;

	for (i = 0; i < notes->len; i++) {
		Item *it = g_ptr_array_index(notes, i);

		for (j = 0; it->events && j < it->events->len; j++)
			rtcom_el_set_read_event(el, g_array_index(it->events, gint, j), TRUE, NULL);
	}
	if (mails->len)
		gconf_client_set_int(gconf, KEY_CLEARED, (gint)time(NULL), NULL);
	g_ptr_array_set_size(notes, 0);
	g_ptr_array_set_size(mails, 0);
	rebuild();
}

/* CY: where in the item (content coordinates) */
static void tap(Item *it, gboolean on_clear, double cy)
{
	if (!it)
		return;
	if (on_clear) {
		clear_notes();
		return;
	}
	switch (it->action) {
	case A_APP:
		launch(it->target);
		/* the notification is read once its application shows it */
		if (it->events) {
			guint j;

			for (j = 0; j < it->events->len; j++)
				rtcom_el_set_read_event(el, g_array_index(it->events, gint, j),
							TRUE, NULL);
		}
		break;
	case A_URL:
		open_url(it->target);
		break;
	case A_SETTINGS:
		open_settings();
		break;
	case A_CALENDAR:
		open_calendar(it);
		break;
	case A_WEATHER:
		/* the credit line: Open-Meteo's site; else a weather application */
		if ((cy >= it->y + it->wlink_y - MM(0.6) &&
		     cy < it->y + it->wlink_y + layout_h(it->lf) + MM(0.6)) ||
		    !launch("nomweather meecast"))
			open_url(WEATHER_SITE);
		break;
	}
}

static void sample(double y)
{
	gint64 now = g_get_monotonic_time();

	if (last_t && now > last_t) {
		double v = -(y - last_y) * 1e6 / (now - last_t);

		/* smoothed over the last moves */
		velocity = velocity * 0.4 + v * 0.6;
	}
	last_y = y;
	last_t = now;
}

static gboolean pointer_cb(GtkWidget *w, GdkEvent *e, gpointer data)
{
	(void)w, (void)data;
	switch (e->type) {
	case GDK_BUTTON_PRESS:
		if (e->button.button != 1)
			break;
		pressed = TRUE;
		dragging = FALSE;
		press_y = e->button.y;
		press_offset = offset - (pull > 0 && !refreshing ? pull : 0);
		velocity = 0;
		last_t = 0;
		sample(e->button.y);
		pressed_item = item_at(e->button.y, &pressed_clear);
		press_cy = e->button.y - (pull > 0 ? pull : 0) + offset;
		if (pressed_clear) {
			GdkRectangle r;
			double cy = e->button.y - (pull > 0 ? pull : 0) + offset;

			clear_rect(pressed_item, &r);
			pressed_clear = e->button.x >= r.x - MM(1) && cy >= r.y - MM(1.5) &&
					cy <= r.y + r.height + MM(1.5);
		}
		redraw();
		break;
	case GDK_MOTION_NOTIFY: {
		double dy, o;

		if (!pressed)
			break;
		dy = e->motion.y - press_y;
		if (!dragging && fabs(dy) < TAP_SLOP)
			break;
		if (!dragging) {
			/* from here on it scrolls, from where the finger is */
			dragging = TRUE;
			press_y = e->motion.y;
			press_offset = offset;
			pressed_item = NULL;
			dy = 0;
		}
		sample(e->motion.y);
		o = press_offset - dy;
		if (o < 0) {
			/* pulled down at the top: half the way, an update when
			 * pulled far enough */
			pull = (refreshing ? PULL_UPDATE : 0) - o / 2;
			offset = 0;
		} else {
			pull = refreshing ? PULL_UPDATE : 0;
			offset = o > max_offset() ? max_offset() + (o - max_offset()) / 2 : o;
		}
		moved_at = g_get_monotonic_time();
		redraw();
		anim_start();
		break;
	}
	case GDK_BUTTON_RELEASE:
		if (!pressed || e->button.button != 1)
			break;
		pressed = FALSE;
		if (dragging) {
			sample(e->button.y);
			if (!refreshing && pull >= PULL_UPDATE) {
				refreshing = TRUE;
				feed_update();
				weather_update();
				reload_soon();
				busy_check();
			}
			if (g_get_monotonic_time() - last_t > 150000)
				velocity = 0;	/* held still before the release */
		} else {
			Item *it = pressed_item;
			gboolean c = pressed_clear;

			pressed_item = NULL;
			redraw();
			tap(it, c, press_cy);
		}
		dragging = FALSE;
		pressed_item = NULL;
		anim_start();
		break;
	case GDK_LEAVE_NOTIFY:
		/* hildon-desktop took the drag (across, to the next page) */
		if (pressed && !dragging) {
			pressed = FALSE;
			pressed_item = NULL;
			redraw();
		}
		break;
	default:
		break;
	}
	return TRUE;
}

/* --- the window ---------------------------------------------------------- */

static void place(void)
{
	GdkScreen *s = gdk_screen_get_default();
	GSList *pos = NULL;
	GError *e = NULL;

	width = gdk_screen_get_width(s);
	height = gdk_screen_get_height(s) - bar_height;
	gtk_widget_set_size_request(win, width, height);
	gtk_window_resize(GTK_WINDOW(win), width, height);
	gtk_window_move(GTK_WINDOW(win), 0, bar_height);
	/* the first home view, right below the status bar (hildon-desktop) */
	gconf_client_set_int(gconf, APPLET_KEY "/view", 1, &e);
	g_clear_error(&e);
	pos = g_slist_append(pos, GINT_TO_POINTER(0));
	pos = g_slist_append(pos, GINT_TO_POINTER(bar_height));
	gconf_client_set_list(gconf, APPLET_KEY "/position", GCONF_VALUE_INT, pos, NULL);
	gconf_client_set_list(gconf, APPLET_KEY "/position_portrait", GCONF_VALUE_INT, pos, NULL);
	g_slist_free(pos);
	layout_all();
}

static void realize_cb(GtkWidget *w, gpointer data)
{
	Display *d = GDK_WINDOW_XDISPLAY(w->window);
	Window x = GDK_WINDOW_XID(w->window);
	Atom type = XInternAtom(d, "_HILDON_WM_WINDOW_TYPE_HOME_APPLET", False);
	long one = 1;

	(void)data;
	gdk_window_set_decorations(w->window, 0);
	XChangeProperty(d, x, XInternAtom(d, "_NET_WM_WINDOW_TYPE", False), XA_ATOM, 32,
			PropModeReplace, (unsigned char *)&type, 1);
	XChangeProperty(d, x, XInternAtom(d, "_HILDON_APPLET_ID", False),
			XInternAtom(d, "UTF8_STRING", False), 8, PropModeReplace,
			(unsigned char *)APPLET_ID, strlen(APPLET_ID));
	/* the drags up and down are the page's (hildon-desktop.patch) */
	XChangeProperty(d, x, XInternAtom(d, "_HILDON_MEEGO_SCROLLING_APPLET", False),
			XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&one, 1);
}

static void show_page(void)
{
	gboolean want = look_on && !closed;

	if (want && !GTK_WIDGET_VISIBLE(win)) {
		place();
		gtk_widget_show(win);
		if (!feed_time || g_get_monotonic_time() - feed_time >
				  (gint64)FEED_EVERY * G_USEC_PER_SEC)
			feed_update();
		if (g_get_monotonic_time() >= weather_next)
			weather_update();
	} else if (!want && GTK_WIDGET_VISIBLE(win))
		gtk_widget_hide(win);
}

/* the look: the root window's _HILDON_MEEGO_LOOK (hildon-desktop's) */
static void look_update(void)
{
	Display *d = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
	Atom type, a = XInternAtom(d, "_HILDON_MEEGO_LOOK", False);
	int format;
	unsigned long n, after;
	unsigned char *v = NULL;
	gboolean on = FALSE;

	if (XGetWindowProperty(d, DefaultRootWindow(d), a, 0, 1, False, XA_CARDINAL, &type,
			       &format, &n, &after, &v) == Success && v) {
		on = n == 1 && format == 32 && *(long *)v != 0;
		XFree(v);
	}
	if (on && !look_on)
		closed = FALSE;	/* closed in Home's edit mode: back with the look */
	look_on = on;
	show_page();
}

static GdkFilterReturn root_filter(GdkXEvent *xe, GdkEvent *e, gpointer data)
{
	XEvent *x = xe;
	static Atom a;

	(void)e, (void)data;
	if (!a)
		a = XInternAtom(x->xany.display, "_HILDON_MEEGO_LOOK", False);
	if (x->type == PropertyNotify && x->xproperty.atom == a)
		look_update();
	return GDK_FILTER_CONTINUE;
}

/* the page on the view in front again (hildon-desktop's
 * _HILDON_APPLET_ON_CURRENT_DESKTOP): an update if it is old */
static gboolean property_cb(GtkWidget *w, GdkEventProperty *e, gpointer data)
{
	(void)w, (void)data;
	if (e->atom == gdk_atom_intern_static_string("_HILDON_APPLET_ON_CURRENT_DESKTOP") &&
	    e->state == GDK_PROPERTY_NEW_VALUE) {
		if (g_get_monotonic_time() - feed_time > (gint64)FEED_EVERY * G_USEC_PER_SEC)
			feed_update();
		if (g_get_monotonic_time() >= weather_next)
			weather_update();
	}
	return FALSE;
}

/* closed in Home's edit mode: hildon-desktop has forgotten its place; until
 * the look is switched on again */
static gboolean delete_cb(GtkWidget *w, GdkEvent *e, gpointer data)
{
	(void)w, (void)e, (void)data;
	closed = TRUE;
	show_page();
	return TRUE;
}

static gboolean gconf_bool(const char *key, gboolean def)
{
	GConfValue *v = gconf_client_get(gconf, key, NULL);
	gboolean r = def;

	if (v) {
		if (v->type == GCONF_VALUE_BOOL)
			r = gconf_value_get_bool(v);
		gconf_value_free(v);
	}
	return r;
}

/* a float (the applet's), an int or a string */
static double gconf_number(const char *key, double def)
{
	GConfValue *v = gconf_client_get(gconf, key, NULL);
	double r = def;

	if (v) {
		if (v->type == GCONF_VALUE_FLOAT)
			r = gconf_value_get_float(v);
		else if (v->type == GCONF_VALUE_INT)
			r = gconf_value_get_int(v);
		else if (v->type == GCONF_VALUE_STRING) {
			const char *s = gconf_value_get_string(v);
			char *end;

			r = g_ascii_strtod(s, &end);
			if (end == s)
				r = def;
		}
		gconf_value_free(v);
	}
	return r;
}

static void read_settings(void)
{
	show_notes = gconf_bool(KEY_SHOW_NOTES, TRUE);
	show_weather = gconf_bool(KEY_SHOW_WEATHER, TRUE);
	show_calendar = gconf_bool(KEY_SHOW_CALENDAR, TRUE);
	show_feed = gconf_bool(KEY_SHOW_FEED, TRUE);
	cal_days = gconf_client_get_int(gconf, KEY_CAL_DAYS, NULL);
	cal_days = cal_days > 0 ? MIN(cal_days, 31) : CAL_DAYS;
	g_free(place_name);
	place_name = gconf_client_get_string(gconf, KEY_PLACE, NULL);
	if (place_name)
		g_strstrip(place_name);
	place_lat = gconf_number(KEY_LAT, 999);
	place_lon = gconf_number(KEY_LON, 999);
}

/* the applet saves the place's keys one by one: once after them */
static guint place_id;

static gboolean place_cb(gpointer data)
{
	(void)data;
	place_id = 0;
	g_free(weather_hint);
	weather_hint = NULL;
	weather_load_cache();
	weather_next = 0;
	if (look_on && !closed)
		weather_update();
	rebuild();
	return FALSE;
}

static void settings_changed(GConfClient *c, guint id, GConfEntry *e, gpointer data)
{
	(void)c, (void)id, (void)data;
	if (!e)
		return;
	read_settings();
	if (!strcmp(e->key, KEY_SERVER) || !strcmp(e->key, KEY_TOKEN)) {
		/* (the page shows none of the old posts from here on) */
		g_ptr_array_set_size(items, 0);
		g_ptr_array_set_size(feed, 0);
		feed_time = 0;
		rebuild();
		if (look_on)
			feed_update();
	} else if (!strcmp(e->key, KEY_WEATHER_ICONS)) {
		iconset_reset();
		rebuild();
	} else if (!strcmp(e->key, KEY_PLACE) || !strcmp(e->key, KEY_LAT) ||
		   !strcmp(e->key, KEY_LON) || !strcmp(e->key, KEY_WEATHER_URL)) {
		if (!place_id)
			place_id = g_timeout_add(300, place_cb, NULL);
	} else if (g_str_has_prefix(e->key, KEY_DIR "/show_") || !strcmp(e->key, KEY_CAL_DAYS)) {
		/* a section on or off */
		calendar_reload();
		rebuild();
		if (look_on && !closed) {
			if (show_feed && !feed->len && !feed_busy)
				feed_update();
			if (show_weather && g_get_monotonic_time() >= weather_next)
				weather_update();
		}
	}
}

/* the date at midnight, the times of the items */
static gboolean minute_cb(gpointer data)
{
	static int day = -1;
	GDateTime *now = g_date_time_new_now_local();
	int d = g_date_time_get_day_of_year(now);

	(void)data;
	g_date_time_unref(now);
	if ((day != -1 && d != day) || time(NULL) >= cal_next)
		reload_soon();
	day = d;
	return TRUE;
}

static void size_changed(GdkScreen *s, gpointer data)
{
	(void)s, (void)data;
	if (GTK_WIDGET_VISIBLE(win))
		place();
	redraw();
}

static double hildon_scale(void)
{
	const char *env = g_getenv("HILDON_SCALE");
	char *theme = NULL;
	double s;

	/* else the factor the theme was made for, as hildon-desktop finds it */
	if (!env && g_file_get_contents("/etc/hildon/theme/.pro1x-theme", &theme, NULL, NULL))
		env = strchr(theme, '\n') ? strchr(theme, '\n') + 1 : NULL;
	s = env ? g_ascii_strtod(env, NULL) : 0;
	g_free(theme);
	if (!(s >= 0.5 && s <= 8)) {
		GdkScreen *sc = gdk_screen_get_default();

		s = MIN(gdk_screen_get_width(sc), gdk_screen_get_height(sc)) / 480.0;
		s = CLAMP(s, 1, 4);
	}
	return s;
}

int main(int argc, char **argv)
{
	GdkWindow *root;

	setlocale(LC_ALL, "");
	bindtextdomain(DOMAIN, LOCALEDIR);
	bind_textdomain_codeset(DOMAIN, "UTF-8");
	curl_global_init(CURL_GLOBAL_DEFAULT);
	gtk_init(&argc, &argv);
	scale = hildon_scale();
	bar_height = (int)(36 * scale + 0.5);
	gconf = gconf_client_get_default();
	gconf_client_add_dir(gconf, KEY_DIR, GCONF_CLIENT_PRELOAD_NONE, NULL);
	gconf_client_notify_add(gconf, KEY_DIR, settings_changed, NULL, NULL, NULL);
	bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	if (bus) {
		g_dbus_connection_signal_subscribe(bus, "com.nokia.modest", NULL, NULL, NULL, NULL,
						   G_DBUS_SIGNAL_FLAGS_NONE, modest_signal, NULL, NULL);
		g_bus_watch_name_on_connection(bus, "com.nokia.modest", G_BUS_NAME_WATCHER_FLAGS_NONE,
					       modest_owner, NULL, NULL, NULL);
		/* calendar-backend's: the appointments changed */
		g_dbus_connection_signal_subscribe(bus, NULL, "com.nokia.calendar", "dbChange",
						   "/com/nokia/calendar", NULL,
						   G_DBUS_SIGNAL_FLAGS_NONE, calendar_signal, NULL,
						   NULL);
	}
	el = rtcom_el_new();
	if (el) {
		static const char *const sig[] = {
			"new-event", "event-updated", "event-deleted", "all-deleted", "refresh-hint",
		};
		guint i;

		for (i = 0; i < G_N_ELEMENTS(sig); i++)
			g_signal_connect_swapped(el, sig[i], G_CALLBACK(el_changed), NULL);
	}
	notes = g_ptr_array_new_with_free_func(item_free);
	mails = g_ptr_array_new_with_free_func(item_free);
	feed = g_ptr_array_new_with_free_func(item_free);
	chrome = g_ptr_array_new_with_free_func(item_free);
	items = g_ptr_array_new();
	appointments = g_ptr_array_new_with_free_func(item_free);
	read_settings();
	weather_load_cache();

	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), "Events");
	gtk_window_set_accept_focus(GTK_WINDOW(win), FALSE);
	gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
	gtk_widget_set_app_paintable(win, TRUE);
	gtk_widget_set_double_buffered(win, TRUE);
	gtk_widget_add_events(win, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
			      GDK_POINTER_MOTION_MASK | GDK_BUTTON_MOTION_MASK |
			      GDK_LEAVE_NOTIFY_MASK | GDK_PROPERTY_CHANGE_MASK);
	g_signal_connect(win, "realize", G_CALLBACK(realize_cb), NULL);
	g_signal_connect(win, "expose-event", G_CALLBACK(expose_cb), NULL);
	g_signal_connect(win, "button-press-event", G_CALLBACK(pointer_cb), NULL);
	g_signal_connect(win, "button-release-event", G_CALLBACK(pointer_cb), NULL);
	g_signal_connect(win, "motion-notify-event", G_CALLBACK(pointer_cb), NULL);
	g_signal_connect(win, "leave-notify-event", G_CALLBACK(pointer_cb), NULL);
	g_signal_connect(win, "property-notify-event", G_CALLBACK(property_cb), NULL);
	g_signal_connect(win, "delete-event", G_CALLBACK(delete_cb), NULL);
	g_signal_connect(gdk_screen_get_default(), "size-changed", G_CALLBACK(size_changed), NULL);
	place();
	gtk_widget_realize(win);

	notes_reload();
	mails_reload();
	calendar_reload();
	rebuild();

	root = gdk_get_default_root_window();
	gdk_window_set_events(root, gdk_window_get_events(root) | GDK_PROPERTY_CHANGE_MASK);
	gdk_window_add_filter(root, root_filter, NULL);
	look_update();
	g_timeout_add_seconds(60, feed_timer, NULL);
	g_timeout_add_seconds(60, minute_cb, NULL);
	gtk_main();
	return 0;
}
