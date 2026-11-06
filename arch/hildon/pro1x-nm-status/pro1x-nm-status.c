/*
 * pro1x-nm-status -- the Internet connection in Hildon's status menu for
 * Arch, where NetworkManager has the network (Leste's connui and icd2 are
 * not there).
 *
 * The status area shows the Wi-Fi signal while a network is connected; the
 * status menu's "Internet" button tells which one, or that none is, and
 * opens the list of networks in reach: a tap connects (a saved network as
 * it is, a new one after its password), and the dialog can disconnect or
 * switch Wi-Fi off and on. A network whose password NetworkManager does
 * not have (one Phosh saved, whose password only Phosh's agent knew) or
 * whose password was not accepted asks for it and keeps it, for all the
 * phone's desktops.
 *
 * Built and installed by arch/40-hildon.sh (the plugin into
 * /usr/lib/hildon-desktop, pro1x-nm-status.desktop into the status menu's
 * directory).
 */
#include <string.h>
#include <gtk/gtk.h>
#include <hildon/hildon.h>
#include <libhildondesktop/libhildondesktop.h>
#include <NetworkManager.h>

#define PRO1X_TYPE_NM_STATUS (pro1x_nm_status_get_type ())
#define PRO1X_NM_STATUS(o) \
	(G_TYPE_CHECK_INSTANCE_CAST ((o), PRO1X_TYPE_NM_STATUS, Pro1xNmStatus))

#define AREA_ICON_SIZE 18
#define MENU_ICON_SIZE 48

typedef struct {
	HDStatusMenuItem parent;
	NMClient *client;
	NMDeviceWifi *wifi;
	NMAccessPoint *ap;		/* the connected one, watched for its strength */
	GtkWidget *button;
	GtkWidget *dialog;		/* the list of networks while it is open */
	GtkWidget *list;
	guint update_id;
	guint refill_id;
} Pro1xNmStatus;

typedef struct {
	HDStatusMenuItemClass parent;
} Pro1xNmStatusClass;

HD_DEFINE_PLUGIN_MODULE (Pro1xNmStatus, pro1x_nm_status, HD_TYPE_STATUS_MENU_ITEM);

static void connect_to (Pro1xNmStatus *self, const char *ssid);

/* --- helpers ------------------------------------------------------------- */

static char *
ap_ssid (NMAccessPoint *ap)
{
	GBytes *b = nm_access_point_get_ssid (ap);
	gsize len;
	const guint8 *d;

	if (!b)
		return NULL;
	d = g_bytes_get_data (b, &len);
	return len ? nm_utils_ssid_to_utf8 (d, len) : NULL;
}

/* the key management a new connection to AP needs: "" for an open network,
 * NULL for one this cannot join (WEP, 802.1X: nmtui can) */
static const char *
ap_key_mgmt (NMAccessPoint *ap)
{
	NM80211ApSecurityFlags sec = nm_access_point_get_wpa_flags (ap) |
	                             nm_access_point_get_rsn_flags (ap);

	if (sec & NM_802_11_AP_SEC_KEY_MGMT_PSK)
		return "wpa-psk";
	if (sec & NM_802_11_AP_SEC_KEY_MGMT_SAE)
		return "sae";
	if (sec || (nm_access_point_get_flags (ap) & NM_802_11_AP_FLAGS_PRIVACY))
		return NULL;
	return "";
}

static GdkPixbuf *
icon (const char *name, int size)
{
	return gtk_icon_theme_load_icon (gtk_icon_theme_get_default (), name, size,
	                                 GTK_ICON_LOOKUP_NO_SVG, NULL);
}

static const char *
signal_icon (guint8 strength)
{
	return strength >= 55 ? "connectivity_wlan_signal_good"
	                      : "connectivity_wlan_signal_fair";
}

static void
banner (const char *format, ...)
{
	va_list ap;
	char *text;

	va_start (ap, format);
	text = g_strdup_vprintf (format, ap);
	va_end (ap);
	hildon_banner_show_information (NULL, NULL, text);
	g_free (text);
}

/* the strongest access point of SSID in reach */
static NMAccessPoint *
find_ap (Pro1xNmStatus *self, const char *ssid)
{
	const GPtrArray *aps;
	NMAccessPoint *best = NULL;
	guint i;

	if (!self->wifi)
		return NULL;
	aps = nm_device_wifi_get_access_points (self->wifi);
	for (i = 0; aps && i < aps->len; i++) {
		NMAccessPoint *ap = g_ptr_array_index (aps, i);
		char *s = ap_ssid (ap);

		if (s && !strcmp (s, ssid) &&
		    (!best || nm_access_point_get_strength (ap) >
		              nm_access_point_get_strength (best)))
			best = ap;
		g_free (s);
	}
	return best;
}

/* a saved connection for AP, if there is one */
static NMRemoteConnection *
saved_for (Pro1xNmStatus *self, NMAccessPoint *ap)
{
	const GPtrArray *all = nm_client_get_connections (self->client);
	GPtrArray *fit;
	NMRemoteConnection *c = NULL;

	if (!all || !ap)
		return NULL;
	fit = nm_access_point_filter_connections (ap, all);
	if (fit->len)
		c = g_object_ref (g_ptr_array_index (fit, 0));
	g_ptr_array_unref (fit);
	return c;
}

/* --- the status area and the button ---------------------------------------- */

static void
watch_ap (Pro1xNmStatus *self, NMAccessPoint *ap);

static gboolean
update (gpointer data)
{
	Pro1xNmStatus *self = data;
	NMDeviceState state = NM_DEVICE_STATE_UNKNOWN;
	NMActiveConnection *primary;
	NMAccessPoint *ap = NULL;
	const char *title = "Internet", *image = "statusarea_internetconnection_no";
	char *value = NULL, *ssid = NULL;
	GdkPixbuf *area = NULL, *pb;

	self->update_id = 0;
	if (self->wifi) {
		state = nm_device_get_state (NM_DEVICE (self->wifi));
		ap = nm_device_wifi_get_active_access_point (self->wifi);
	}
	watch_ap (self, ap);
	if (ap)
		ssid = ap_ssid (ap);

	if (!self->client) {
		value = g_strdup ("NetworkManager is not running");
	} else if (!nm_client_wireless_get_enabled (self->client)) {
		value = g_strdup ("Wi-Fi is off");
	} else if (state == NM_DEVICE_STATE_ACTIVATED && ap) {
		value = g_strdup_printf ("%s, %u %%", ssid ? ssid : "Wi-Fi",
		                         nm_access_point_get_strength (ap));
		image = "general_wlan";
		area = icon (signal_icon (nm_access_point_get_strength (ap)),
		             AREA_ICON_SIZE);
	} else if (state >= NM_DEVICE_STATE_PREPARE &&
	           state < NM_DEVICE_STATE_ACTIVATED) {
		value = g_strdup_printf ("Connecting to %s...", ssid ? ssid : "Wi-Fi");
		image = "general_wlan";
	} else if ((primary = nm_client_get_primary_connection (self->client))) {
		value = g_strdup (nm_active_connection_get_id (primary));
		image = "statusarea_internetconn_packetdata_dimmed";
	} else {
		value = g_strdup ("Not connected");
	}

	hildon_button_set_text (HILDON_BUTTON (self->button), title, value);
	pb = icon (image, MENU_ICON_SIZE);
	hildon_button_set_image (HILDON_BUTTON (self->button),
	                         pb ? gtk_image_new_from_pixbuf (pb) : NULL);
	if (pb)
		g_object_unref (pb);
	hd_status_plugin_item_set_status_area_icon (HD_STATUS_PLUGIN_ITEM (self), area);
	if (area)
		g_object_unref (area);
	g_free (value);
	g_free (ssid);
	return FALSE;
}

static void
queue_update (Pro1xNmStatus *self)
{
	if (!self->update_id)
		self->update_id = g_idle_add (update, self);
}

static void
changed_cb (GObject *object, GParamSpec *pspec, gpointer data)
{
	queue_update (data);
}

static void
watch_ap (Pro1xNmStatus *self, NMAccessPoint *ap)
{
	if (ap == self->ap)
		return;
	if (self->ap) {
		g_signal_handlers_disconnect_by_data (self->ap, self);
		g_object_unref (self->ap);
	}
	self->ap = ap ? g_object_ref (ap) : NULL;
	if (ap)
		g_signal_connect (ap, "notify::" NM_ACCESS_POINT_STRENGTH,
		                  G_CALLBACK (changed_cb), self);
}

/* --- connecting -------------------------------------------------------------- */

typedef struct {
	Pro1xNmStatus *self;
	char *ssid;
	gboolean asked;		/* a password was entered for this try */
} Attempt;

static void
attempt_free (Attempt *a)
{
	g_free (a->ssid);
	g_free (a);
}

static void ask_password (Pro1xNmStatus *self, const char *ssid, gboolean again);

static void
ac_state_cb (NMActiveConnection *ac, guint state, guint reason, gpointer data)
{
	Attempt *a = data;

	if (state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED) {
		banner ("Connected to %s", a->ssid);
	} else if (state == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED) {
		if (reason == NM_ACTIVE_CONNECTION_STATE_REASON_NO_SECRETS ||
		    reason == NM_ACTIVE_CONNECTION_STATE_REASON_LOGIN_FAILED)
			ask_password (a->self, a->ssid, a->asked);
		else if (reason != NM_ACTIVE_CONNECTION_STATE_REASON_USER_DISCONNECTED)
			banner ("Could not connect to %s", a->ssid);
	} else {
		return;
	}
	g_signal_handlers_disconnect_by_data (ac, a);
	g_object_unref (ac);
	attempt_free (a);
}

static void
watch_attempt (NMActiveConnection *ac, Attempt *a)
{
	/* ac is ours now; ac_state_cb lets it go with the attempt */
	g_signal_connect (ac, "state-changed", G_CALLBACK (ac_state_cb), a);
}

static void
activated_cb (GObject *client, GAsyncResult *res, gpointer data)
{
	Attempt *a = data;
	GError *err = NULL;
	NMActiveConnection *ac;

	ac = nm_client_activate_connection_finish (NM_CLIENT (client), res, &err);
	if (!ac) {
		banner ("Could not connect to %s: %s", a->ssid, err->message);
		g_error_free (err);
		attempt_free (a);
		return;
	}
	watch_attempt (ac, a);
}

static void
added_cb (GObject *client, GAsyncResult *res, gpointer data)
{
	Attempt *a = data;
	GError *err = NULL;
	NMActiveConnection *ac;

	ac = nm_client_add_and_activate_connection_finish (NM_CLIENT (client), res, &err);
	if (!ac) {
		banner ("Could not connect to %s: %s", a->ssid, err->message);
		g_error_free (err);
		attempt_free (a);
		return;
	}
	watch_attempt (ac, a);
}

static void
start (Pro1xNmStatus *self, const char *ssid, const char *password)
{
	NMAccessPoint *ap = find_ap (self, ssid);
	NMRemoteConnection *saved;
	Attempt *a;

	if (!ap) {
		banner ("%s is not in reach", ssid);
		return;
	}
	a = g_new0 (Attempt, 1);
	a->self = self;
	a->ssid = g_strdup (ssid);
	a->asked = password != NULL;
	saved = saved_for (self, ap);
	if (saved) {
		nm_client_activate_connection_async (self->client, NM_CONNECTION (saved),
		                                     NM_DEVICE (self->wifi),
		                                     nm_object_get_path (NM_OBJECT (ap)),
		                                     NULL, activated_cb, a);
		g_object_unref (saved);
	} else {
		NMConnection *c = NULL;
		const char *km = ap_key_mgmt (ap);

		if (km && *km) {
			NMSetting *s = nm_setting_wireless_security_new ();

			g_object_set (s, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, km,
			              NM_SETTING_WIRELESS_SECURITY_PSK, password, NULL);
			c = nm_simple_connection_new ();
			nm_connection_add_setting (c, s);
		}
		nm_client_add_and_activate_connection_async (self->client, c,
		                                             NM_DEVICE (self->wifi),
		                                             nm_object_get_path (NM_OBJECT (ap)),
		                                             NULL, added_cb, a);
		if (c)
			g_object_unref (c);
	}
}

typedef struct {
	Pro1xNmStatus *self;
	char *ssid;
} Commit;

static void
committed_cb (GObject *object, GAsyncResult *res, gpointer data)
{
	Commit *c = data;
	GError *err = NULL;

	if (nm_remote_connection_commit_changes_finish (NM_REMOTE_CONNECTION (object),
	                                                res, &err)) {
		start (c->self, c->ssid, "");
	} else {
		banner ("Could not save the password: %s", err->message);
		g_error_free (err);
	}
	g_free (c->ssid);
	g_free (c);
}

/* a password for SSID: into its saved connection (kept by NetworkManager,
 * not by an agent), or into a new one */
static void
use_password (Pro1xNmStatus *self, const char *ssid, const char *password)
{
	NMAccessPoint *ap = find_ap (self, ssid);
	NMRemoteConnection *saved = ap ? saved_for (self, ap) : NULL;
	NMSettingWirelessSecurity *sec;

	if (!saved) {
		start (self, ssid, password);
		return;
	}
	sec = nm_connection_get_setting_wireless_security (NM_CONNECTION (saved));
	if (!sec) {
		sec = NM_SETTING_WIRELESS_SECURITY (nm_setting_wireless_security_new ());
		g_object_set (sec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
		              ap_key_mgmt (ap) ? ap_key_mgmt (ap) : "wpa-psk", NULL);
		nm_connection_add_setting (NM_CONNECTION (saved), NM_SETTING (sec));
	}
	g_object_set (sec, NM_SETTING_WIRELESS_SECURITY_PSK, password,
	              NM_SETTING_WIRELESS_SECURITY_PSK_FLAGS, NM_SETTING_SECRET_FLAG_NONE,
	              NULL);
	{
		Commit *c = g_new0 (Commit, 1);

		c->self = self;
		c->ssid = g_strdup (ssid);
		nm_remote_connection_commit_changes_async (saved, TRUE, NULL,
		                                           committed_cb, c);
	}
	g_object_unref (saved);
}

static void
ask_password (Pro1xNmStatus *self, const char *ssid, gboolean again)
{
	GtkWidget *dialog, *entry, *label, *box;
	char *text;

	dialog = hildon_dialog_new_with_buttons (ssid, NULL, GTK_DIALOG_MODAL,
	                                         "Connect", GTK_RESPONSE_OK, NULL);
	box = gtk_vbox_new (FALSE, 8);
	text = again ? g_strdup ("The password was not accepted. Password:")
	             : g_strdup ("Password:");
	label = gtk_label_new (text);
	g_free (text);
	gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
	entry = hildon_entry_new (HILDON_SIZE_FINGER_HEIGHT | HILDON_SIZE_AUTO_WIDTH);
	gtk_entry_set_visibility (GTK_ENTRY (entry), FALSE);
	gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
	hildon_gtk_entry_set_input_mode (GTK_ENTRY (entry),
	                                 HILDON_GTK_INPUT_MODE_FULL |
	                                 HILDON_GTK_INPUT_MODE_INVISIBLE);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (box), entry, FALSE, FALSE, 0);
	gtk_container_add (GTK_CONTAINER (GTK_DIALOG (dialog)->vbox), box);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
	gtk_widget_show_all (dialog);

	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK) {
		const char *pw = gtk_entry_get_text (GTK_ENTRY (entry));

		if (strlen (pw) >= 8 && strlen (pw) <= 64) {
			char *copy = g_strdup (pw);

			gtk_widget_destroy (dialog);
			use_password (self, ssid, copy);
			g_free (copy);
			return;
		}
		banner ("A Wi-Fi password has 8 to 63 characters");
	}
	gtk_widget_destroy (dialog);
}

/* a tap on a network in the list */
static void
connect_to (Pro1xNmStatus *self, const char *ssid)
{
	NMAccessPoint *ap = find_ap (self, ssid);
	NMRemoteConnection *saved;
	const char *km;

	if (!ap) {
		banner ("%s is not in reach", ssid);
		return;
	}
	saved = saved_for (self, ap);
	km = ap_key_mgmt (ap);
	if (saved) {
		g_object_unref (saved);
		start (self, ssid, NULL);
	} else if (!km) {
		banner ("%s needs a kind of login this cannot do: nmtui in X Terminal can", ssid);
	} else if (!*km) {
		start (self, ssid, NULL);
	} else {
		ask_password (self, ssid, FALSE);
	}
}

/* --- the list of networks ------------------------------------------------- */

static void
network_clicked_cb (GtkButton *button, gpointer data)
{
	Pro1xNmStatus *self = data;
	char *ssid = g_strdup (g_object_get_data (G_OBJECT (button), "ssid"));

	gtk_dialog_response (GTK_DIALOG (self->dialog), GTK_RESPONSE_CLOSE);
	connect_to (self, ssid);
	g_free (ssid);
}

static gint
by_strength (gconstpointer a, gconstpointer b)
{
	return nm_access_point_get_strength (*(NMAccessPoint **) b) -
	       nm_access_point_get_strength (*(NMAccessPoint **) a);
}

static void
fill_list (Pro1xNmStatus *self)
{
	GList *old = gtk_container_get_children (GTK_CONTAINER (self->list)), *l;
	const GPtrArray *aps = self->wifi ? nm_device_wifi_get_access_points (self->wifi) : NULL;
	GPtrArray *sorted = g_ptr_array_new ();
	GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	NMAccessPoint *active = self->wifi ?
		nm_device_wifi_get_active_access_point (self->wifi) : NULL;
	char *active_ssid = active ? ap_ssid (active) : NULL;
	guint i, shown = 0;

	for (l = old; l; l = l->next)
		gtk_widget_destroy (l->data);
	g_list_free (old);

	for (i = 0; aps && i < aps->len; i++)
		g_ptr_array_add (sorted, g_ptr_array_index (aps, i));
	g_ptr_array_sort (sorted, by_strength);

	for (i = 0; i < sorted->len; i++) {
		NMAccessPoint *ap = g_ptr_array_index (sorted, i);
		char *ssid = ap_ssid (ap), *detail;
		NMRemoteConnection *saved;
		const char *km, *what;
		GtkWidget *b;
		GdkPixbuf *pb;

		if (!ssid || g_hash_table_contains (seen, ssid)) {
			g_free (ssid);
			continue;
		}
		g_hash_table_add (seen, g_strdup (ssid));
		saved = saved_for (self, ap);
		km = ap_key_mgmt (ap);
		if (active_ssid && !strcmp (ssid, active_ssid))
			what = "connected";
		else if (saved)
			what = "saved";
		else if (km && !*km)
			what = "open";
		else
			what = "password";
		detail = g_strdup_printf ("%s, %u %%", what, nm_access_point_get_strength (ap));
		b = hildon_button_new_with_text (HILDON_SIZE_AUTO_WIDTH | HILDON_SIZE_FINGER_HEIGHT,
		                                 HILDON_BUTTON_ARRANGEMENT_VERTICAL, ssid, detail);
		hildon_button_set_style (HILDON_BUTTON (b), HILDON_BUTTON_STYLE_PICKER);
		hildon_button_set_alignment (HILDON_BUTTON (b), 0.0, 0.5, 1.0, 1.0);
		pb = icon (signal_icon (nm_access_point_get_strength (ap)), MENU_ICON_SIZE);
		if (pb) {
			hildon_button_set_image (HILDON_BUTTON (b), gtk_image_new_from_pixbuf (pb));
			g_object_unref (pb);
		}
		g_object_set_data_full (G_OBJECT (b), "ssid", ssid, g_free);
		g_signal_connect (b, "clicked", G_CALLBACK (network_clicked_cb), self);
		gtk_box_pack_start (GTK_BOX (self->list), b, FALSE, FALSE, 0);
		if (saved)
			g_object_unref (saved);
		g_free (detail);
		shown++;
	}
	if (!shown) {
		GtkWidget *label = gtk_label_new (
			self->client && nm_client_wireless_get_enabled (self->client) ?
			"Looking for networks..." : "Wi-Fi is off");
		gtk_box_pack_start (GTK_BOX (self->list), label, FALSE, FALSE, 16);
	}
	gtk_widget_show_all (self->list);
	g_ptr_array_unref (sorted);
	g_hash_table_unref (seen);
	g_free (active_ssid);
}

static gboolean
refill (gpointer data)
{
	Pro1xNmStatus *self = data;

	self->refill_id = 0;
	if (self->dialog)
		fill_list (self);
	return FALSE;
}

static void
aps_changed_cb (NMDeviceWifi *wifi, NMAccessPoint *ap, gpointer data)
{
	Pro1xNmStatus *self = data;

	if (self->dialog && !self->refill_id)
		self->refill_id = g_timeout_add (500, refill, self);
}

static void
scanned_cb (GObject *wifi, GAsyncResult *res, gpointer data)
{
	nm_device_wifi_request_scan_finish (NM_DEVICE_WIFI (wifi), res, NULL);
}

static void
wireless_set_cb (GObject *client, GAsyncResult *res, gpointer data)
{
	GError *err = NULL;

	if (!nm_client_dbus_set_property_finish (NM_CLIENT (client), res, &err)) {
		banner ("Could not switch Wi-Fi: %s", err->message);
		g_error_free (err);
	}
}

static void
deactivated_cb (GObject *client, GAsyncResult *res, gpointer data)
{
	nm_client_deactivate_connection_finish (NM_CLIENT (client), res, NULL);
}

enum { RESPONSE_DISCONNECT = 1, RESPONSE_WIFI };

static void
button_clicked_cb (GtkButton *button, gpointer data)
{
	Pro1xNmStatus *self = data;
	GtkWidget *area;
	NMActiveConnection *ac = NULL;
	gboolean enabled;
	gint response;

	if (!self->client || self->dialog)
		return;
	enabled = nm_client_wireless_get_enabled (self->client);
	if (self->wifi)
		ac = nm_device_get_active_connection (NM_DEVICE (self->wifi));
	/* the status menu closes, as Maemo's own items do */
	gtk_widget_hide (gtk_widget_get_toplevel (GTK_WIDGET (button)));

	self->dialog = hildon_dialog_new_with_buttons ("Wi-Fi networks", NULL,
	                                               GTK_DIALOG_MODAL, NULL);
	if (ac)
		gtk_dialog_add_button (GTK_DIALOG (self->dialog), "Disconnect",
		                       RESPONSE_DISCONNECT);
	gtk_dialog_add_button (GTK_DIALOG (self->dialog),
	                       enabled ? "Wi-Fi off" : "Wi-Fi on",
	                       RESPONSE_WIFI);
	self->list = gtk_vbox_new (FALSE, 0);
	area = hildon_pannable_area_new ();
	g_object_set (area, "size-request-policy", HILDON_SIZE_REQUEST_CHILDREN, NULL);
	hildon_pannable_area_add_with_viewport (HILDON_PANNABLE_AREA (area), self->list);
	gtk_container_add (GTK_CONTAINER (GTK_DIALOG (self->dialog)->vbox), area);
	fill_list (self);
	gtk_widget_show_all (self->dialog);
	if (self->wifi && enabled)
		nm_device_wifi_request_scan_async (self->wifi, NULL, scanned_cb, self);

	response = gtk_dialog_run (GTK_DIALOG (self->dialog));
	if (self->refill_id) {
		g_source_remove (self->refill_id);
		self->refill_id = 0;
	}
	gtk_widget_destroy (self->dialog);
	self->dialog = self->list = NULL;

	if (response == RESPONSE_DISCONNECT && self->wifi &&
	    (ac = nm_device_get_active_connection (NM_DEVICE (self->wifi))))
		nm_client_deactivate_connection_async (self->client, ac, NULL,
		                                       deactivated_cb, self);
	else if (response == RESPONSE_WIFI)
		nm_client_dbus_set_property (self->client, NM_DBUS_PATH, NM_DBUS_INTERFACE,
		                             "WirelessEnabled",
		                             g_variant_new_boolean (!enabled), -1, NULL,
		                             wireless_set_cb, self);
}

/* --- NetworkManager's objects ------------------------------------------------ */

static void
find_wifi (Pro1xNmStatus *self)
{
	const GPtrArray *devs = nm_client_get_devices (self->client);
	NMDeviceWifi *wifi = NULL;
	guint i;

	for (i = 0; devs && i < devs->len && !wifi; i++)
		if (NM_IS_DEVICE_WIFI (g_ptr_array_index (devs, i)))
			wifi = g_ptr_array_index (devs, i);
	if (wifi == self->wifi)
		return;
	if (self->wifi) {
		g_signal_handlers_disconnect_by_data (self->wifi, self);
		g_object_unref (self->wifi);
	}
	self->wifi = wifi ? g_object_ref (wifi) : NULL;
	if (wifi) {
		g_signal_connect (wifi, "notify::" NM_DEVICE_STATE, G_CALLBACK (changed_cb), self);
		g_signal_connect (wifi, "notify::" NM_DEVICE_WIFI_ACTIVE_ACCESS_POINT,
		                  G_CALLBACK (changed_cb), self);
		g_signal_connect (wifi, "access-point-added", G_CALLBACK (aps_changed_cb), self);
		g_signal_connect (wifi, "access-point-removed", G_CALLBACK (aps_changed_cb), self);
	}
	queue_update (self);
}

static void
devices_cb (NMClient *client, NMDevice *device, gpointer data)
{
	find_wifi (data);
}

static void
client_cb (GObject *source, GAsyncResult *res, gpointer data)
{
	Pro1xNmStatus *self = data;
	GError *err = NULL;

	self->client = nm_client_new_finish (res, &err);
	if (!self->client) {
		g_warning ("pro1x-nm-status: %s", err->message);
		g_error_free (err);
		queue_update (self);
		return;
	}
	g_signal_connect (self->client, "notify::" NM_CLIENT_WIRELESS_ENABLED,
	                  G_CALLBACK (changed_cb), self);
	g_signal_connect (self->client, "notify::" NM_CLIENT_PRIMARY_CONNECTION,
	                  G_CALLBACK (changed_cb), self);
	g_signal_connect (self->client, "device-added", G_CALLBACK (devices_cb), self);
	g_signal_connect (self->client, "device-removed", G_CALLBACK (devices_cb), self);
	find_wifi (self);
}

/* --- the plugin ----------------------------------------------------------------- */

static void
pro1x_nm_status_init (Pro1xNmStatus *self)
{
	self->button = hildon_button_new (HILDON_SIZE_AUTO_WIDTH | HILDON_SIZE_FINGER_HEIGHT,
	                                  HILDON_BUTTON_ARRANGEMENT_VERTICAL);
	hildon_button_set_style (HILDON_BUTTON (self->button), HILDON_BUTTON_STYLE_PICKER);
	hildon_button_set_alignment (HILDON_BUTTON (self->button), 0.0, 0.5, 1.0, 1.0);
	g_signal_connect (self->button, "clicked", G_CALLBACK (button_clicked_cb), self);
	gtk_container_add (GTK_CONTAINER (self), self->button);
	update (self);
	gtk_widget_show_all (GTK_WIDGET (self));
	nm_client_new_async (NULL, client_cb, self);
}

static void
pro1x_nm_status_finalize (GObject *object)
{
	Pro1xNmStatus *self = PRO1X_NM_STATUS (object);

	if (self->update_id)
		g_source_remove (self->update_id);
	watch_ap (self, NULL);
	if (self->wifi) {
		g_signal_handlers_disconnect_by_data (self->wifi, self);
		g_object_unref (self->wifi);
	}
	if (self->client) {
		g_signal_handlers_disconnect_by_data (self->client, self);
		g_object_unref (self->client);
	}
	G_OBJECT_CLASS (pro1x_nm_status_parent_class)->finalize (object);
}

static void
pro1x_nm_status_class_init (Pro1xNmStatusClass *klass)
{
	G_OBJECT_CLASS (klass)->finalize = pro1x_nm_status_finalize;
}

static void
pro1x_nm_status_class_finalize (Pro1xNmStatusClass *klass)
{
}
