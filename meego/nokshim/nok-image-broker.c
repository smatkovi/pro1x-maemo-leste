/*
 * nok-image-broker: keeps the dma-bufs behind EGL_NOK_image_shared handles.
 *
 * Harmattan's theme daemon renders every theme graphic once and hands the
 * other processes a global handle (EGL_NOK_image_shared, a PowerVR driver
 * feature of the N9). Mesa has no such handles, so the EGL shim exports
 * the image as a dma-buf and parks the file descriptors here; importers
 * fetch them by handle over the socket. The broker is started on demand by
 * the first process that needs it and runs as long as its user's session.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "nokshim.h"

struct entry {
	int used;
	int owner;			/* connection fd that registered it */
	struct nok_image image;
	int fds[NOK_MAX_PLANES];
};

static struct entry *table;
static uint32_t table_len;
static uint32_t next_handle = 1;

static struct pollfd *conns;
static int nconns;

static struct entry *lookup(uint32_t handle)
{
	if (handle == 0 || handle > table_len || !table[handle - 1].used)
		return NULL;
	return &table[handle - 1];
}

static void drop(struct entry *e)
{
	for (uint32_t i = 0; i < e->image.planes; i++)
		close(e->fds[i]);
	e->used = 0;
}

static uint32_t store(int owner, const struct nok_image *img, const int *fds)
{
	if (next_handle > table_len) {
		uint32_t len = table_len ? table_len * 2 : 1024;
		struct entry *t = realloc(table, len * sizeof(*t));
		if (!t)
			return 0;
		memset(t + table_len, 0, (len - table_len) * sizeof(*t));
		table = t;
		table_len = len;
	}
	uint32_t h = next_handle++;
	struct entry *e = &table[h - 1];
	e->used = 1;
	e->owner = owner;
	e->image = *img;
	memcpy(e->fds, fds, sizeof(e->fds));
	return h;
}

static int recv_msg(int fd, struct nok_msg *m, int *fds, int *nfds)
{
	char cbuf[CMSG_SPACE(sizeof(int) * NOK_MAX_PLANES)];
	struct iovec iov = { m, sizeof(*m) };
	struct msghdr mh = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = cbuf, .msg_controllen = sizeof(cbuf),
	};
	ssize_t n = recvmsg(fd, &mh, MSG_CMSG_CLOEXEC);
	if (n != sizeof(*m))
		return -1;
	*nfds = 0;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
			int k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			memcpy(fds, CMSG_DATA(c), k * sizeof(int));
			*nfds = k;
		}
	}
	return 0;
}

static void send_msg(int fd, const struct nok_msg *m, const int *fds, int nfds)
{
	char cbuf[CMSG_SPACE(sizeof(int) * NOK_MAX_PLANES)];
	struct iovec iov = { (void *)m, sizeof(*m) };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
	if (nfds) {
		mh.msg_control = cbuf;
		mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
		struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
		memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
	}
	sendmsg(fd, &mh, MSG_NOSIGNAL);
}

static void client_gone(int fd)
{
	for (uint32_t i = 0; i < table_len; i++)
		if (table[i].used && table[i].owner == fd)
			drop(&table[i]);
	close(fd);
}

static int handle_client(int fd)
{
	struct nok_msg m, r;
	int fds[NOK_MAX_PLANES], nfds;
	struct entry *e;

	if (recv_msg(fd, &m, fds, &nfds) < 0)
		return -1;
	memset(&r, 0, sizeof(r));
	r.op = m.op;
	switch (m.op) {
	case NOK_REGISTER:
		if (nfds < 1 || (uint32_t)nfds != m.image.planes || m.image.planes > NOK_MAX_PLANES) {
			for (int i = 0; i < nfds; i++)
				close(fds[i]);
		} else {
			r.handle = store(fd, &m.image, fds);
			if (!r.handle)
				for (int i = 0; i < nfds; i++)
					close(fds[i]);
		}
		send_msg(fd, &r, NULL, 0);
		break;
	case NOK_LOOKUP:
		e = lookup(m.handle);
		if (e) {
			r.handle = m.handle;
			r.image = e->image;
			send_msg(fd, &r, e->fds, e->image.planes);
		} else {
			send_msg(fd, &r, NULL, 0);
		}
		break;
	case NOK_DESTROY:
		e = lookup(m.handle);
		if (e)
			drop(e);
		break;
	default:
		return -1;
	}
	return 0;
}

int main(void)
{
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
		rl.rlim_cur = rl.rlim_max;
		setrlimit(RLIMIT_NOFILE, &rl);
	}
	signal(SIGPIPE, SIG_IGN);

	int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int n = nok_socket_name(sa.sun_path, sizeof(sa.sun_path), getuid());
	if (bind(ls, (struct sockaddr *)&sa, offsetof(struct sockaddr_un, sun_path) + n) < 0) {
		/* Another broker is already running. */
		return errno == EADDRINUSE ? 0 : 1;
	}
	listen(ls, 64);

	conns = calloc(1, sizeof(*conns));
	conns[0].fd = ls;
	conns[0].events = POLLIN;
	nconns = 1;

	for (;;) {
		if (poll(conns, nconns, -1) < 0) {
			if (errno == EINTR)
				continue;
			return 1;
		}
		if (conns[0].revents & POLLIN) {
			int c = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
			if (c >= 0) {
				struct pollfd *p = realloc(conns, (nconns + 1) * sizeof(*p));
				if (p) {
					conns = p;
					conns[nconns].fd = c;
					conns[nconns].events = POLLIN;
					conns[nconns].revents = 0;
					nconns++;
				} else {
					close(c);
				}
			}
		}
		for (int i = 1; i < nconns; i++) {
			int dead = 0;
			if (conns[i].revents & POLLIN)
				dead = handle_client(conns[i].fd) < 0;
			else if (conns[i].revents & (POLLHUP | POLLERR))
				dead = 1;
			if (dead) {
				client_gone(conns[i].fd);
				conns[i] = conns[--nconns];
				i--;
			}
		}
	}
}
