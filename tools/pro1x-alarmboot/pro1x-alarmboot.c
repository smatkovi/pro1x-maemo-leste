/*
 * pro1x-alarmboot -- after the phone switched itself on for an alarm.
 *
 * The boot menu starts the system that set an RTC alarm when that alarm
 * powered the phone on (tools/pro1x-boot alarm-check, docs/alarm-power-on.md)
 * and leaves a note of it: /host/pro1x-boot/alarmboot on Linux (the
 * initramfs writes it at every such start and removes it at every other),
 * /var/run/pro1x-alarmboot on FreeBSD (rc.d/pro1x_alarmboot, from the
 * kernel environment). Started with the Hildon session, this program does
 * what Maemo's acting-dead mode did around such an alarm, in the running
 * system:
 *
 *   - the alarm itself rings through alarmd and the system UI as always;
 *     this only watches the alarm dialog (com.nokia.system_ui.signal
 *     alarm_dialog_status on the system bus);
 *   - should no alarm dialog have come 2 minutes after the alarm's time
 *     (alarmd or the system UI missing, the queue lost with FreeBSD's
 *     memory /var), it rings itself: a dialog with a tone through
 *     PulseAudio, until Stop or for 5 minutes; not when alarmd answers
 *     that it has no such alarm any more (one deleted after the RTC was
 *     set: Leste's alarmd leaves the RTC alarm then);
 *   - once the alarm dialog is gone (stopped or snoozed: a snoozed alarm
 *     sets the RTC again and switches the phone on again), it asks
 *     whether to switch the phone off again, as the N900 asked whether to
 *     switch it on; no answer in 60 s keeps it on.
 *
 * Without a note for this system it ends at once. Once per alarm (a note
 * in the user's runtime directory).
 *
 *   pro1x-alarmboot [-f NOTE] [-s SYSTEM] [-v]
 *       -f  the note (default as above), -s  this system's key
 *       (default: /host/pro1x-boot/current; FreeBSD: freebsd),
 *       -v  say what it does on stderr
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <hildon/hildon.h>
#include <libalarm.h>
#include <math.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#define NOTE_PATH "/host/pro1x-boot/alarmboot"
#define CURRENT_PATH "/host/pro1x-boot/current"
#else
#define NOTE_PATH "/var/run/pro1x-alarmboot"
#define CURRENT_PATH NULL
#endif

/* alarm_dialog_status (systemui/alarm_dialog-dbus-names.h) */
#define DIALOG_ON_SCREEN 5
#define DIALOG_NOT_ON_SCREEN 7

#define RING_AFTER_DUE 120	/* s after the alarm's time without its dialog */
#define RING_AFTER_START 90	/* and at least this long after our start */
#define RING_FOR 300		/* the own ringing at most this long */
#define ASK_FOR 60		/* the question at most this long */
#define LIFETIME (30 * 60)	/* then give up */

static gboolean verbose;
static GDBusConnection *sysbus;
static time_t due;		/* the alarm's real time */
static gboolean dialog_seen, asked, ringing;
static guint ask_timer, ring_timer;
static volatile gint ring_stop;
static GThread *tone;
static GtkWidget *ring_dialog;

#define say(...) do { if (verbose) { fprintf(stderr, "pro1x-alarmboot: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* key=value lines; value of KEY into buf, FALSE without it */
static gboolean note_get(const char *text, const char *key, char *buf, size_t n)
{
	size_t kl = strlen(key);
	const char *p = text;

	while (p && *p) {
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			const char *v = p + kl + 1, *e = strchr(v, '\n');
			size_t l = e ? (size_t)(e - v) : strlen(v);
			if (l >= n)
				l = n - 1;
			memcpy(buf, v, l);
			buf[l] = 0;
			return TRUE;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return FALSE;
}

static void mce_call(const char *method, GVariant *args)
{
	g_dbus_connection_call(sysbus, "com.nokia.mce", "/com/nokia/mce/request",
			       "com.nokia.mce.request", method, args, NULL,
			       G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
}

static void power_off(void)
{
	GError *err = NULL;

	say("switching off");
#ifdef __linux__
	/* as the power key menu does: dsme shuts down (the RTC alarm of a
	 * snoozed or the next alarm stays set) */
	GVariant *r = g_dbus_connection_call_sync(sysbus, "com.nokia.dsme",
						  "/com/nokia/dsme/request",
						  "com.nokia.dsme.request",
						  "req_shutdown", NULL, NULL,
						  G_DBUS_CALL_FLAGS_NONE, 5000,
						  NULL, &err);
	if (r)
		g_variant_unref(r);
#else
	g_spawn_command_line_async("/sbin/shutdown -p now", &err);
#endif
	if (err) {
		fprintf(stderr, "pro1x-alarmboot: switching off: %s\n", err->message);
		g_error_free(err);
	}
}

static guint close_timer;

static gboolean close_dialog(gpointer dialog)
{
	close_timer = 0;
	gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
	return FALSE;
}

/* "Switch the phone off again?"; no answer keeps it on */
static gboolean ask(gpointer unused)
{
	GtkWidget *note;
	gint r;

	(void)unused;
	ask_timer = 0;
	if (asked || ringing)
		return FALSE;
	asked = TRUE;
	say("asking whether to switch off");
	note = hildon_note_new_confirmation_add_buttons(NULL,
		"The phone switched itself on for the alarm.\n"
		"Switch it off again?",
		"Switch off", GTK_RESPONSE_OK, "Keep on", GTK_RESPONSE_CANCEL, NULL);
	close_timer = g_timeout_add_seconds(ASK_FOR, close_dialog, note);
	r = gtk_dialog_run(GTK_DIALOG(note));
	if (close_timer)
		g_source_remove(close_timer);
	close_timer = 0;
	gtk_widget_destroy(note);
	if (r == GTK_RESPONSE_OK)
		power_off();
	else
		say("staying on");
	gtk_main_quit();
	return FALSE;
}

/* A tone of its own, through PulseAudio: four beeps, a pause, again */
static gpointer tone_thread(gpointer unused)
{
	static const pa_sample_spec ss = { PA_SAMPLE_S16LE, 44100, 1 };
	enum { RATE = 44100, BEEP = RATE * 15 / 100, GAP = RATE / 10, PAUSE = RATE * 8 / 10 };
	static int16_t pattern[4 * (BEEP + GAP) + PAUSE];
	pa_simple *s;
	int err, i, k;

	(void)unused;
	for (k = 0; k < 4; k++)
		for (i = 0; i < BEEP; i++)
			pattern[k * (BEEP + GAP) + i] =
				(int16_t)(24000 * sin(2 * M_PI * 880 * i / RATE));
	s = pa_simple_new(NULL, "pro1x-alarmboot", PA_STREAM_PLAYBACK, NULL,
			  "alarm", &ss, NULL, NULL, &err);
	if (!s) {
		fprintf(stderr, "pro1x-alarmboot: no sound: %s\n", pa_strerror(err));
		return NULL;
	}
	while (!g_atomic_int_get(&ring_stop))
		if (pa_simple_write(s, pattern, sizeof(pattern), &err) < 0)
			break;
	pa_simple_flush(s, NULL);
	pa_simple_free(s);
	return NULL;
}

static void ring_end(void)
{
	if (!ringing)
		return;
	ringing = FALSE;
	g_atomic_int_set(&ring_stop, 1);
	if (tone) {
		g_thread_join(tone);
		tone = NULL;
	}
	if (ring_timer)
		g_source_remove(ring_timer);
	ring_timer = 0;
	if (ring_dialog)
		gtk_widget_destroy(ring_dialog);
	ring_dialog = NULL;
	ask(NULL);
}

static gboolean ring_timeout(gpointer unused)
{
	(void)unused;
	ring_timer = 0;
	say("rang for %d s", RING_FOR);
	ring_end();
	return FALSE;
}

static void ring_response(GtkDialog *d, gint r, gpointer unused)
{
	(void)d; (void)r; (void)unused;
	ring_end();
}

/* alarmd's answer for the alarm: 1 there is one, 0 it has none, -1 no answer */
static int alarmd_has_alarm(time_t now)
{
	cookie_t *c = alarmd_event_query(due - 600, now + 900, 0, 0, NULL);
	int n = 0;

	if (!c)
		return -1;
	for (int i = 0; c[i]; i++) {
		alarm_event_t *e = alarmd_event_get(c[i]);
		if (e && (e->flags & (ALARM_EVENT_ACTDEAD | ALARM_EVENT_BOOT)))
			n++;
		if (e)
			alarm_event_delete(e);
	}
	free(c);
	return n > 0;
}

static gboolean ring_check(gpointer unused)
{
	time_t now = time(NULL);
	int has;
	char when[16];
	struct tm tm;

	(void)unused;
	if (dialog_seen || asked)
		return FALSE;
	has = alarmd_has_alarm(now);
	say("no alarm dialog %ld s after the alarm; alarmd: %s", (long)(now - due),
	    has < 0 ? "no answer" : has ? "has it" : "has none");
	if (has == 0) {
		/* deleted after the RTC was set: nothing to ring for */
		ask(NULL);
		return FALSE;
	}
	ringing = TRUE;
	g_atomic_int_set(&ring_stop, 0);
	localtime_r(&due, &tm);
	strftime(when, sizeof(when), "%H:%M", &tm);
	mce_call("req_tklock_mode_change", g_variant_new("(s)", "unlocked"));
	mce_call("req_display_state_on", NULL);
	ring_dialog = gtk_dialog_new_with_buttons("Alarm", NULL, GTK_DIALOG_MODAL,
						  "Stop", GTK_RESPONSE_OK, NULL);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(ring_dialog))),
			   gtk_label_new(when), TRUE, TRUE, 12);
	g_signal_connect(ring_dialog, "response", G_CALLBACK(ring_response), NULL);
	gtk_widget_show_all(ring_dialog);
	tone = g_thread_new("tone", tone_thread, NULL);
	ring_timer = g_timeout_add_seconds(RING_FOR, ring_timeout, NULL);
	return FALSE;
}

static void dialog_status(GDBusConnection *c, const gchar *sender, const gchar *path,
			  const gchar *iface, const gchar *signal, GVariant *params,
			  gpointer unused)
{
	guint32 st = 0;

	(void)c; (void)sender; (void)path; (void)iface; (void)signal; (void)unused;
	if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(u)")))
		return;
	g_variant_get(params, "(u)", &st);
	say("alarm dialog status %u", st);
	if (st == DIALOG_ON_SCREEN) {
		dialog_seen = TRUE;
		/* our own ringing gives way to the real alarm */
		if (ringing) {
			asked = TRUE;	/* the real one's end asks */
			ring_end();
			asked = FALSE;
		}
		if (ask_timer)
			g_source_remove(ask_timer);
		ask_timer = 0;
	} else if (st == DIALOG_NOT_ON_SCREEN && dialog_seen && !asked && !ask_timer) {
		/* stopped or snoozed: ask once it stays gone a moment */
		ask_timer = g_timeout_add_seconds(5, ask, NULL);
	}
}

static gboolean give_up(gpointer unused)
{
	(void)unused;
	if (ringing || asked)
		return FALSE;
	say("no alarm dialog came to an end in %d min: done", LIFETIME / 60);
	gtk_main_quit();
	return FALSE;
}

int main(int argc, char **argv)
{
	const char *note_path = NOTE_PATH, *self = NULL;
	gchar *note = NULL, *done_path, *done = NULL;
	char sys[64], buf[32], rtc[32] = "";
	GError *err = NULL;
	time_t now = time(NULL);
	long late;
	int opt;

	while ((opt = getopt(argc, argv, "f:s:v")) != -1) {
		switch (opt) {
		case 'f': note_path = optarg; break;
		case 's': self = optarg; break;
		case 'v': verbose = TRUE; break;
		default:
			fprintf(stderr, "usage: pro1x-alarmboot [-f note] [-s system] [-v]\n");
			return 2;
		}
	}
	if (!g_file_get_contents(note_path, &note, NULL, NULL) ||
	    !note_get(note, "system", sys, sizeof(sys))) {
		say("not started by an alarm (%s)", note_path);
		return 0;
	}
	if (!self) {
#ifdef __linux__
		gchar *cur = NULL;

		if (g_file_get_contents(CURRENT_PATH, &cur, NULL, NULL))
			g_strstrip(cur);
		self = cur ? cur : "leste";
#else
		self = "freebsd";
#endif
	}
	/* the note names the system the boot menu meant to start (a kexec
	 * that failed falls back to Leste) */
	if (strcmp(sys, self)) {
		say("the alarm was %s's, this is %s", sys, self);
		return 0;
	}
	note_get(note, "rtc_alarm", rtc, sizeof(rtc));
	done_path = g_build_filename(g_get_user_runtime_dir(), "pro1x-alarmboot.done", NULL);
	if (g_file_get_contents(done_path, &done, NULL, NULL) && !strcmp(g_strstrip(done), rtc)) {
		say("this alarm was handled already");
		return 0;
	}
	g_file_set_contents(done_path, rtc, -1, NULL);

	due = note_get(note, "due", buf, sizeof(buf)) ? (time_t)strtoll(buf, NULL, 10) : 0;
	if (due <= 0)
		due = now;

	gtk_init(&argc, &argv);
	sysbus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!sysbus) {
		fprintf(stderr, "pro1x-alarmboot: system bus: %s\n", err->message);
		return 1;
	}
	g_dbus_connection_signal_subscribe(sysbus, NULL, "com.nokia.system_ui.signal",
					   "alarm_dialog_status", "/com/nokia/system_ui/signal",
					   NULL, G_DBUS_SIGNAL_FLAGS_NONE, dialog_status, NULL, NULL);
	late = (long)(now - due);
	say("started by %s's alarm at %ld (%+ld s)", sys, (long)due, late);
	{
		long wait = RING_AFTER_DUE - late;
		if (wait < RING_AFTER_START)
			wait = RING_AFTER_START;
		g_timeout_add_seconds((guint)wait, ring_check, NULL);
	}
	g_timeout_add_seconds(LIFETIME, give_up, NULL);
	gtk_main();
	return 0;
}
