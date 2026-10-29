/*
 * pro1x-light -- one reading of the Pro1X's ambient light sensor, in lux.
 *
 *   pro1x-light        prints the illuminance (an integer, lux) and exits
 *
 * The sensor is one of the ADSP's (libssc, as iio-sensor-proxy reads it on
 * Leste). It streams only while this runs: on for the first readings, the
 * last of them printed, then off again, so that an app (the photo light
 * meter) can ask for one value without a daemon keeping the sensor on.
 * Exits 1 without a reading (no DSP: hexagonrpcd not running, or no
 * measurement within the time limit).
 *
 * Runs as root (FastRPC/QRTR).
 */
#include <stdio.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <libssc-sensor-light.h>

#define TIMEOUT_S 10
#define READINGS 3	/* the first one after opening can be stale */
#define SETTLE_MS 1500	/* or as many as come within this after the first */

static GMainLoop *loop;
static float lux = -1;
static int readings;
static guint settle_id;

static gboolean done(gpointer data)
{
	(void)data;
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static void measurement_cb(SSCSensorLight *s, float intensity, gpointer data)
{
	(void)s, (void)data;
	lux = intensity;
	if (++readings >= READINGS)
		g_main_loop_quit(loop);
	else if (!settle_id)
		settle_id = g_timeout_add(SETTLE_MS, done, NULL);
}

int main(void)
{
	g_autoptr(GError) err = NULL;
	SSCSensorLight *sensor;

	loop = g_main_loop_new(NULL, FALSE);
	sensor = ssc_sensor_light_new_sync(NULL, &err);
	if (!sensor) {
		fprintf(stderr, "pro1x-light: no light sensor: %s\n", err->message);
		return 1;
	}
	g_signal_connect(sensor, "measurement", G_CALLBACK(measurement_cb), NULL);
	if (!ssc_sensor_light_open_sync(sensor, NULL, &err)) {
		fprintf(stderr, "pro1x-light: cannot open the light sensor: %s\n", err->message);
		g_object_unref(sensor);
		return 1;
	}
	g_timeout_add_seconds(TIMEOUT_S, done, NULL);
	g_main_loop_run(loop);
	ssc_sensor_light_close_sync(sensor, NULL, NULL);
	g_object_unref(sensor);
	if (lux < 0) {
		fprintf(stderr, "pro1x-light: no reading within %d s\n", TIMEOUT_S);
		return 1;
	}
	printf("%ld\n", (long)(lux + 0.5f));
	return 0;
}
