/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * GLINK, the channel protocol to the Qualcomm DSPs (Linux's
 * qcom_glink_native.c): the protocol core (glink_core.c) over two byte
 * FIFOs, one per direction (qcom_glink.c puts them in SMEM). It is plain C
 * over a small OS layer (glink_os.h), so that the same file runs in the
 * kernel and in a test on the PC (freebsd/dsp/test).
 *
 * A channel is named; it is open when both sides have opened it and each
 * acknowledged the other's open. Data goes into buffers the receiver
 * offers ("intents"): the sender picks one big enough or asks for one. The
 * receive callback runs in the edge's receive context and must not wait for
 * the edge (no glink_send from it): it copies what it needs.
 */

#ifndef _GLINK_H_
#define	_GLINK_H_

#include "glink_os.h"

struct glink_edge;
struct glink_channel;

/* One direction of the transport; offsets wrap at length */
struct glink_fifo_ops {
	size_t	(*rx_avail)(void *arg);
	void	(*rx_peek)(void *arg, void *buf, size_t off, size_t len);
	void	(*rx_advance)(void *arg, size_t len);
	size_t	(*tx_avail)(void *arg);
	/* hdr then data, the head rounded up to 8 bytes, then published */
	void	(*tx_write)(void *arg, const void *hdr, size_t hlen,
		    const void *data, size_t dlen);
	void	(*tx_kick)(void *arg);
	size_t	(*tx_length)(void *arg);
};

typedef void glink_rx_cb_t(void *arg, const void *data, size_t len);
/* up: the channel is open in both directions; down: the remote closed it */
typedef void glink_state_cb_t(void *arg, bool up);
/* A channel the remote opened that nobody here has asked for (yet) */
typedef void glink_announce_cb_t(void *arg, const char *name);

struct glink_edge *glink_edge_create(const char *label,
	    const struct glink_fifo_ops *ops, void *ops_arg,
	    glink_announce_cb_t *announce, void *announce_arg);
/* The remote side's messages: from the edge's interrupt, in a task */
void	glink_edge_rx(struct glink_edge *edge);
/*
 * The remote is gone (stopped, crashed): every waiter fails, the users get
 * their state callback (down) with the edge locked, so they must not call
 * back into GLINK from it; they glink_close their channels later.
 */
void	glink_edge_abort(struct glink_edge *edge);
/* After the abort: freed now, or when the last user closes its channel */
void	glink_edge_destroy(struct glink_edge *edge);
const char *glink_edge_label(struct glink_edge *edge);

/*
 * Open NAME: waits up to timeout_ms for both opens. nintents intents of
 * intent_size bytes are offered for receiving (Linux's default: 5 of 1K).
 */
int	glink_open(struct glink_edge *edge, const char *name, int nintents,
	    size_t intent_size, glink_rx_cb_t *rx, glink_state_cb_t *state,
	    void *arg, u_int timeout_ms, struct glink_channel **chp);
int	glink_send(struct glink_channel *ch, const void *data, size_t len);
void	glink_close(struct glink_channel *ch);
const char *glink_channel_name(struct glink_channel *ch);

#endif /* _GLINK_H_ */
