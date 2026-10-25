/*
 * The IP heartbeat service of Harmattan's dsme, for the MeeGo session.
 * Runs on Leste, as root.
 *
 * Harmattan programs that wake up periodically (the status bar clock,
 * e-mail, alarms) ask dsme's iphb module for their wakeups through libiphb:
 * a stream socket at /dev/shm/iphb (shared with the chroot), one request
 * per wait,
 *
 *   struct { int cmd; union { struct { u16 mintime, maxtime; pid_t pid; }
 *            wait; char dummy[64]; } u; }      cmd 0 = wait, 1 = statistics
 *
 * and a 32-bit "seconds waited" back when the wait is over. Leste's dsme
 * has no such module, so their clients never woke up. dsme lined up the
 * wakeups of all clients between their min and max time to save power;
 * here each client is woken at its min time.
 *
 * min = max = 0 registers the client or cancels its wait.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SOCKET_PATH "/dev/shm/iphb"
#define MAX_CLIENTS 64

struct req {			/* the 32-bit clients' struct _iphb_req_t */
	int32_t cmd;
	union {
		struct {
			uint16_t mintime, maxtime;
			int32_t pid;
		} wait;
		char dummy[64];
	} u;
};

static struct client {
	int fd;
	time_t started;		/* 0 = not waiting */
	time_t wake_at;
} clients[MAX_CLIENTS];
static int nclients;

static time_t now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

static void drop(int i)
{
	close(clients[i].fd);
	clients[i] = clients[--nclients];
}

static void wake(struct client *c, time_t t)
{
	int32_t waited = t - c->started;
	c->started = 0;
	if (send(c->fd, &waited, sizeof(waited), MSG_DONTWAIT | MSG_NOSIGNAL) != sizeof(waited))
		c->wake_at = -1;	/* dropped by the caller */
}

/* Returns 0 to keep the client, -1 to drop it. */
static int handle(struct client *c)
{
	struct req r;
	ssize_t n = recv(c->fd, &r, sizeof(r), MSG_DONTWAIT);

	if (n == 0 || (n < 0 && errno != EAGAIN))
		return -1;
	if (n != sizeof(r))
		return 0;
	if (r.cmd == 1) {		/* statistics: clients, waiting, next wakeup */
		uint32_t stats[3] = { nclients, 0, 0 };
		for (int i = 0; i < nclients; i++)
			stats[1] += clients[i].started != 0;
		send(c->fd, stats, sizeof(stats), MSG_DONTWAIT | MSG_NOSIGNAL);
		return 0;
	}
	if (r.cmd != 0)
		return 0;
	if (r.u.wait.mintime == 0 && r.u.wait.maxtime == 0) {
		c->started = 0;
		return 0;
	}
	unsigned min = r.u.wait.mintime ? r.u.wait.mintime : r.u.wait.maxtime;
	c->started = now();
	c->wake_at = c->started + min;
	return 0;
}

int main(void)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	strcpy(addr.sun_path, SOCKET_PATH);
	unlink(SOCKET_PATH);
	if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(lfd, 16) < 0) {
		perror(SOCKET_PATH);
		return 1;
	}
	chmod(SOCKET_PATH, 0666);

	for (;;) {
		struct pollfd pfd[MAX_CLIENTS + 1];
		time_t t = now();
		int timeout = -1;

		pfd[0] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		for (int i = 0; i < nclients; i++) {
			pfd[i + 1] = (struct pollfd){ .fd = clients[i].fd, .events = POLLIN };
			if (clients[i].started) {
				int ms = clients[i].wake_at > t ? (clients[i].wake_at - t) * 1000 : 0;
				if (timeout < 0 || ms < timeout)
					timeout = ms;
			}
		}
		if (poll(pfd, nclients + 1, timeout) < 0 && errno != EINTR)
			return 1;

		t = now();
		for (int i = nclients - 1; i >= 0; i--) {
			if ((pfd[i + 1].revents & (POLLIN | POLLHUP | POLLERR)) && handle(&clients[i]) < 0) {
				drop(i);
				continue;
			}
			if (clients[i].started && clients[i].wake_at <= t) {
				wake(&clients[i], t);
				if (clients[i].wake_at < 0)
					drop(i);
			}
		}
		if (pfd[0].revents & POLLIN) {
			int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
			if (fd >= 0 && nclients < MAX_CLIENTS)
				clients[nclients++] = (struct client){ .fd = fd };
			else if (fd >= 0)
				close(fd);
		}
	}
}
