/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-modemd [-v] [-d qrtr://NODE | -d QMUX-FILE] [-p PIN-FILE] [-P MARK]
 *              [-r TRIES] [-A CALLAUDIO] [-n] [-S] [-s SECONDS] [-m PATH]
 *              [-c DATA-CONF] [-I HOOK] [-W qmapv5|qmapv4|qmap] [-E RX,TX]
 *              [-D] [-X]
 *
 * The Pro1X modem's calls, SMS and network on D-Bus as oFono's API
 * (freebsd/modem/README.md):
 *
 *   -d  the modem: a QRTR node (default qrtr://0, the modem's), or a file
 *       speaking QMUX (the tests' simulated modem on a pty)
 *   -p  the SIM PIN file (default /dev/pro1x-host/freebsd/sim-pin: the
 *       user's, on userdata; never written, the PIN never logged); "" none
 *   -P  the mark of a PIN attempt in this boot (default
 *       /var/run/pro1x-modemd.pin-tried): the file is tried once per boot
 *   -r  no attempt from the file with fewer tries left (default 3)
 *   -A  a program run while there are calls (pro1x-callaudio: the voice
 *       path on the ADSP), ended with SIGTERM after the last one
 *   -n  leave the modem's operating mode alone (default: online)
 *   -S  the session bus instead of the system bus (tests)
 *   -s  seconds between signal readings (default 30; 0: none)
 *   -m  the modem's object path (default /qrtr_0)
 *
 * Mobile data (data.c, freebsd/data/README.md):
 *   -c  the user's APN file (default /dev/pro1x-host/freebsd/data.conf;
 *       apn=, username=, password=, auth=, protocol=, roaming=,
 *       autoconnect=); "" none
 *   -I  what puts a session's addresses on its interface (default
 *       /usr/local/libexec/pro1x-data-ifup); "" nothing
 *   -W  only this QMAP format with WDA (default: QMAPv5, QMAPv4, QMAP)
 *   -E  the IPA's endpoint ids facing the modem (default: qcom_ipa's
 *       dev.qcom_ipa.0.modem.{rx,tx}_endpoint_id)
 *   -D  no mobile data;  -X  no QMAP interface needed (the tests; also
 *       mobile data over a QMUX file, which is off without it)
 */

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib-unix.h>
#include <libqrtr-glib.h>

#include "modemd.h"

struct modem M;
struct options opt = {
	.device = "qrtr://0",
	.pin_file = "/dev/pro1x-host/freebsd/sim-pin",
	.pin_tried = "/var/run/pro1x-modemd.pin-tried",
	.pin_min_retries = 3,
	.online = TRUE,
	.signal_poll = 30,
};
int verbose;

static GMainLoop *loop;
static int exit_code;
static QrtrBus *qbus;
static QmiDevice *device;
static GPid audio_pid;
static gboolean audio_failed;

static void
vlog(const char *fmt, va_list ap)
{
	char ts[16];
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
	printf("%s " MODEMD_NAME ": ", ts);
	vprintf(fmt, ap);
	putchar('\n');
	fflush(stdout);
}

void
mlog(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(fmt, ap);
	va_end(ap);
}

void
mdbg(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	va_start(ap, fmt);
	vlog(fmt, ap);
	va_end(ap);
}

void
modemd_quit(int code)
{
	exit_code = code;
	g_main_loop_quit(loop);
}

/* --- the call audio child ------------------------------------------------ */

static void
audio_exited(GPid pid, gint status, gpointer data)
{
	(void)data;
	g_spawn_close_pid(pid);
	if (pid != audio_pid)
		return;
	audio_pid = 0;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		mlog("callaudio: ended");
	else {
		mlog("callaudio: ended with status %d%s", WIFEXITED(status) ?
		    WEXITSTATUS(status) : -WTERMSIG(status),
		    calls_live() ? " during a call (not again for these calls)" : "");
		if (calls_live())
			audio_failed = TRUE;
	}
}

/*
 * While a call exists (dialling, ringing either way, active, held: as
 * Linux's pro1x-voice-call does), the call audio program runs; after the
 * last one it gets SIGTERM and takes the voice path down.
 */
void
calls_changed(void)
{
	gboolean live = calls_live();

	if (!live)
		audio_failed = FALSE;
	if (opt.callaudio == NULL)
		return;
	if (live && audio_pid == 0 && !audio_failed) {
		char *argv[] = { (char *)opt.callaudio, NULL };
		GError *error = NULL;

		if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
		    &audio_pid, &error)) {
			mlog("callaudio: %s: %s", opt.callaudio, error->message);
			g_error_free(error);
			audio_failed = TRUE;
			return;
		}
		mlog("callaudio: %s started (pid %d)", opt.callaudio, (int)audio_pid);
		g_child_watch_add(audio_pid, audio_exited, NULL);
	} else if (!live && audio_pid != 0) {
		mlog("callaudio: the last call is over, stopping it");
		kill(audio_pid, SIGTERM);
	}
}

/* --- the modem --------------------------------------------------------------- */

static void
open_ready(QmiDevice *dev, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;

	(void)data;
	if (!qmi_device_open_finish(dev, res, &error)) {
		mlog("qmi: open %s: %s", opt.device, error->message);
		g_error_free(error);
		modemd_quit(1);
		return;
	}
	mlog("qmi: %s open", qmi_device_get_path_display(dev));
	qmi_start(dev);
	data_start(dev);
}

static void
have_device(QmiDevice *dev)
{
	device = dev;
	qmi_device_open(device, QMI_DEVICE_OPEN_FLAGS_NONE, 15, NULL,
	    (GAsyncReadyCallback)open_ready, NULL);
}

static void
node_device_ready(GObject *src, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	QmiDevice *dev = qmi_device_new_from_node_finish(res, &error);

	(void)src; (void)data;
	if (dev == NULL) {
		mlog("qmi: device on the node: %s", error->message);
		g_error_free(error);
		modemd_quit(1);
		return;
	}
	have_device(dev);
}

static void
node_ready(QrtrBus *b, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	QrtrNode *node = qrtr_bus_wait_for_node_finish(b, res, &error);
	GList *svcs;

	(void)data;
	if (node == NULL) {
		mlog("qrtr: %s: %s", opt.device, error->message);
		g_error_free(error);
		modemd_quit(1);
		return;
	}
	svcs = qrtr_node_peek_service_info_list(node);
	mlog("qrtr: node %u with %u services", qrtr_node_get_id(node), g_list_length(svcs));
	for (GList *l = svcs; verbose && l != NULL; l = l->next)
		mdbg("qrtr: service %u version %u instance %u port %u",
		    qrtr_node_service_info_get_service(l->data),
		    qrtr_node_service_info_get_version(l->data),
		    qrtr_node_service_info_get_instance(l->data),
		    qrtr_node_service_info_get_port(l->data));
	qmi_device_new_from_node(node, NULL, node_device_ready, NULL);
	g_object_unref(node);
}

static void
bus_ready(GObject *src, GAsyncResult *res, gpointer node)
{
	GError *error = NULL;

	(void)src;
	qbus = qrtr_bus_new_finish(res, &error);
	if (qbus == NULL) {
		mlog("qrtr: %s", error->message);
		g_error_free(error);
		modemd_quit(1);
		return;
	}
	mlog("qrtr: waiting for node %u", GPOINTER_TO_UINT(node));
	qrtr_bus_wait_for_node(qbus, GPOINTER_TO_UINT(node), 60000, NULL,
	    (GAsyncReadyCallback)node_ready, NULL);
}

static void
file_device_ready(GObject *src, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	GObject *dev = g_async_initable_new_finish(G_ASYNC_INITABLE(src), res, &error);

	(void)data;
	if (dev == NULL) {
		mlog("qmi: %s: %s", opt.device, error->message);
		g_error_free(error);
		modemd_quit(1);
		return;
	}
	have_device(QMI_DEVICE(dev));
}

static void
open_modem(void)
{
	if (g_str_has_prefix(opt.device, "qrtr://")) {
		char *end;
		unsigned long node = strtoul(opt.device + 7, &end, 10);

		if (*end || end == opt.device + 7) {
			mlog("not a QRTR node: %s", opt.device);
			modemd_quit(1);
			return;
		}
		qrtr_bus_new(1000, NULL, bus_ready, GUINT_TO_POINTER((guint)node));
	} else {
		GFile *f = g_file_new_for_path(opt.device);

		/* a QMUX stream that is no USB device (the tests' pty) */
		g_async_initable_new_async(QMI_TYPE_DEVICE, G_PRIORITY_DEFAULT, NULL,
		    file_device_ready, NULL, QMI_DEVICE_FILE, f,
		    QMI_DEVICE_NO_FILE_CHECK, TRUE, NULL);
		g_object_unref(f);
	}
}

/* --- the end ------------------------------------------------------------------- */

static void
stopped(void)
{
	g_main_loop_quit(loop);
}

static gboolean
stop_timeout(gpointer data)
{
	(void)data;
	mlog("the modem did not release its clients in time");
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static gboolean
on_signal(gpointer data)
{
	static gboolean once;

	(void)data;
	if (once)
		return G_SOURCE_CONTINUE;
	once = TRUE;
	mlog("stopping");
	if (audio_pid != 0)
		kill(audio_pid, SIGTERM);
	data_stop();
	if (device != NULL) {
		qmi_stop(stopped);
		g_timeout_add_seconds(3, stop_timeout, NULL);
	} else {
		g_main_loop_quit(loop);
	}
	return G_SOURCE_CONTINUE;
}

static void
usage(void)
{
	fprintf(stderr, "usage: " MODEMD_NAME " [-v] [-d qrtr://NODE|QMUX-FILE] [-p PIN-FILE] "
	    "[-P MARK] [-r TRIES] [-A CALLAUDIO] [-n] [-S] [-s SECONDS] [-m PATH]\n"
	    "    [-c DATA-CONF] [-I HOOK] [-W qmapv5|qmapv4|qmap] [-E RX,TX] [-D] [-X]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	GDBusConnection *bus;
	GError *error = NULL;
	const char *path = "/qrtr_0";
	int c;

	while ((c = getopt(argc, argv, "vd:p:P:r:A:nSs:m:c:I:W:E:DX")) != -1) {
		switch (c) {
		case 'v': verbose++; break;
		case 'd': opt.device = optarg; break;
		case 'p': opt.pin_file = optarg[0] ? optarg : NULL; break;
		case 'P': opt.pin_tried = optarg; break;
		case 'r': opt.pin_min_retries = atoi(optarg); break;
		case 'A': opt.callaudio = optarg[0] ? optarg : NULL; break;
		case 'n': opt.online = FALSE; break;
		case 'S': opt.session_bus = TRUE; break;
		case 's': opt.signal_poll = atoi(optarg); break;
		case 'm': path = optarg; break;
		case 'c': dopt.conf = optarg[0] ? optarg : NULL; break;
		case 'I': dopt.hook = optarg; break;
		case 'W': dopt.wda = optarg; break;
		case 'E':
			if (sscanf(optarg, "%u,%u", &dopt.rx_ep, &dopt.tx_ep) != 2)
				usage();
			break;
		case 'D': dopt.disabled = TRUE; break;
		case 'X': dopt.no_ifcheck = TRUE; break;
		default: usage();
		}
	}
	if (optind != argc || path[0] != '/' || !g_variant_is_object_path(path))
		usage();
	/* mobile data needs the IPA, behind the modem's QRTR node; over a
	 * QMUX file (the tests' simulated modem) only with -X */
	if (!g_str_has_prefix(opt.device, "qrtr://") && !dopt.no_ifcheck)
		dopt.disabled = TRUE;
	M.path = g_strdup(path);
	M.pin_retries = M.puk_retries = -1;
	M.strength = -1;
	loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGTERM, on_signal, NULL);
	g_unix_signal_add(SIGINT, on_signal, NULL);
	/* the child's end is reaped through GLib; a broken stdout pipe must not end us */
	signal(SIGPIPE, SIG_IGN);

	bus = g_bus_get_sync(opt.session_bus ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM, NULL,
	    &error);
	if (bus == NULL) {
		mlog("D-Bus: %s", error->message);
		return 1;
	}
	if (ofono_init(bus) != 0 || data_init(bus) != 0)
		return 1;
	mlog("modem %s, PIN file %s%s", opt.device, opt.pin_file ? opt.pin_file : "none",
	    opt.callaudio ? ", call audio" : "");
	open_modem();
	g_main_loop_run(loop);
	if (audio_pid != 0)
		kill(audio_pid, SIGTERM);
	g_clear_object(&device);
	g_clear_object(&qbus);
	g_object_unref(bus);
	return exit_code;
}
