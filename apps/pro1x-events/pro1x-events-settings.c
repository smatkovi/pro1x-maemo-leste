/*
 * pro1x-events-settings -- the control panel applet for the Events page of
 * Hildon's MeeGo-like look (tools/pro1x-events): the Mastodon account
 * whose home timeline it shows, its server and an access token.
 *
 * The token comes from the account's web page (Preferences, Development,
 * New application, with the scope read:statuses: "Your access token"); it
 * is kept in GConf (/apps/pro1x/events/mastodon_token), readable by the
 * user's programs, as Hildon keeps its other accounts' settings. Saving
 * empty fields removes the account.
 */
#include <libintl.h>
#include <string.h>
#include <gtk/gtk.h>
#include <hildon/hildon.h>
#include <gconf/gconf-client.h>
#include <hildon-cp-plugin/hildon-cp-plugin-interface.h>

#define DOMAIN "pro1x-events"
#define _(s) dgettext(DOMAIN, s)
#define KEY_SERVER "/apps/pro1x/events/mastodon_server"
#define KEY_TOKEN "/apps/pro1x/events/mastodon_token"

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

osso_return_t execute(osso_context_t *osso, gpointer data, gboolean user_activated)
{
	GConfClient *gc = gconf_client_get_default();
	GtkSizeGroup *group = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
	GtkWidget *dialog, *box, *server, *token, *label;
	const char *save = dgettext("hildon-libs", "wdgt_bd_save");
	char *s, *t;

	(void)osso, (void)user_activated;
	bindtextdomain(DOMAIN, LOCALEDIR);
	bind_textdomain_codeset(DOMAIN, "UTF-8");

	/* Hildon's own word, where its texts are installed */
	if (!strcmp(save, "wdgt_bd_save"))
		save = _("Save");
	dialog = gtk_dialog_new_with_buttons(_("Events"), GTK_WINDOW(data),
					     GTK_DIALOG_MODAL | GTK_DIALOG_NO_SEPARATOR,
					     save, GTK_RESPONSE_ACCEPT, NULL);
	server = hildon_entry_new(HILDON_SIZE_FINGER_HEIGHT);
	hildon_gtk_entry_set_placeholder_text(GTK_ENTRY(server), "mastodon.social");
	hildon_gtk_entry_set_input_mode(GTK_ENTRY(server), HILDON_GTK_INPUT_MODE_FULL);
	token = hildon_entry_new(HILDON_SIZE_FINGER_HEIGHT);
	gtk_entry_set_visibility(GTK_ENTRY(token), FALSE);
	hildon_gtk_entry_set_input_mode(GTK_ENTRY(token), HILDON_GTK_INPUT_MODE_FULL |
					HILDON_GTK_INPUT_MODE_INVISIBLE);

	s = gconf_client_get_string(gc, KEY_SERVER, NULL);
	t = gconf_client_get_string(gc, KEY_TOKEN, NULL);
	if (s)
		gtk_entry_set_text(GTK_ENTRY(server), s);
	if (t)
		gtk_entry_set_text(GTK_ENTRY(token), t);
	g_free(s);
	g_free(t);

	label = gtk_label_new(_("The Events page shows the home timeline of this Mastodon "
				"account. Its access token: on the account's web page, "
				"Preferences, Development, New application, with the scope "
				"read:statuses, then \"Your access token\"."));
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_misc_set_alignment(GTK_MISC(label), 0, 0);
	hildon_helper_set_logical_font(label, "SmallSystemFont");
	hildon_helper_set_logical_color(label, GTK_RC_FG, GTK_STATE_NORMAL,
					"SecondaryTextColor");
	g_signal_connect(label, "size-allocate", G_CALLBACK(label_size_cb), NULL);

	box = gtk_vbox_new(FALSE, HILDON_MARGIN_DEFAULT);
	gtk_box_pack_start(GTK_BOX(box), row(group, _("Mastodon server"), server), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), row(group, _("Access token"), token), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, HILDON_MARGIN_DEFAULT);
	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), box, TRUE, TRUE, 0);
	gtk_widget_show_all(dialog);

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		char *ns = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(server))));
		char *nt = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(token))));

		if (*ns && *nt) {
			gconf_client_set_string(gc, KEY_SERVER, ns, NULL);
			gconf_client_set_string(gc, KEY_TOKEN, nt, NULL);
		} else {
			gconf_client_unset(gc, KEY_TOKEN, NULL);
			gconf_client_unset(gc, KEY_SERVER, NULL);
		}
		g_free(ns);
		g_free(nt);
	}
	gtk_widget_destroy(dialog);
	g_object_unref(group);
	g_object_unref(gc);
	return OSSO_OK;
}
