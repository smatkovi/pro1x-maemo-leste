/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The QRTR sockets on FreeBSD (kldload qcom_dsp qrtr), without a remote
 * processor: a server announces itself to the name service, a client looks
 * it up and gets its address with the end marker after it, they exchange
 * data, and when the client closes the server hears a DEL_CLIENT.
 *
 *   qrtr_sock_test     (freebsd/build.sh builds it as dsp-tools)
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <assert.h>
#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "qrtr_sock.h"

#define	PORT_CTRL	0xfffffffeu
#define	NEW_SERVER	4
#define	DEL_CLIENT	6
#define	NEW_LOOKUP	10

struct ctrl {
	uint32_t	cmd;
	uint32_t	a, b, c, d;	/* service instance node port / node port */
};

static int
qsock(void)
{
	struct timeval tv = { 3, 0 };
	int s;

	if ((s = socket(AF_QIPCRTR, SOCK_DGRAM, 0)) < 0)
		err(1, "socket(AF_QIPCRTR) (kldload qrtr?)");
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return (s);
}

static struct sockaddr_qrtr
addr(uint32_t node, uint32_t port)
{
	struct sockaddr_qrtr sq;

	memset(&sq, 0, sizeof(sq));
	sq.sq_len = sizeof(sq);
	sq.sq_family = AF_QIPCRTR;
	sq.sq_node = node;
	sq.sq_port = port;
	return (sq);
}

static void
send_ctrl(int s, uint32_t cmd, uint32_t a, uint32_t b)
{
	struct ctrl c = { cmd, a, b, 0, 0 };
	struct sockaddr_qrtr ns = addr(1, PORT_CTRL);

	if (sendto(s, &c, sizeof(c), 0, (struct sockaddr *)&ns, sizeof(ns)) !=
	    sizeof(c))
		err(1, "sendto name service");
}

static ssize_t
recv_from(int s, void *buf, size_t len, struct sockaddr_qrtr *from)
{
	socklen_t fl = sizeof(*from);
	ssize_t n;

	n = recvfrom(s, buf, len, 0, (struct sockaddr *)from, &fl);
	if (n < 0)
		err(1, "recvfrom");
	return (n);
}

int
main(void)
{
	struct sockaddr_qrtr me, from, sa;
	struct ctrl c;
	socklen_t len;
	char buf[64];
	int srv, cli;
	ssize_t n;

	/* a server on a free port */
	srv = qsock();
	len = sizeof(me);
	if (getsockname(srv, (struct sockaddr *)&me, &len) < 0)
		err(1, "getsockname");
	printf("server socket: node %u port %#x\n", me.sq_node, me.sq_port);
	assert(me.sq_node == 1 && me.sq_port >= 0x4000);
	send_ctrl(srv, NEW_SERVER, 4242, 1);

	/* a client finds it */
	cli = qsock();
	send_ctrl(cli, NEW_LOOKUP, 4242, 0);
	n = recv_from(cli, &c, sizeof(c), &from);
	printf("lookup: %zd bytes from %u:%#x: cmd %u service %u node %u port %#x\n",
	    n, from.sq_node, from.sq_port, c.cmd, c.a, c.c, c.d);
	assert(from.sq_port == PORT_CTRL && c.cmd == NEW_SERVER && c.a == 4242 &&
	    c.c == 1 && c.d == me.sq_port);
	n = recv_from(cli, &c, sizeof(c), &from);
	assert(c.cmd == NEW_SERVER && c.a == 0);
	printf("end marker: ok\n");

	/* data both ways */
	sa = addr(c.c ? c.c : 1, me.sq_port);
	sa.sq_node = 1;
	if (sendto(cli, "ping", 4, 0, (struct sockaddr *)&sa, sizeof(sa)) != 4)
		err(1, "sendto server");
	n = recv_from(srv, buf, sizeof(buf), &from);
	assert(n == 4 && memcmp(buf, "ping", 4) == 0 && from.sq_node == 1);
	if (sendto(srv, "pong!", 5, 0, (struct sockaddr *)&from, sizeof(from)) != 5)
		err(1, "sendto client");
	n = recv_from(cli, buf, sizeof(buf), &from);
	assert(n == 5 && memcmp(buf, "pong!", 5) == 0 &&
	    from.sq_port == me.sq_port);
	printf("data both ways: ok\n");

	/* the client goes: the server hears it from the name service */
	len = sizeof(sa);
	getsockname(cli, (struct sockaddr *)&sa, &len);
	close(cli);
	n = recv_from(srv, &c, sizeof(c), &from);
	printf("after close: cmd %u node %u port %#x\n", c.cmd, c.a, c.b);
	assert(from.sq_port == PORT_CTRL && c.cmd == DEL_CLIENT && c.a == 1 &&
	    c.b == sa.sq_port);
	close(srv);
	printf("all QRTR socket tests passed\n");
	return (0);
}
