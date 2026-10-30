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
 * SLIMbus to the DSP, which records it (see sound() below); a PulseAudio
 * loopback plays it while the radio is on and not muted. Without that
 * path (a kernel before 7.1.0-pro1x.6) the radio tunes and shows the
 * station, silent.
 *
 *   Left/Right  0.1 MHz down/up      Up/Down  seek up/down
 *   m           mute
 *
 * All ioctls run in one thread of their own: tuning waits for the chip,
 * a seek takes seconds.
 *
 * On FreeBSD (freebsd/fm/README.md) there is no V4L2 and no radio driver:
 * pro1x-fmd drives the chip from user space, and the same requests go to
 * its socket instead (radio_open() and below). The sound is pro1x-fmd's
 * "audio on|off" there (pro1x-fmaudio: the SLIMbus and the DSP's
 * loopback), asked in the radio thread like the rest; FreeBSD has no
 * headphone output behind it yet. pro1x-audiod's jack detection
 * (freebsd/audio) says whether a plug is in: without one the app says to
 * plug in headphones, their cable being the antenna.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <linux/videodev2.h>
#else
#include <stdio.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <libintl.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <hildon/hildon.h>

#define DOMAIN "pro1x-fm"
#define _(s) dgettext(DOMAIN, s)
#ifdef __linux__
#define DEVICE "/dev/radio0"
#else
#ifndef FMD_SOCKET
#define FMD_SOCKET "/var/run/pro1x-fm.sock"
#endif
/* what pro1x-fmd answers for a seek that finds nothing */
#ifndef ENODATA
#define ENODATA ENOMSG
#endif
#endif
#define BAND_LOW 87500		/* kHz */
#define BAND_HIGH 108000
#define STEP 100
#define POLL_MS 1500

enum req { OPEN, TUNE, SEEK_UP, SEEK_DOWN, MUTE, UNMUTE, POLL, QUIT,
	   AUDIO_ON, AUDIO_OFF };

struct result {
	enum req req;
	int err;		/* errno of the request, 0 */
	char note[96];		/* AUDIO_ON: what the daemon said after "ok" */
	unsigned freq;		/* kHz */
	unsigned signal;	/* 0..100 */
	gboolean stereo, rds;
	char ps[9], rt[65];
	int jack;		/* 1 a plug in, 0 none, -1 unknown */
};

static GAsyncQueue *requests;
static int fd = -1;
static unsigned freq = BAND_LOW;
static gboolean muted, busy, opened, jack_hint;
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

#ifdef __linux__
static int radio_open(void)
{
	fd = open(DEVICE, O_RDWR);
	return fd < 0 ? errno : 0;
}

static void radio_close(void)
{
	if (fd >= 0)
		close(fd);
	fd = -1;
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
#else
/* one request to pro1x-fmd: 0 and what follows its "ok", or an errno */
static int fmd(const char *req, char *ans, size_t size)
{
	static const struct { const char *name; int err; } errs[] = {
		{ "ENODATA", ENODATA }, { "ETIMEDOUT", ETIMEDOUT },
		{ "ENETDOWN", ENETDOWN }, { "ENODEV", ENODEV },
		{ "ENOENT", ENOENT }, { "EBUSY", EBUSY }, { "EINVAL", EINVAL },
	};
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	char buf[512];
	size_t len = 0, i;
	ssize_t n;
	char *nl;

	if (fd < 0)
		return ENOENT;
	n = snprintf(buf, sizeof(buf), "%s\n", req);
	if (write(fd, buf, (size_t)n) != n)
		return EIO;
	/* a seek goes round the band in some 7 s, the daemon gives it 30 */
	while ((nl = memchr(buf, '\n', len)) == NULL) {
		if (len == sizeof(buf) - 1 || poll(&pfd, 1, 40000) <= 0)
			return ETIMEDOUT;
		n = read(fd, buf + len, sizeof(buf) - 1 - len);
		if (n <= 0)
			return EIO;
		len += (size_t)n;
	}
	*nl = '\0';
	if (strncmp(buf, "ok", 2) == 0) {
		if (ans)
			g_strlcpy(ans, buf[2] == ' ' ? buf + 3 : "", size);
		return 0;
	}
	for (i = 0; i < G_N_ELEMENTS(errs); i++)
		if (strncmp(buf + 6, errs[i].name, strlen(errs[i].name)) == 0)
			return errs[i].err;
	return EIO;
}

static int radio_open(void)
{
	struct sockaddr_un sun = { .sun_family = AF_UNIX };

	g_strlcpy(sun.sun_path, FMD_SOCKET, sizeof(sun.sun_path));
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return errno;
	if (connect(fd, (struct sockaddr *)&sun, sizeof(sun))) {
		close(fd);
		fd = -1;
		return ENOENT;
	}
	return fmd("on", NULL, 0);
}

static void radio_close(void)
{
	if (fd < 0)
		return;
	fmd("off", NULL, 0);
	close(fd);
	fd = -1;
}

static int set_mute(int on)
{
	return fmd(on ? "mute 1" : "mute 0", NULL, 0);
}

static int set_freq(unsigned khz)
{
	char req[32];

	g_snprintf(req, sizeof(req), "tune %u", khz);
	return fmd(req, NULL, 0);
}

static int seek(int up)
{
	return fmd(up ? "seek up" : "seek down", NULL, 0);
}

/* the sound path (pro1x-fmaudio behind pro1x-fmd) on or off */
static int audio(int on, char *ans, size_t size)
{
	return fmd(on ? "audio on" : "audio off", ans, size);
}

/* "on=1 freq=99900 rssi=-85 signal=87 stereo=1 rds=1 ..." */
static unsigned field(const char *s, const char *key)
{
	const char *p = strstr(s, key);

	return p ? strtoul(p + strlen(key), NULL, 10) : 0;
}

/*
 * pro1x-audiod's headset jack ("jack": "ok plug=headset watched=1 ..."):
 * 1 a plug in, 0 none, -1 when nobody watches the jack
 */
static int jack_plugged(void)
{
	struct sockaddr_un sun = { .sun_family = AF_UNIX };
	char ans[256];
	ssize_t n;
	int s, r = -1;

	g_strlcpy(sun.sun_path, "/var/run/pro1x-audio.sock", sizeof(sun.sun_path));
	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	if (connect(s, (struct sockaddr *)&sun, sizeof(sun)) == 0 &&
	    write(s, "jack\n", 5) == 5 && (n = read(s, ans, sizeof(ans) - 1)) > 0) {
		ans[n] = '\0';
		if (strstr(ans, " watched=1"))
			r = strstr(ans, "plug=none") ? 0 : 1;
	}
	close(s);
	return r;
}

static void status(struct result *r)
{
	char ans[256];
	int err;

	r->jack = jack_plugged();
	if ((err = fmd("status", ans, sizeof(ans)))) {
		if (!r->err)
			r->err = err;
		return;
	}
	r->freq = field(ans, " freq=");
	r->signal = field(ans, " signal=");
	r->stereo = field(ans, " stereo=") != 0;
	r->rds = field(ans, " rds=") != 0;
	if (fmd("ps", r->ps, sizeof(r->ps)))
		r->ps[0] = '\0';
	if (fmd("rt", r->rt, sizeof(r->rt)))
		r->rt[0] = '\0';
}
#endif

static gboolean result_cb(gpointer data);

static gpointer radio_thread(gpointer data)
{
	(void)data;
	for (;;) {
		guint v = GPOINTER_TO_UINT(g_async_queue_pop(requests));
		struct result *r = g_new0(struct result, 1);

		r->jack = -1;

		r->req = (v & 0xf) - 1;
		switch (r->req) {
		case OPEN:
			r->err = radio_open();
			if (!r->err)
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
		case AUDIO_ON:
		case AUDIO_OFF:
#ifndef __linux__
			r->err = audio(r->req == AUDIO_ON, r->note, sizeof(r->note));
#endif
			break;
		case QUIT:
			radio_close();
			g_free(r);
			return NULL;
		}
		if (fd >= 0)
			status(r);
		g_idle_add(result_cb, r);
	}
}

/* ---- sound: the SLIMbus capture played back ---- */

#ifdef __linux__
/*
 * The FM receiver's stereo reaches the DSP on SLIMBUS_8_TX (the kernel's
 * "FM Capture" link, patches 0018-0021). Routed to the MultiMedia3 front
 * end it is an ALSA capture device, which PulseAudio's UCM profile leaves
 * alone; here it becomes the source "fm_radio", and a loopback plays it on
 * the default sink.
 */
#define FM_ROUTE "MultiMedia3 Mixer SLIMBUS_8_TX"
#define FM_FRONT_END "MultiMedia3"

static int fm_source_module = -1, fm_loopback_module = -1;

/* runs a command; its first line of output as a number, or -1 */
static int run(const char *cmd)
{
	char *out = NULL;
	int n = -1, status;

	if (g_spawn_command_line_sync(cmd, &out, NULL, &status, NULL) &&
	    g_spawn_check_wait_status(status, NULL))
		n = out && g_ascii_isdigit(*out) ? atoi(out) : 0;
	g_free(out);
	return n;
}

static void unload(int *module)
{
	char *cmd;

	if (*module < 0)
		return;
	cmd = g_strdup_printf("pactl unload-module %d", *module);
	run(cmd);
	g_free(cmd);
	*module = -1;
}

static void route(gboolean on)
{
	char *cmd = g_strdup_printf("amixer -q -c Pro1X cset name='" FM_ROUTE "' %d", on);
	int ok = run(cmd) >= 0;

	g_free(cmd);
	if (!ok && on)
		gtk_label_set_text(GTK_LABEL(status_label),
				   _("No FM sound yet: the receiver's SLIMbus path is missing."));
}

/* the PCM device of the front end, from /proc/asound/pcm ("00-02: MultiMedia3 ...") */
static int front_end_pcm(void)
{
	char *text = NULL, **lines;
	int i, dev = -1;

	if (!g_file_get_contents("/proc/asound/pcm", &text, NULL, NULL))
		return -1;
	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] && dev < 0; i++)
		if (strstr(lines[i], ": " FM_FRONT_END " ") && strlen(lines[i]) > 5)
			dev = atoi(lines[i] + 3);
	g_strfreev(lines);
	g_free(text);
	return dev;
}

static void sound(gboolean on)
{
	char *cmd;
	int dev;

	if (!on) {
		unload(&fm_loopback_module);
		unload(&fm_source_module);
		route(FALSE);
		return;
	}
	if (fm_loopback_module >= 0)
		return;

	route(TRUE);
	dev = front_end_pcm();
	if (dev < 0)
		return;
	cmd = g_strdup_printf("pactl load-module module-alsa-source device=hw:Pro1X,%d "
			      "source_name=fm_radio rate=48000 channels=2 format=s16le "
			      "tsched=0", dev);
	fm_source_module = run(cmd);
	g_free(cmd);
	if (fm_source_module < 0) {
		gtk_label_set_text(GTK_LABEL(status_label),
				   _("The FM sound does not start (see the kernel log)."));
		route(FALSE);
		return;
	}
	fm_loopback_module = run("pactl load-module module-loopback source=fm_radio "
				 "latency_msec=100");
}
#else
/* pro1x-fmd's "audio on|off", in the radio thread; result_cb says how it went */
static void sound(gboolean on)
{
	push(on ? AUDIO_ON : AUDIO_OFF, 0);
}
#endif

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
#ifdef __linux__
	case ENOENT:
	case ENODEV:
		return _("No FM receiver (kernel without radio-qca-fm?)");
	case ENETDOWN:
	case ERFKILL:
		return _("Bluetooth is blocked or failed: the receiver needs it powered.");
#else
	case ENOENT:
		return _("The FM daemon pro1x-fmd is not running.");
	case ENODEV:
	case ENETDOWN:
		return _("The Bluetooth chip did not come up (see pro1x-fmd's log).");
#endif
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
	if (r->req != POLL && r->req != AUDIO_ON && r->req != AUDIO_OFF)
		busy = FALSE;
	if (r->req == AUDIO_ON)
		gtk_label_set_text(GTK_LABEL(status_label),
				   r->err ? _("No FM sound: its path did not come up (see pro1x-fmd's log).")
				   : strstr(r->note, "no codec") || strstr(r->note, "no output")
				   ? _("The FM sound reaches the DSP; FreeBSD has no headphone output for it yet.")
				   : "");
	else if (r->err)
		gtk_label_set_text(GTK_LABEL(status_label), strerr(r->err));
	else if (r->req == POLL && r->jack == 0) {
		gtk_label_set_text(GTK_LABEL(status_label),
				   _("Plug in headphones: their cable is the antenna."));
		jack_hint = TRUE;
	} else if (r->req == POLL && r->jack == 1 && jack_hint) {
		gtk_label_set_text(GTK_LABEL(status_label), "");
		jack_hint = FALSE;
	}
	else if (r->req != POLL && r->req != AUDIO_OFF)
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
