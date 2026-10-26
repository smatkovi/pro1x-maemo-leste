/*
 * Linux's <linux/qrtr.h> for programs ported to FreeBSD (freebsd/dsp): the
 * socket address of qrtr_sock.c (family 45 here) and the control packets.
 */
#ifndef _LINUX_QRTR_H
#define	_LINUX_QRTR_H

#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>
#include <linux/types.h>

#include "../../sys/arm64/qualcomm/dsp/qrtr_sock.h"

#define	QRTR_NODE_BCAST	0xffffffffu
#define	QRTR_PORT_CTRL	0xfffffffeu

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

#endif /* _LINUX_QRTR_H */
