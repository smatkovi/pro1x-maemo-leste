/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Requests to the RPM (the SoC's resource power manager: the PMIC's
 * regulators, clocks, power domains) over its GLINK edge in the RPM's
 * message RAM, as Linux's qcom_glink_rpm.c, qcom_glink_native.c and
 * smd-rpm.c send them (freebsd/dsp/README.md). Plain C without kernel
 * dependencies: rpm_core.c runs in the kernel (qcom_rpm.c) and in the
 * host test (test/rpm_test.c).
 *
 * The message RAM ends in a table of contents ("grt0") naming two FIFOs,
 * "ap2r" (to the RPM) and "r2ap" (from it), each a tail word, a head word
 * and the data. The writer moves the head, the reader the tail. GLINK
 * commands in them are 8-byte aligned; the RPM's edge has no intents, data
 * goes as TX_DATA (cmd 9, the sender's channel id, intent 0, the chunk's
 * size, what is left). An SMD-RPM request: "req\0", its length, then the
 * message id, the state (0: active set), the resource type and id, the
 * size of the key/value pairs and the pairs (key, 4, value). The answer:
 * "req\0", then "msg#" with the id, and "err\0" with a text if refused.
 *
 * After a kexec the channel "rpm_requests" is still open on the RPM's side
 * with the id Linux gave it (Linux's GLINK has no shutdown hook, and the
 * RPM does not notice a new kernel). rpm_edge_init finds that id in the
 * requests Linux left in the transmit FIFO and sends on it; no OPEN.
 */

#ifndef _RPM_H_
#define	_RPM_H_

#include <sys/types.h>
#ifndef _KERNEL
#include <stdbool.h>
#include <stdint.h>
#endif

#define	RPM_TOC_SIZE		256
#define	RPM_TOC_MAGIC		0x67727430u	/* "grt0" */
#define	RPM_TX_FIFO_ID		0x61703272u	/* "ap2r" */
#define	RPM_RX_FIFO_ID		0x72326170u	/* "r2ap" */

#define	RPM_GLINK_CMD_OPEN	2
#define	RPM_GLINK_CMD_INTENT	5
#define	RPM_GLINK_CMD_TX_DATA	9
#define	RPM_GLINK_CMD_TX_DATA_CONT 12

#define	RPM_SERVICE_TYPE_REQUEST 0x00716572u	/* "req\0" */
#define	RPM_MSG_TYPE_ERR	0x00727265u	/* "err\0" */
#define	RPM_MSG_TYPE_MSG_ID	0x2367736du	/* "msg#" */

#define	RPM_ACTIVE_STATE	0
#define	RPM_SLEEP_STATE		1

/* resource types and keys (Linux's smd-rpm.h, qcom_smd-regulator.c) */
#define	RPM_RES_SMPA		0x61706d73u	/* "smpa" */
#define	RPM_RES_LDOA		0x616f646cu	/* "ldoa" */
#define	RPM_KEY_SWEN		0x6e657773u	/* "swen": on */
#define	RPM_KEY_UV		0x00007675u	/* "uv": microvolts */
#define	RPM_KEY_MA		0x0000616du	/* "ma": the load */

/* Linux: SMD packets to the RPM may not exceed 256 bytes */
#define	RPM_MAX_PAIRS		8

struct rpm_fifo {
	uint32_t	off;		/* its tail word in the message RAM */
	uint32_t	len;		/* bytes of data */
};

struct rpm_edge {
	volatile uint32_t *ram;		/* the message RAM: 32-bit accesses */
	uint32_t	size;
	struct rpm_fifo	tx, rx;
	uint16_t	lcid;		/* the channel's id, Linux's */
	u_int		lcid_seen;	/* Linux's requests that showed it */
	uint32_t	msg_id;		/* the last one sent */
	/* the last answer's refusal, if any */
	char		err[32];
};

/*
 * The table of contents and the FIFOs of ram (size bytes), the channel id
 * (lcid 0: found in the transmit FIFO; else this one), what the RPM sent
 * before dropped. 0, or an errno: ENXIO without the table or the FIFOs.
 */
int	rpm_edge_init(struct rpm_edge *e, volatile void *ram, uint32_t size,
	    uint16_t lcid);

/*
 * A request into the transmit FIFO (the caller kicks the RPM): state,
 * the resource, npairs key/value pairs (kv: key, value, key, value, ...).
 * 0 with *msg_id set, ENOSPC without room, EINVAL for too many pairs.
 */
int	rpm_send(struct rpm_edge *e, uint32_t state, uint32_t type,
	    uint32_t id, const uint32_t *kv, int npairs, uint32_t *msg_id);

/*
 * What the RPM answered to msg_id, from the receive FIFO (answers to other
 * ids are passed over): 0 nothing yet, 1 acknowledged, -ENXIO refused as
 * "resource does not exist", -EINVAL refused otherwise (e->err the text).
 */
int	rpm_poll(struct rpm_edge *e, uint32_t msg_id);

#endif /* _RPM_H_ */
