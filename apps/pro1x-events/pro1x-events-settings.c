/*
 * pro1x-events-settings -- the control panel applet for the Events page of
 * Hildon's MeeGo-like look (tools/pro1x-events): which of its sections it
 * shows (weather, notifications, calendar, Mastodon), the place of the
 * weather, how many days of appointments, and the Mastodon account whose
 * home timeline it shows, its server and an access token.
 *
 * The place: a name and its coordinates, found by Search with Open-Meteo's
 * geocoding (geocoding-api.open-meteo.com, GeoNames' places, no key; GConf
 * geocoding_url takes another server), or typed in.
 *
 * The token comes from the account's web page (Preferences, Development,
 * New application, with the scope read:statuses: "Your access token"); it
 * is kept in GConf (/apps/pro1x/events/mastodon_token), readable by the
 * user's programs, as Hildon keeps its other accounts' settings. Saving
 * empty fields removes the account.
 */
#include <libintl.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>
#include <hildon/hildon.h>
#include <gconf/gconf-client.h>
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <hildon-cp-plugin/hildon-cp-plugin-interface.h>

#define DOMAIN "pro1x-events"
#define _(s) dgettext(DOMAIN, s)
#define N_(s) (s)
#define KEY_DIR "/apps/pro1x/events"
#define KEY_SERVER KEY_DIR "/mastodon_server"
#define KEY_TOKEN KEY_DIR "/mastodon_token"
#define KEY_PLACE KEY_DIR "/weather_place"
#define KEY_LAT KEY_DIR "/weather_latitude"
#define KEY_LON KEY_DIR "/weather_longitude"
#define KEY_CAL_DAYS KEY_DIR "/calendar_days"
#define KEY_GEOCODING_URL KEY_DIR "/geocoding_url"
#define GEOCODING_URL "https://geocoding-api.open-meteo.com/v1/search"

/* the page's sections, in its order */
static const struct {
	const char *key, *title;
} sections[] = {
	{ KEY_DIR "/show_weather", N_("Weather") },
	{ KEY_DIR "/show_notifications", N_("Notifications") },
	{ KEY_DIR "/show_calendar", N_("Calendar") },
	{ KEY_DIR "/show_mastodon", "Mastodon" },
};

static const int day_counts[] = { 1, 3, 7, 14 };

/* a wrapping label as wide as it is given, not as GTK guesses */
static void label_size_cb(GtkWidget *w, GtkAllocation *a, gpointer data)
{
	(void)data;
	if (w->requisition.width != a->width)
		gtk_widget_set_size_request(w, a->width, -1);
}

static GtkWidget *row(GtkSizeGroup *g, const char *title, GtkWidget *entry)
{
	GtkWidget *caption = hildon_caption_new(g, title, entry, NULL,
						HILDON_CAPTION_OPTIONAL);

	hildon_caption_set_separator(HILDON_CAPTION(caption), "");
	return caption;
}

static GtkWidget *note(const char *text)
{
	GtkWidget *label = gtk_label_new(text);

	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_misc_set_alignment(GTK_MISC(label), 0, 0);
	hildon_helper_set_logical_font(label, "SmallSystemFont");
	hildon_helper_set_logical_color(label, GTK_RC_FG, GTK_STATE_NORMAL,
					"SecondaryTextColor");
	g_signal_connect(label, "size-allocate", G_CALLBACK(label_size_cb), NULL);
	return label;
}

static GtkWidget *heading(const char *text)
{
	GtkWidget *label = gtk_label_new(text);

	gtk_misc_set_alignment(GTK_MISC(label), 0, 1);
	hildon_helper_set_logical_color(label, GTK_RC_FG, GTK_STATE_NORMAL,
					"SecondaryTextColor");
	return label;
}

static GtkWidget *entry_new(const char *placeholder)
{
	GtkWidget *e = hildon_entry_new(HILDON_SIZE_FINGER_HEIGHT);

	if (placeholder)
		hildon_gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
	hildon_gtk_entry_set_input_mode(GTK_ENTRY(e), HILDON_GTK_INPUT_MODE_FULL);
	return e;
}

static gboolean gconf_bool(GConfClient *gc, const char *key, gboolean def)
{
	GConfValue *v = gconf_client_get(gc, key, NULL);
	gboolean r = def;

	if (v) {
		if (v->type == GCONF_VALUE_BOOL)
			r = gconf_value_get_bool(v);
		gconf_value_free(v);
	}
	return r;
}

static gboolean gconf_number(GConfClient *gc, const char *key, double *r)
{
	GConfValue *v = gconf_client_get(gc, key, NULL);
	gboolean ok = FALSE;

	if (v) {
		if (v->type == GCONF_VALUE_FLOAT)
			*r = gconf_value_get_float(v), ok = TRUE;
		else if (v->type == GCONF_VALUE_INT)
			*r = gconf_value_get_int(v), ok = TRUE;
		gconf_value_free(v);
	}
	return ok;
}

static void set_number(GtkWidget *entry, double v)
{
	char buf[G_ASCII_DTOSTR_BUF_SIZE];

	gtk_entry_set_text(GTK_ENTRY(entry), g_ascii_formatd(buf, sizeof buf, "%.4f", v));
}

/* a coordinate as typed, with a point or a comma */
static gboolean get_number(GtkWidget *entry, double min, double max, double *r)
{
	char *s = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)))), *p, *end;
	gboolean ok;

	for (p = s; *p; p++)
		if (*p == ',')
			*p = '.';
	*r = g_ascii_strtod(s, &end);
	ok = *s && !*end && *r >= min && *r <= max;
	g_free(s);
	return ok;
}

/* --- the search: Open-Meteo's geocoding, in a thread --------------------- */

typedef struct {
	GtkWidget *dialog, *place, *lat, *lon, *button;
	GThread *thread;
	char *url;
	/* the answer */
	GPtrArray *names, *labels;	/* the places' names, with their region */
	GArray *coords;			/* latitude, longitude of each */
	char *error;
} Search;

static size_t curl_write(void *p, size_t size, size_t n, void *data)
{
	GByteArray *b = data;

	if (b->len + size * n > 1 << 20)
		return 0;
	g_byte_array_append(b, p, size * n);
	return size * n;
}

static const char *jstr(JsonObject *o, const char *name)
{
	JsonNode *n = o && json_object_has_member(o, name) ? json_object_get_member(o, name) : NULL;

	return n && JSON_NODE_TYPE(n) == JSON_NODE_VALUE &&
	       json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : NULL;
}

static gboolean jnum(JsonObject *o, const char *name, double *r)
{
	JsonNode *n = o && json_object_has_member(o, name) ? json_object_get_member(o, name) : NULL;
	GType t;

	if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE)
		return FALSE;
	t = json_node_get_value_type(n);
	if (t == G_TYPE_DOUBLE)
		*r = json_node_get_double(n);
	else if (t == G_TYPE_INT64)
		*r = json_node_get_int(n);
	else
		return FALSE;
	return TRUE;
}

static void parse_places(Search *s, const char *data, gsize len)
{
	JsonParser *jp = json_parser_new();
	JsonNode *root, *n = NULL;
	JsonObject *o;
	JsonArray *a;
	guint i;

	if (!json_parser_load_from_data(jp, data, len, NULL) ||
	    !(root = json_parser_get_root(jp)) || JSON_NODE_TYPE(root) != JSON_NODE_OBJECT) {
		s->error = g_strdup(_("Open-Meteo's answer was not understood."));
		g_object_unref(jp);
		return;
	}
	/* (no "results" at all: nothing found) */
	o = json_node_get_object(root);
	if (json_object_has_member(o, "results"))
		n = json_object_get_member(o, "results");
	a = n && JSON_NODE_TYPE(n) == JSON_NODE_ARRAY ? json_node_get_array(n) : NULL;
	for (i = 0; a && i < json_array_get_length(a); i++) {
		JsonObject *p = json_array_get_object_element(a, i);
		const char *name = jstr(p, "name"), *region = jstr(p, "admin1");
		const char *country = jstr(p, "country");
		GString *label;
		double ll[2];

		if (!name || !jnum(p, "latitude", &ll[0]) || !jnum(p, "longitude", &ll[1]))
			continue;
		label = g_string_new(name);
		if (region && *region && strcmp(region, name))
			g_string_append_printf(label, ", %s", region);
		if (country && *country)
			g_string_append_printf(label, ", %s", country);
		g_ptr_array_add(s->names, g_strdup(name));
		g_ptr_array_add(s->labels, g_string_free(label, FALSE));
		g_array_append_vals(s->coords, ll, 2);
	}
	g_object_unref(jp);
}

static gboolean search_done(gpointer data);

static gpointer search_thread(gpointer data)
{
	Search *s = data;
	CURL *c = curl_easy_init();
	GByteArray *b = g_byte_array_new();
	char errbuf[CURL_ERROR_SIZE] = "";
	long status = 0;
	CURLcode r = CURLE_FAILED_INIT;

	if (c) {
		curl_easy_setopt(c, CURLOPT_URL, s->url);
		curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
		curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
		curl_easy_setopt(c, CURLOPT_USERAGENT, "pro1x-events/1.1");
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
		curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
		r = curl_easy_perform(c);
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
		curl_easy_cleanup(c);
	}
	if (r != CURLE_OK)
		s->error = g_strdup_printf(_("No connection to Open-Meteo (%s)."),
					   *errbuf ? errbuf : curl_easy_strerror(r));
	else if (status != 200)
		s->error = g_strdup_printf(_("Open-Meteo answered %ld."), status);
	else
		parse_places(s, (const char *)b->data, b->len);
	g_byte_array_free(b, TRUE);
	/* the answer to the main loop */
	g_idle_add(search_done, s);
	return NULL;
}

static void take_place(Search *s, guint i)
{
	gtk_entry_set_text(GTK_ENTRY(s->place), g_ptr_array_index(s->names, i));
	set_number(s->lat, g_array_index(s->coords, double, 2 * i));
	set_number(s->lon, g_array_index(s->coords, double, 2 * i + 1));
}

static gboolean search_done(gpointer data)
{
	Search *s = data;

	/* (its thread is at its end) */
	g_thread_join(s->thread);
	s->thread = NULL;
	hildon_gtk_window_set_progress_indicator(GTK_WINDOW(s->dialog), 0);
	gtk_widget_set_sensitive(s->button, TRUE);
	if (s->error)
		hildon_banner_show_information(s->dialog, NULL, s->error);
	else if (!s->names->len)
		hildon_banner_show_information(s->dialog, NULL, _("No place of this name was found."));
	else if (s->names->len == 1)
		take_place(s, 0);
	else {
		GtkWidget *picker = hildon_picker_dialog_new(GTK_WINDOW(s->dialog));
		GtkWidget *sel = hildon_touch_selector_new_text();
		guint i;

		gtk_window_set_title(GTK_WINDOW(picker), _("Place"));
		for (i = 0; i < s->labels->len; i++)
			hildon_touch_selector_append_text(HILDON_TOUCH_SELECTOR(sel),
							  g_ptr_array_index(s->labels, i));
		hildon_picker_dialog_set_selector(HILDON_PICKER_DIALOG(picker),
						  HILDON_TOUCH_SELECTOR(sel));
		if (gtk_dialog_run(GTK_DIALOG(picker)) == GTK_RESPONSE_OK) {
			int a = hildon_touch_selector_get_active(HILDON_TOUCH_SELECTOR(sel), 0);

			if (a >= 0)
				take_place(s, a);
		}
		gtk_widget_destroy(picker);
	}
	return FALSE;
}

/* the language of the places' names: the user's */
static char *language(void)
{
	const char *const *l = g_get_language_names();
	const char *s = l && l[0] ? l[0] : "en";

	if (g_ascii_isalpha(s[0]) && g_ascii_isalpha(s[1]) && !g_ascii_isalpha(s[2]))
		return g_ascii_strdown(s, 2);
	return g_strdup("en");
}

static void search_cb(GtkWidget *button, gpointer data)
{
	Search *s = data;
	GConfClient *gc;
	char *name, *q, *base, *lang;
	CURL *c;

	(void)button;
	if (s->thread)
		return;
	name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(s->place))));
	if (!*name) {
		hildon_banner_show_information(s->dialog, NULL, _("Type the place's name first."));
		g_free(name);
		return;
	}
	c = curl_easy_init();
	q = c ? curl_easy_escape(c, name, 0) : NULL;
	if (c)
		curl_easy_cleanup(c);
	g_free(name);
	if (!q)
		return;
	gc = gconf_client_get_default();
	base = gconf_client_get_string(gc, KEY_GEOCODING_URL, NULL);
	g_object_unref(gc);
	if (base)
		g_strstrip(base);
	lang = language();
	g_free(s->url);
	s->url = g_strdup_printf("%s?name=%s&count=10&language=%s&format=json",
				 base && *base ? base : GEOCODING_URL, q, lang);
	curl_free(q);
	g_free(base), g_free(lang);
	g_ptr_array_set_size(s->names, 0);
	g_ptr_array_set_size(s->labels, 0);
	g_array_set_size(s->coords, 0);
	g_free(s->error);
	s->error = NULL;
	s->thread = g_thread_try_new("geocoding", search_thread, s, NULL);
	if (s->thread) {
		hildon_gtk_window_set_progress_indicator(GTK_WINDOW(s->dialog), 1);
		gtk_widget_set_sensitive(s->button, FALSE);
	}
}

/* --- the dialog ----------------------------------------------------------- */

osso_return_t execute(osso_context_t *osso, gpointer data, gboolean user_activated)
{
	GConfClient *gc = gconf_client_get_default();
	GtkSizeGroup *group = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
	GtkWidget *dialog, *pan, *box, *server, *token, *hbox, *days, *sel;
	GtkWidget *checks[G_N_ELEMENTS(sections)];
	const char *save = dgettext("hildon-libs", "wdgt_bd_save");
	Search s = { 0 };
	GSource *src;
	char *str;
	double v;
	guint i, best;
	int n;

	(void)osso, (void)user_activated;
	bindtextdomain(DOMAIN, LOCALEDIR);
	bind_textdomain_codeset(DOMAIN, "UTF-8");

	/* Hildon's own word, where its texts are installed */
	if (!strcmp(save, "wdgt_bd_save"))
		save = _("Save");
	dialog = gtk_dialog_new_with_buttons(_("Events"), GTK_WINDOW(data),
					     GTK_DIALOG_MODAL | GTK_DIALOG_NO_SEPARATOR,
					     save, GTK_RESPONSE_ACCEPT, NULL);
	box = gtk_vbox_new(FALSE, HILDON_MARGIN_DEFAULT);

	/* the sections */
	gtk_box_pack_start(GTK_BOX(box), heading(_("On the page")), FALSE, FALSE, 0);
	for (i = 0; i < G_N_ELEMENTS(sections); i++) {
		checks[i] = hildon_check_button_new(HILDON_SIZE_FINGER_HEIGHT | HILDON_SIZE_AUTO_WIDTH);
		gtk_button_set_label(GTK_BUTTON(checks[i]), _(sections[i].title));
		hildon_check_button_set_active(HILDON_CHECK_BUTTON(checks[i]),
					       gconf_bool(gc, sections[i].key, TRUE));
		gtk_box_pack_start(GTK_BOX(box), checks[i], FALSE, FALSE, 0);
	}

	/* the weather's place */
	gtk_box_pack_start(GTK_BOX(box), heading(_("Weather")), FALSE, FALSE, HILDON_MARGIN_DEFAULT);
	s.dialog = dialog;
	s.place = entry_new(_("Vienna"));
	s.lat = entry_new("48.2085");
	s.lon = entry_new("16.3721");
	hildon_gtk_entry_set_input_mode(GTK_ENTRY(s.lat), HILDON_GTK_INPUT_MODE_NUMERIC |
					HILDON_GTK_INPUT_MODE_SPECIAL);
	hildon_gtk_entry_set_input_mode(GTK_ENTRY(s.lon), HILDON_GTK_INPUT_MODE_NUMERIC |
					HILDON_GTK_INPUT_MODE_SPECIAL);
	s.names = g_ptr_array_new_with_free_func(g_free);
	s.labels = g_ptr_array_new_with_free_func(g_free);
	s.coords = g_array_new(FALSE, FALSE, sizeof(double));
	str = gconf_client_get_string(gc, KEY_PLACE, NULL);
	if (str)
		gtk_entry_set_text(GTK_ENTRY(s.place), str);
	g_free(str);
	if (gconf_number(gc, KEY_LAT, &v))
		set_number(s.lat, v);
	if (gconf_number(gc, KEY_LON, &v))
		set_number(s.lon, v);
	s.button = hildon_gtk_button_new(HILDON_SIZE_FINGER_HEIGHT | HILDON_SIZE_AUTO_WIDTH);
	gtk_button_set_label(GTK_BUTTON(s.button), _("Search"));
	g_signal_connect(s.button, "clicked", G_CALLBACK(search_cb), &s);
	hbox = gtk_hbox_new(FALSE, HILDON_MARGIN_DEFAULT);
	gtk_box_pack_start(GTK_BOX(hbox), row(group, _("Place"), s.place), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), s.button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), hbox, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), row(group, _("Latitude"), s.lat), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), row(group, _("Longitude"), s.lon), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box),
			   note(_("Search finds the place's coordinates, or type them in. "
				  "Weather data by Open-Meteo.com (CC BY 4.0), places by "
				  "GeoNames.")), FALSE, FALSE, 0);

	/* the calendar's days */
	gtk_box_pack_start(GTK_BOX(box), heading(_("Calendar")), FALSE, FALSE, HILDON_MARGIN_DEFAULT);
	sel = hildon_touch_selector_new_text();
	hildon_touch_selector_append_text(HILDON_TOUCH_SELECTOR(sel), _("Today"));
	hildon_touch_selector_append_text(HILDON_TOUCH_SELECTOR(sel), _("3 days"));
	hildon_touch_selector_append_text(HILDON_TOUCH_SELECTOR(sel), _("1 week"));
	hildon_touch_selector_append_text(HILDON_TOUCH_SELECTOR(sel), _("2 weeks"));
	days = hildon_picker_button_new(HILDON_SIZE_FINGER_HEIGHT, HILDON_BUTTON_ARRANGEMENT_VERTICAL);
	hildon_button_set_title(HILDON_BUTTON(days), _("Appointments of"));
	hildon_picker_button_set_selector(HILDON_PICKER_BUTTON(days), HILDON_TOUCH_SELECTOR(sel));
	n = gconf_client_get_int(gc, KEY_CAL_DAYS, NULL);
	if (n <= 0)
		n = 7;
	for (best = 0, i = 1; i < G_N_ELEMENTS(day_counts); i++)
		if (abs(day_counts[i] - n) < abs(day_counts[best] - n))
			best = i;
	hildon_picker_button_set_active(HILDON_PICKER_BUTTON(days), best);
	gtk_box_pack_start(GTK_BOX(box), days, FALSE, FALSE, 0);

	/* the Mastodon account */
	gtk_box_pack_start(GTK_BOX(box), heading("Mastodon"), FALSE, FALSE, HILDON_MARGIN_DEFAULT);
	server = entry_new("mastodon.social");
	token = hildon_entry_new(HILDON_SIZE_FINGER_HEIGHT);
	gtk_entry_set_visibility(GTK_ENTRY(token), FALSE);
	hildon_gtk_entry_set_input_mode(GTK_ENTRY(token), HILDON_GTK_INPUT_MODE_FULL |
					HILDON_GTK_INPUT_MODE_INVISIBLE);
	str = gconf_client_get_string(gc, KEY_SERVER, NULL);
	if (str)
		gtk_entry_set_text(GTK_ENTRY(server), str);
	g_free(str);
	str = gconf_client_get_string(gc, KEY_TOKEN, NULL);
	if (str)
		gtk_entry_set_text(GTK_ENTRY(token), str);
	g_free(str);
	gtk_box_pack_start(GTK_BOX(box), row(group, _("Mastodon server"), server), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), row(group, _("Access token"), token), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box),
			   note(_("The Events page shows the home timeline of this Mastodon "
				  "account. Its access token: on the account's web page, "
				  "Preferences, Development, New application, with the scope "
				  "read:statuses, then \"Your access token\".")),
			   FALSE, FALSE, HILDON_MARGIN_DEFAULT);

	/* all of it panned, as high as most of the screen */
	pan = hildon_pannable_area_new();
	g_object_set(pan, "hscrollbar-policy", GTK_POLICY_NEVER, NULL);
	hildon_pannable_area_add_with_viewport(HILDON_PANNABLE_AREA(pan), box);
	gtk_widget_set_size_request(pan, -1,
				    gdk_screen_get_height(gtk_widget_get_screen(dialog)) * 3 / 5);
	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), pan, TRUE, TRUE, 0);
	gtk_widget_show_all(dialog);

	while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		char *ns = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(server))));
		char *nt = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(token))));
		char *np = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(s.place))));
		double la = 0, lo = 0;
		gboolean coords = get_number(s.lat, -90, 90, &la) &&
				  get_number(s.lon, -180, 180, &lo);
		int a;

		if (*np && !coords) {
			hildon_banner_show_information(dialog, NULL,
						       _("The place needs its coordinates: Search "
							 "finds them."));
			g_free(ns), g_free(nt), g_free(np);
			continue;
		}
		for (i = 0; i < G_N_ELEMENTS(sections); i++)
			gconf_client_set_bool(gc, sections[i].key,
					      hildon_check_button_get_active(
						      HILDON_CHECK_BUTTON(checks[i])), NULL);
		a = hildon_picker_button_get_active(HILDON_PICKER_BUTTON(days));
		gconf_client_set_int(gc, KEY_CAL_DAYS,
				     day_counts[CLAMP(a, 0, (int)G_N_ELEMENTS(day_counts) - 1)], NULL);
		/* (the place last: the page takes all three when it changes) */
		if (*np) {
			gconf_client_set_float(gc, KEY_LAT, la, NULL);
			gconf_client_set_float(gc, KEY_LON, lo, NULL);
			gconf_client_set_string(gc, KEY_PLACE, np, NULL);
		} else {
			gconf_client_unset(gc, KEY_LAT, NULL);
			gconf_client_unset(gc, KEY_LON, NULL);
			gconf_client_unset(gc, KEY_PLACE, NULL);
		}
		if (*ns && *nt) {
			gconf_client_set_string(gc, KEY_SERVER, ns, NULL);
			gconf_client_set_string(gc, KEY_TOKEN, nt, NULL);
		} else {
			gconf_client_unset(gc, KEY_TOKEN, NULL);
			gconf_client_unset(gc, KEY_SERVER, NULL);
		}
		g_free(ns), g_free(nt), g_free(np);
		break;
	}
	/* a search still running ends here, before the applet is unloaded,
	 * and its answer is not shown */
	if (s.thread)
		g_thread_join(s.thread);
	while ((src = g_main_context_find_source_by_user_data(NULL, &s)))
		g_source_destroy(src);
	gtk_widget_destroy(dialog);
	g_ptr_array_unref(s.names);
	g_ptr_array_unref(s.labels);
	g_array_free(s.coords, TRUE);
	g_free(s.url), g_free(s.error);
	g_object_unref(group);
	g_object_unref(gc);
	return OSSO_OK;
}
