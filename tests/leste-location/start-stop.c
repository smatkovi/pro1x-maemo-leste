/*
 * A LocationGPSDControl created, started, stopped and started again, with
 * the control's signals and, after each step, whether a client holds
 * location-daemon's lock (the daemon runs while one does: it exits when
 * it gets an exclusive lock). Fixed liblocation: the lock free after the
 * creation, held after each start, free after the stop; gpsd-running after
 * both starts and location-daemon's start called twice. Unfixed: the lock
 * held from the creation on, and the second start does nothing (no
 * gpsd-running, no call), as gpsd_shutdown() did not clear gpsd_running.
 * Run with the disclaimer accepted, so that the starts need no dialog.
 *
 *   gcc -o start-stop start-stop.c $(pkg-config --cflags --libs liblocation)
 *   start-stop [LOCK_FILE]    (default /run/lock/location-daemon.lock)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#include <location/location-gpsd-control.h>

static const char *lock_file = "/run/lock/location-daemon.lock";

static void lock_state(const char *step)
{
	int fd = open(lock_file, O_RDONLY);

	if (fd < 0) {
		printf("%s: lock file: %s\n", step, strerror(errno));
	} else if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
		flock(fd, LOCK_UN);
		printf("%s: lock free (the daemon would exit)\n", step);
	} else {
		printf("%s: lock held by a client\n", step);
	}
	if (fd >= 0)
		close(fd);
	fflush(stdout);
}

static void on_signal(LocationGPSDControl *control, gpointer name)
{
	(void)control;
	printf("  signal: %s\n", (const char *)name);
	fflush(stdout);
}

static gboolean quit(gpointer loop)
{
	g_main_loop_quit(loop);
	return FALSE;
}

static void run(GMainLoop *loop, guint seconds)
{
	g_timeout_add_seconds(seconds, quit, loop);
	g_main_loop_run(loop);
}

int main(int argc, char **argv)
{
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	LocationGPSDControl *control;

	if (argc > 1)
		lock_file = argv[1];

	control = location_gpsd_control_get_default();
	g_signal_connect(control, "error", G_CALLBACK(on_signal), "error");
	g_signal_connect(control, "gpsd-running", G_CALLBACK(on_signal), "gpsd-running");
	g_signal_connect(control, "gpsd-stopped", G_CALLBACK(on_signal), "gpsd-stopped");
	run(loop, 2);
	lock_state("created");

	printf("start\n");
	location_gpsd_control_start(control);
	run(loop, 3);
	lock_state("started");

	printf("stop\n");
	location_gpsd_control_stop(control);
	run(loop, 2);
	lock_state("stopped");

	printf("start again\n");
	location_gpsd_control_start(control);
	run(loop, 3);
	lock_state("started again");

	location_gpsd_control_stop(control);
	g_object_unref(control);
	lock_state("freed");
	printf("done\n");
	return 0;
}
