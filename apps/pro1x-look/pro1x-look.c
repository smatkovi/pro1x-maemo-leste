/*
 * pro1x-look -- Hildon's control panel applet for the desktop's style.
 *
 *   Maemo   Hildon as on the N900: the launcher and the task switcher
 *           behind the buttons in the top left corner
 *   MeeGo   Hildon as on the Nokia N9 (patches/meego): the home views, the
 *           launcher and the open applications as pages side by side,
 *           changed by a swipe, and the swipes from the screen's edges
 *   N9      FreeBSD only: ends the Hildon session, the Nokia N9's own user
 *           interface (harmattan-session) follows on the same X server
 *           (pro1x-desktop)
 *   Xfce    FreeBSD only: ends the Hildon session, Xfce follows on the same
 *           X server (pro1x-desktop)
 *
 * Maemo and MeeGo are the GConf key /apps/osso/hildon-desktop/meego_pages,
 * which hildon-desktop follows at once. MeeGo is offered only when
 * hildon-desktop knows the key (its schema is there), N9 and Xfce only when
 * pro1x-desktop and their sessions are there.
 */
#include <libintl.h>
#include <gtk/gtk.h>
#include <hildon/hildon.h>
#include <gconf/gconf-client.h>
#include <hildon-cp-plugin/hildon-cp-plugin-interface.h>

#define DOMAIN "pro1x-look"
#define _(s) dgettext(DOMAIN, s)
#define N_(s) s
#define KEY_MEEGO "/apps/osso/hildon-desktop/meego_pages"

enum { MAEMO, MEEGO, N9, XFCE, STYLES };

static const char *const names[STYLES] = {
	"Maemo", "MeeGo", "N9", "Xfce",
};

/* what switches to the others: pro1x-desktop's names */
static const char *const sessions[STYLES] = {
	NULL, NULL, "harmattan", "xfce",
};

static const char *const about[STYLES] = {
	N_("Home views, with the application menu and the open applications "
	   "behind the buttons in the top left corner, as on the Nokia N900."),
	N_("The home views, the applications and the open applications as pages "
	   "side by side, changed by a swipe. In an application a swipe from the "
	   "left or right edge goes to the open applications, one down from the "
	   "top edge closes it, as on the Nokia N9."),
	N_("Ends the Hildon session and starts the Nokia N9's own user interface "
	   "(MeeGo 1.2 Harmattan); Hildon among its applications switches back."),
	N_("Ends the Hildon session and starts Xfce; Hildon in Xfce's menu "
	   "switches back."),
};

static GtkWidget *buttons[STYLES], *label;

/* the schema itself: the entry GConfClient keeps after a change of the
 * key has no schema name */
static gboolean meego_known(GConfClient *gc)
{
	GConfSchema *schema = gconf_client_get_schema(gc, "/schemas" KEY_MEEGO, NULL);

	if (!schema)
		return FALSE;
	gconf_schema_free(schema);
	return TRUE;
}

/* pro1x-desktop and what starts the session, harmattan-session or Xfce */
static gboolean session_there(int style)
{
	char *d = g_find_program_in_path("pro1x-desktop");
	char *x = g_find_program_in_path(style == N9 ? "harmattan-session" : "startxfce4");
	gboolean there = d && x;

	g_free(d);
	g_free(x);
	return there;
}

static int chosen(void)
{
	int i;

	for (i = 0; i < STYLES; i++)
		if (buttons[i] && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(buttons[i])))
			return i;
	return MAEMO;
}

/* a wrapping label as wide as it is given, not as GTK guesses */
static void label_size_cb(GtkWidget *w, GtkAllocation *a, gpointer data)
{
	(void)data;
	if (w->requisition.width != a->width)
		gtk_widget_set_size_request(w, a->width, -1);
}

static void toggled_cb(GtkToggleButton *b, gpointer data)
{
	(void)data;
	if (gtk_toggle_button_get_active(b))
		gtk_label_set_text(GTK_LABEL(label), _(about[chosen()]));
}

osso_return_t execute(osso_context_t *osso, gpointer data, gboolean user_activated)
{
	GConfClient *gc = gconf_client_get_default();
	GtkWidget *dialog, *row, *box;
	GSList *group = NULL;
	gboolean meego;
	int i, now;

	(void)osso, (void)user_activated;
	bindtextdomain(DOMAIN, LOCALEDIR);
	bind_textdomain_codeset(DOMAIN, "UTF-8");

	dialog = gtk_dialog_new_with_buttons(_("Desktop"), GTK_WINDOW(data),
					     GTK_DIALOG_MODAL | GTK_DIALOG_NO_SEPARATOR,
					     dgettext("hildon-libs", "wdgt_bd_save"),
					     GTK_RESPONSE_ACCEPT, NULL);
	row = gtk_hbox_new(TRUE, 0);
	for (i = 0; i < STYLES; i++) {
		if ((i == MEEGO && !meego_known(gc)) || (sessions[i] && !session_there(i))) {
			buttons[i] = NULL;
			continue;
		}
		buttons[i] = hildon_gtk_radio_button_new(HILDON_SIZE_FINGER_HEIGHT, group);
		group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(buttons[i]));
		gtk_button_set_label(GTK_BUTTON(buttons[i]), _(names[i]));
		/* the N900's look for a choice of a few: buttons in a row */
		gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(buttons[i]), FALSE);
		gtk_box_pack_start(GTK_BOX(row), buttons[i], TRUE, TRUE, 0);
	}

	label = gtk_label_new(NULL);
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_misc_set_alignment(GTK_MISC(label), 0, 0);
	hildon_helper_set_logical_font(label, "SmallSystemFont");
	hildon_helper_set_logical_color(label, GTK_RC_FG, GTK_STATE_NORMAL,
					"SecondaryTextColor");
	g_signal_connect(label, "size-allocate", G_CALLBACK(label_size_cb), NULL);

	box = gtk_vbox_new(FALSE, HILDON_MARGIN_DOUBLE);
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), box, TRUE, TRUE, 0);

	meego = gconf_client_get_bool(gc, KEY_MEEGO, NULL);
	now = meego && buttons[MEEGO] ? MEEGO : MAEMO;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(buttons[now]), TRUE);
	gtk_label_set_text(GTK_LABEL(label), _(about[now]));
	for (i = 0; i < STYLES; i++)
		if (buttons[i])
			g_signal_connect(buttons[i], "toggled", G_CALLBACK(toggled_cb), NULL);

	gtk_widget_show_all(dialog);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		switch (chosen()) {
		case MAEMO:
		case MEEGO:
			if ((chosen() == MEEGO) != meego)
				gconf_client_set_bool(gc, KEY_MEEGO, chosen() == MEEGO, NULL);
			break;
		default: {
			char *cmd = g_strconcat("pro1x-desktop ", sessions[chosen()], NULL);

			g_spawn_command_line_async(cmd, NULL);
			g_free(cmd);
			break;
		}
		}
	}
	gtk_widget_destroy(dialog);
	g_object_unref(gc);
	return OSSO_OK;
}
