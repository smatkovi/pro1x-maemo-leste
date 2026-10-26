/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * QRTR sockets for FreeBSD (Linux's AF_QIPCRTR, here family 45: Linux's
 * 42 is FreeBSD's AF_INET6_SDP). Datagram sockets on a QRTR port of this
 * node (qrtr_core.c): bind(2) takes a port (0: a free one), sendto(2) goes
 * to a node and port, recvfrom(2) tells where a packet came from. A socket
 * not bound gets a free port when it first sends or asks for its name, as
 * on Linux. sockaddr_qrtr keeps node and port at Linux's offsets.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/domain.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/priv.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sx.h>

#include "qrtr.h"
#include "qrtr_sock.h"

#define	QRTR_SENDSPACE		(64 * 1024)
#define	QRTR_RECVSPACE		(256 * 1024)
#define	QRTR_MAX_PACKET		(64 * 1024)

static MALLOC_DEFINE(M_QRTR, "qrtr", "QRTR sockets");

struct qrtr_pcb {
	struct socket	*so;
	struct sx	lock;		/* port, peer */
	struct qrtr_port *port;
	bool		connected;
	uint32_t	peer_node, peer_port;
};

static void
qrtr_sock_rx(void *arg, const struct qrtr_rx_info *info, const void *data,
    size_t len)
{
	struct qrtr_pcb *pcb = arg;
	struct socket *so = pcb->so;
	struct sockaddr_qrtr from;
	struct mbuf *m;

	m = m_getm2(NULL, MAX(len, 1), M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL)
		return;
	m_copyback(m, 0, len, data);
	if (m_length(m, NULL) != len) {
		m_freem(m);
		return;
	}
	m->m_pkthdr.len = len;
	memset(&from, 0, sizeof(from));
	from.sq_len = sizeof(from);
	from.sq_family = AF_QIPCRTR;
	from.sq_node = info->src_node;
	from.sq_port = info->src_port;
	SOCKBUF_LOCK(&so->so_rcv);
	if (sbappendaddr_locked(&so->so_rcv, (struct sockaddr *)&from, m,
	    NULL) == 0) {
		SOCKBUF_UNLOCK(&so->so_rcv);
		m_freem(m);		/* no room: dropped, as on Linux */
		return;
	}
	sorwakeup_locked(so);
}

static int
qrtr_check_addr(struct sockaddr *nam)
{
	if (nam->sa_family != AF_QIPCRTR ||
	    nam->sa_len < sizeof(struct sockaddr_qrtr))
		return (EINVAL);
	return (0);
}

/* With pcb->lock: a port, the asked one or any */
static int
qrtr_bind_locked(struct qrtr_pcb *pcb, uint32_t port)
{
	if (pcb->port != NULL)
		return (EINVAL);
	return (qrtr_port_open(port, qrtr_sock_rx, pcb, &pcb->port));
}

static int
qrtr_attach(struct socket *so, int proto, struct thread *td)
{
	struct qrtr_pcb *pcb;
	int error;

	if (so->so_type != SOCK_DGRAM)
		return (EPROTOTYPE);
	if ((error = soreserve(so, QRTR_SENDSPACE, QRTR_RECVSPACE)) != 0)
		return (error);
	pcb = malloc(sizeof(*pcb), M_QRTR, M_WAITOK | M_ZERO);
	pcb->so = so;
	sx_init(&pcb->lock, "qrtr pcb");
	so->so_pcb = pcb;
	return (0);
}

static void
qrtr_detach(struct socket *so)
{
	struct qrtr_pcb *pcb = so->so_pcb;

	if (pcb == NULL)
		return;
	/* no packet is delivered to it after this */
	if (pcb->port != NULL)
		qrtr_port_close(pcb->port);
	so->so_pcb = NULL;
	sx_destroy(&pcb->lock);
	free(pcb, M_QRTR);
}

static int
qrtr_bind(struct socket *so, struct sockaddr *nam, struct thread *td)
{
	struct qrtr_pcb *pcb = so->so_pcb;
	struct sockaddr_qrtr *sq = (struct sockaddr_qrtr *)nam;
	int error;

	if ((error = qrtr_check_addr(nam)) != 0)
		return (error);
	if (sq->sq_node != QRTR_LOCAL_NODE && sq->sq_node != 0 &&
	    sq->sq_node != QRTR_NODE_BCAST)
		return (EADDRNOTAVAIL);
	/* ports below the free range are for root (Linux: CAP_NET_ADMIN) */
	if (sq->sq_port != 0 && sq->sq_port < 0x4000 &&
	    (error = priv_check(td, PRIV_NETINET_RESERVEDPORT)) != 0)
		return (error);
	sx_xlock(&pcb->lock);
	error = qrtr_bind_locked(pcb, sq->sq_port);
	sx_xunlock(&pcb->lock);
	return (error);
}

static int
qrtr_connect(struct socket *so, struct sockaddr *nam, struct thread *td)
{
	struct qrtr_pcb *pcb = so->so_pcb;
	struct sockaddr_qrtr *sq = (struct sockaddr_qrtr *)nam;
	int error;

	if ((error = qrtr_check_addr(nam)) != 0)
		return (error);
	sx_xlock(&pcb->lock);
	if (pcb->port == NULL && (error = qrtr_bind_locked(pcb, 0)) != 0) {
		sx_xunlock(&pcb->lock);
		return (error);
	}
	pcb->peer_node = sq->sq_node;
	pcb->peer_port = sq->sq_port;
	pcb->connected = true;
	sx_xunlock(&pcb->lock);
	soisconnected(so);
	return (0);
}

static int
qrtr_disconnect(struct socket *so)
{
	struct qrtr_pcb *pcb = so->so_pcb;

	sx_xlock(&pcb->lock);
	pcb->connected = false;
	sx_xunlock(&pcb->lock);
	soisdisconnected(so);
	return (0);
}

static int
qrtr_send(struct socket *so, int flags, struct mbuf *m, struct sockaddr *nam,
    struct mbuf *control, struct thread *td)
{
	struct qrtr_pcb *pcb = so->so_pcb;
	struct sockaddr_qrtr *sq = (struct sockaddr_qrtr *)nam;
	uint32_t node, port;
	size_t len;
	void *buf = NULL;
	int error = 0;

	if (control != NULL) {
		m_freem(control);
		error = EINVAL;
		goto out;
	}
	if ((flags & PRUS_OOB) != 0) {
		error = EOPNOTSUPP;
		goto out;
	}
	sx_xlock(&pcb->lock);
	if (sq != NULL) {
		if ((error = qrtr_check_addr(nam)) != 0) {
			sx_xunlock(&pcb->lock);
			goto out;
		}
		node = sq->sq_node;
		port = sq->sq_port;
	} else if (pcb->connected) {
		node = pcb->peer_node;
		port = pcb->peer_port;
	} else {
		sx_xunlock(&pcb->lock);
		error = EDESTADDRREQ;
		goto out;
	}
	if (pcb->port == NULL && (error = qrtr_bind_locked(pcb, 0)) != 0) {
		sx_xunlock(&pcb->lock);
		goto out;
	}
	sx_xunlock(&pcb->lock);
	len = m_length(m, NULL);
	if (len > QRTR_MAX_PACKET) {
		error = EMSGSIZE;
		goto out;
	}
	buf = malloc(MAX(len, 1), M_QRTR, M_WAITOK);
	m_copydata(m, 0, len, buf);
	/* may wait for flow control: the remote's RESUME_TX */
	error = qrtr_sendto(pcb->port, node, port, buf, len);
out:
	free(buf, M_QRTR);
	m_freem(m);
	return (error);
}

static int
qrtr_sockaddr(struct socket *so, struct sockaddr *nam)
{
	struct qrtr_pcb *pcb = so->so_pcb;
	struct sockaddr_qrtr *sq = (struct sockaddr_qrtr *)nam;
	int error = 0;

	sx_xlock(&pcb->lock);
	if (pcb->port == NULL)
		error = qrtr_bind_locked(pcb, 0);
	memset(sq, 0, sizeof(*sq));
	sq->sq_len = sizeof(*sq);
	sq->sq_family = AF_QIPCRTR;
	sq->sq_node = QRTR_LOCAL_NODE;
	sq->sq_port = pcb->port != NULL ? qrtr_port_id(pcb->port) : 0;
	sx_xunlock(&pcb->lock);
	return (error);
}

static int
qrtr_peeraddr(struct socket *so, struct sockaddr *nam)
{
	struct qrtr_pcb *pcb = so->so_pcb;
	struct sockaddr_qrtr *sq = (struct sockaddr_qrtr *)nam;

	sx_xlock(&pcb->lock);
	if (!pcb->connected) {
		sx_xunlock(&pcb->lock);
		return (ENOTCONN);
	}
	memset(sq, 0, sizeof(*sq));
	sq->sq_len = sizeof(*sq);
	sq->sq_family = AF_QIPCRTR;
	sq->sq_node = pcb->peer_node;
	sq->sq_port = pcb->peer_port;
	sx_xunlock(&pcb->lock);
	return (0);
}

static int
qrtr_shutdown(struct socket *so, enum shutdown_how how)
{
	switch (how) {
	case SHUT_RD:
		sorflush(so);
		break;
	case SHUT_RDWR:
		sorflush(so);
		/* FALLTHROUGH */
	case SHUT_WR:
		socantsendmore(so);
	}
	return (0);
}

static void
qrtr_close(struct socket *so)
{
	soisdisconnected(so);
}

static struct protosw qrtr_dgram_sw = {
	.pr_type =		SOCK_DGRAM,
	.pr_flags =		PR_ATOMIC | PR_ADDR,
	.pr_attach =		qrtr_attach,
	.pr_detach =		qrtr_detach,
	.pr_bind =		qrtr_bind,
	.pr_connect =		qrtr_connect,
	.pr_disconnect =	qrtr_disconnect,
	.pr_send =		qrtr_send,
	.pr_sockaddr =		qrtr_sockaddr,
	.pr_peeraddr =		qrtr_peeraddr,
	.pr_shutdown =		qrtr_shutdown,
	.pr_close =		qrtr_close,
	.pr_abort =		qrtr_close,
};

static struct domain qrtrdomain = {
	.dom_family =		AF_QIPCRTR,
	.dom_name =		"qrtr",
	.dom_nprotosw =		1,
	.dom_protosw =		{ &qrtr_dgram_sw },
};

DOMAIN_SET(qrtr);
