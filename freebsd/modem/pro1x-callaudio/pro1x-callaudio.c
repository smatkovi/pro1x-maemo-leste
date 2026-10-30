/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-callaudio [-v] [-S socket] [-r route] [-w seconds] [-p pidfile]: the
 * voice of a call on FreeBSD (freebsd/modem/README.md, "Stage 2").
 *
 * pro1x-modemd runs it while there are calls (-A), as Linux's
 * pro1x-voice-call runs while ModemManager lists one, and sends it SIGTERM
 * after the last. The voice never passes the application processor: the
 * modem hands it to the ADSP, which runs it through its voice processing to
 * the codec DMA ports and from the microphone back. Setting that up is the
 * ADSP's audio services over APR, whose one user on FreeBSD is
 * pro1x-audiod (freebsd/audio: /dev/apr takes one process; its "voice
 * start" is the MVM/CVP session as Linux's q6voice and KolibriOS start it,
 * then the back ends, RX_CODEC_DMA_RX_0 and VA_CODEC_DMA_TX_0, with the
 * codec's paths of the current route). So this asks pro1x-audiod, on its
 * control socket (one request per connection, "ok ..." or "error ..."):
 *
 *   start   "status" (the route before the call), "route ear" (-r: the
 *           earpiece), "voice start"; "callaudio: ready" on stdout
 *   SIGTERM "voice stop", the route of before; exit 0
 *   SIGUSR1 the loudspeaker ("route spk"), SIGUSR2 the call's route again
 *           (sphone's Speaker button through its module route-pro1x, which
 *           finds this process in -p's file, /var/run/pro1x-callaudio.pid,
 *           written once the voice is up)
 *
 * Without pro1x-audiod (none within -w seconds, default 5) or when the
 * voice does not start, it says so and exits 1: pro1x-modemd then does not
 * run it again for these calls, which go on without sound.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define	SOCK_PATH	"/var/run/pro1x-audio.sock"

#define	PID_PATH	"/var/run/pro1x-callaudio.pid"

static const char *sock_path = SOCK_PATH;
static const char *pid_path = PID_PATH;
static int verbose;
static volatile sig_atomic_t got_term, want_spk = -1;

static void
say(const char *fmt, ...)
{
	va_list ap;

	printf("callaudio: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

/* one request, its answer (without the newline); -1: no daemon */
static int
ask(const char *req, char *ans, size_t alen)
{
	struct sockaddr_un sun;
	char line[128];
	size_t got = 0;
	ssize_t n;
	int s;

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	if (strlen(sock_path) >= sizeof(sun.sun_path))
		return (-1);
	strcpy(sun.sun_path, sock_path);
	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return (-1);
	if (connect(s, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
		close(s);
		return (-1);
	}
	/* the request and its newline in one write: pro1x-audiod reads once
	 * and closes, a second write would find the socket closed */
	snprintf(line, sizeof(line), "%s\n", req);
	if (write(s, line, strlen(line)) != (ssize_t)strlen(line)) {
		close(s);
		return (-1);
	}
	while (got + 1 < alen && (n = read(s, ans + got, alen - 1 - got)) > 0)
		got += (size_t)n;
	close(s);
	ans[got] = '\0';
	ans[strcspn(ans, "\r\n")] = '\0';
	if (verbose)
		say("%s -> %s", req, ans);
	return (strncmp(ans, "ok", 2) == 0 ? 0 : 1);
}

static void
on_signal(int sig)
{
	if (sig == SIGUSR1)
		want_spk = 1;
	else if (sig == SIGUSR2)
		want_spk = 0;
	else
		got_term = 1;
}

static void
usage(void)
{
	fprintf(stderr, "usage: pro1x-callaudio [-v] [-S socket] [-r route] [-w seconds] "
	    "[-p pidfile]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	const char *route = "ear";
	char ans[512], before[32] = "", cmd[64];
	const char *p;
	int c, wait_s = 5, r = -1, speaker = 0;
	struct sigaction sa;

	while ((c = getopt(argc, argv, "vS:r:w:p:")) != -1) {
		switch (c) {
		case 'p': pid_path = optarg; break;
		case 'v': verbose = 1; break;
		case 'S': sock_path = optarg; break;
		case 'r': route = optarg; break;
		case 'w': wait_s = atoi(optarg); break;
		default: usage();
		}
	}
	if (optind != argc)
		usage();
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	/* a daemon that closes early is an error answer, not our end */
	signal(SIGPIPE, SIG_IGN);

	/* pro1x-audiod, which may be starting too */
	for (int i = 0; i <= wait_s * 10 && !got_term; i++) {
		if ((r = ask("status", ans, sizeof(ans))) >= 0)
			break;
		usleep(100000);
	}
	if (r < 0) {
		say("no pro1x-audiod on %s: the call has no sound", sock_path);
		return (1);
	}
	if ((p = strstr(ans, " route=")) != NULL)
		sscanf(p + 7, "%31[a-z]", before);
	snprintf(cmd, sizeof(cmd), "route %s", route);
	if (ask(cmd, ans, sizeof(ans)) != 0)
		say("%s: %s (the voice goes to the current route)", cmd, ans);
	if (ask("voice start", ans, sizeof(ans)) != 0) {
		say("voice start: %s: the call has no sound", ans);
		if (before[0] && strcmp(before, route)) {
			snprintf(cmd, sizeof(cmd), "route %s", before);
			ask(cmd, ans, sizeof(ans));
		}
		return (1);
	}
	say("ready (%s; route %s, before %s)", ans, route, before[0] ? before : "?");
	{
		FILE *f = fopen(pid_path, "w");

		if (f != NULL) {
			fprintf(f, "%ld\n", (long)getpid());
			fclose(f);
		} else {
			say("%s: %s (no loudspeaker from sphone)", pid_path, strerror(errno));
		}
	}

	/* the signals held between the checks and the wait (sigsuspend) */
	sigset_t block, old;

	sigemptyset(&block);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGUSR1);
	sigaddset(&block, SIGUSR2);
	sigprocmask(SIG_BLOCK, &block, &old);
	while (!got_term) {
		if (want_spk < 0)
			sigsuspend(&old);
		if (want_spk >= 0 && !got_term) {
			int w = want_spk;

			want_spk = -1;
			if (w == speaker)
				continue;
			speaker = w;
			snprintf(cmd, sizeof(cmd), "route %s", speaker ? "spk" : route);
			r = ask(cmd, ans, sizeof(ans));
			say("%s: %s", cmd, r == 0 ? "done" : ans);
		}
	}
	unlink(pid_path);
	r = ask("voice stop", ans, sizeof(ans));
	if (r != 0)
		say("voice stop: %s", r < 0 ? "pro1x-audiod is gone" : ans);
	if (before[0] && r >= 0) {
		snprintf(cmd, sizeof(cmd), "route %s", before);
		ask(cmd, ans, sizeof(ans));
	}
	say("down");
	return (0);
}
