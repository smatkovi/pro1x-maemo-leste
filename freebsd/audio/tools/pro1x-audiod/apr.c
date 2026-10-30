/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * APR for pro1x-audiod: headers as Linux's apr_send_pkt completes them,
 * transactions that keep serving the packets nobody waits for (the ASM
 * data events of a running stream arrive while the back end of another
 * is set up), and the dispatcher.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "audiod.h"

#define	APR_MSG_TYPE_SEQ_CMD	0x2
#define	APR_HDR_FIELD(mt, len, ver) \
	((uint16_t)(((mt) & 0x3) << 8 | (((len) / 4) & 0xf) << 4 | ((ver) & 0xf)))

/* struct apr_hdr: the same service on both ends, apps to ADSP */
size_t
apr_hdr_port(uint8_t *pkt, uint8_t svc, uint16_t src_port, uint16_t dest_port,
    uint32_t opcode, uint32_t token, size_t len)
{
	le16enc_(pkt, APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, APR_HDR_SIZE, 0));
	le16enc_(pkt + 2, (uint16_t)len);
	pkt[4] = svc;
	pkt[5] = APR_DOMAIN_APPS;
	le16enc_(pkt + 6, src_port);
	pkt[8] = svc;
	pkt[9] = APR_DOMAIN_ADSP;
	le16enc_(pkt + 10, dest_port);
	le32enc_(pkt + 12, token);
	le32enc_(pkt + 16, opcode);
	return (APR_HDR_SIZE);
}

size_t
apr_hlen(const uint8_t *p, size_t n)
{
	size_t h;

	if (n < APR_HDR_SIZE)
		return (0);
	h = ((le16dec_(p) >> 4) & 0xf) * 4;
	return (h < APR_HDR_SIZE || h > n ? 0 : h);
}

const char *
q6_err(int r)
{
	static char buf[64];

	if (r == 0)
		return ("done");
	if (r > 0)
		snprintf(buf, sizeof(buf), "ADSP status %#x", (unsigned)r);
	else
		snprintf(buf, sizeof(buf), "%s", strerror(-r));
	return (buf);
}

int
apr_send(struct q6 *q, const uint8_t *pkt, size_t len)
{
	if (q->io->send(q->io->ctx, pkt, len) != 0)
		return (-EIO);
	q->sent++;
	return (0);
}

static void
dispatch(struct q6 *q, const uint8_t *buf, size_t n)
{
	q->async_pkts++;
	if (q->async != NULL)
		q->async(q, buf, n);
	else
		fma_dbg("apr: from service %u: opcode %#x token %#x (%zu bytes)",
		    buf[4], apr_opcode(buf), apr_token(buf), n);
}

int
apr_poll(struct q6 *q, int timeout_ms)
{
	uint8_t buf[APR_PKT_MAX];
	int n, handled = 0;

	for (;;) {
		n = q->io->recv(q->io->ctx, buf, sizeof(buf), timeout_ms);
		if (n < 0)
			return (-EIO);
		if (n == 0)
			return (handled);
		q->received++;
		if (apr_hlen(buf, (size_t)n) != 0)
			dispatch(q, buf, (size_t)n);
		handled++;
		timeout_ms = 0;
	}
}

int
apr_txn(struct q6 *q, const uint8_t *pkt, size_t len, uint32_t want,
    uint8_t *rsp, size_t *rsplen)
{
	uint8_t buf[APR_PKT_MAX];
	uint32_t opcode = apr_opcode(pkt), token = apr_token(pkt);
	uint8_t svc = pkt[8];
	uint64_t until;
	size_t hlen;
	int n;
	bool basic_ok = false;

	if (apr_send(q, pkt, len) != 0)
		return (-EIO);
	until = fma_now_ms() + (uint64_t)q->timeout_ms;
	for (;;) {
		uint64_t now = fma_now_ms();

		if (now >= until)
			break;
		n = q->io->recv(q->io->ctx, buf, sizeof(buf), (int)(until - now));
		if (n < 0)
			return (-EIO);
		if (n == 0)
			break;
		q->received++;
		if ((hlen = apr_hlen(buf, (size_t)n)) == 0)
			continue;
		if (apr_opcode(buf) == APR_BASIC_RSP_RESULT &&
		    (size_t)n >= hlen + 8 && apr_src_svc(buf) == svc &&
		    le32dec_(buf + hlen) == opcode && apr_token(buf) == token) {
			q->last_status = le32dec_(buf + hlen + 4);
			if (want == 0 || q->last_status != 0) {
				if (rsp != NULL) {
					memcpy(rsp, buf, (size_t)n);
					*rsplen = (size_t)n;
				}
				if (q->last_status != 0)
					q->refused++;
				return ((int)q->last_status);
			}
			basic_ok = true;
			continue;
		}
		if (want != 0 && apr_opcode(buf) == want &&
		    apr_src_svc(buf) == svc) {
			if (rsp != NULL) {
				memcpy(rsp, buf, (size_t)n);
				*rsplen = (size_t)n;
			}
			return (0);
		}
		dispatch(q, buf, (size_t)n);
	}
	q->timeouts++;
	return (basic_ok ? -ENOMSG : -ETIMEDOUT);
}
