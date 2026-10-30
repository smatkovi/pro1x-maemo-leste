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
 * not there.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
		fprintf(stderr, "usage: pro1x-audioctl [-S socket] request ...\n");
		return (2);
	}
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
