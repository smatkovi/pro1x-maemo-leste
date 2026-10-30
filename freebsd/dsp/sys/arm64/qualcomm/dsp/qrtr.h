/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * QRTR, Qualcomm's IPC router (Linux's net/qrtr): every processor is a
 * node with numbered ports; packets carry source and destination. This
 * node is 1. The name service on port CTRL (0xfffffffe) keeps the list of
 * servers (service, instance, node, port) the nodes announce and answers
 * lookups, as Linux's in-kernel ns.c.
 *
 * qrtr_core.c is plain C over glink_os.h, like glink_core.c; qrtr_sock.c
 * makes it a socket family (AF_QIPCRTR) and qrtr_glink.c the endpoint over
 * the DSPs' GLINK channel "IPCRTR".
 */

#ifndef _QRTR_H_
#define	_QRTR_H_

#include "glink_os.h"

#define	QRTR_NODE_BCAST		0xffffffffu
#define	QRTR_PORT_CTRL		0xfffffffeu
#define	QRTR_LOCAL_NODE		1

enum qrtr_pkt_type {
	QRTR_TYPE_DATA		= 1,
	QRTR_TYPE_HELLO		= 2,
	QRTR_TYPE_BYE		= 3,
	QRTR_TYPE_NEW_SERVER	= 4,
	QRTR_TYPE_DEL_SERVER	= 5,
	QRTR_TYPE_DEL_CLIENT	= 6,
	QRTR_TYPE_RESUME_TX	= 7,
	QRTR_TYPE_EXIT		= 8,
	QRTR_TYPE_PING		= 9,
	QRTR_TYPE_NEW_LOOKUP	= 10,
	QRTR_TYPE_DEL_LOOKUP	= 11,
};

/* The control packets' payload (little endian, as on the processors) */
struct qrtr_ctrl_pkt {
	uint32_t	cmd;
	union {
		struct {
			uint32_t service;
			uint32_t instance;
			uint32_t node;
			uint32_t port;
		} server;
		struct {
			uint32_t node;
			uint32_t port;
		} client;
	};
} __attribute__((__packed__));

struct qrtr_port;
struct qrtr_node;

struct qrtr_rx_info {
	uint32_t	src_node, src_port;
	int		type;
};

/*
 * A packet for a local port, in the router's receive path: it must not
 * wait (a socket appends it to its buffer), and the data is gone after.
 */
typedef void qrtr_rx_cb_t(void *arg, const struct qrtr_rx_info *info,
	    const void *data, size_t len);
/* Hand a packet to an endpoint; it must not wait (queue it) */
typedef int qrtr_xmit_t(void *arg, const void *pkt, size_t len);

int	qrtr_init(void);
void	qrtr_fini(void);

/* port 0: a free one (0x4000-0x7fff) */
int	qrtr_port_open(uint32_t port, qrtr_rx_cb_t *cb, void *arg,
	    struct qrtr_port **pp);
uint32_t qrtr_port_id(struct qrtr_port *p);
/* to the name service (port CTRL): the type is the payload's cmd */
int	qrtr_sendto(struct qrtr_port *p, uint32_t node, uint32_t port,
	    const void *data, size_t len);
void	qrtr_port_close(struct qrtr_port *p);

struct qrtr_node *qrtr_endpoint_register(qrtr_xmit_t *xmit, void *arg);
/* A packet from the endpoint, with its QRTR header (v1 or v2) */
int	qrtr_endpoint_post(struct qrtr_node *n, const void *data, size_t len);
void	qrtr_endpoint_unregister(struct qrtr_node *n);
/* A HELLO from our name service over this endpoint (a node started again) */
void	qrtr_endpoint_hello(struct qrtr_node *n);

/* For the test runs: the servers the name service knows */
typedef void qrtr_server_cb_t(void *arg, uint32_t service, uint32_t instance,
	    uint32_t node, uint32_t port);
void	qrtr_ns_servers(qrtr_server_cb_t *cb, void *arg);
/* the endpoints: their first node id (QRTR_NODE_BCAST: none seen yet),
 * the node ids behind them, packets in and out, HELLOs seen */
struct qrtr_node_info {
	uint32_t	nid;
	uint32_t	nids[8];
	int		nnids;
	u_int		rx, tx, hellos;
	bool		gone;
};
typedef void qrtr_node_cb_t(void *arg, const struct qrtr_node_info *ni);
void	qrtr_nodes(qrtr_node_cb_t *cb, void *arg);

#ifdef _KERNEL
/* The endpoints over GLINK's IPCRTR channel (qrtr_glink.c) */
void	qrtr_glink_init(void);
void	qrtr_glink_fini(void);
#endif

#endif /* _QRTR_H_ */
