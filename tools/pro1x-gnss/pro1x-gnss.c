/*
 * pro1x-gnss -- NMEA from the Pro1X modem's GNSS engine, for gpsd.
 *
 * The SM6115 modem exposes its GPS through the QMI LOC service, reached over
 * QRTR (node 0). With QRTR every process has its own connection, so enabling
 * the NMEA output, starting the session and receiving the sentences has to
 * happen in one process -- which is all this tool does: it opens a LOC
 * client, registers for NMEA and position events, starts a periodic session
 * and sends NMEA sentences as UDP datagrams to gpsd
 * (DEVICES="udp://127.0.0.1:PORT"). The Pro1X modem reports positions but no
 * NMEA, so GGA and RMC are built from the position report of each fix;
 * NMEA the modem does send is passed through unchanged.
 *
 *   pro1x-gnss [-n node] [-p port] [-i interval_ms] [-v] [-d qmux-file]
 *
 * -d takes the modem from a file that speaks QMUX instead of a QRTR node
 * (the tests' simulated modem on a pty, freebsd/modem/test).
 *
 * Build: gcc -O2 -Wall -o pro1x-gnss pro1x-gnss.c \
 *          $(pkg-config --cflags --libs qmi-glib qrtr-glib)
 * Needs libqmi >= 1.28 built with QRTR support.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <arpa/inet.h>
#include <glib-unix.h>
#include <libqmi-glib.h>
#include <libqrtr-glib.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SESSION_ID 1

static guint node_id;
static const char *qmux_file;
static guint udp_port = 29470;
static guint interval_ms = 1000;
static gboolean verbose;

static GMainLoop *loop;
static QrtrBus *bus;
static QmiDevice *device;
static QmiClientLoc *client;
static int udp_fd = -1;
static struct sockaddr_in udp_dst;
static int exit_code = EXIT_FAILURE;

static void die (const char *what, GError *error)
{
    g_printerr ("pro1x-gnss: %s: %s\n", what, error ? error->message : "failed");
    g_clear_error (&error);
    g_main_loop_quit (loop);
}

/* Monotonic time of the last NMEA sentence the modem sent itself. */
static gint64 last_modem_nmea;

static void send_line (const char *line)
{
    if (verbose)
        g_print ("%s", line);
    sendto (udp_fd, line, strlen (line), 0, (struct sockaddr *) &udp_dst, sizeof udp_dst);
}

static void nmea_cb (QmiClientLoc *self, QmiIndicationLocNmeaOutput *output, gpointer data)
{
    const gchar *nmea = NULL;
    gchar *line;
    gsize len;

    (void) self;
    (void) data;
    if (!qmi_indication_loc_nmea_output_get_nmea_string (output, &nmea, NULL) || !nmea)
        return;

    /* The modem may or may not end the sentence; gpsd wants CR LF. */
    len = strcspn (nmea, "\r\n");
    line = g_strdup_printf ("%.*s\r\n", (int) len, nmea);
    send_line (line);
    g_free (line);
    last_modem_nmea = g_get_monotonic_time ();
}

/* "$<body>*CS\r\n" with the NMEA XOR checksum over <body>. */
static void send_sentence (const char *body)
{
    guint8 cs = 0;
    gchar *line;

    for (const char *p = body; *p; p++)
        cs ^= (guint8) *p;
    line = g_strdup_printf ("$%s*%02X\r\n", body, cs);
    send_line (line);
    g_free (line);
}

/* ddmm.mmmm,N / dddmm.mmmm,E as NMEA wants them. */
static gchar *nmea_coord (gdouble deg, gboolean is_lat)
{
    gchar hemi = is_lat ? (deg < 0 ? 'S' : 'N') : (deg < 0 ? 'W' : 'E');
    gdouble a = deg < 0 ? -deg : deg;
    int d = (int) a;
    gdouble m = (a - d) * 60.0;

    return g_strdup_printf (is_lat ? "%02d%07.4f,%c" : "%03d%07.4f,%c", d, m, hemi);
}

/*
 * This modem reports positions but no NMEA, so the GGA and RMC sentences
 * gpsd needs are built from the LOC position report of a valid fix.
 */
static void synthesize_nmea (QmiIndicationLocPositionReportOutput *o, gdouble lat, gdouble lon)
{
    guint64 utc_ms = 0;
    gfloat alt = 0, hdop = 0, pdop = 0, vdop = 0, speed = 0, heading = 0;
    GArray *sats = NULL;
    time_t t;
    struct tm tm;
    gchar hms[16], dmy[8], body[160];
    gchar s_hdop[16] = "", s_alt[16] = "", s_speed[16] = "", s_heading[16] = "";
    gchar *la, *lo;
    gboolean have_alt, have_dop, have_speed, have_heading;

    if (!qmi_indication_loc_position_report_output_get_utc_timestamp (o, &utc_ms, NULL))
        return;
    have_alt = qmi_indication_loc_position_report_output_get_altitude_from_sealevel (o, &alt, NULL);
    have_dop = qmi_indication_loc_position_report_output_get_dop (o, &pdop, &hdop, &vdop, NULL);
    have_speed = qmi_indication_loc_position_report_output_get_horizontal_speed (o, &speed, NULL);
    have_heading = qmi_indication_loc_position_report_output_get_heading (o, &heading, NULL);
    qmi_indication_loc_position_report_output_get_satellites_used (o, &sats, NULL);

    t = (time_t) (utc_ms / 1000);
    gmtime_r (&t, &tm);
    g_snprintf (hms, sizeof hms, "%02d%02d%02d.%02u", tm.tm_hour, tm.tm_min, tm.tm_sec,
                (guint) ((utc_ms % 1000) / 10));
    g_snprintf (dmy, sizeof dmy, "%02d%02d%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100);
    la = nmea_coord (lat, TRUE);
    lo = nmea_coord (lon, FALSE);

    if (have_dop)
        g_snprintf (s_hdop, sizeof s_hdop, "%.1f", hdop);
    if (have_alt)
        g_snprintf (s_alt, sizeof s_alt, "%.1f", alt);
    if (have_speed)
        g_snprintf (s_speed, sizeof s_speed, "%.2f", speed * 1.943844);   /* m/s -> knots */
    if (have_heading)
        g_snprintf (s_heading, sizeof s_heading, "%.1f", heading);

    g_snprintf (body, sizeof body, "GPGGA,%s,%s,%s,1,%02u,%s,%s,M,,M,,", hms, la, lo,
                sats ? sats->len : 0u, s_hdop, s_alt);
    send_sentence (body);
    g_snprintf (body, sizeof body, "GPRMC,%s,A,%s,%s,%s,%s,%s,,,A", hms, la, lo,
                s_speed, s_heading, dmy);
    send_sentence (body);
    g_free (la);
    g_free (lo);
}

static void engine_state_cb (QmiClientLoc *self, QmiIndicationLocEngineStateOutput *output, gpointer data)
{
    QmiLocEngineState state;

    (void) self;
    (void) data;
    if (verbose && qmi_indication_loc_engine_state_output_get_engine_state (output, &state, NULL))
        g_print ("pro1x-gnss: engine %s\n", qmi_loc_engine_state_get_string (state));
}

static void position_cb (QmiClientLoc *self, QmiIndicationLocPositionReportOutput *output, gpointer data)
{
    QmiLocSessionStatus status;
    gdouble lat, lon;

    gboolean have_pos;

    (void) self;
    (void) data;
    if (!qmi_indication_loc_position_report_output_get_session_status (output, &status, NULL))
        return;
    have_pos = qmi_indication_loc_position_report_output_get_latitude (output, &lat, NULL) &&
               qmi_indication_loc_position_report_output_get_longitude (output, &lon, NULL);
    if (verbose) {
        if (have_pos)
            g_print ("pro1x-gnss: position %s %.6f %.6f\n", qmi_loc_session_status_get_string (status), lat, lon);
        else
            g_print ("pro1x-gnss: position %s\n", qmi_loc_session_status_get_string (status));
    }

    /* Only a real fix, and only if the modem is not sending NMEA itself. */
    if (status == QMI_LOC_SESSION_STATUS_SUCCESS && have_pos &&
        g_get_monotonic_time () - last_modem_nmea > 5 * G_USEC_PER_SEC)
        synthesize_nmea (output, lat, lon);
}

static void start_ready (QmiClientLoc *c, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;
    QmiMessageLocStartOutput *out;

    (void) data;
    out = qmi_client_loc_start_finish (c, res, &error);
    if (!out || !qmi_message_loc_start_output_get_result (out, &error)) {
        if (out)
            qmi_message_loc_start_output_unref (out);
        die ("start session", error);
        return;
    }
    qmi_message_loc_start_output_unref (out);
    g_print ("pro1x-gnss: session %d started, NMEA to udp://127.0.0.1:%u\n", SESSION_ID, udp_port);
    exit_code = EXIT_SUCCESS;
}

static void start_session (void)
{
    QmiMessageLocStartInput *in = qmi_message_loc_start_input_new ();

    qmi_message_loc_start_input_set_session_id (in, SESSION_ID, NULL);
    qmi_message_loc_start_input_set_intermediate_report_state (in, QMI_LOC_INTERMEDIATE_REPORT_STATE_ENABLE, NULL);
    qmi_message_loc_start_input_set_minimum_interval_between_position_reports (in, interval_ms, NULL);
    qmi_message_loc_start_input_set_fix_recurrence_type (in, QMI_LOC_FIX_RECURRENCE_TYPE_REQUEST_PERIODIC_FIXES, NULL);
    qmi_client_loc_start (client, in, 10, NULL, (GAsyncReadyCallback) start_ready, NULL);
    qmi_message_loc_start_input_unref (in);
}

static void nmea_types_ready (QmiClientLoc *c, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;
    QmiMessageLocSetNmeaTypesOutput *out;

    (void) data;
    /* Not fatal: if the modem keeps its own list, NMEA may still flow. */
    out = qmi_client_loc_set_nmea_types_finish (c, res, &error);
    if (!out || !qmi_message_loc_set_nmea_types_output_get_result (out, &error)) {
        g_printerr ("pro1x-gnss: set NMEA types: %s\n", error ? error->message : "failed");
        g_clear_error (&error);
    }
    if (out)
        qmi_message_loc_set_nmea_types_output_unref (out);
    start_session ();
}

static void register_ready (QmiClientLoc *c, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;
    QmiMessageLocRegisterEventsOutput *out;
    QmiMessageLocSetNmeaTypesInput *in;

    (void) data;
    out = qmi_client_loc_register_events_finish (c, res, &error);
    if (!out || !qmi_message_loc_register_events_output_get_result (out, &error)) {
        if (out)
            qmi_message_loc_register_events_output_unref (out);
        die ("register NMEA events", error);
        return;
    }
    qmi_message_loc_register_events_output_unref (out);

    in = qmi_message_loc_set_nmea_types_input_new ();
    qmi_message_loc_set_nmea_types_input_set_nmea_types (in,
        QMI_LOC_NMEA_TYPE_GGA | QMI_LOC_NMEA_TYPE_RMC | QMI_LOC_NMEA_TYPE_GSV |
        QMI_LOC_NMEA_TYPE_GSA | QMI_LOC_NMEA_TYPE_VTG, NULL);
    qmi_client_loc_set_nmea_types (client, in, 10, NULL, (GAsyncReadyCallback) nmea_types_ready, NULL);
    qmi_message_loc_set_nmea_types_input_unref (in);
}

static void client_ready (QmiDevice *dev, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;
    QmiClient *c;
    QmiMessageLocRegisterEventsInput *in;

    (void) data;
    c = qmi_device_allocate_client_finish (dev, res, &error);
    if (!c) {
        die ("allocate LOC client", error);
        return;
    }
    client = QMI_CLIENT_LOC (c);
    g_signal_connect (client, "nmea", G_CALLBACK (nmea_cb), NULL);
    g_signal_connect (client, "engine-state", G_CALLBACK (engine_state_cb), NULL);
    g_signal_connect (client, "position-report", G_CALLBACK (position_cb), NULL);

    in = qmi_message_loc_register_events_input_new ();
    qmi_message_loc_register_events_input_set_event_registration_mask (in,
        QMI_LOC_EVENT_REGISTRATION_FLAG_NMEA | QMI_LOC_EVENT_REGISTRATION_FLAG_POSITION_REPORT |
        QMI_LOC_EVENT_REGISTRATION_FLAG_GNSS_SATELLITE_INFO | QMI_LOC_EVENT_REGISTRATION_FLAG_ENGINE_STATE |
        QMI_LOC_EVENT_REGISTRATION_FLAG_FIX_SESSION_STATE, NULL);
    qmi_client_loc_register_events (client, in, 10, NULL, (GAsyncReadyCallback) register_ready, NULL);
    qmi_message_loc_register_events_input_unref (in);
}

static void open_ready (QmiDevice *dev, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;

    (void) data;
    if (!qmi_device_open_finish (dev, res, &error)) {
        die ("open QMI device", error);
        return;
    }
    qmi_device_allocate_client (dev, QMI_SERVICE_LOC, QMI_CID_NONE, 10, NULL,
                                (GAsyncReadyCallback) client_ready, NULL);
}

static void device_ready (GObject *src, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;

    (void) src;
    (void) data;
    device = qmi_device_new_from_node_finish (res, &error);
    if (!device) {
        die ("create QMI device", error);
        return;
    }
    qmi_device_open (device, QMI_DEVICE_OPEN_FLAGS_NONE, 15, NULL, (GAsyncReadyCallback) open_ready, NULL);
}

static void node_ready (QrtrBus *b, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;
    QrtrNode *node;

    (void) data;
    node = qrtr_bus_wait_for_node_finish (b, res, &error);
    if (!node) {
        die ("wait for QRTR node", error);
        return;
    }
    qmi_device_new_from_node (node, NULL, device_ready, NULL);
    g_object_unref (node);
}

static void bus_ready (GObject *src, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;

    (void) src;
    (void) data;
    bus = qrtr_bus_new_finish (res, &error);
    if (!bus) {
        die ("open QRTR bus", error);
        return;
    }
    qrtr_bus_wait_for_node (bus, node_id, 30000, NULL, (GAsyncReadyCallback) node_ready, NULL);
}

static void file_device_ready (GObject *src, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;
    GObject *dev = g_async_initable_new_finish (G_ASYNC_INITABLE (src), res, &error);

    (void) data;
    if (!dev) {
        die ("create QMI device", error);
        return;
    }
    device = QMI_DEVICE (dev);
    qmi_device_open (device, QMI_DEVICE_OPEN_FLAGS_NONE, 15, NULL, (GAsyncReadyCallback) open_ready, NULL);
}

static void stop_ready (QmiClientLoc *c, GAsyncResult *res, gpointer data)
{
    QmiMessageLocStopOutput *out = qmi_client_loc_stop_finish (c, res, NULL);

    (void) data;
    if (out)
        qmi_message_loc_stop_output_unref (out);
    qmi_device_release_client (device, QMI_CLIENT (client), QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
                               3, NULL, NULL, NULL);
    g_main_loop_quit (loop);
}

static gboolean on_signal (gpointer data)
{
    QmiMessageLocStopInput *in;

    (void) data;
    if (!client) {
        g_main_loop_quit (loop);
        return G_SOURCE_REMOVE;
    }
    in = qmi_message_loc_stop_input_new ();
    qmi_message_loc_stop_input_set_session_id (in, SESSION_ID, NULL);
    qmi_client_loc_stop (client, in, 3, NULL, (GAsyncReadyCallback) stop_ready, NULL);
    qmi_message_loc_stop_input_unref (in);
    return G_SOURCE_REMOVE;
}

int main (int argc, char **argv)
{
    int opt;

    while ((opt = getopt (argc, argv, "n:p:i:vd:")) != -1) {
        switch (opt) {
        case 'd': qmux_file = optarg; break;
        case 'n': node_id = (guint) strtoul (optarg, NULL, 10); break;
        case 'p': udp_port = (guint) strtoul (optarg, NULL, 10); break;
        case 'i': interval_ms = (guint) strtoul (optarg, NULL, 10); break;
        case 'v': verbose = TRUE; break;
        default:
            fprintf (stderr, "usage: %s [-n qrtr-node] [-p udp-port] [-i interval-ms] [-v] [-d qmux-file]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    udp_fd = socket (AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        perror ("socket");
        return EXIT_FAILURE;
    }
    memset (&udp_dst, 0, sizeof udp_dst);
    udp_dst.sin_family = AF_INET;
    udp_dst.sin_port = htons ((uint16_t) udp_port);
    udp_dst.sin_addr.s_addr = htonl (INADDR_LOOPBACK);

    loop = g_main_loop_new (NULL, FALSE);
    g_unix_signal_add (SIGINT, on_signal, NULL);
    g_unix_signal_add (SIGTERM, on_signal, NULL);
    if (qmux_file) {
        GFile *f = g_file_new_for_path (qmux_file);

        g_async_initable_new_async (QMI_TYPE_DEVICE, G_PRIORITY_DEFAULT, NULL, file_device_ready, NULL,
                                    QMI_DEVICE_FILE, f, QMI_DEVICE_NO_FILE_CHECK, TRUE, NULL);
        g_object_unref (f);
    } else
        qrtr_bus_new (1000, NULL, bus_ready, NULL);
    g_main_loop_run (loop);

    g_clear_object (&client);
    g_clear_object (&device);
    g_clear_object (&bus);
    g_main_loop_unref (loop);
    close (udp_fd);
    return exit_code;
}
