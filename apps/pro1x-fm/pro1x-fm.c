/*
 * pro1x-fm -- FM radio for the F(x)tec Pro1X on Hildon.
 *
 * Drives the WCN3950's FM receiver through /dev/radio0 (the kernel's
 * radio-qca-fm, device/kernel/patches/0015): frequency, seek, mute, the
 * signal and stereo, the RDS station name and radio text, saved stations.
 * The receiver runs while the window is open (the driver brings Bluetooth
 * up for it if it is off). The headphone cable is the antenna.
 *
 * The sound does not come through /dev/radio0: the chip sends it over
 * SLIMbus to the DSP, which gives a capture source. When PulseAudio has a
 * source with "fm" in its name, a loopback plays it while the radio is on
 * and not muted; without one the radio tunes and shows the station, silent.
 *
 *   Left/Right  0.1 MHz down/up      Up/Down  seek up/down
 *   m           mute
 *
 * All ioctls run in one thread of their own: tuning waits for the chip,
 * a seek takes seconds.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <libintl.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <hildon/hildon.h>

#define DOMAIN "pro1x-fm"
#define _(s) dgettext(DOMAIN, s)
#define DEVICE "/dev/radio0"
#define BAND_LOW 87500		/* kHz */
#define BAND_HIGH 108000
#define STEP 100
#define POLL_MS 1500

enum req { OPEN, TUNE, SEEK_UP, SEEK_DOWN, MUTE, UNMUTE, POLL, QUIT };

struct result {
	enum req req;
	int err;		/* errno of the request, 0 */
	unsigned freq;		/* kHz */
	unsigned signal;	/* 0..100 */
	gboolean stereo, rds;
	char ps[9], rt[65];
};

static GAsyncQueue *requests;
static int fd = -1;
static unsigned freq = BAND_LOW;
static gboolean muted, busy, opened;
static int loopback = -1;	/* PulseAudio module */
static GtkWidget *window, *freq_label, *ps_label, *rt_label, *signal_bar,
		 *stereo_label, *status_label, *mute_button;
static GKeyFile *conf;
static char *conf_path;

/* ---- the radio thread ---- */

/* a request and its argument in one (never NULL) pointer */
static void push(enum req req, unsigned arg)
{
	g_async_queue_push(requests, GUINT_TO_POINTER((req + 1) | arg << 4));
}

static int set_mute(int on)
{
	struct v4l2_control c = { .id = V4L2_CID_AUDIO_MUTE, .value = on };

	return ioctl(fd, VIDIOC_S_CTRL, &c) ? errno : 0;
}

static int set_freq(unsigned khz)
{
	struct v4l2_frequency f = {
		.type = V4L2_TUNER_RADIO, .frequency = khz * 16,
	};

	return ioctl(fd, VIDIOC_S_FREQUENCY, &f) ? errno : 0;
}

static int seek(int up)
{
	struct v4l2_hw_freq_seek s = {
		.type = V4L2_TUNER_RADIO, .seek_upward = up, .wrap_around = 1,
	};

	return ioctl(fd, VIDIOC_S_HW_FREQ_SEEK, &s) ? errno : 0;
}

static void status(struct result *r)
{
	struct v4l2_frequency f = { .type = V4L2_TUNER_RADIO };
	struct v4l2_tuner t = { .index = 0 };
	struct v4l2_ext_control c[2] = {
		{ .id = V4L2_CID_RDS_RX_PS_NAME, .size = sizeof(r->ps), .string = r->ps },
		{ .id = V4L2_CID_RDS_RX_RADIO_TEXT, .size = sizeof(r->rt), .string = r->rt },
	};
	struct v4l2_ext_controls cs = {
		.which = V4L2_CTRL_WHICH_CUR_VAL, .count = 2, .controls = c,
	};

	if (!ioctl(fd, VIDIOC_G_FREQUENCY, &f))
		r->freq = f.frequency / 16;
	if (!ioctl(fd, VIDIOC_G_TUNER, &t)) {
		r->signal = t.signal * 100 / 65535;
		r->stereo = !!(t.rxsubchans & V4L2_TUNER_SUB_STEREO);
		r->rds = !!(t.rxsubchans & V4L2_TUNER_SUB_RDS);
	} else if (!r->err) {
		r->err = errno;
	}
	if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &cs))
		r->ps[0] = r->rt[0] = '\0';
}

static gboolean result_cb(gpointer data);

static gpointer radio_thread(gpointer data)
{
	(void)data;
	for (;;) {
		guint v = GPOINTER_TO_UINT(g_async_queue_pop(requests));
		struct result *r = g_new0(struct result, 1);

		r->req = (v & 0xf) - 1;
		switch (r->req) {
		case OPEN:
			fd = open(DEVICE, O_RDWR);
			if (fd < 0)
				r->err = errno;
			else
				r->err = set_freq(v >> 4);
			break;
		case TUNE:
			r->err = set_freq(v >> 4);
			break;
		case SEEK_UP:
		case SEEK_DOWN:
			r->err = seek(r->req == SEEK_UP);
			break;
		case MUTE:
		case UNMUTE:
			r->err = set_mute(r->req == MUTE);
			break;
		case POLL:
			break;
		case QUIT:
			if (fd >= 0)
				close(fd);
			g_free(r);
			return NULL;
		}
		if (fd >= 0)
			status(r);
		g_idle_add(result_cb, r);
	}
}

/* ---- sound: the SLIMbus capture source played back ---- */

static char *fm_source(void)
{
	char *out = NULL, **lines, *name = NULL;
	int i;

	if (!g_spawn_command_line_sync("pactl list short sources", &out, NULL, NULL, NULL))
		return NULL;
	lines = g_strsplit(out, "\n", -1);
	for (i = 0; lines[i] && !name; i++) {
		char **f = g_strsplit(lines[i], "\t", 3);
		char *low = f[0] && f[1] ? g_ascii_strdown(f[1], -1) : NULL;

		if (low && strstr(low, "fm") && !g_str_has_suffix(low, ".monitor"))
			name = g_strdup(f[1]);
		g_free(low);
		g_strfreev(f);
	}
	g_strfreev(lines);
	g_free(out);
	return name;
}

static void sound(gboolean on)
{
	char *cmd, *out = NULL, *src;

	if (!on && loopback >= 0) {
		cmd = g_strdup_printf("pactl unload-module %d", loopback);
		g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);
		g_free(cmd);
		loopback = -1;
	}
	if (!on || loopback >= 0)
		return;
	src = fm_source();
	if (!src) {
		gtk_label_set_text(GTK_LABEL(status_label),
				   _("No FM sound yet: the receiver's SLIMbus path is missing."));
		return;
	}
	cmd = g_strdup_printf("pactl load-module module-loopback source=%s latency_msec=100", src);
	if (g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL) && out && *out)
		loopback = atoi(out);
	g_free(out);
	g_free(cmd);
	g_free(src);
}

/* ---- settings: the last frequency, saved stations ---- */

static void save_conf(void)
{
	char *dir = g_path_get_dirname(conf_path);

	g_mkdir_with_parents(dir, 0700);
	g_key_file_set_integer(conf, "radio", "frequency", freq);
	g_key_file_save_to_file(conf, conf_path, NULL);
	g_free(dir);
}

/* ---- the window ---- */

static void show_freq(unsigned khz)
{
	char *s = g_strdup_printf("%u.%u MHz", khz / 1000, khz % 1000 / 100);

	gtk_label_set_text(GTK_LABEL(freq_label), s);
	g_free(s);
}

static const char *strerr(int err)
{
	switch (err) {
	case ENOENT:
	case ENODEV:
		return _("No FM receiver (kernel without radio-qca-fm?)");
	case ENETDOWN:
	case ERFKILL:
		return _("Bluetooth is blocked or failed: the receiver needs it powered.");
	case ETIMEDOUT:
		return _("The receiver does not answer.");
	case ENODATA:
		return _("No station found.");
	case EBUSY:
		return _("The receiver is busy.");
	default:
		return g_strerror(err);
	}
}

static gboolean result_cb(gpointer data)
{
	struct result *r = data;

	if (r->req == OPEN && !r->err) {
		opened = TRUE;
		push(muted ? MUTE : UNMUTE, 0);
		sound(!muted);
	}
	if (r->req != POLL)
		busy = FALSE;
	if (r->err)
		gtk_label_set_text(GTK_LABEL(status_label), strerr(r->err));
	else if (r->req != POLL)
		gtk_label_set_text(GTK_LABEL(status_label), "");

	if (opened) {
		if (r->freq >= BAND_LOW && r->freq <= BAND_HIGH && r->freq != freq) {
			freq = r->freq;
			save_conf();
		}
		show_freq(freq);
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(signal_bar), r->signal / 100.0);
		gtk_label_set_text(GTK_LABEL(stereo_label),
				   r->rds ? (r->stereo ? _("Stereo  RDS") : _("Mono  RDS"))
					  : (r->stereo ? _("Stereo") : _("Mono")));
		gtk_label_set_text(GTK_LABEL(ps_label), g_strstrip(r->ps));
		gtk_label_set_text(GTK_LABEL(rt_label), g_strstrip(r->rt));
	}
	g_free(r);
	return FALSE;
}

static gboolean poll_cb(gpointer data)
{
	(void)data;
	if (opened && !busy && g_async_queue_length(requests) == 0)
		push(POLL, 0);
	return TRUE;
}

static void request(enum req req, unsigned arg)
{
	if (!opened || busy)
		return;
	busy = TRUE;
	if (req == SEEK_UP || req == SEEK_DOWN)
		gtk_label_set_text(GTK_LABEL(status_label), _("Searching..."));
	push(req, arg);
}

static void step(int dir)
{
	unsigned f = freq + dir * STEP;

	if (f < BAND_LOW)
		f = BAND_HIGH;
	else if (f > BAND_HIGH)
		f = BAND_LOW;
	freq = f;
	show_freq(f);
	request(TUNE, f);
}

static void down_cb(GtkWidget *w, gpointer data) { (void)w, (void)data; step(-1); }
static void up_cb(GtkWidget *w, gpointer data) { (void)w, (void)data; step(1); }
static void seek_down_cb(GtkWidget *w, gpointer data) { (void)w, (void)data; request(SEEK_DOWN, 0); }
static void seek_up_cb(GtkWidget *w, gpointer data) { (void)w, (void)data; request(SEEK_UP, 0); }

static void mute_cb(GtkToggleButton *b, gpointer data)
{
	(void)data;
	muted = gtk_toggle_button_get_active(b);
	if (opened) {
		push(muted ? MUTE : UNMUTE, 0);
		sound(!muted);
	}
}

static gboolean key_cb(GtkWidget *w, GdkEventKey *e, gpointer data)
{
	(void)w, (void)data;
	switch (e->keyval) {
	case GDK_Left:
		step(-1);
		return TRUE;
	case GDK_Right:
		step(1);
		return TRUE;
	case GDK_Up:
		request(SEEK_UP, 0);
		return TRUE;
	case GDK_Down:
		request(SEEK_DOWN, 0);
		return TRUE;
	case GDK_m:
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mute_button), !muted);
		return TRUE;
	}
	return FALSE;
}

/* saved stations: [stations] <kHz>=<name> */
static void save_station_cb(GtkButton *b, gpointer data)
{
	const char *ps = gtk_label_get_text(GTK_LABEL(ps_label));
	char key[16];

	(void)b, (void)data;
	g_snprintf(key, sizeof(key), "%u", freq);
	g_key_file_set_string(conf, "stations", key, *ps ? ps : "");
	save_conf();
	hildon_banner_show_information(window, NULL, _("Station saved"));
}

static void stations_cb(GtkButton *b, gpointer data)
{
	GtkWidget *dialog, *sel;
	char **keys;
	gsize n, i;

	(void)b, (void)data;
	keys = g_key_file_get_keys(conf, "stations", &n, NULL);
	if (!n) {
		hildon_banner_show_information(window, NULL, _("No saved stations"));
		g_strfreev(keys);
		return;
	}
	sel = hildon_touch_selector_new_text();
	for (i = 0; i < n; i++) {
		unsigned khz = strtoul(keys[i], NULL, 10);
		char *name = g_key_file_get_string(conf, "stations", keys[i], NULL);
		char *row = g_strdup_printf("%u.%u MHz  %s", khz / 1000, khz % 1000 / 100,
					    name ? name : "");

		hildon_touch_selector_append_text(HILDON_TOUCH_SELECTOR(sel), row);
		g_free(row);
		g_free(name);
	}
	dialog = hildon_picker_dialog_new(GTK_WINDOW(window));
	gtk_window_set_title(GTK_WINDOW(dialog), _("Stations"));
	hildon_picker_dialog_set_selector(HILDON_PICKER_DIALOG(dialog),
					  HILDON_TOUCH_SELECTOR(sel));
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
		int row = hildon_touch_selector_get_active(HILDON_TOUCH_SELECTOR(sel), 0);

		if (row >= 0 && (gsize)row < n) {
			freq = strtoul(keys[row], NULL, 10);
			show_freq(freq);
			request(TUNE, freq);
		}
	}
	gtk_widget_destroy(dialog);
	g_strfreev(keys);
}

static void forget_station_cb(GtkButton *b, gpointer data)
{
	char key[16];

	(void)b, (void)data;
	g_snprintf(key, sizeof(key), "%u", freq);
	if (g_key_file_remove_key(conf, "stations", key, NULL)) {
		save_conf();
		hildon_banner_show_information(window, NULL, _("Station removed"));
	}
}

static GtkWidget *button(const char *label, GCallback cb)
{
	GtkWidget *b = hildon_gtk_button_new(HILDON_SIZE_FINGER_HEIGHT);

	gtk_button_set_label(GTK_BUTTON(b), label);
	g_signal_connect(b, "clicked", cb, NULL);
	return b;
}

static GtkWidget *label(const char *font, gboolean wrap)
{
	GtkWidget *l = gtk_label_new(NULL);

	hildon_helper_set_logical_font(l, font);
	gtk_label_set_line_wrap(GTK_LABEL(l), wrap);
	gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
	return l;
}

static void build(void)
{
	HildonAppMenu *menu = HILDON_APP_MENU(hildon_app_menu_new());
	GtkWidget *box, *row, *b;
	PangoFontDescription *big;

	window = hildon_stackable_window_new();
	gtk_window_set_title(GTK_WINDOW(window), _("FM Radio"));
	g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(window, "key-press-event", G_CALLBACK(key_cb), NULL);

	freq_label = gtk_label_new(NULL);
	big = pango_font_description_from_string("Sans Bold 40");
	gtk_widget_modify_font(freq_label, big);
	pango_font_description_free(big);
	ps_label = label("LargeSystemFont", FALSE);
	rt_label = label("SystemFont", TRUE);
	stereo_label = label("SmallSystemFont", FALSE);
	status_label = label("SmallSystemFont", TRUE);
	hildon_helper_set_logical_color(status_label, GTK_RC_FG, GTK_STATE_NORMAL,
					"SecondaryTextColor");
	signal_bar = gtk_progress_bar_new();

	row = gtk_hbox_new(TRUE, HILDON_MARGIN_DEFAULT);
	gtk_box_pack_start(GTK_BOX(row), button(_("Seek down"), G_CALLBACK(seek_down_cb)), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), button("-0.1", G_CALLBACK(down_cb)), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), button("+0.1", G_CALLBACK(up_cb)), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), button(_("Seek up"), G_CALLBACK(seek_up_cb)), TRUE, TRUE, 0);
	mute_button = hildon_gtk_toggle_button_new(HILDON_SIZE_FINGER_HEIGHT);
	gtk_button_set_label(GTK_BUTTON(mute_button), _("Mute"));
	g_signal_connect(mute_button, "toggled", G_CALLBACK(mute_cb), NULL);
	gtk_box_pack_start(GTK_BOX(row), mute_button, TRUE, TRUE, 0);

	box = gtk_vbox_new(FALSE, HILDON_MARGIN_DEFAULT);
	gtk_container_set_border_width(GTK_CONTAINER(box), HILDON_MARGIN_DOUBLE);
	gtk_box_pack_start(GTK_BOX(box), freq_label, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), ps_label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), rt_label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), signal_bar, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), stereo_label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), status_label, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(window), box);

	b = hildon_gtk_button_new(HILDON_SIZE_AUTO);
	gtk_button_set_label(GTK_BUTTON(b), _("Stations"));
	g_signal_connect(b, "clicked", G_CALLBACK(stations_cb), NULL);
	hildon_app_menu_append(menu, GTK_BUTTON(b));
	b = hildon_gtk_button_new(HILDON_SIZE_AUTO);
	gtk_button_set_label(GTK_BUTTON(b), _("Save station"));
	g_signal_connect(b, "clicked", G_CALLBACK(save_station_cb), NULL);
	hildon_app_menu_append(menu, GTK_BUTTON(b));
	b = hildon_gtk_button_new(HILDON_SIZE_AUTO);
	gtk_button_set_label(GTK_BUTTON(b), _("Remove station"));
	g_signal_connect(b, "clicked", G_CALLBACK(forget_station_cb), NULL);
	hildon_app_menu_append(menu, GTK_BUTTON(b));
	gtk_widget_show_all(GTK_WIDGET(menu));
	hildon_window_set_app_menu(HILDON_WINDOW(window), menu);

	show_freq(freq);
	gtk_label_set_text(GTK_LABEL(status_label),
			   _("Switching the receiver on (the headphone cable is the antenna)..."));
	gtk_widget_show_all(window);
}

int main(int argc, char **argv)
{
	GThread *thread;
	int f;

	bindtextdomain(DOMAIN, LOCALEDIR);
	bind_textdomain_codeset(DOMAIN, "UTF-8");
	hildon_gtk_init(&argc, &argv);

	conf = g_key_file_new();
	conf_path = g_build_filename(g_get_user_config_dir(), DOMAIN, "settings", NULL);
	g_key_file_load_from_file(conf, conf_path, G_KEY_FILE_KEEP_COMMENTS, NULL);
	f = g_key_file_get_integer(conf, "radio", "frequency", NULL);
	if (f >= BAND_LOW && f <= BAND_HIGH)
		freq = f;

	build();
	requests = g_async_queue_new();
	thread = g_thread_new("radio", radio_thread, NULL);
	push(OPEN, freq);
	g_timeout_add(POLL_MS, poll_cb, NULL);

	gtk_main();

	sound(FALSE);
	push(QUIT, 0);
	g_thread_join(thread);
	return 0;
}
