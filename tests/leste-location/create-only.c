/*
 * Creates a LocationGPSDevice and a LocationGPSDControl, as an app does
 * before it decides to start positioning, runs the main loop for a few
 * seconds and ends without ever calling location_gpsd_control_start().
 * Prints the control's signals (those after its creation; what its init
 * does shows in the system bus' monitor). Creating the control must not
 * start positioning (Maemo 5's API: location_gpsd_control_start() is how
 * an app tells that it wants the location service): no dialog request to
 * location-ui, no "start" to location-daemon, no signal. Unfixed
 * liblocation does both from the control's init when GConf has a method
 * pair (location-control's schema always sets one): the disclaimer dialog
 * when it is not accepted, location-daemon's start when it is.
 *
 *   gcc -o create-only create-only.c $(pkg-config --cflags --libs liblocation)
 *   create-only [SECONDS]
 */
#include <stdio.h>
#include <stdlib.h>
#include <location/location-gps-device.h>
#include <location/location-gpsd-control.h>

static void on_signal(LocationGPSDControl *control, gpointer name)
{
	(void)control;
	printf("signal: %s\n", (const char *)name);
	fflush(stdout);
}

static void on_error_verbose(LocationGPSDControl *control, gint error, gpointer data)
{
	(void)control;
	(void)data;
	printf("signal: error-verbose %d\n", error);
	fflush(stdout);
}

static gboolean quit(gpointer loop)
{
	g_main_loop_quit(loop);
	return FALSE;
}

int main(int argc, char **argv)
{
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	LocationGPSDevice *device;
	LocationGPSDControl *control;

	device = g_object_new(LOCATION_TYPE_GPS_DEVICE, NULL);
	control = location_gpsd_control_get_default();
	printf("control created, location_gpsd_control_start() not called\n");
	fflush(stdout);
	g_signal_connect(control, "error", G_CALLBACK(on_signal), "error");
	g_signal_connect(control, "error-verbose", G_CALLBACK(on_error_verbose), NULL);
	g_signal_connect(control, "gpsd-running", G_CALLBACK(on_signal), "gpsd-running");
	g_signal_connect(control, "gpsd-stopped", G_CALLBACK(on_signal), "gpsd-stopped");

	g_timeout_add_seconds(argc > 1 ? atoi(argv[1]) : 4, quit, loop);
	g_main_loop_run(loop);

	g_object_unref(control);
	g_object_unref(device);
	printf("done\n");
	return 0;
}
