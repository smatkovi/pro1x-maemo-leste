/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-q6 -- what FreeBSD can ask of the ADSP's audio side so far
 * (freebsd/fm/README.md, stage 3 of the FM radio):
 *
 *   pro1x-q6 core      the ADSP's core service over APR (/dev/apr,
 *                      qcom_apr.ko): ready?, its framework and services'
 *                      versions (Linux's q6core.c does the same at boot)
 *   pro1x-q6 slim      the ADSP's SLIMbus service over QMI (QRTR service
 *                      769, version 1): the controller instance selected
 *                      with the ADSP as master, the bus powered up and down
 *                      again (the first steps of Linux's qcom-ngd-ctrl.c)
 *   pro1x-q6 test      both
 *   pro1x-q6 send SVC OPCODE [HEXPAYLOAD]
 *                      one APR command to service SVC of the ADSP, the
 *                      answers for 2 s
 *
 * The packets are Linux's (include/linux/soc/qcom/apr.h, q6core.c,
 * qcom-ngd-ctrl.c); the encoding is checked on the PC by
 * freebsd/fm/test/test_q6.c.
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __FreeBSD__
#include "qrtr_sock.h"
#endif

/* --- APR (include/linux/soc/qcom/apr.h, dt-bindings/soc/qcom,apr.h) --- */
#define	APR_HDR_SIZE		20	/* sizeof(struct apr_hdr) */
#define	APR_MSG_TYPE_SEQ_CMD	0x2
#define	APR_HDR_FIELD(mt, len, ver) \
	((uint16_t)(((mt) & 0x3) << 8 | (((len) / 4) & 0xf) << 4 | ((ver) & 0xf)))
#define	APR_DOMAIN_ADSP		0x4
#define	APR_DOMAIN_APPS		0x5
#define	APR_SVC_ADSP_CORE	0x3
#define	APR_BASIC_RSP_RESULT	0x000110e8
#define	ADSP_EUNSUPPORTED	0x00000003

/* q6core.c */
#define	AVCS_CMD_ADSP_EVENT_GET_STATE	0x0001290c
#define	AVCS_CMDRSP_ADSP_EVENT_GET_STATE 0x0001290d
#define	AVCS_GET_VERSIONS		0x00012905
#define	AVCS_GET_VERSIONS_RSP		0x00012906
#define	AVCS_CMD_GET_FWK_VERSION	0x0001292c
#define	AVCS_CMDRSP_GET_FWK_VERSION	0x0001292d

/* --- QMI over QRTR: the SLIMbus service (qcom-ngd-ctrl.c) --- */
#define	SLIMBUS_QMI_SVC_ID	0x0301
#define	SLIMBUS_QMI_SVC_V1	1
#define	SLIMBUS_QMI_INS_ID	0
#define	SLIMBUS_QMI_SELECT_INSTANCE_REQ	0x0020
#define	SLIMBUS_QMI_POWER_REQ		0x0021
#define	SLIMBUS_MODE_MASTER	2	/* the ADSP the bus's manager */
#define	SLIMBUS_PM_INACTIVE	1
#define	SLIMBUS_PM_ACTIVE	2
/* the NGD of the device tree's slim@1: instance (id >> 1) */
#define	SLIMBUS_NGD_ID		1
#define	QMI_REQUEST		0x00
#define	QMI_RESPONSE		0x02
#define	QRTR_TYPE_NEW_SERVER	4
#define	QRTR_TYPE_NEW_LOOKUP	10
#define	QRTR_PORT_CTRL		0xfffffffeu

static void
put16(uint8_t *p, uint16_t v)
{
	p[0] = v & 0xff;
	p[1] = v >> 8;
}

static void
put32(uint8_t *p, uint32_t v)
{
	put16(p, v & 0xffff);
	put16(p + 2, v >> 16);
}

static uint16_t
get16(const uint8_t *p)
{
	return ((uint16_t)(p[0] | p[1] << 8));
}

static uint32_t
get32(const uint8_t *p)
{
	return ((uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16);
}

/* An APR command from the apps to an ADSP service: its header, payload */
static size_t
apr_build(uint8_t *pkt, uint8_t svc, uint32_t opcode, uint32_t token,
    const uint8_t *payload, size_t plen)
{
	size_t len = APR_HDR_SIZE + plen;

	put16(pkt, APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, APR_HDR_SIZE, 0));
	put16(pkt + 2, (uint16_t)len);
	pkt[4] = svc;			/* source service: the same */
	pkt[5] = APR_DOMAIN_APPS;
	put16(pkt + 6, 0);		/* source port */
	pkt[8] = svc;
	pkt[9] = APR_DOMAIN_ADSP;
	put16(pkt + 10, 0);		/* destination port */
	put32(pkt + 12, token);
	put32(pkt + 16, opcode);
	if (plen > 0)
		memcpy(pkt + APR_HDR_SIZE, payload, plen);
	return (len);
}

struct apr_msg {
	uint16_t	hdr_field, pkt_size;
	uint8_t		src_svc, src_domain, dest_svc, dest_domain;
	uint16_t	src_port, dest_port;
	uint32_t	token, opcode;
	const uint8_t	*payload;
	size_t		plen;
};

static int
apr_parse(const uint8_t *pkt, size_t len, struct apr_msg *m)
{
	size_t hlen;

	if (len < APR_HDR_SIZE)
		return (-1);
	m->hdr_field = get16(pkt);
	m->pkt_size = get16(pkt + 2);
	m->src_svc = pkt[4];
	m->src_domain = pkt[5];
	m->src_port = get16(pkt + 6);
	m->dest_svc = pkt[8];
	m->dest_domain = pkt[9];
	m->dest_port = get16(pkt + 10);
	m->token = get32(pkt + 12);
	m->opcode = get32(pkt + 16);
	/* the header's size in words (APR_HDR_FIELD_SIZE_BYTES) */
	hlen = ((m->hdr_field >> 4) & 0xf) * 4;
	if (hlen < APR_HDR_SIZE || hlen > len || m->pkt_size > len)
		return (-1);
	m->payload = pkt + hlen;
	m->plen = m->pkt_size - hlen;
	return (0);
}

/*
 * A QMI request: type, transaction, message id, length, then the TLVs
 * (type, 16-bit length, value) of u32 values.
 */
static size_t
qmi_build_u32s(uint8_t *msg, uint16_t txn, uint16_t msg_id, int n,
    const uint8_t *types, const uint32_t *vals)
{
	size_t off = 7;
	int i;

	for (i = 0; i < n; i++) {
		msg[off] = types[i];
		put16(msg + off + 1, 4);
		put32(msg + off + 3, vals[i]);
		off += 7;
	}
	msg[0] = QMI_REQUEST;
	put16(msg + 1, txn);
	put16(msg + 3, msg_id);
	put16(msg + 5, (uint16_t)(off - 7));
	return (off);
}

/* slimbus_select_inst_req_msg_v01: instance (TLV 1), mode (TLV 0x10) */
static size_t
qmi_select_instance(uint8_t *msg, uint16_t txn)
{
	static const uint8_t types[] = { 0x01, 0x10 };
	const uint32_t vals[] = { SLIMBUS_NGD_ID >> 1, SLIMBUS_MODE_MASTER };

	return (qmi_build_u32s(msg, txn, SLIMBUS_QMI_SELECT_INSTANCE_REQ, 2,
	    types, vals));
}

/* slimbus_power_req_msg_v01: pm_req (TLV 1), no resp_type */
static size_t
qmi_power(uint8_t *msg, uint16_t txn, bool on)
{
	static const uint8_t types[] = { 0x01 };
	const uint32_t vals[] = { on ? SLIMBUS_PM_ACTIVE : SLIMBUS_PM_INACTIVE };

	return (qmi_build_u32s(msg, txn, SLIMBUS_QMI_POWER_REQ, 1, types,
	    vals));
}

/*
 * A QMI response for txn/msg_id: its result TLV (2: result, error).
 * 0 with *result, *error set; -1 not that response.
 */
static int
qmi_parse_result(const uint8_t *msg, size_t len, uint16_t txn,
    uint16_t msg_id, uint16_t *result, uint16_t *error)
{
	size_t off, mlen;

	if (len < 7 || msg[0] != QMI_RESPONSE || get16(msg + 1) != txn ||
	    get16(msg + 3) != msg_id)
		return (-1);
	mlen = get16(msg + 5);
	if (7 + mlen > len)
		return (-1);
	for (off = 7; off + 3 <= 7 + mlen;
	    off += 3 + get16(msg + off + 1)) {
		if (msg[off] == 0x02 && get16(msg + off + 1) >= 4 &&
		    off + 7 <= len) {
			*result = get16(msg + off + 3);
			*error = get16(msg + off + 5);
			return (0);
		}
	}
	return (-1);
}

#ifndef PRO1X_Q6_NO_MAIN

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

static void
hexdump(const char *what, const uint8_t *p, size_t len)
{
	size_t i;

	printf("%s (%zu):", what, len);
	for (i = 0; i < len && i < 64; i++)
		printf(" %02x", p[i]);
	printf("%s\n", i < len ? " ..." : "");
}

/* --- APR: q6core ------------------------------------------------------------ */

static int apr_fd = -1;

static int
apr_open(void)
{
	if (apr_fd >= 0)
		return (0);
	apr_fd = open("/dev/apr", O_RDWR);
	if (apr_fd < 0) {
		printf("apr: /dev/apr: %s (kldload qcom_apr; the ADSP running "
		    "and its apr_audio_svc open: sysctl hw.qcom_apr)\n",
		    strerror(errno));
		return (-1);
	}
	return (0);
}

/* the next packet within timeout_ms: its length, 0 none, -1 error */
static ssize_t
apr_recv(uint8_t *buf, size_t size, int timeout_ms)
{
	struct pollfd pfd = { .fd = apr_fd, .events = POLLIN };
	int r;

	r = poll(&pfd, 1, timeout_ms);
	if (r <= 0)
		return (r);
	return (read(apr_fd, buf, size));
}

/* one command to the core; the answer with one of the opcodes */
static int
core_cmd(uint32_t opcode, uint32_t want, struct apr_msg *m, uint8_t *buf,
    size_t size, int timeout_ms)
{
	uint8_t pkt[APR_HDR_SIZE];
	uint64_t until;
	ssize_t n;
	size_t len;

	len = apr_build(pkt, APR_SVC_ADSP_CORE, opcode, 0, NULL, 0);
	if (write(apr_fd, pkt, len) != (ssize_t)len) {
		printf("apr: send %#x: %s\n", opcode, strerror(errno));
		return (-1);
	}
	until = now_ms() + (uint64_t)timeout_ms;
	while (now_ms() < until) {
		n = apr_recv(buf, size, (int)(until - now_ms()));
		if (n <= 0)
			break;
		if (apr_parse(buf, (size_t)n, m) != 0) {
			hexdump("apr: malformed", buf, (size_t)n);
			continue;
		}
		if (m->opcode == want)
			return (0);
		if (m->opcode == APR_BASIC_RSP_RESULT && m->plen >= 8 &&
		    get32(m->payload) == opcode)
			return (1);	/* a basic result: status in payload */
		printf("apr: from service %u: opcode %#x (%zu bytes)\n",
		    m->src_svc, m->opcode, m->plen);
	}
	return (-1);
}

static int
cmd_core(void)
{
	uint8_t buf[4096];
	struct apr_msg m;
	uint32_t i, n;
	int r, tries;

	if (apr_open() != 0)
		return (1);
	/* q6core_is_adsp_ready: up to 10 s */
	for (tries = 0; tries < 50; tries++) {
		r = core_cmd(AVCS_CMD_ADSP_EVENT_GET_STATE,
		    AVCS_CMDRSP_ADSP_EVENT_GET_STATE, &m, buf, sizeof(buf), 200);
		if (r == 0 && m.plen >= 4) {
			printf("core: ADSP state %u%s\n", get32(m.payload),
			    get32(m.payload) ? " (ready)" : "");
			if (get32(m.payload))
				break;
		} else if (r == 1) {
			printf("core: GET_STATE: status %#x (%s)\n",
			    get32(m.payload + 4), get32(m.payload + 4) ==
			    ADSP_EUNSUPPORTED ? "unsupported: taken as ready" :
			    "error");
			break;
		}
	}
	if (tries == 50) {
		printf("core: no answer from the ADSP's core service\n");
		return (1);
	}
	r = core_cmd(AVCS_CMD_GET_FWK_VERSION, AVCS_CMDRSP_GET_FWK_VERSION, &m,
	    buf, sizeof(buf), 1000);
	if (r == 0 && m.plen >= 20) {
		n = get32(m.payload + 16);
		printf("core: framework %u.%u.%u.%u, %u services\n",
		    get32(m.payload), get32(m.payload + 4),
		    get32(m.payload + 8), get32(m.payload + 12), n);
		for (i = 0; i < n && 20 + 12 * (i + 1) <= m.plen; i++)
			printf("core:   service %#x api %u branch %u\n",
			    get32(m.payload + 20 + 12 * i),
			    get32(m.payload + 24 + 12 * i),
			    get32(m.payload + 28 + 12 * i));
		return (0);
	}
	/* older ADSPs: the services' versions */
	r = core_cmd(AVCS_GET_VERSIONS, AVCS_GET_VERSIONS_RSP, &m, buf,
	    sizeof(buf), 1000);
	if (r == 0 && m.plen >= 8) {
		n = get32(m.payload + 4);
		printf("core: build %#x, %u services\n", get32(m.payload), n);
		for (i = 0; i < n && 8 + 8 * (i + 1) <= m.plen; i++)
			printf("core:   service %#x version %#x\n",
			    get32(m.payload + 8 + 8 * i),
			    get32(m.payload + 12 + 8 * i));
		return (0);
	}
	printf("core: no versions (%d)\n", r);
	return (1);
}

static int
cmd_send(int argc, char **argv)
{
	uint8_t pkt[1024], payload[1000], buf[4096];
	struct apr_msg m;
	size_t plen = 0, len;
	const char *h;
	uint64_t until;
	ssize_t n;

	if (argc < 2)
		return (2);
	for (h = argc > 2 ? argv[2] : ""; h[0] && h[1] && plen <
	    sizeof(payload); h += 2) {
		char x[3] = { h[0], h[1], 0 };

		payload[plen++] = (uint8_t)strtoul(x, NULL, 16);
	}
	if (apr_open() != 0)
		return (1);
	len = apr_build(pkt, (uint8_t)strtoul(argv[0], NULL, 0),
	    (uint32_t)strtoul(argv[1], NULL, 0), 0, payload, plen);
	hexdump("apr: sent", pkt, len);
	if (write(apr_fd, pkt, len) != (ssize_t)len) {
		printf("apr: %s\n", strerror(errno));
		return (1);
	}
	until = now_ms() + 2000;
	while (now_ms() < until &&
	    (n = apr_recv(buf, sizeof(buf), (int)(until - now_ms()))) > 0) {
		if (apr_parse(buf, (size_t)n, &m) == 0)
			printf("apr: from service %u port %u: opcode %#x token "
			    "%#x\n", m.src_svc, m.src_port, m.opcode, m.token);
		hexdump("apr: received", buf, (size_t)n);
	}
	return (0);
}

/* --- QMI: the SLIMbus service ------------------------------------------------ */

#ifdef __FreeBSD__
static int
qrtr_socket(void)
{
	struct timeval tv = { 2, 0 };
	int s;

	if ((s = socket(AF_QIPCRTR, SOCK_DGRAM, 0)) < 0) {
		printf("slim: socket(AF_QIPCRTR): %s (kldload qrtr)\n",
		    strerror(errno));
		return (-1);
	}
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return (s);
}

static struct sockaddr_qrtr
qaddr(uint32_t node, uint32_t port)
{
	struct sockaddr_qrtr sq;

	memset(&sq, 0, sizeof(sq));
	sq.sq_len = sizeof(sq);
	sq.sq_family = AF_QIPCRTR;
	sq.sq_node = node;
	sq.sq_port = port;
	return (sq);
}

/* the service's address from the name service; 0, or -1 not there */
static int
qrtr_lookup(int s, uint32_t service, uint32_t version,
    struct sockaddr_qrtr *srv)
{
	struct sockaddr_qrtr ns = qaddr(1, QRTR_PORT_CTRL), from;
	uint32_t pkt[5] = { QRTR_TYPE_NEW_LOOKUP, service, 0, 0, 0 }, rsp[5];
	socklen_t fl;
	int found = -1;

	if (sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&ns,
	    sizeof(ns)) != sizeof(pkt))
		return (-1);
	for (;;) {
		fl = sizeof(from);
		if (recvfrom(s, rsp, sizeof(rsp), 0, (struct sockaddr *)&from,
		    &fl) < (ssize_t)sizeof(rsp))
			break;
		if (rsp[0] != QRTR_TYPE_NEW_SERVER || rsp[1] == 0)
			break;		/* the end of the list */
		printf("slim: service %u instance %#x at %u:%u\n", rsp[1],
		    rsp[2], rsp[3], rsp[4]);
		/* the instance word: version | instance << 8 */
		if (rsp[1] == service && (rsp[2] & 0xff) == version &&
		    found < 0) {
			*srv = qaddr(rsp[3], rsp[4]);
			found = 0;
		}
	}
	return (found);
}

static int
qmi_call(int s, struct sockaddr_qrtr *srv, const uint8_t *req, size_t len,
    uint16_t txn, uint16_t msg_id, const char *what)
{
	uint8_t rsp[256];
	uint16_t result, error;
	ssize_t n;
	int i;

	if (sendto(s, req, len, 0, (struct sockaddr *)srv, sizeof(*srv)) !=
	    (ssize_t)len) {
		printf("slim: %s: send: %s\n", what, strerror(errno));
		return (-1);
	}
	for (i = 0; i < 4; i++) {
		n = recv(s, rsp, sizeof(rsp), 0);
		if (n < 0)
			break;
		if (qmi_parse_result(rsp, (size_t)n, txn, msg_id, &result,
		    &error) == 0) {
			printf("slim: %s: result %u error %u%s\n", what, result,
			    error, result == 0 ? " (done)" : "");
			return (result == 0 ? 0 : -1);
		}
	}
	printf("slim: %s: no answer\n", what);
	return (-1);
}

static int
cmd_slim(void)
{
	struct sockaddr_qrtr srv;
	uint8_t req[64];
	size_t len;
	int s, rc = 1;

	if ((s = qrtr_socket()) < 0)
		return (1);
	if (qrtr_lookup(s, SLIMBUS_QMI_SVC_ID, SLIMBUS_QMI_SVC_V1, &srv) != 0) {
		printf("slim: no SLIMbus service (QMI %u, version %u) on QRTR: "
		    "the NGD cannot come up\n", SLIMBUS_QMI_SVC_ID,
		    SLIMBUS_QMI_SVC_V1);
		close(s);
		return (1);
	}
	if (connect(s, (struct sockaddr *)&srv, sizeof(srv)) != 0)
		printf("slim: connect: %s\n", strerror(errno));
	len = qmi_select_instance(req, 1);
	if (qmi_call(s, &srv, req, len, 1, SLIMBUS_QMI_SELECT_INSTANCE_REQ,
	    "select instance 0, ADSP master") == 0) {
		len = qmi_power(req, 2, true);
		if (qmi_call(s, &srv, req, len, 2, SLIMBUS_QMI_POWER_REQ,
		    "power active") == 0)
			rc = 0;
		len = qmi_power(req, 3, false);
		qmi_call(s, &srv, req, len, 3, SLIMBUS_QMI_POWER_REQ,
		    "power inactive");
	}
	close(s);
	return (rc);
}
#else
static int
cmd_slim(void)
{
	printf("slim: QRTR sockets are FreeBSD's (qrtr.ko) here\n");
	return (1);
}
#endif

int
main(int argc, char **argv)
{
	int rc;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2)
		goto usage;
	if (strcmp(argv[1], "core") == 0)
		return (cmd_core());
	if (strcmp(argv[1], "slim") == 0)
		return (cmd_slim());
	if (strcmp(argv[1], "test") == 0) {
		rc = cmd_core();
		rc |= cmd_slim();
		return (rc);
	}
	if (strcmp(argv[1], "send") == 0 && (rc = cmd_send(argc - 2,
	    argv + 2)) != 2)
		return (rc);
usage:
	fprintf(stderr, "usage: pro1x-q6 core | slim | test | "
	    "send SVC OPCODE [HEXPAYLOAD]\n");
	return (2);
}
#endif /* PRO1X_Q6_NO_MAIN */
