/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pro1x-fmctl -- one request to pro1x-fmd (its socket), the answer on
 * stdout; exit status 0 for "ok", 1 for "error ...", 2 without a daemon.
 *
 *   pro1x-fmctl on
 *   pro1x-fmctl tune 99.9
 *   pro1x-fmctl seek up
 *   pro1x-fmctl status | station | ps | rt | info
 *   pro1x-fmctl -t 10 watch       the events for 10 s
 *   pro1x-fmctl off
 *
 * -S socket: another socket than /var/run/pro1x-fm.sock.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int
connect_to(const char *path)
{
	struct sockaddr_un sun = { .sun_family = AF_UNIX };
	int fd;

	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", path);
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return (-1);
	if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
		close(fd);
		return (-1);
	}
	return (fd);
}

int
main(int argc, char **argv)
{
	const char *path = "/var/run/pro1x-fm.sock";
	char line[512], buf[1024];
	struct pollfd pfd;
	size_t len = 0;
	ssize_t n;
	int ch, fd, i, watch_s = -1, got = 0, rc = 2;
	time_t end = 0;

	while ((ch = getopt(argc, argv, "S:t:")) != -1) {
		switch (ch) {
		case 'S':	path = optarg; break;
		case 't':	watch_s = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: pro1x-fmctl [-S socket] "
			    "[-t seconds] request [argument]\n");
			return (2);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1) {
		fprintf(stderr, "pro1x-fmctl: no request\n");
		return (2);
	}
	line[0] = '\0';
	for (i = 0; i < argc; i++)
		snprintf(line + strlen(line), sizeof(line) - strlen(line),
		    "%s%s", i ? " " : "", argv[i]);
	strncat(line, "\n", sizeof(line) - strlen(line) - 1);
	if ((fd = connect_to(path)) < 0) {
		fprintf(stderr, "pro1x-fmctl: %s: %s (is pro1x-fmd running?)\n",
		    path, strerror(errno));
		return (2);
	}
	if (write(fd, line, strlen(line)) < 0) {
		perror("pro1x-fmctl");
		return (2);
	}
	if (strcmp(argv[0], "watch") == 0)
		end = time(NULL) + (watch_s < 0 ? 3600 * 24 : watch_s);
	pfd.fd = fd;
	pfd.events = POLLIN;
	for (;;) {
		int timeout = end ? (int)(end - time(NULL)) * 1000 : 60000;

		if (timeout <= 0 || poll(&pfd, 1, timeout) <= 0)
			break;
		n = read(fd, buf + len, sizeof(buf) - 1 - len);
		if (n <= 0)
			break;
		len += (size_t)n;
		buf[len] = '\0';
		while (len > 0) {
			char *nl = memchr(buf, '\n', len);

			if (nl == NULL)
				break;
			*nl = '\0';
			printf("%s\n", buf);
			fflush(stdout);
			if (!got) {
				got = 1;
				rc = strncmp(buf, "ok", 2) == 0 ? 0 : 1;
			}
			len -= (size_t)(nl + 1 - buf);
			memmove(buf, nl + 1, len + 1);
		}
		if (got && !end)
			break;
		if (len == sizeof(buf) - 1)
			len = 0;
	}
	close(fd);
	return (rc);
}
