/*
 * pro1x-wifi-status -- the Wi-Fi in Hildon's status menu on FreeBSD, where
 * wpa_supplicant has the network (Leste's connui needs icd2, which FreeBSD
 * has not got). The same as Arch's pro1x-nm-status, through
 * wpa_supplicant's control interface instead of NetworkManager.
 *
 * The status area shows the signal while a network is connected; the
 * status menu's "Internet" button tells which one, or that none is, and
 * opens the list of networks in reach: a tap connects (a saved network as
 * it is, a new one after its password, which is asked again when it was
 * wrong), and the dialog can disconnect or switch Wi-Fi off and on
 * (wlan0 down and up). New networks are saved with SAVE_CONFIG into the
 * configuration rc.d/pro1x_wifi made (/var/db/pro1x-wifi): kept on the
 * writable pool, until the next start on the read-only root.
 *
 * The session runs as root (rc.local's startx), as wpa_supplicant's
 * control socket wants it (ctrl_interface_group wheel).
 *
 * Built by freebsd/hildon/build.sh (part pro1x-wifi-status).
 */
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <gtk/gtk.h>
#include <hildon/hildon.h>
#include <libhildondesktop/libhildondesktop.h>

#define CTRL_DIR "/var/run/wpa_supplicant"
#define IFACE "wlan0"
#define IFCONFIG "/sbin/ifconfig"
#define AREA_ICON_SIZE 18
#define MENU_ICON_SIZE 48
#define REPLY_MAX 65536

#define PRO1X_TYPE_WIFI_STATUS (pro1x_wifi_status_get_type ())
#define PRO1X_WIFI_STATUS(o) \
	(G_TYPE_CHECK_INSTANCE_CAST ((o), PRO1X_TYPE_WIFI_STATUS, Pro1xWifiStatus))

typedef struct {
	char *ssid;		/* as bytes, NUL-terminated (an SSID has no NUL) */
	char *flags;
	char bssid[18];
	int level;
} Bss;

typedef struct {
	HDStatusMenuItem parent;
	int req, mon;		/* wpa_supplicant's control sockets, -1 when closed */
	char *req_path, *mon_path;
	guint mon_watch, retry_id, update_id;
	guint retry_delay;
	GPtrArray *bss;		/* Bss, the last scan results */
	char *pending;		/* the SSID this plugin is connecting to */
	int pending_id;		/* its network id */
	gboolean pending_new;	/* added for this try (removed if given up) */
	gboolean asked;		/* a password was entered for this try */
	GtkWidget *button, *dialog, *list;
} Pro1xWifiStatus;

typedef struct {
	HDStatusMenuItemClass parent;
} Pro1xWifiStatusClass;

HD_DEFINE_PLUGIN_MODULE (Pro1xWifiStatus, pro1x_wifi_status, HD_TYPE_STATUS_MENU_ITEM);

static void connect_to (Pro1xWifiStatus *self, const char *ssid);
static void ask_password (Pro1xWifiStatus *self, const char *ssid, int id, gboolean again);
static void ctrl_close (Pro1xWifiStatus *self);
static void queue_update (Pro1xWifiStatus *self);

/* --- helpers ----------------------------------------------------------------- */

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

static GdkPixbuf *
icon (const char *name, int size)
{
	return gtk_icon_theme_load_icon (gtk_icon_theme_get_default (), name, size,
	                                 GTK_ICON_LOOKUP_NO_SVG, NULL);
}

/* a scan result's level as a percentage: dBm when negative, else (BSD's
 * driver) the RSSI above the noise floor in half dB */
static int
percent (int level)
{
	int p = level < 0 ? 2 * (level + 100) : level * 100 / 80;

	return CLAMP (p, 0, 100);
}

static const char *
signal_icon (int pct)
{
	return pct >= 55 ? "connectivity_wlan_signal_good"
	                 : "connectivity_wlan_signal_fair";
}

/* wpa_supplicant prints SSIDs printf-encoded (\" \\ \e \n \r \t \xNN) */
static char *
ssid_decode (const char *s)
{
	GString *o = g_string_new (NULL);

	while (*s) {
		if (*s != '\\' || !s[1]) {
			g_string_append_c (o, *s++);
			continue;
		}
		s++;
		switch (*s) {
		case 'n': g_string_append_c (o, '\n'); s++; break;
		case 'r': g_string_append_c (o, '\r'); s++; break;
		case 't': g_string_append_c (o, '\t'); s++; break;
		case 'e': g_string_append_c (o, '\033'); s++; break;
		case 'x':
			if (g_ascii_isxdigit (s[1]) && g_ascii_isxdigit (s[2])) {
				g_string_append_c (o, g_ascii_xdigit_value (s[1]) * 16 +
				                      g_ascii_xdigit_value (s[2]));
				s += 3;
				break;
			}
			/* fall through */
		default: g_string_append_c (o, *s++); break;
		}
	}
	return g_string_free (o, FALSE);
}

/* an SSID for the screen */
static char *
ssid_show (const char *ssid)
{
	return g_utf8_validate (ssid, -1, NULL) ? g_strdup (ssid)
	                                        : g_strescape (ssid, NULL);
}

/* an SSID for SET_NETWORK: hex, whatever bytes it has */
static char *
ssid_hex (const char *ssid)
{
	GString *o = g_string_new (NULL);

	for (; *ssid; ssid++)
		g_string_append_printf (o, "%02x", (guint8) *ssid);
	return g_string_free (o, FALSE);
}

/* what joining a network with these scan flags needs: "NONE" for an open
 * one, NULL for one this cannot join (WEP, 802.1X: by hand in the file) */
static const char *
key_mgmt (const char *flags)
{
	if (strstr (flags, "EAP") || strstr (flags, "WEP"))
		return NULL;
	if (strstr (flags, "PSK"))
		return "WPA-PSK";
	if (strstr (flags, "SAE"))
		return "SAE";
	if (strstr (flags, "WPA") || strstr (flags, "RSN"))
		return NULL;
	return "NONE";
}

/* --- the control interface ------------------------------------------------ */

static int
ctrl_socket (char **local)
{
	static int n;
	struct sockaddr_un sa;
	socklen_t len;
	int fd;

	fd = socket (AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	memset (&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	*local = g_strdup_printf ("/tmp/pro1x-wifi-status-%d-%d", (int) getpid (), n++);
	unlink (*local);
	g_strlcpy (sa.sun_path, *local, sizeof sa.sun_path);
	len = offsetof (struct sockaddr_un, sun_path) + strlen (sa.sun_path) + 1;
	if (bind (fd, (struct sockaddr *) &sa, len) < 0)
		goto fail;
	g_strlcpy (sa.sun_path, CTRL_DIR "/" IFACE, sizeof sa.sun_path);
	len = offsetof (struct sockaddr_un, sun_path) + strlen (sa.sun_path) + 1;
	if (connect (fd, (struct sockaddr *) &sa, len) < 0)
		goto fail;
	return fd;
fail:
	close (fd);
	unlink (*local);
	g_free (*local);
	*local = NULL;
	return -1;
}

/* a command and its reply (NULL when wpa_supplicant does not answer) */
static char *
ctrl_ask (Pro1xWifiStatus *self, int fd, const char *cmd)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	char *buf;
	ssize_t n;

	if (fd < 0)
		return NULL;
	if (send (fd, cmd, strlen (cmd), 0) < 0)
		goto gone;
	buf = g_malloc (REPLY_MAX);
	for (;;) {
		if (poll (&p, 1, 3000) <= 0) {
			g_free (buf);
			goto gone;
		}
		n = recv (fd, buf, REPLY_MAX - 1, 0);
		if (n < 0) {
			g_free (buf);
			goto gone;
		}
		buf[n] = '\0';
		/* an event that came in between on the monitor socket */
		if (buf[0] == '<' && fd == self->mon)
			continue;
		return buf;
	}
gone:
	ctrl_close (self);
	queue_update (self);
	return NULL;
}

static char *
req (Pro1xWifiStatus *self, const char *format, ...)
{
	va_list ap;
	char *cmd, *r;

	va_start (ap, format);
	cmd = g_strdup_vprintf (format, ap);
	va_end (ap);
	r = ctrl_ask (self, self->req, cmd);
	g_free (cmd);
	return r;
}

static gboolean
req_ok (Pro1xWifiStatus *self, const char *format, ...)
{
	va_list ap;
	char *cmd, *r;
	gboolean ok;

	va_start (ap, format);
	cmd = g_strdup_vprintf (format, ap);
	va_end (ap);
	r = ctrl_ask (self, self->req, cmd);
	ok = r && g_str_has_prefix (r, "OK");
	g_free (cmd);
	g_free (r);
	return ok;
}

/* key=value of STATUS */
static char *
status_value (const char *status, const char *key)
{
	char **lines = g_strsplit (status, "\n", -1), **l, *v = NULL;
	size_t k = strlen (key);

	for (l = lines; *l && !v; l++)
		if (!strncmp (*l, key, k) && (*l)[k] == '=')
			v = g_strdup (*l + k + 1);
	g_strfreev (lines);
	return v;
}

static void
bss_free (gpointer p)
{
	Bss *b = p;

	g_free (b->ssid);
	g_free (b->flags);
	g_free (b);
}

/* SCAN_RESULTS: bssid / frequency / signal level / flags / ssid */
static void
read_scan (Pro1xWifiStatus *self)
{
	char *r = req (self, "SCAN_RESULTS"), **lines, **l;

	if (!r)
		return;
	g_ptr_array_set_size (self->bss, 0);
	lines = g_strsplit (r, "\n", -1);
	for (l = lines; *l; l++) {
		char **f = g_strsplit (*l, "\t", 5);

		if (g_strv_length (f) == 5 && strlen (f[0]) == 17 && *f[4]) {
			Bss *b = g_new0 (Bss, 1);

			g_strlcpy (b->bssid, f[0], sizeof b->bssid);
			b->level = atoi (f[2]);
			b->flags = g_strdup (f[3]);
			b->ssid = ssid_decode (f[4]);
			g_ptr_array_add (self->bss, b);
		}
		g_strfreev (f);
	}
	g_strfreev (lines);
	g_free (r);
}

static Bss *
find_bss (Pro1xWifiStatus *self, const char *ssid)
{
	Bss *best = NULL;
	guint i;

	for (i = 0; i < self->bss->len; i++) {
		Bss *b = g_ptr_array_index (self->bss, i);

		if (!strcmp (b->ssid, ssid) && (!best || b->level > best->level))
			best = b;
	}
	return best;
}

/* the id of the saved network for SSID, or -1 (LIST_NETWORKS: network id /
 * ssid / bssid / flags) */
static int
saved_id (Pro1xWifiStatus *self, const char *ssid)
{
	char *r = req (self, "LIST_NETWORKS"), **lines, **l;
	int id = -1;

	if (!r)
		return -1;
	lines = g_strsplit (r, "\n", -1);
	for (l = lines[0] ? lines + 1 : lines; *l && id < 0; l++) {
		char **f = g_strsplit (*l, "\t", 4);

		if (g_strv_length (f) >= 2) {
			char *s = ssid_decode (f[1]);

			if (!strcmp (s, ssid))
				id = atoi (f[0]);
			g_free (s);
		}
		g_strfreev (f);
	}
	g_strfreev (lines);
	g_free (r);
	return id;
}

/* the SSIDs of all saved networks, in one request */
static GHashTable *
saved_set (Pro1xWifiStatus *self)
{
	GHashTable *set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	char *r = req (self, "LIST_NETWORKS"), **lines, **l;

	if (!r)
		return set;
	lines = g_strsplit (r, "\n", -1);
	for (l = lines[0] ? lines + 1 : lines; *l; l++) {
		char **f = g_strsplit (*l, "\t", 4);

		if (g_strv_length (f) >= 2)
			g_hash_table_add (set, ssid_decode (f[1]));
		g_strfreev (f);
	}
	g_strfreev (lines);
	g_free (r);
	return set;
}

/* --- the status area and the button --------------------------------------- */

static gboolean
update (gpointer data)
{
	Pro1xWifiStatus *self = data;
	char *status = NULL, *state = NULL, *ssid = NULL, *bssid = NULL, *value;
	const char *image = "statusarea_internetconnection_no";
	GdkPixbuf *area = NULL, *pb;

	self->update_id = 0;
	if (self->req >= 0)
		status = req (self, "STATUS");
	if (status) {
		char *raw;

		state = status_value (status, "wpa_state");
		raw = status_value (status, "ssid");
		ssid = raw ? ssid_decode (raw) : NULL;
		bssid = status_value (status, "bssid");
		g_free (raw);
	}

	if (!status || !g_strcmp0 (state, "INTERFACE_DISABLED")) {
		value = g_strdup ("Wi-Fi is off");
	} else if (!g_strcmp0 (state, "COMPLETED") && ssid) {
		char *show = ssid_show (ssid);
		Bss *b = NULL;
		guint i;

		for (i = 0; bssid && i < self->bss->len && !b; i++)
			if (!g_ascii_strcasecmp (((Bss *) g_ptr_array_index (self->bss, i))->bssid, bssid))
				b = g_ptr_array_index (self->bss, i);
		if (b) {
			value = g_strdup_printf ("%s, %d %%", show, percent (b->level));
			area = icon (signal_icon (percent (b->level)), AREA_ICON_SIZE);
		} else {
			value = g_strdup (show);
			area = icon ("connectivity_wlan_signal_good", AREA_ICON_SIZE);
		}
		image = "general_wlan";
		g_free (show);
	} else if (state && (!strcmp (state, "ASSOCIATING") || !strcmp (state, "ASSOCIATED") ||
	                     !strcmp (state, "AUTHENTICATING") || !strncmp (state, "4WAY", 4) ||
	                     !strncmp (state, "GROUP", 5))) {
		char *show = ssid_show (self->pending ? self->pending : ssid ? ssid : "Wi-Fi");

		value = g_strdup_printf ("Connecting to %s...", show);
		image = "general_wlan";
		g_free (show);
	} else {
		value = g_strdup ("Not connected");
	}

	hildon_button_set_text (HILDON_BUTTON (self->button), "Internet", value);
	pb = icon (image, MENU_ICON_SIZE);
	hildon_button_set_image (HILDON_BUTTON (self->button),
	                         pb ? gtk_image_new_from_pixbuf (pb) : NULL);
	if (pb)
		g_object_unref (pb);
	hd_status_plugin_item_set_status_area_icon (HD_STATUS_PLUGIN_ITEM (self), area);
	if (area)
		g_object_unref (area);
	g_free (value);
	g_free (status);
	g_free (state);
	g_free (ssid);
	g_free (bssid);
	return FALSE;
}

static void
queue_update (Pro1xWifiStatus *self)
{
	if (!self->update_id)
		self->update_id = g_idle_add (update, self);
}

/* --- connecting ------------------------------------------------------------------- */

static void
pending_clear (Pro1xWifiStatus *self, gboolean give_up)
{
	if (give_up && self->pending_new && self->pending_id >= 0)
		req_ok (self, "REMOVE_NETWORK %d", self->pending_id);
	g_free (self->pending);
	self->pending = NULL;
	self->pending_id = -1;
	self->pending_new = FALSE;
	self->asked = FALSE;
}

/* the network ID, with the other networks back in the running afterwards
 * (SELECT_NETWORK disables them) */
static void
select_network (Pro1xWifiStatus *self, const char *ssid, int id, gboolean new)
{
	if (!self->pending || strcmp (self->pending, ssid)) {
		pending_clear (self, FALSE);
		self->pending = g_strdup (ssid);
	}
	self->pending_id = id;
	self->pending_new = self->pending_new || new;
	if (!req_ok (self, "SELECT_NETWORK %d", id)) {
		char *show = ssid_show (ssid);

		banner ("Could not connect to %s", show);
		g_free (show);
		pending_clear (self, TRUE);
	}
	queue_update (self);
}

/* a new network for SSID, NULL when wpa_supplicant refused it */
static int
add_network (Pro1xWifiStatus *self, const char *ssid, const char *km, const char *password)
{
	char *r = req (self, "ADD_NETWORK"), *hex;
	int id;

	if (!r || !g_ascii_isdigit (r[0])) {
		g_free (r);
		return -1;
	}
	id = atoi (r);
	g_free (r);
	hex = ssid_hex (ssid);
	if (!req_ok (self, "SET_NETWORK %d ssid %s", id, hex) ||
	    !req_ok (self, "SET_NETWORK %d key_mgmt %s", id, km) ||
	    (password && !req_ok (self, "SET_NETWORK %d psk \"%s\"", id, password)) ||
	    (!strcmp (km, "SAE") && !req_ok (self, "SET_NETWORK %d ieee80211w 2", id))) {
		req_ok (self, "REMOVE_NETWORK %d", id);
		id = -1;
	}
	g_free (hex);
	return id;
}

static void
ask_password (Pro1xWifiStatus *self, const char *ssid, int id, gboolean again)
{
	GtkWidget *dialog, *entry, *label, *box;
	char *title = ssid_show (ssid);
	const char *pw;
	Bss *b;

	dialog = hildon_dialog_new_with_buttons (title, NULL, GTK_DIALOG_MODAL,
	                                         "Connect", GTK_RESPONSE_OK, NULL);
	g_free (title);
	box = gtk_vbox_new (FALSE, 8);
	label = gtk_label_new (again ? "The password was not accepted. Password:" : "Password:");
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

	if (gtk_dialog_run (GTK_DIALOG (dialog)) != GTK_RESPONSE_OK) {
		gtk_widget_destroy (dialog);
		if (self->pending && !strcmp (self->pending, ssid))
			pending_clear (self, TRUE);
		return;
	}
	pw = gtk_entry_get_text (GTK_ENTRY (entry));
	if (strlen (pw) < 8 || strlen (pw) > 63 || strchr (pw, '\n')) {
		gtk_widget_destroy (dialog);
		banner ("A Wi-Fi password has 8 to 63 characters");
		if (self->pending && !strcmp (self->pending, ssid))
			pending_clear (self, TRUE);
		return;
	}
	if (id >= 0) {
		/* a saved network whose password was wrong or missing */
		if (req_ok (self, "SET_NETWORK %d psk \"%s\"", id, pw))
			select_network (self, ssid, id, FALSE);
	} else if ((b = find_bss (self, ssid)) && key_mgmt (b->flags) &&
	           (id = add_network (self, ssid, key_mgmt (b->flags), pw)) >= 0) {
		select_network (self, ssid, id, TRUE);
	} else {
		char *show = ssid_show (ssid);

		banner ("Could not add %s", show);
		g_free (show);
	}
	self->asked = TRUE;
	gtk_widget_destroy (dialog);
}

/* a tap on a network in the list */
static void
connect_to (Pro1xWifiStatus *self, const char *ssid)
{
	int id = saved_id (self, ssid);
	Bss *b = find_bss (self, ssid);
	const char *km = b ? key_mgmt (b->flags) : NULL;
	char *show;

	if (id >= 0) {
		select_network (self, ssid, id, FALSE);
		return;
	}
	show = ssid_show (ssid);
	if (!b)
		banner ("%s is not in reach", show);
	else if (!km)
		banner ("%s needs a kind of login this cannot do", show);
	else if (!strcmp (km, "NONE")) {
		if ((id = add_network (self, ssid, km, NULL)) >= 0)
			select_network (self, ssid, id, TRUE);
	} else
		ask_password (self, ssid, -1, FALSE);
	g_free (show);
}

/* --- events ----------------------------------------------------------------------- */

static void
refill (Pro1xWifiStatus *self);

static void
on_event (Pro1xWifiStatus *self, const char *ev)
{
	if (strstr (ev, "CTRL-EVENT-CONNECTED")) {
		if (self->pending) {
			char *show = ssid_show (self->pending);

			banner ("Connected to %s", show);
			g_free (show);
			/* the others back as candidates, the new one kept */
			req_ok (self, "ENABLE_NETWORK all");
			req_ok (self, "SAVE_CONFIG");
			pending_clear (self, FALSE);
		}
		read_scan (self);
	} else if (strstr (ev, "CTRL-EVENT-SSID-TEMP-DISABLED") && strstr (ev, "reason=WRONG_KEY")) {
		if (self->pending && self->pending_id >= 0) {
			char *ssid = g_strdup (self->pending);
			int id = self->pending_id;

			ask_password (self, ssid, id, TRUE);
			g_free (ssid);
		}
	} else if (strstr (ev, "CTRL-EVENT-SCAN-RESULTS")) {
		read_scan (self);
		if (self->dialog)
			refill (self);
	} else if (strstr (ev, "CTRL-EVENT-TERMINATING")) {
		ctrl_close (self);
	}
	queue_update (self);
}

static gboolean ctrl_open (gpointer data);

static gboolean
mon_cb (GIOChannel *ch, GIOCondition cond, gpointer data)
{
	Pro1xWifiStatus *self = data;
	char buf[4096];
	ssize_t n;

	if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
		self->mon_watch = 0;
		ctrl_close (self);
		queue_update (self);
		return FALSE;
	}
	while ((n = recv (self->mon, buf, sizeof buf - 1, MSG_DONTWAIT)) > 0) {
		buf[n] = '\0';
		on_event (self, buf);
		if (self->mon < 0)
			return FALSE;
	}
	return TRUE;
}

static void
schedule_retry (Pro1xWifiStatus *self)
{
	if (self->retry_id)
		return;
	self->retry_id = g_timeout_add_seconds (self->retry_delay, ctrl_open, self);
	/* 5 s, 10, 20, ... up to two minutes while Wi-Fi is off */
	self->retry_delay = MIN (self->retry_delay * 2, 120);
}

static void
ctrl_close (Pro1xWifiStatus *self)
{
	if (self->mon_watch) {
		g_source_remove (self->mon_watch);
		self->mon_watch = 0;
	}
	if (self->req >= 0) {
		close (self->req);
		self->req = -1;
	}
	if (self->mon >= 0) {
		close (self->mon);
		self->mon = -1;
	}
	if (self->req_path) {
		unlink (self->req_path);
		g_free (self->req_path);
		self->req_path = NULL;
	}
	if (self->mon_path) {
		unlink (self->mon_path);
		g_free (self->mon_path);
		self->mon_path = NULL;
	}
	g_ptr_array_set_size (self->bss, 0);
	schedule_retry (self);
}

static gboolean
ctrl_open (gpointer data)
{
	Pro1xWifiStatus *self = data;
	char *r;

	self->retry_id = 0;
	if (self->req >= 0)
		return FALSE;
	self->req = ctrl_socket (&self->req_path);
	self->mon = self->req >= 0 ? ctrl_socket (&self->mon_path) : -1;
	if (self->mon < 0) {
		ctrl_close (self);
		queue_update (self);
		return FALSE;
	}
	r = ctrl_ask (self, self->mon, "ATTACH");
	if (!r || !g_str_has_prefix (r, "OK")) {
		g_free (r);
		if (self->req >= 0)
			ctrl_close (self);
		queue_update (self);
		return FALSE;
	}
	g_free (r);
	{
		GIOChannel *ch = g_io_channel_unix_new (self->mon);

		self->mon_watch = g_io_add_watch (ch, G_IO_IN | G_IO_HUP | G_IO_ERR,
		                                  mon_cb, self);
		g_io_channel_unref (ch);
	}
	self->retry_delay = 5;
	read_scan (self);
	queue_update (self);
	return FALSE;
}

/* --- the list of networks --------------------------------------------------- */

static void
network_clicked_cb (GtkButton *button, gpointer data)
{
	Pro1xWifiStatus *self = data;
	char *ssid = g_strdup (g_object_get_data (G_OBJECT (button), "ssid"));

	gtk_dialog_response (GTK_DIALOG (self->dialog), GTK_RESPONSE_CLOSE);
	connect_to (self, ssid);
	g_free (ssid);
}

static gint
by_level (gconstpointer a, gconstpointer b)
{
	return (*(Bss **) b)->level - (*(Bss **) a)->level;
}

static void
refill (Pro1xWifiStatus *self)
{
	GList *old = gtk_container_get_children (GTK_CONTAINER (self->list)), *l;
	GPtrArray *sorted = g_ptr_array_new ();
	GHashTable *seen = g_hash_table_new (g_str_hash, g_str_equal), *saved;
	char *status, *cur = NULL, *raw;
	guint i, shown = 0;

	/* the requests first: one that fails closes the connection, which
	 * empties the scan list the loop below walks */
	status = req (self, "STATUS");
	saved = saved_set (self);

	if (status && (raw = status_value (status, "ssid"))) {
		char *state = status_value (status, "wpa_state");

		if (!g_strcmp0 (state, "COMPLETED"))
			cur = ssid_decode (raw);
		g_free (state);
		g_free (raw);
	}
	for (l = old; l; l = l->next)
		gtk_widget_destroy (l->data);
	g_list_free (old);

	for (i = 0; i < self->bss->len; i++)
		g_ptr_array_add (sorted, g_ptr_array_index (self->bss, i));
	g_ptr_array_sort (sorted, by_level);

	for (i = 0; i < sorted->len; i++) {
		Bss *b = g_ptr_array_index (sorted, i);
		const char *km = key_mgmt (b->flags), *what;
		char *show, *detail;
		GtkWidget *w;
		GdkPixbuf *pb;

		if (g_hash_table_contains (seen, b->ssid))
			continue;
		g_hash_table_add (seen, b->ssid);
		if (cur && !strcmp (cur, b->ssid))
			what = "connected";
		else if (g_hash_table_contains (saved, b->ssid))
			what = "saved";
		else if (km && !strcmp (km, "NONE"))
			what = "open";
		else if (km)
			what = "password";
		else
			what = "not supported";
		show = ssid_show (b->ssid);
		detail = g_strdup_printf ("%s, %d %%", what, percent (b->level));
		w = hildon_button_new_with_text (HILDON_SIZE_AUTO_WIDTH | HILDON_SIZE_FINGER_HEIGHT,
		                                 HILDON_BUTTON_ARRANGEMENT_VERTICAL, show, detail);
		hildon_button_set_style (HILDON_BUTTON (w), HILDON_BUTTON_STYLE_PICKER);
		hildon_button_set_alignment (HILDON_BUTTON (w), 0.0, 0.5, 1.0, 1.0);
		pb = icon (signal_icon (percent (b->level)), MENU_ICON_SIZE);
		if (pb) {
			hildon_button_set_image (HILDON_BUTTON (w), gtk_image_new_from_pixbuf (pb));
			g_object_unref (pb);
		}
		g_object_set_data_full (G_OBJECT (w), "ssid", g_strdup (b->ssid), g_free);
		g_signal_connect (w, "clicked", G_CALLBACK (network_clicked_cb), self);
		gtk_box_pack_start (GTK_BOX (self->list), w, FALSE, FALSE, 0);
		g_free (show);
		g_free (detail);
		shown++;
	}
	if (!shown)
		gtk_box_pack_start (GTK_BOX (self->list),
		                    gtk_label_new (self->req >= 0 ? "Looking for networks..." : "Wi-Fi is off"),
		                    FALSE, FALSE, 16);
	gtk_widget_show_all (self->list);
	g_ptr_array_unref (sorted);
	g_hash_table_unref (seen);
	g_hash_table_unref (saved);
	g_free (status);
	g_free (cur);
}

/* wlan0 down or up (the session is root's); wpa_supplicant follows. With
 * no wpa_supplicant at all the Wi-Fi's service is started (rc.d/pro1x_wifi:
 * the modem's firmware server, ath10k, wlan0; up to a minute). */
static void
wifi_switch (Pro1xWifiStatus *self, gboolean on)
{
	char *argv[] = { IFCONFIG, IFACE, on ? "up" : "down", NULL };
	char *service[] = { "/usr/sbin/service", "pro1x_wifi", "onestart", NULL };
	GError *err = NULL;
	int st = 0;

	if (on && self->req < 0) {
		if (g_spawn_async (NULL, service, NULL,
		                   G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
		                   NULL, NULL, NULL, &err)) {
			banner ("Starting Wi-Fi...");
			if (self->retry_id)
				g_source_remove (self->retry_id);
			self->retry_id = 0;
			self->retry_delay = 5;
			schedule_retry (self);
		} else {
			banner ("Could not start Wi-Fi");
			g_clear_error (&err);
		}
		return;
	}

	if (!g_spawn_sync (NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
	                   NULL, NULL, NULL, NULL, &st, &err) || st != 0) {
		banner ("Could not switch Wi-Fi %s", on ? "on" : "off");
		g_clear_error (&err);
	}
	if (on)
		req_ok (self, "RECONNECT");
	queue_update (self);
}

enum { RESPONSE_DISCONNECT = 1, RESPONSE_WIFI };

static void
button_clicked_cb (GtkButton *button, gpointer data)
{
	Pro1xWifiStatus *self = data;
	GtkWidget *area;
	char *status, *state = NULL;
	gboolean off, connected;
	gint response;

	if (self->dialog)
		return;
	status = self->req >= 0 ? req (self, "STATUS") : NULL;
	/* not open, or wpa_supplicant started anew since (its old socket is
	 * gone and a killed one sends no TERMINATING): open now */
	if (!status) {
		ctrl_open (self);
		status = self->req >= 0 ? req (self, "STATUS") : NULL;
	}
	/* the status menu closes, as Maemo's own items do */
	gtk_widget_hide (gtk_widget_get_toplevel (GTK_WIDGET (button)));
	if (status)
		state = status_value (status, "wpa_state");
	off = !status || !g_strcmp0 (state, "INTERFACE_DISABLED");
	connected = !g_strcmp0 (state, "COMPLETED");
	g_free (status);
	g_free (state);

	self->dialog = hildon_dialog_new_with_buttons ("Wi-Fi networks", NULL,
	                                               GTK_DIALOG_MODAL, NULL);
	if (connected)
		gtk_dialog_add_button (GTK_DIALOG (self->dialog), "Disconnect", RESPONSE_DISCONNECT);
	gtk_dialog_add_button (GTK_DIALOG (self->dialog),
	                       off ? "Wi-Fi on" : "Wi-Fi off", RESPONSE_WIFI);
	self->list = gtk_vbox_new (FALSE, 0);
	area = hildon_pannable_area_new ();
	g_object_set (area, "size-request-policy", HILDON_SIZE_REQUEST_CHILDREN, NULL);
	hildon_pannable_area_add_with_viewport (HILDON_PANNABLE_AREA (area), self->list);
	gtk_container_add (GTK_CONTAINER (GTK_DIALOG (self->dialog)->vbox), area);
	refill (self);
	gtk_widget_show_all (self->dialog);
	if (!off)
		req_ok (self, "SCAN");

	response = gtk_dialog_run (GTK_DIALOG (self->dialog));
	gtk_widget_destroy (self->dialog);
	self->dialog = self->list = NULL;

	if (response == RESPONSE_DISCONNECT)
		req_ok (self, "DISCONNECT");
	else if (response == RESPONSE_WIFI)
		wifi_switch (self, off);
	queue_update (self);
}

/* --- the plugin -------------------------------------------------------------- */

static void
pro1x_wifi_status_init (Pro1xWifiStatus *self)
{
	self->req = self->mon = -1;
	self->pending_id = -1;
	self->retry_delay = 5;
	self->bss = g_ptr_array_new_with_free_func (bss_free);
	self->button = hildon_button_new (HILDON_SIZE_AUTO_WIDTH | HILDON_SIZE_FINGER_HEIGHT,
	                                  HILDON_BUTTON_ARRANGEMENT_VERTICAL);
	hildon_button_set_style (HILDON_BUTTON (self->button), HILDON_BUTTON_STYLE_PICKER);
	hildon_button_set_alignment (HILDON_BUTTON (self->button), 0.0, 0.5, 1.0, 1.0);
	g_signal_connect (self->button, "clicked", G_CALLBACK (button_clicked_cb), self);
	gtk_container_add (GTK_CONTAINER (self), self->button);
	ctrl_open (self);
	update (self);
	gtk_widget_show_all (GTK_WIDGET (self));
}

static void
pro1x_wifi_status_finalize (GObject *object)
{
	Pro1xWifiStatus *self = PRO1X_WIFI_STATUS (object);

	if (self->update_id)
		g_source_remove (self->update_id);
	ctrl_close (self);
	if (self->retry_id)
		g_source_remove (self->retry_id);
	pending_clear (self, FALSE);
	g_ptr_array_unref (self->bss);
	G_OBJECT_CLASS (pro1x_wifi_status_parent_class)->finalize (object);
}

static void
pro1x_wifi_status_class_init (Pro1xWifiStatusClass *klass)
{
	G_OBJECT_CLASS (klass)->finalize = pro1x_wifi_status_finalize;
}

static void
pro1x_wifi_status_class_finalize (Pro1xWifiStatusClass *klass)
{
}
