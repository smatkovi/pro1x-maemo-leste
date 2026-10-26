/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The QRTR socket address on FreeBSD (qrtr_sock.c), for the kernel and for
 * programs (include/linux/qrtr.h of the ported tools). Linux's AF_QIPCRTR is
 * 42, which FreeBSD has as AF_INET6_SDP: here 45. sq_node and sq_port are at
 * the same offsets as in Linux's struct, after BSD's length and family.
 */

#ifndef _QRTR_SOCK_H_
#define	_QRTR_SOCK_H_

#include <sys/types.h>

#define	AF_QIPCRTR	45
#define	PF_QIPCRTR	AF_QIPCRTR

struct sockaddr_qrtr {
	uint8_t		sq_len;
	uint8_t		sq_family;	/* sa_family_t */
	uint16_t	sq_pad;
	uint32_t	sq_node;
	uint32_t	sq_port;
};

#endif /* _QRTR_SOCK_H_ */
