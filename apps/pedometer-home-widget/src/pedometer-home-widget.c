/*
 * Pedometer Home Widget for Maemo Leste on the F(x)tec Pro1X.
 *
 * A port of Andrei Mirestean's Pedometer Home Widget for Maemo 5
 * (pedometerhomewidget 0.3-4, GPL-3.0-or-later): the same figures, settings
 * and formulas for distance and calories. The N900 widget counted the steps
 * itself and read the accelerometer every 25 ms to do so; on the Pro1X the
 * sensor core counts them in hardware and pro1x-steps offers the count on
 * the system bus (org.pro1x.Steps, at most one signal every few seconds).
 * The widget listens only while it is on the visible desktop; otherwise it
 * reads the count when it comes back into view and just before midnight,
 * so it does not wake the phone while it is in the pocket.
 *
 * Tapping the widget shows the last seven days.
 *
 * Copyright (C) 2010 Andrei Mirestean (original widget)
 * Copyright (C) 2026 the pro1x-maemo-leste authors (this port)
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <hildon/hildon.h>
#include <libhildondesktop/libhildondesktop.h>

#define STEPS_NAME "org.pro1x.Steps"
#define STEPS_PATH "/org/pro1x/Steps"
#define SAVE_DELAY_S 60
#define DAYS_SHOWN 7

enum { UNIT_METRIC, UNIT_IMPERIAL };
enum { MODE_WALK, MODE_RUN };
enum { VIEW_WEEK, VIEW_7DAYS, VIEW_ALLTIME, N_VIEWS };
#define HEIGHT_CUSTOM 5

static const char *const view_titles[N_VIEWS] = { "This week", "Last 7 days", "All time" };
/* step length per height class, from the original widget */
static const double step_lengths[] = { 0.59, 0.64, 0.71, 0.77, 0.83 };
static const char *const heights_metric[] = {
	"< 1.50 m", "1.50 - 1.65 m", "1.66 - 1.80 m", "1.81 - 1.95 m", "> 1.95 m", "Custom step length"
};
static const char *const heights_imperial[] = {
	"< 5 ft", "5 - 5.5 ft", "5.5 - 6 ft", "6 - 6.5 ft", "> 6.5 ft", "Custom step length"
};

typedef struct {
	HDHomePluginItem parent;

	/* settings */
	int unit, mode, height, view, goal;
	double custom_step;	/* metres */
	double weight;		/* kg or lb, after the unit */

	/* steps per day, "YYYY-MM-DD" -> guint64 */
	GHashTable *days;
	guint64 last_total;	/* the counter at the last reading */
	char *boot_id;		/* ... and the boot it belongs to */

	GDBusConnection *bus;
	guint signal_id;
	guint midnight_id;
	guint save_id;
	gboolean available;

	GtkWidget *title, *view_head;
	GtkWidget *val[2][3];	/* [today|view][steps|distance|calories] */
	GtkWidget *goal_label;
} PedometerHomePlugin;

typedef struct {
	HDHomePluginItemClass parent_class;
} PedometerHomePluginClass;

HD_DEFINE_PLUGIN_MODULE(PedometerHomePlugin, pedometer_home_plugin, HD_TYPE_HOME_PLUGIN_ITEM)

/* --- files ------------------------------------------------------------------------ */

static char *config_path(const char *name)
{
	char *dir = g_build_filename(g_get_user_config_dir(), "pedometer-home-widget", NULL);
	char *path;

	g_mkdir_with_parents(dir, 0755);
	path = g_build_filename(dir, name, NULL);
	g_free(dir);
	return path;
}

static char *read_boot_id(void)
{
	char *s = NULL;

	if (g_file_get_contents("/proc/sys/kernel/random/boot_id", &s, NULL, NULL))
		g_strstrip(s);
	return s ? s : g_strdup("");
}

static void load_settings(PedometerHomePlugin *self)
{
	char *path = config_path("settings.ini");
	GKeyFile *kf = g_key_file_new();

	self->unit = UNIT_METRIC;
	self->mode = MODE_WALK;
	self->height = 2;
	self->view = VIEW_WEEK;
	self->goal = 0;
	self->custom_step = 0.7;
	self->weight = 70;
	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
		self->unit = CLAMP(g_key_file_get_integer(kf, "settings", "unit", NULL), 0, 1);
		self->mode = CLAMP(g_key_file_get_integer(kf, "settings", "mode", NULL), 0, 1);
		self->height = CLAMP(g_key_file_get_integer(kf, "settings", "height", NULL), 0, HEIGHT_CUSTOM);
		self->view = CLAMP(g_key_file_get_integer(kf, "settings", "view", NULL), 0, N_VIEWS - 1);
		self->goal = MAX(g_key_file_get_integer(kf, "settings", "goal", NULL), 0);
		if (g_key_file_has_key(kf, "settings", "step_length", NULL))
			self->custom_step = g_key_file_get_double(kf, "settings", "step_length", NULL);
		if (g_key_file_has_key(kf, "settings", "weight", NULL))
			self->weight = g_key_file_get_double(kf, "settings", "weight", NULL);
	}
	g_key_file_free(kf);
	g_free(path);
}

static void save_settings(PedometerHomePlugin *self)
{
	char *path = config_path("settings.ini");
	GKeyFile *kf = g_key_file_new();

	g_key_file_set_integer(kf, "settings", "unit", self->unit);
	g_key_file_set_integer(kf, "settings", "mode", self->mode);
	g_key_file_set_integer(kf, "settings", "height", self->height);
	g_key_file_set_integer(kf, "settings", "view", self->view);
	g_key_file_set_integer(kf, "settings", "goal", self->goal);
	g_key_file_set_double(kf, "settings", "step_length", self->custom_step);
	g_key_file_set_double(kf, "settings", "weight", self->weight);
	g_key_file_save_to_file(kf, path, NULL);
	g_key_file_free(kf);
	g_free(path);
}

/* history: one line "YYYY-MM-DD steps" per day; state: "boot_id counter" */
static void load_history(PedometerHomePlugin *self)
{
	char *path = config_path("history"), *text = NULL;

	if (g_file_get_contents(path, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);
		for (char **l = lines; *l; l++) {
			char day[16];
			guint64 steps;
			if (sscanf(*l, "%15s %" G_GUINT64_FORMAT, day, &steps) == 2)
				g_hash_table_insert(self->days, g_strdup(day), g_memdup2(&steps, sizeof(steps)));
		}
		g_strfreev(lines);
		g_free(text);
	}
	g_free(path);

	path = config_path("state");
	if (g_file_get_contents(path, &text, NULL, NULL)) {
		char id[64];
		guint64 total;
		if (sscanf(text, "%63s %" G_GUINT64_FORMAT, id, &total) == 2) {
			self->boot_id = g_strdup(id);
			self->last_total = total;
		}
		g_free(text);
	}
	g_free(path);
}

static gint compare_days(gconstpointer a, gconstpointer b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static gboolean save_history(gpointer data)
{
	PedometerHomePlugin *self = data;
	GString *s = g_string_new(NULL);
	GPtrArray *keys = g_ptr_array_new();
	GHashTableIter it;
	gpointer k, v;
	char *path, *state;

	self->save_id = 0;
	g_hash_table_iter_init(&it, self->days);
	while (g_hash_table_iter_next(&it, &k, &v))
		g_ptr_array_add(keys, k);
	g_ptr_array_sort(keys, compare_days);
	for (guint i = 0; i < keys->len; i++)
		g_string_append_printf(s, "%s %" G_GUINT64_FORMAT "\n", (char *)keys->pdata[i],
				       *(guint64 *)g_hash_table_lookup(self->days, keys->pdata[i]));
	g_ptr_array_free(keys, TRUE);

	path = config_path("history");
	g_file_set_contents(path, s->str, -1, NULL);
	g_free(path);
	g_string_free(s, TRUE);

	path = config_path("state");
	state = g_strdup_printf("%s %" G_GUINT64_FORMAT "\n", self->boot_id ? self->boot_id : "", self->last_total);
	g_file_set_contents(path, state, -1, NULL);
	g_free(state);
	g_free(path);
	return G_SOURCE_REMOVE;
}

static void save_later(PedometerHomePlugin *self)
{
	if (!self->save_id)
		self->save_id = g_timeout_add_seconds(SAVE_DELAY_S, save_history, self);
}

/* --- figures ------------------------------------------------------------------------ */

static char *day_key(GDateTime *t)
{
	return g_date_time_format(t, "%Y-%m-%d");
}

static guint64 steps_on(PedometerHomePlugin *self, GDateTime *t)
{
	char *key = day_key(t);
	guint64 *v = g_hash_table_lookup(self->days, key);

	g_free(key);
	return v ? *v : 0;
}

static double step_length(PedometerHomePlugin *self)
{
	double l = self->height == HEIGHT_CUSTOM ? self->custom_step : step_lengths[self->height];
	return self->mode == MODE_RUN ? l * 1.45 : l;
}

static double distance_m(PedometerHomePlugin *self, guint64 steps)
{
	return step_length(self) * steps;
}

/* the original's formula: pounds x miles x 0.53 walking, 0.75 running */
static double calories(PedometerHomePlugin *self, guint64 steps)
{
	double miles = distance_m(self, steps) * 0.000621371192;
	double pounds = self->unit == UNIT_METRIC ? self->weight * 2.20462262 : self->weight;
	return pounds * miles * (self->mode == MODE_RUN ? 0.75 : 0.53);
}

static char *format_distance(PedometerHomePlugin *self, double m)
{
	if (self->unit == UNIT_METRIC)
		return m > 1000 ? g_strdup_printf("%.2f km", m / 1000) : g_strdup_printf("%d m", (int)m);
	return m > 1609.344 ? g_strdup_printf("%.2f mi", m / 1609.344) : g_strdup_printf("%d ft", (int)(m * 3.2808));
}

static guint64 view_steps(PedometerHomePlugin *self)
{
	GDateTime *now = g_date_time_new_now_local();
	guint64 sum = 0;

	if (self->view == VIEW_ALLTIME) {
		GHashTableIter it;
		gpointer v;
		g_hash_table_iter_init(&it, self->days);
		while (g_hash_table_iter_next(&it, NULL, &v))
			sum += *(guint64 *)v;
	} else {
		int n = self->view == VIEW_7DAYS ? DAYS_SHOWN : g_date_time_get_day_of_week(now);
		for (int i = 0; i < n; i++) {
			GDateTime *d = g_date_time_add_days(now, -i);
			sum += steps_on(self, d);
			g_date_time_unref(d);
		}
	}
	g_date_time_unref(now);
	return sum;
}

static void set_value(GtkWidget *label, const char *text)
{
	char *m = g_markup_printf_escaped("<span size='large'>%s</span>", text);
	gtk_label_set_markup(GTK_LABEL(label), m);
	g_free(m);
}

static void update_labels(PedometerHomePlugin *self)
{
	GDateTime *now = g_date_time_new_now_local();
	guint64 steps[2] = { steps_on(self, now), view_steps(self) };

	g_date_time_unref(now);
	for (int c = 0; c < 2; c++) {
		char *s = g_strdup_printf("%" G_GUINT64_FORMAT, steps[c]);
		char *d = format_distance(self, distance_m(self, steps[c]));
		char *k = g_strdup_printf("%.0f kcal", calories(self, steps[c]));
		set_value(self->val[c][0], s);
		set_value(self->val[c][1], d);
		set_value(self->val[c][2], k);
		g_free(s);
		g_free(d);
		g_free(k);
	}
	gtk_label_set_text(GTK_LABEL(self->view_head), view_titles[self->view]);
	if (!self->available) {
		gtk_label_set_text(GTK_LABEL(self->goal_label), "No step counter (pro1x-steps)");
	} else if (self->goal > 0) {
		char *g = g_strdup_printf("%d %% of %d steps", (int)(100 * steps[0] / self->goal), self->goal);
		gtk_label_set_text(GTK_LABEL(self->goal_label), g);
		g_free(g);
	} else {
		gtk_label_set_text(GTK_LABEL(self->goal_label), "");
	}
}

/* --- the step counter ---------------------------------------------------------------- */

/* Books the steps since the last reading on today. */
static void count(PedometerHomePlugin *self, guint64 total)
{
	char *boot = read_boot_id();
	guint64 delta;

	/* a new boot starts the counter over: all of it is new */
	if (!self->boot_id || strcmp(boot, self->boot_id) != 0 || total < self->last_total)
		delta = self->boot_id ? total : 0;
	else
		delta = total - self->last_total;
	g_free(self->boot_id);
	self->boot_id = boot;
	self->last_total = total;

	if (delta > 0) {
		GDateTime *now = g_date_time_new_now_local();
		char *key = day_key(now);
		guint64 v = steps_on(self, now) + delta;
		g_hash_table_replace(self->days, key, g_memdup2(&v, sizeof(v)));
		g_date_time_unref(now);
	}
	save_later(self);
}

/* Reads the counter and books its steps; FALSE without pro1x-steps. */
static gboolean fetch(PedometerHomePlugin *self)
{
	GVariant *r = NULL;

	if (self->bus)
		r = g_dbus_connection_call_sync(self->bus, STEPS_NAME, STEPS_PATH, STEPS_NAME, "GetSteps",
						NULL, G_VARIANT_TYPE("(t)"), G_DBUS_CALL_FLAGS_NONE, 2000,
						NULL, NULL);
	if (!r)
		return FALSE;
	guint64 total;
	g_variant_get(r, "(t)", &total);
	g_variant_unref(r);
	count(self, total);
	return TRUE;
}

static void read_counter(PedometerHomePlugin *self)
{
	self->available = fetch(self);
	update_labels(self);
}

static void steps_changed_cb(GDBusConnection *bus, const char *sender, const char *path,
			     const char *iface, const char *signal, GVariant *params, gpointer data)
{
	PedometerHomePlugin *self = data;
	guint64 total;

	(void)bus, (void)sender, (void)path, (void)iface, (void)signal;
	g_variant_get(params, "(t)", &total);
	self->available = TRUE;
	count(self, total);
	update_labels(self);
}

static void listen(PedometerHomePlugin *self, gboolean on)
{
	if (on && !self->signal_id && self->bus)
		self->signal_id = g_dbus_connection_signal_subscribe(self->bus, NULL, STEPS_NAME, "StepsChanged",
								     STEPS_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
								     steps_changed_cb, self, NULL);
	else if (!on && self->signal_id) {
		g_dbus_connection_signal_unsubscribe(self->bus, self->signal_id);
		self->signal_id = 0;
	}
}

static gboolean midnight_cb(gpointer data);

/* One wakeup a day: read the counter just before midnight, so that the
 * day's steps stay on that day even when nobody looked at the desktop. */
static void schedule_midnight(PedometerHomePlugin *self)
{
	GDateTime *now = g_date_time_new_now_local();
	GDateTime *today = g_date_time_new_local(g_date_time_get_year(now), g_date_time_get_month(now),
						  g_date_time_get_day_of_month(now), 23, 59, 50);
	GTimeSpan left = g_date_time_difference(today, now);

	if (left <= 0)
		left += G_TIME_SPAN_DAY;
	self->midnight_id = g_timeout_add_seconds((guint)(left / G_TIME_SPAN_SECOND) + 1, midnight_cb, self);
	g_date_time_unref(today);
	g_date_time_unref(now);
}

static gboolean midnight_cb(gpointer data)
{
	PedometerHomePlugin *self = data;

	read_counter(self);
	save_history(self);
	schedule_midnight(self);
	return G_SOURCE_REMOVE;
}

static void on_current_desktop_cb(GObject *obj, GParamSpec *pspec, gpointer data)
{
	PedometerHomePlugin *self = data;
	gboolean on = FALSE;

	(void)pspec;
	g_object_get(obj, "is-on-current-desktop", &on, NULL);
	listen(self, on);
	if (on)
		read_counter(self);
}

/* --- history -------------------------------------------------------------------------- */

static gboolean history_expose_cb(GtkWidget *area, GdkEventExpose *ev, gpointer data)
{
	PedometerHomePlugin *self = data;
	cairo_t *cr = gdk_cairo_create(area->window);
	int w = area->allocation.width, h = area->allocation.height;
	GDateTime *now = g_date_time_new_now_local();
	guint64 steps[DAYS_SHOWN], max = 1;
	double bar = (double)w / DAYS_SHOWN;

	(void)ev;
	for (int i = 0; i < DAYS_SHOWN; i++) {
		GDateTime *d = g_date_time_add_days(now, i - (DAYS_SHOWN - 1));
		steps[i] = steps_on(self, d);
		max = MAX(max, steps[i]);
		g_date_time_unref(d);
	}
	if (self->goal > 0)
		max = MAX(max, (guint64)self->goal);

	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 18);
	for (int i = 0; i < DAYS_SHOWN; i++) {
		GDateTime *d = g_date_time_add_days(now, i - (DAYS_SHOWN - 1));
		char *name = g_date_time_format(d, "%a");
		char *num = g_strdup_printf("%" G_GUINT64_FORMAT, steps[i]);
		double bh = (h - 60) * (double)steps[i] / max;
		cairo_text_extents_t te;

		cairo_set_source_rgb(cr, 0.2, 0.6, 1.0);
		cairo_rectangle(cr, i * bar + 8, h - 30 - bh, bar - 16, bh);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_text_extents(cr, name, &te);
		cairo_move_to(cr, i * bar + (bar - te.width) / 2, h - 8);
		cairo_show_text(cr, name);
		cairo_text_extents(cr, num, &te);
		cairo_move_to(cr, i * bar + (bar - te.width) / 2, h - 36 - bh);
		cairo_show_text(cr, num);
		g_free(name);
		g_free(num);
		g_date_time_unref(d);
	}
	if (self->goal > 0) {
		double gy = h - 30 - (h - 60) * (double)self->goal / max;
		cairo_set_source_rgba(cr, 1, 0.6, 0.2, 0.9);
		cairo_set_line_width(cr, 2);
		cairo_move_to(cr, 0, gy);
		cairo_line_to(cr, w, gy);
		cairo_stroke(cr);
	}
	g_date_time_unref(now);
	cairo_destroy(cr);
	return TRUE;
}

static gboolean show_history_cb(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
	PedometerHomePlugin *self = data;
	GtkWidget *dlg, *area;

	(void)w, (void)ev;
	read_counter(self);
	dlg = gtk_dialog_new_with_buttons("Pedometer: last 7 days", NULL, GTK_DIALOG_MODAL | GTK_DIALOG_NO_SEPARATOR,
					  NULL, NULL);
	area = gtk_drawing_area_new();
	gtk_widget_set_size_request(area, -1, 300);
	g_signal_connect(area, "expose-event", G_CALLBACK(history_expose_cb), self);
	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), area, TRUE, TRUE, 0);
	gtk_widget_show_all(dlg);
	gtk_dialog_run(GTK_DIALOG(dlg));
	gtk_widget_destroy(dlg);
	return TRUE;
}

/* --- settings ------------------------------------------------------------------------- */

static GtkWidget *picker(const char *title, const char *const *items, int n, int active)
{
	GtkWidget *b = hildon_picker_button_new(HILDON_SIZE_FINGER_HEIGHT, HILDON_BUTTON_ARRANGEMENT_VERTICAL);
	GtkWidget *sel = hildon_touch_selector_new_text();

	for (int i = 0; i < n; i++)
		hildon_touch_selector_append_text(HILDON_TOUCH_SELECTOR(sel), items[i]);
	hildon_picker_button_set_selector(HILDON_PICKER_BUTTON(b), HILDON_TOUCH_SELECTOR(sel));
	hildon_button_set_title(HILDON_BUTTON(b), title);
	hildon_picker_button_set_active(HILDON_PICKER_BUTTON(b), active);
	return b;
}

static GtkWidget *entry_row(const char *title, const char *value, GtkWidget **entry)
{
	GtkWidget *box = gtk_hbox_new(FALSE, 8);

	*entry = hildon_entry_new(HILDON_SIZE_FINGER_HEIGHT);
	hildon_gtk_entry_set_input_mode(GTK_ENTRY(*entry), HILDON_GTK_INPUT_MODE_NUMERIC);
	gtk_entry_set_text(GTK_ENTRY(*entry), value);
	gtk_box_pack_start(GTK_BOX(box), gtk_label_new(title), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), *entry, TRUE, TRUE, 0);
	return box;
}

static void show_settings_cb(HDHomePluginItem *item, gpointer data)
{
	PedometerHomePlugin *self = data;
	static const char *const units[] = { "Metric (kg/km)", "English (lb/mi)" };
	static const char *const modes[] = { "Walking", "Running" };
	GtkWidget *dlg, *vbox, *pan, *unit, *mode, *height, *view, *weight, *step, *goal;
	char buf[32];

	(void)item;
	dlg = gtk_dialog_new_with_buttons("Settings", NULL, GTK_DIALOG_MODAL | GTK_DIALOG_NO_SEPARATOR,
					  "Save", GTK_RESPONSE_OK, NULL);
	vbox = gtk_vbox_new(FALSE, 4);
	unit = picker("Unit", units, 2, self->unit);
	mode = picker("Mode", modes, 2, self->mode);
	height = picker("Height", self->unit == UNIT_METRIC ? heights_metric : heights_imperial,
			HEIGHT_CUSTOM + 1, self->height);
	view = picker("Second column", view_titles, N_VIEWS, self->view);
	gtk_box_pack_start(GTK_BOX(vbox), unit, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), mode, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), height, FALSE, FALSE, 0);
	g_snprintf(buf, sizeof(buf), "%.2f", self->unit == UNIT_METRIC ? self->custom_step : self->custom_step * 3.2808);
	gtk_box_pack_start(GTK_BOX(vbox), entry_row(self->unit == UNIT_METRIC ? "Custom step length (m)" :
						    "Custom step length (ft)", buf, &step), FALSE, FALSE, 0);
	g_snprintf(buf, sizeof(buf), "%.0f", self->weight);
	gtk_box_pack_start(GTK_BOX(vbox), entry_row(self->unit == UNIT_METRIC ? "Weight (kg)" : "Weight (lb)",
						    buf, &weight), FALSE, FALSE, 0);
	g_snprintf(buf, sizeof(buf), "%d", self->goal);
	gtk_box_pack_start(GTK_BOX(vbox), entry_row("Daily goal (steps, 0 = none)", buf, &goal), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), view, FALSE, FALSE, 0);

	pan = hildon_pannable_area_new();
	hildon_pannable_area_add_with_viewport(HILDON_PANNABLE_AREA(pan), vbox);
	gtk_widget_set_size_request(pan, -1, 340);
	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), pan, TRUE, TRUE, 0);
	gtk_widget_show_all(dlg);

	if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
		int new_unit = hildon_picker_button_get_active(HILDON_PICKER_BUTTON(unit));
		double w = g_ascii_strtod(gtk_entry_get_text(GTK_ENTRY(weight)), NULL);
		double s = g_ascii_strtod(gtk_entry_get_text(GTK_ENTRY(step)), NULL);

		/* the entries were shown in the old unit */
		if (self->unit == UNIT_IMPERIAL)
			s /= 3.2808;
		if (new_unit != self->unit)
			w = new_unit == UNIT_METRIC ? w / 2.20462262 : w * 2.20462262;
		self->unit = new_unit;
		self->mode = hildon_picker_button_get_active(HILDON_PICKER_BUTTON(mode));
		self->height = MAX(hildon_picker_button_get_active(HILDON_PICKER_BUTTON(height)), 0);
		self->view = MAX(hildon_picker_button_get_active(HILDON_PICKER_BUTTON(view)), 0);
		if (w > 0)
			self->weight = w;
		if (s > 0.2 && s < 3)
			self->custom_step = s;
		else if (self->height == HEIGHT_CUSTOM)
			hildon_banner_show_information(GTK_WIDGET(self), NULL, "Invalid step length");
		self->goal = MAX(atoi(gtk_entry_get_text(GTK_ENTRY(goal))), 0);
		save_settings(self);
		update_labels(self);
	}
	gtk_widget_destroy(dlg);
}

/* --- the widget ------------------------------------------------------------------------ */

static gboolean expose_cb(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
	cairo_t *cr = gdk_cairo_create(w->window);
	double r = 12, x = 0, y = 0, wd = w->allocation.width, ht = w->allocation.height;

	(void)ev, (void)data;
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + wd - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + wd - r, y + ht - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + ht - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
	cairo_fill(cr);
	cairo_destroy(cr);
	return FALSE;
}

static GtkWidget *head(const char *text)
{
	GtkWidget *l = gtk_label_new(NULL);
	char *m = g_markup_printf_escaped("<span size='small' foreground='#9cf'>%s</span>", text);

	gtk_label_set_markup(GTK_LABEL(l), m);
	gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
	g_free(m);
	return l;
}

static void pedometer_home_plugin_init(PedometerHomePlugin *self)
{
	static const char *const rows[] = { "Steps", "Distance", "Calories" };
	GdkColormap *rgba = gdk_screen_get_rgba_colormap(gtk_widget_get_screen(GTK_WIDGET(self)));
	GtkWidget *ebox, *vbox, *table;

	self->days = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	load_settings(self);
	load_history(self);
	self->bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);

	if (rgba)
		gtk_widget_set_colormap(GTK_WIDGET(self), rgba);
	gtk_widget_set_app_paintable(GTK_WIDGET(self), TRUE);
	g_signal_connect(self, "expose-event", G_CALLBACK(expose_cb), NULL);

	ebox = gtk_event_box_new();
	gtk_event_box_set_visible_window(GTK_EVENT_BOX(ebox), FALSE);
	g_signal_connect(ebox, "button-release-event", G_CALLBACK(show_history_cb), self);
	vbox = gtk_vbox_new(FALSE, 2);
	gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

	self->title = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(self->title), "<b>Pedometer</b>");
	gtk_misc_set_alignment(GTK_MISC(self->title), 0, 0.5);
	gtk_box_pack_start(GTK_BOX(vbox), self->title, FALSE, FALSE, 0);

	table = gtk_table_new(4, 3, FALSE);
	gtk_table_set_col_spacings(GTK_TABLE(table), 18);
	gtk_table_attach_defaults(GTK_TABLE(table), head("Today"), 1, 2, 0, 1);
	self->view_head = head(view_titles[self->view]);
	gtk_table_attach_defaults(GTK_TABLE(table), self->view_head, 2, 3, 0, 1);
	for (int r = 0; r < 3; r++) {
		gtk_table_attach_defaults(GTK_TABLE(table), head(rows[r]), 0, 1, r + 1, r + 2);
		for (int c = 0; c < 2; c++) {
			self->val[c][r] = gtk_label_new("-");
			gtk_misc_set_alignment(GTK_MISC(self->val[c][r]), 0, 0.5);
			gtk_table_attach_defaults(GTK_TABLE(table), self->val[c][r], c + 1, c + 2, r + 1, r + 2);
		}
	}
	gtk_box_pack_start(GTK_BOX(vbox), table, FALSE, FALSE, 0);
	self->goal_label = gtk_label_new(NULL);
	gtk_misc_set_alignment(GTK_MISC(self->goal_label), 0, 0.5);
	gtk_box_pack_start(GTK_BOX(vbox), self->goal_label, FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(ebox), vbox);
	gtk_container_add(GTK_CONTAINER(self), ebox);
	gtk_widget_show_all(ebox);

	hd_home_plugin_item_set_settings(HD_HOME_PLUGIN_ITEM(self), TRUE);
	g_signal_connect(self, "show-settings", G_CALLBACK(show_settings_cb), self);
	g_signal_connect(self, "notify::is-on-current-desktop", G_CALLBACK(on_current_desktop_cb), self);

	read_counter(self);
	schedule_midnight(self);
}

static void pedometer_home_plugin_finalize(GObject *obj)
{
	PedometerHomePlugin *self = (PedometerHomePlugin *)obj;

	listen(self, FALSE);
	if (self->midnight_id)
		g_source_remove(self->midnight_id);
	if (self->save_id)
		g_source_remove(self->save_id);
	fetch(self);
	save_history(self);
	g_hash_table_destroy(self->days);
	g_free(self->boot_id);
	if (self->bus)
		g_object_unref(self->bus);
	G_OBJECT_CLASS(pedometer_home_plugin_parent_class)->finalize(obj);
}

static void pedometer_home_plugin_class_init(PedometerHomePluginClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = pedometer_home_plugin_finalize;
}

static void pedometer_home_plugin_class_finalize(PedometerHomePluginClass *klass)
{
	(void)klass;
}
