/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pro1x-q6's packets on the PC (freebsd/dsp/tools/pro1x-q6): the APR
 * header against Linux's struct apr_hdr filled as q6core.c and
 * apr_send_pkt fill it, an answer parsed, the SLIMbus QMI requests in the
 * TLVs of qcom-ngd-ctrl.c's element tables (and their maximum lengths),
 * a response's result.
 */
#include <stdio.h>
#include <string.h>

#define	PRO1X_Q6_NO_MAIN
#include "../../dsp/tools/pro1x-q6/pro1x-q6.c"

/* include/linux/soc/qcom/apr.h */
struct linux_apr_hdr {
	uint16_t hdr_field;
	uint16_t pkt_size;
	uint8_t src_svc;
	uint8_t src_domain;
	uint16_t src_port;
	uint8_t dest_svc;
	uint8_t dest_domain;
	uint16_t dest_port;
	uint32_t token;
	uint32_t opcode;
} __attribute__((packed));

static int failures;
#define	CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); \
	failures++; } } while (0)

int
main(void)
{
	struct linux_apr_hdr h;
	uint8_t pkt[64], rsp[64], msg[64];
	struct apr_msg m;
	uint16_t result, error;
	size_t len;

	/* q6core: __q6core_is_adsp_ready, apr_send_pkt */
	memset(&h, 0, sizeof(h));
	h.hdr_field = (APR_MSG_TYPE_SEQ_CMD & 0x3) << 8 |
	    ((sizeof(h) / 4) & 0xf) << 4 | 0;
	h.pkt_size = sizeof(h);
	h.opcode = AVCS_CMD_ADSP_EVENT_GET_STATE;
	h.src_domain = APR_DOMAIN_APPS;
	h.src_svc = APR_SVC_ADSP_CORE;
	h.dest_domain = APR_DOMAIN_ADSP;
	h.dest_svc = APR_SVC_ADSP_CORE;
	CHECK(sizeof(h) == APR_HDR_SIZE);
	len = apr_build(pkt, APR_SVC_ADSP_CORE, AVCS_CMD_ADSP_EVENT_GET_STATE,
	    0, NULL, 0);
	CHECK(len == sizeof(h) && memcmp(pkt, &h, sizeof(h)) == 0);
	printf("GET_STATE:");
	for (size_t i = 0; i < len; i++)
		printf(" %02x", pkt[i]);
	printf("\n");

	/* the answer: AVCS_CMDRSP_GET_FWK_VERSION with one service */
	memset(&h, 0, sizeof(h));
	h.hdr_field = 0x0150;		/* CMD_RSP, 5 words */
	h.pkt_size = sizeof(h) + 20 + 12;
	h.src_svc = 3;
	h.src_domain = 4;
	h.dest_svc = 3;
	h.dest_domain = 5;
	h.opcode = AVCS_CMDRSP_GET_FWK_VERSION;
	memcpy(rsp, &h, sizeof(h));
	memset(rsp + sizeof(h), 0, 32);
	put32(rsp + sizeof(h), 2);		/* major */
	put32(rsp + sizeof(h) + 16, 1);		/* one service */
	put32(rsp + sizeof(h) + 20, 0x4);	/* AFE */
	CHECK(apr_parse(rsp, h.pkt_size, &m) == 0);
	CHECK(m.opcode == AVCS_CMDRSP_GET_FWK_VERSION && m.plen == 32 &&
	    get32(m.payload) == 2 && get32(m.payload + 16) == 1 &&
	    get32(m.payload + 20) == 4 && m.src_domain == 4);
	CHECK(apr_parse(rsp, 10, &m) == -1);
	rsp[2] = 200;				/* pkt_size beyond the data */
	CHECK(apr_parse(rsp, sizeof(h) + 32, &m) == -1);

	/* SLIMbus: select instance (instance 0, master), 14 bytes of TLVs */
	len = qmi_select_instance(msg, 1);
	CHECK(len == 7 + 14);
	CHECK(memcmp(msg, "\x00\x01\x00\x20\x00\x0e\x00"
	    "\x01\x04\x00\x00\x00\x00\x00" "\x10\x04\x00\x02\x00\x00\x00",
	    len) == 0);
	/* power: pm_req only (resp_type not valid) */
	len = qmi_power(msg, 2, true);
	CHECK(len == 7 + 7 && msg[3] == 0x21 && msg[7] == 0x01 &&
	    get32(msg + 10) == SLIMBUS_PM_ACTIVE);
	len = qmi_power(msg, 3, false);
	CHECK(get32(msg + 10) == SLIMBUS_PM_INACTIVE);
	/* a response: result 0, error 0 (qmi_response_type_v01) */
	memcpy(msg, "\x02\x01\x00\x20\x00\x07\x00" "\x02\x04\x00\x00\x00\x00\x00",
	    14);
	CHECK(qmi_parse_result(msg, 14, 1, 0x20, &result, &error) == 0 &&
	    result == 0 && error == 0);
	msg[10] = 1;		/* result failure, error 0x2a */
	msg[12] = 0x2a;
	CHECK(qmi_parse_result(msg, 14, 1, 0x20, &result, &error) == 0 &&
	    result == 1 && error == 0x2a);
	CHECK(qmi_parse_result(msg, 14, 2, 0x20, &result, &error) == -1);
	printf("%s: %d failures\n", failures ? "FAIL" : "ok", failures);
	return (failures != 0);
}
