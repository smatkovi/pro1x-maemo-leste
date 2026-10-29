/*
 * pro1x-events -- the Nokia N9's Events page for Hildon's MeeGo-like look
 * (patches/meego): the first home view shows, below the status bar, the
 * day and the date, the notifications (missed calls, messages and chats
 * that rtcom-eventlogger has unread, unread e-mail from modest while it
 * runs) and the home timeline of a Mastodon account, in one list that
 * scrolls; pulled down at its top it updates. Only while the look is on
 * (the root window's _HILDON_MEEGO_LOOK, which hildon-desktop sets), else it
 * waits unseen.
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

#define DOMAIN "pro1x-events"
#define _(s) dgettext(DOMAIN, s)
#ifndef LOCALEDIR
#define LOCALEDIR "/usr/share/locale"
#endif

#define KEY_MEEGO "/apps/osso/hildon-desktop/meego_pages"
#define KEY_DIR "/apps/pro1x/events"
#define KEY_SERVER KEY_DIR "/mastodon_server"
#define KEY_TOKEN KEY_DIR "/mastodon_token"
#define KEY_CLEARED KEY_DIR "/mail_cleared"
#define APPLET_ID "pro1x-events"
#define APPLET_KEY "/apps/osso/hildon-desktop/applets/" APPLET_ID

#define FEED_LIMIT 30		/* posts (the API gives 40 at most) */
#define FEED_EVERY (15 * 60)	/* s between updates while seen */
#define MAIL_PER_ACCOUNT 3
#define AVATAR_MAX (2 << 20)	/* bytes */
#define AVATAR_DAYS 7

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

enum kind { K_DATE, K_SECTION, K_NOTE, K_FEED, K_HINT };
enum action { A_NONE, A_APP, A_URL, A_SETTINGS };

typedef struct {
	int kind, action;
	char *title, *body, *footer;
	GdkPixbuf *icon;
	gint64 time;
	char *target;		/* application ids (space separated) or a URL */
	GArray *events;		/* rtcom-eventlogger's ids of a notification */
	gboolean clear;		/* a section with the Clear button */
	/* laid out */
	int y, h;
	PangoLayout *lt, *lb, *lf;
} Item;

static GPtrArray *notes;	/* calls and messages (rtcom-eventlogger) */
static GPtrArray *mails;	/* unread e-mail (modest) */
static GPtrArray *feed;		/* the Mastodon posts */
static char *feed_hint;		/* instead of them: why there are none */
static gboolean feed_hint_settings;	/* a tap on the hint opens the settings */
static GPtrArray *items;	/* what the page shows, in its order */
static GPtrArray *chrome;	/* the date and the sections (owned) */
static int content_height;

static void item_free(gpointer p)
{
	Item *it = p;

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
static double press_y, press_offset, last_y;
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
	return margin() + (it->kind == K_NOTE || it->kind == K_FEED ?
			   MM(0.2) + icon_size() + MM(1.6) : 0);
}

static void layout_item(Item *it)
{
	PangoFontDescription *d;
	int w = width - text_x(it) - margin();

	if (it->lt)
		g_object_unref(it->lt);
	if (it->lb)
		g_object_unref(it->lb);
	if (it->lf)
		g_object_unref(it->lf);
	it->lt = it->lb = it->lf = NULL;
	switch (it->kind) {
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
	refreshing = FALSE;
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
	g_idle_add(fetch_done, f);
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

	if (feed_busy)
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
		refreshing = FALSE;
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

static gboolean feed_timer(gpointer data)
{
	(void)data;
	if (look_on && !closed &&
	    g_get_monotonic_time() - feed_time > (gint64)FEED_EVERY * G_USEC_PER_SEC)
		feed_update();
	return TRUE;
}

/* --- the page's items --------------------------------------------------- */

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

	for (i = 0; i < notes->len; i++)
		g_ptr_array_add(n, g_ptr_array_index(notes, i));
	for (i = 0; i < mails->len; i++)
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

/* an icon in a rounded square (the avatars, as the N9 masked its feeds'
 * icons), or a letter on grey for none */
static void draw_icon(cairo_t *cr, Item *it, double x, double y)
{
	int s = icon_size();

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
		text_at(cr, it->lt, x, y + MM(0.8), it->action == A_SETTINGS ? 0xc8c8c8 : 0x808080);
		break;
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
static void launch(const char *ids)
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
				return;
			}
		}
	g_strfreev(v);
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

static void tap(Item *it, gboolean on_clear)
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
				if (!feed_busy)
					refreshing = FALSE;
			}
			if (g_get_monotonic_time() - last_t > 150000)
				velocity = 0;	/* held still before the release */
		} else {
			Item *it = pressed_item;
			gboolean c = pressed_clear;

			pressed_item = NULL;
			redraw();
			tap(it, c);
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
	    e->state == GDK_PROPERTY_NEW_VALUE &&
	    g_get_monotonic_time() - feed_time > (gint64)FEED_EVERY * G_USEC_PER_SEC)
		feed_update();
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

static void settings_changed(GConfClient *c, guint id, GConfEntry *e, gpointer data)
{
	(void)c, (void)id, (void)data;
	if (e && (!strcmp(e->key, KEY_SERVER) || !strcmp(e->key, KEY_TOKEN))) {
		/* (the page shows none of the old posts from here on) */
		g_ptr_array_set_size(items, 0);
		g_ptr_array_set_size(feed, 0);
		feed_time = 0;
		rebuild();
		if (look_on)
			feed_update();
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
	if (day != -1 && d != day)
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
