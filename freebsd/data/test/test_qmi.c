/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ipa_qmi.c on the PC, on freebsd/dsp's qmi_core.c and qrtr_core.c (as
 * dsp/test/qmi_test.c runs them): the test plays the modem, node 5, with
 * its IPA service (0x31 instance 2) on port 7 and a client on port 8 of our
 * IPA service (0x31 instance 1). Its messages are encoded and decoded with
 * Linux 7.1's own tables (drivers/net/ipa/ipa_qmi_msg.c, compiled from the
 * Linux tree under other names), and the INIT_DRIVER request's bytes are
 * compared with what Linux's tables make of the same values; the values
 * against ipa_data-v4.2.c's memory layout, worked out by hand.
 *
 * The handshake of a first boot (INDICATION_REGISTER, INIT_DRIVER and its
 * response, DRIVER_INIT_COMPLETE, then INIT_COMPLETE and the data path's
 * start), the modem's BYE (its side reset), and a second boot (the uC
 * loaded: skip_uc_load, no second indication needed).
 */
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/queue.h>

#include "ipa_var.h"
#include "qrtr.h"
#include "qrtr_sock.h"
#include "qcom_qmi.h"
#include "ipa_qmi_msg.h"

/* Linux's tables, renamed at compile time */
extern const struct qmi_elem_info lx_ipa_indication_register_req_ei[];
extern const struct qmi_elem_info lx_ipa_indication_register_rsp_ei[];
extern const struct qmi_elem_info lx_ipa_driver_init_complete_req_ei[];
extern const struct qmi_elem_info lx_ipa_driver_init_complete_rsp_ei[];
extern const struct qmi_elem_info lx_ipa_init_complete_ind_ei[];
extern const struct qmi_elem_info lx_ipa_init_modem_driver_req_ei[];
extern const struct qmi_elem_info lx_ipa_init_modem_driver_rsp_ei[];

struct hdr {
	uint32_t	version, type, src_node, src_port, confirm_rx, size,
			dst_node, dst_port;
};

/* --- node 5 ------------------------------------------------------------------- */

struct sent {
	TAILQ_ENTRY(sent) link;
	struct hdr	h;
	uint8_t		*data;
};
static TAILQ_HEAD(, sent) sentq = TAILQ_HEAD_INITIALIZER(sentq);
static pthread_mutex_t sent_lock = PTHREAD_MUTEX_INITIALIZER;
static struct qrtr_node *node5;
static uint32_t our_server_port;

static int
xmit(void *arg, const void *pkt, size_t len)
{
	struct sent *s = calloc(1, sizeof(*s));
	const struct qrtr_ctrl_pkt *c;

	memcpy(&s->h, pkt, sizeof(s->h));
	s->data = malloc(s->h.size + 1);
	memcpy(s->data, (const char *)pkt + sizeof(s->h), s->h.size);
	/* our IPA service's port, from the name service's announcement */
	if (s->h.type == QRTR_TYPE_NEW_SERVER && s->h.size >= sizeof(*c)) {
		c = (const void *)s->data;
		if (c->server.service == 0x31 && c->server.instance == (1 | 1 << 8))
			our_server_port = c->server.port;
	}
	pthread_mutex_lock(&sent_lock);
	TAILQ_INSERT_TAIL(&sentq, s, link);
	pthread_mutex_unlock(&sent_lock);
	return (0);
}

static struct sent *
next_data(void)
{
	struct sent *s;
	int i;

	for (i = 0; i < 1000; i++) {
		pthread_mutex_lock(&sent_lock);
		while ((s = TAILQ_FIRST(&sentq)) != NULL && s->h.type != QRTR_TYPE_DATA) {
			TAILQ_REMOVE(&sentq, s, link);
			free(s->data);
			free(s);
		}
		if (s != NULL)
			TAILQ_REMOVE(&sentq, s, link);
		pthread_mutex_unlock(&sent_lock);
		if (s != NULL)
			return (s);
		usleep(5000);
	}
	assert(!"no data packet to node 5");
	return (NULL);
}

static void
free_sent(struct sent *s)
{
	free(s->data);
	free(s);
}

static void
from5(int type, uint32_t src_port, uint32_t dst_port, const void *data,
    size_t len)
{
	size_t n = sizeof(struct hdr) + roundup2(len, 4);
	uint8_t *buf = calloc(1, n);
	struct hdr h = { 1, type, 5, src_port, 0, len, QRTR_LOCAL_NODE, dst_port };

	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), data, len);
	assert(qrtr_endpoint_post(node5, buf, n) == 0);
	free(buf);
}

static void
ctrl5(uint32_t cmd, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	struct qrtr_ctrl_pkt p = { cmd, { { a, b, c, d } } };

	from5(cmd, QRTR_PORT_CTRL, QRTR_PORT_CTRL, &p, sizeof(p));
}

static void
msg5(uint32_t src_port, uint32_t dst_port, int type, int msg_id,
    unsigned int txn, size_t max, const struct qmi_elem_info *ei,
    const void *c_struct)
{
	size_t len = max;
	void *m = qmi_encode_message(type, msg_id, &len, txn, ei, c_struct);

	assert(m != NULL);
	from5(QRTR_TYPE_DATA, src_port, dst_port, m, len);
	free(m);
}

static const struct qmi_header *
qh(const struct sent *s)
{
	return ((const struct qmi_header *)s->data);
}

/* --- the core's side, as much as ipa_qmi.c uses ----------------------------------- */

static struct ipa IPA;
static struct ipa_endpoint modem_rx = { .endpoint_id = 9 };
static int modem_starts, modem_crashes, verbose;

/* ipa_data-v4.2.c's regions (id: offset, size) */
static const struct ipa_mem mem42[IPA_MEM_COUNT] = {
	[IPA_MEM_UC_SHARED] = { 0x0000, 0x0080, 0 },
	[IPA_MEM_UC_INFO] = { 0x0080, 0x0200, 0 },
	[IPA_MEM_V4_FILTER_HASHED] = { 0x0288, 0, 2 },
	[IPA_MEM_V4_FILTER] = { 0x0290, 0x0078, 2 },
	[IPA_MEM_V6_FILTER_HASHED] = { 0x0310, 0, 2 },
	[IPA_MEM_V6_FILTER] = { 0x0318, 0x0078, 2 },
	[IPA_MEM_V4_ROUTE_HASHED] = { 0x0398, 0, 2 },
	[IPA_MEM_V4_ROUTE] = { 0x03a0, 0x0078, 2 },
	[IPA_MEM_V6_ROUTE_HASHED] = { 0x0420, 0, 2 },
	[IPA_MEM_V6_ROUTE] = { 0x0428, 0x0078, 2 },
	[IPA_MEM_MODEM_HEADER] = { 0x04a8, 0x0140, 2 },
	[IPA_MEM_MODEM_PROC_CTX] = { 0x05f0, 0x0200, 2 },
	[IPA_MEM_AP_PROC_CTX] = { 0x07f0, 0x0200, 0 },
	[IPA_MEM_PDN_CONFIG] = { 0x09f8, 0x0050, 2 },
	[IPA_MEM_STATS_QUOTA_MODEM] = { 0x0a50, 0x0060, 2 },
	[IPA_MEM_STATS_TETHERING] = { 0x0ab0, 0x0140, 0 },
	[IPA_MEM_MODEM] = { 0x0bf0, 0x140c, 0 },
	[IPA_MEM_END_MARKER] = { 0x2000, 0, 1 },
};

const struct ipa_mem *
ipa_mem_find(struct ipa *ipa, enum ipa_mem_id id)
{
	return (&mem42[id]);
}

int
ipa_modem_start(struct ipa *ipa)
{
	modem_starts++;
	ipa->modem_state = IPA_MODEM_STATE_RUNNING;
	return (0);
}

void
ipa_modem_crashed(struct ipa *ipa)
{
	modem_crashes++;
	ipa->modem_state = IPA_MODEM_STATE_STOPPED;
}

void
ipa_os_log(struct ipa *ipa, const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	va_start(ap, fmt);
	printf("    ipa: ");
	vprintf(fmt, ap);
	va_end(ap);
}

struct work {
	struct ipa	*ipa;
	enum ipa_work	w;
};

static void *
work_thread(void *arg)
{
	struct work *w = arg;

	ipa_work_run(w->ipa, w->w);
	free(w);
	return (NULL);
}

/* the driver's own thread: here one per work */
void
ipa_os_schedule(struct ipa *ipa, enum ipa_work wk)
{
	struct work *w = malloc(sizeof(*w));
	pthread_t t;

	w->ipa = ipa;
	w->w = wk;
	assert(pthread_create(&t, NULL, work_thread, w) == 0);
	pthread_detach(t);
}

static void
wait_int(int *v, int want)
{
	int i;

	for (i = 0; i < 400 && __atomic_load_n(v, __ATOMIC_SEQ_CST) < want; i++)
		usleep(5000);
	assert(*v >= want);
}

/* --- the test -------------------------------------------------------------------------- */

static void
check_init_driver(const struct sent *s, uint8_t skip_uc)
{
	struct ipa_init_modem_driver_req r;
	size_t len;
	void *lx;

	assert(qh(s)->type == QMI_REQUEST && qh(s)->msg_id == IPA_QMI_INIT_DRIVER);
	assert(s->h.dst_node == 5 && s->h.dst_port == 7);
	memset(&r, 0, sizeof(r));
	assert(qmi_decode_message(s->data, s->h.size, lx_ipa_init_modem_driver_req_ei,
	    &r) >= 0);
	assert(r.platform_type_valid && r.platform_type == 3);
	assert(r.hdr_tbl_info_valid && r.hdr_tbl_info.start == 0x4a8 &&
	    r.hdr_tbl_info.end == 0x4a8 + 0x140 - 1);
	assert(r.v4_route_tbl_info_valid && r.v4_route_tbl_info.start == 0x3a0 &&
	    r.v4_route_tbl_info.end == 7);
	assert(r.v6_route_tbl_info_valid && r.v6_route_tbl_info.start == 0x428 &&
	    r.v6_route_tbl_info.end == 7);
	assert(r.v4_filter_tbl_start_valid && r.v4_filter_tbl_start == 0x290);
	assert(r.v6_filter_tbl_start_valid && r.v6_filter_tbl_start == 0x318);
	assert(r.modem_mem_info_valid && r.modem_mem_info.start == 0xbf0 &&
	    r.modem_mem_info.size == 0x140c);
	assert(r.ctrl_comm_dest_end_pt_valid && r.ctrl_comm_dest_end_pt == 9);
	assert(r.skip_uc_load_valid && r.skip_uc_load == skip_uc);
	assert(r.hdr_proc_ctx_tbl_info_valid &&
	    r.hdr_proc_ctx_tbl_info.start == 0x5f0 &&
	    r.hdr_proc_ctx_tbl_info.end == 0x5f0 + 0x200 - 1);
	assert(!r.zip_tbl_info_valid && !r.v4_hash_route_tbl_info_valid &&
	    !r.v6_hash_route_tbl_info_valid && !r.v4_hash_filter_tbl_start_valid &&
	    !r.v6_hash_filter_tbl_start_valid);
	assert(r.hw_stats_quota_base_addr_valid && r.hw_stats_quota_base_addr ==
	    0xa50 && r.hw_stats_quota_size_valid && r.hw_stats_quota_size == 0x60);
	assert(!r.hw_stats_drop_base_addr_valid && !r.hw_stats_drop_size_valid);
	/* byte for byte what Linux's tables make of it */
	len = IPA_QMI_INIT_DRIVER_REQ_SZ;
	lx = qmi_encode_message(QMI_REQUEST, IPA_QMI_INIT_DRIVER, &len,
	    qh(s)->txn_id, lx_ipa_init_modem_driver_req_ei, &r);
	assert(lx != NULL && len == s->h.size && memcmp(lx, s->data, len) == 0);
	free(lx);
}

static void
reply_init_driver(const struct sent *req)
{
	struct ipa_init_modem_driver_rsp rsp;

	memset(&rsp, 0, sizeof(rsp));
	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.ctrl_comm_dest_end_pt_valid = 1;
	rsp.ctrl_comm_dest_end_pt = 3;
	rsp.default_end_pt_valid = 1;
	rsp.default_end_pt = 4;
	msg5(7, req->h.src_port, QMI_RESPONSE, IPA_QMI_INIT_DRIVER,
	    qh(req)->txn_id, IPA_QMI_INIT_DRIVER_RSP_SZ,
	    lx_ipa_init_modem_driver_rsp_ei, &rsp);
}

static void
expect_response(int msg_id, unsigned int txn)
{
	struct ipa_indication_register_rsp rsp;
	struct sent *s = next_data();

	assert(qh(s)->type == QMI_RESPONSE && qh(s)->msg_id == msg_id &&
	    qh(s)->txn_id == txn && s->h.dst_port == 8);
	memset(&rsp, 0, sizeof(rsp));
	assert(qmi_decode_message(s->data, s->h.size,
	    lx_ipa_indication_register_rsp_ei, &rsp) >= 0);
	assert(rsp.rsp.result == QMI_RESULT_SUCCESS_V01);
	free_sent(s);
}

int
main(int argc, char **argv)
{
	struct ipa_indication_register_req ireg;
	struct ipa_driver_init_complete_req done;
	struct ipa_init_complete_ind ind;
	struct ipa *ipa = &IPA;
	struct sent *s;
	int i;

	verbose = argc > 1;
	setvbuf(stdout, NULL, _IONBF, 0);
	ipa->modem_route_count = 8;
	ipa->name_map[IPA_EP_AP_MODEM_RX] = &modem_rx;
	assert(qrtr_init() == 0);
	node5 = qrtr_endpoint_register(xmit, NULL);
	assert(ipa_qmi_setup(ipa) == 0);
	/* the modem's node comes up (its GLINK channel): our servers to it */
	ctrl5(QRTR_TYPE_HELLO, 0, 0, 0, 0);
	for (i = 0; i < 200 && our_server_port == 0; i++)
		usleep(5000);
	assert(our_server_port != 0);
	printf("qmi: our IPA service 0x31.1 announced (port %u): ok\n",
	    our_server_port);

	/* 1. the modem's IPA service: INIT_DRIVER to it */
	ctrl5(QRTR_TYPE_NEW_SERVER, 0x31, 1 | 2 << 8, 5, 7);
	s = next_data();
	check_init_driver(s, 0);
	printf("qmi: INIT_DRIVER to 5:7, its values and bytes as Linux's: ok\n");

	/* 2. the modem registers for the indication (client 5:8) */
	memset(&ireg, 0, sizeof(ireg));
	ireg.master_driver_init_complete_valid = 1;
	ireg.master_driver_init_complete = 1;
	msg5(8, our_server_port, QMI_REQUEST, IPA_QMI_INDICATION_REGISTER, 11,
	    IPA_QMI_INDICATION_REGISTER_REQ_SZ, lx_ipa_indication_register_req_ei,
	    &ireg);
	expect_response(IPA_QMI_INDICATION_REGISTER, 11);
	/* 3. the response to INIT_DRIVER: not ready without DRIVER_INIT_COMPLETE */
	reply_init_driver(s);
	free_sent(s);
	usleep(100000);
	assert(modem_starts == 0);
	/* 4. DRIVER_INIT_COMPLETE: then INIT_COMPLETE and the start */
	memset(&done, 0, sizeof(done));
	done.status = 1;
	msg5(8, our_server_port, QMI_REQUEST, IPA_QMI_DRIVER_INIT_COMPLETE, 12,
	    IPA_QMI_DRIVER_INIT_COMPLETE_REQ_SZ, lx_ipa_driver_init_complete_req_ei,
	    &done);
	expect_response(IPA_QMI_DRIVER_INIT_COMPLETE, 12);
	s = next_data();
	assert(qh(s)->type == QMI_INDICATION && qh(s)->msg_id == IPA_QMI_INIT_COMPLETE);
	/* Linux sends it to the modem's IPA server's address */
	assert(s->h.dst_node == 5 && s->h.dst_port == 7);
	memset(&ind, 0, sizeof(ind));
	assert(qmi_decode_message(s->data, s->h.size, lx_ipa_init_complete_ind_ei,
	    &ind) >= 0 && ind.status.result == QMI_RESULT_SUCCESS_V01);
	free_sent(s);
	wait_int(&modem_starts, 1);
	assert(ipa_qmi_state(ipa)->rsp_ctrl_endpoint == 3);
	printf("qmi: INDICATION_REGISTER, the INIT_DRIVER response, "
	    "DRIVER_INIT_COMPLETE, INIT_COMPLETE, the data path's start: ok\n");

	/* 5. the modem's node goes: its side reset */
	ctrl5(QRTR_TYPE_BYE, 0, 0, 0, 0);
	wait_int(&modem_crashes, 1);
	assert(!ipa_qmi_state(ipa)->modem_ready && ipa_qmi_state(ipa)->n_bye == 1);
	printf("qmi: the modem's BYE, its side of the IPA reset: ok\n");

	/* 6. it boots again, the uC loaded: no indication needed this time */
	ipa->uc_loaded = true;
	ctrl5(QRTR_TYPE_NEW_SERVER, 0x31, 1 | 2 << 8, 5, 7);
	s = next_data();
	check_init_driver(s, 1);
	reply_init_driver(s);
	free_sent(s);
	wait_int(&modem_starts, 2);
	printf("qmi: the second boot (skip_uc_load 1), started again: ok\n");

	ipa_qmi_teardown(ipa);
	qrtr_endpoint_unregister(node5);
	qrtr_fini();
	return (0);
}
