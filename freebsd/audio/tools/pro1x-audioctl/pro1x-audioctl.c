/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pro1x-audioctl -- one request to pro1x-audiod, its answer on stdout
 * (freebsd/audio/README.md):
 *
 *   pro1x-audioctl status | route hph | volume 60 | voice start | ...
 *   pro1x-audioctl -S socket ...
 *
 * Exit status 0 for "ok ...", 1 for "error ...", 2 when the daemon is
 * not there. And two tests of /dev/dsp itself (OSS, 48 kHz, 16 bit; the
 * device tests, freebsd/rootfs/usr/local/libexec/pro1x-audiotest):
 *
 *   pro1x-audioctl tone SECONDS [HZ]       a sine at -20 dBFS, both
 *                                          channels, 5 ms ramps
 *   pro1x-audioctl record SECONDS [FILE]   mono; peak and RMS, the raw
 *                                          samples to FILE
 *
 * Both say how long the device took (real time if the ADSP moves the
 * sound, the driver's clock otherwise).
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define	RATE	48000

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec / 1e9);
}

static int
dsp_open(int mode, int ch)
{
	int fd, fmt = AFMT_S16_LE, rate = RATE;

	if ((fd = open("/dev/dsp", mode)) < 0) {
		perror("/dev/dsp");
		return (-1);
	}
	if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) < 0 ||
	    ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) < 0 ||
	    ioctl(fd, SNDCTL_DSP_SPEED, &rate) < 0) {
		perror("/dev/dsp: format");
		close(fd);
		return (-1);
	}
	printf("/dev/dsp: %d Hz, %d channel(s), format %#x\n", rate, ch, fmt);
	return (fd);
}

static int
tone(double secs, double hz)
{
	int16_t buf[2 * 960];
	long n = (long)(secs * RATE), i = 0, j, k;
	double t0, amp = 3277.0, ramp = RATE * 0.005, g;
	int fd;

	if ((fd = dsp_open(O_WRONLY, 2)) < 0)
		return (2);
	t0 = now();
	while (i < n) {
		for (j = 0; j < 960 && i < n; j++, i++) {
			g = i < ramp ? i / ramp : n - i < ramp ? (n - i) / ramp : 1;
			buf[2 * j] = buf[2 * j + 1] = (int16_t)(amp * g *
			    sin(2 * M_PI * hz * (double)i / RATE));
		}
		k = j * 4;
		if (write(fd, buf, (size_t)k) != k) {
			perror("write");
			close(fd);
			return (1);
		}
	}
	ioctl(fd, SNDCTL_DSP_SYNC, NULL);
	printf("tone: %ld frames of %.0f Hz in %.2f s\n", n, hz, now() - t0);
	close(fd);
	return (0);
}

static int
record(double secs, const char *file)
{
	int16_t buf[960];
	long n = (long)(secs * RATE), got = 0, i;
	double t0, sum = 0;
	int fd, peak = 0;
	ssize_t r;
	FILE *f = NULL;

	if ((fd = dsp_open(O_RDONLY, 1)) < 0)
		return (2);
	if (file != NULL && (f = fopen(file, "w")) == NULL)
		perror(file);
	t0 = now();
	while (got < n) {
		r = read(fd, buf, sizeof(buf));
		if (r <= 0) {
			perror("read");
			break;
		}
		for (i = 0; i < r / 2; i++) {
			if (abs(buf[i]) > peak)
				peak = abs(buf[i]);
			sum += (double)buf[i] * buf[i];
		}
		if (f != NULL)
			fwrite(buf, 1, (size_t)r, f);
		got += r / 2;
	}
	printf("record: %ld frames in %.2f s, peak %d, RMS %.1f\n", got,
	    now() - t0, peak, got ? sqrt(sum / got) : 0.0);
	if (f != NULL)
		fclose(f);
	close(fd);
	return (got == n ? 0 : 1);
}

int
main(int argc, char **argv)
{
	const char *path = "/var/run/pro1x-audio.sock";
	struct sockaddr_un sun;
	char req[256] = "", ans[1024];
	ssize_t n;
	int s, i = 1;

	if (argc > 2 && strcmp(argv[1], "-S") == 0) {
		path = argv[2];
		i = 3;
	}
	if (i >= argc) {
		fprintf(stderr, "usage: pro1x-audioctl [-S socket] request ...\n"
		    "       pro1x-audioctl tone SECONDS [HZ]\n"
		    "       pro1x-audioctl record SECONDS [FILE]\n");
		return (2);
	}
	if (strcmp(argv[i], "tone") == 0 && i + 1 < argc)
		return (tone(atof(argv[i + 1]), i + 2 < argc ?
		    atof(argv[i + 2]) : 1000));
	if (strcmp(argv[i], "record") == 0 && i + 1 < argc)
		return (record(atof(argv[i + 1]), i + 2 < argc ? argv[i + 2] :
		    NULL));
	for (; i < argc; i++) {
		strlcat(req, argv[i], sizeof(req));
		strlcat(req, i + 1 < argc ? " " : "\n", sizeof(req));
	}
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, path, sizeof(sun.sun_path));
	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 ||
	    connect(s, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
		perror(path);
		return (2);
	}
	if (write(s, req, strlen(req)) != (ssize_t)strlen(req)) {
		perror("write");
		return (2);
	}
	while ((n = read(s, ans, sizeof(ans) - 1)) > 0) {
		ans[n] = '\0';
		fputs(ans, stdout);
	}
	close(s);
	return (strncmp(ans, "ok", 2) == 0 ? 0 : 1);
}
