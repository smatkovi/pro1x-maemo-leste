/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * qmi_core.c and qmi_encdec.c on the PC, as ath10k's qmi.c uses them with
 * the modem: the test plays node 5 with a WLFW server (service 0x45) on
 * port 7, with ath10k's own message tables (qmi_wlfw_v01.c). Checks the
 * lookup and new_server, a request with its decoded response, a 6 KB
 * board data segment, indications to their handlers, a response nobody
 * waits for, a timeout, del_server on BYE, and release with a transaction
 * pending elsewhere.
 */

#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/queue.h>

#include "qrtr.h"
#include "qrtr_sock.h"
#include <linux/soc/qcom/qmi.h>
#include "qmi_wlfw_v01.h"

struct hdr {
	uint32_t	version, type, src_node, src_port, confirm_rx, size,
			dst_node, dst_port;
};

/* --- node 5 ------------------------------------------------------------------------------ */

struct sent {
	TAILQ_ENTRY(sent) link;
	struct hdr	h;
	uint8_t		*data;
};
static TAILQ_HEAD(, sent) sent = TAILQ_HEAD_INITIALIZER(sent);
static pthread_mutex_t sent_lock = PTHREAD_MUTEX_INITIALIZER;
static struct qrtr_node *node5;

static int
xmit(void *arg, const void *pkt, size_t len)
{
	struct sent *s = calloc(1, sizeof(*s));

	memcpy(&s->h, pkt, sizeof(s->h));
	assert(s->h.version == 1 && len == sizeof(s->h) + roundup2(s->h.size, 4));
	s->data = malloc(s->h.size + 1);
	memcpy(s->data, (const char *)pkt + sizeof(s->h), s->h.size);
	pthread_mutex_lock(&sent_lock);
	TAILQ_INSERT_TAIL(&sent, s, link);
	pthread_mutex_unlock(&sent_lock);
	return (0);
}

/* The next data packet to node 5 (control packets are skipped) */
static struct sent *
next_data(void)
{
	struct sent *s;
	int i;

	for (i = 0; i < 400; i++) {
		pthread_mutex_lock(&sent_lock);
		while ((s = TAILQ_FIRST(&sent)) != NULL && s->h.type != QRTR_TYPE_DATA) {
			TAILQ_REMOVE(&sent, s, link);
			free(s->data);
			free(s);
		}
		if (s != NULL)
			TAILQ_REMOVE(&sent, s, link);
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

/* node 5's port 7 answers: a QMI message to our client's port */
static void
reply5(uint32_t dst_port, int type, int msg_id, unsigned int txn_id,
    size_t max, const struct qmi_elem_info *ei, const void *c_struct)
{
	size_t len = max;
	void *msg = qmi_encode_message(type, msg_id, &len, txn_id, ei, c_struct);

	assert(msg != NULL);
	from5(QRTR_TYPE_DATA, 7, dst_port, msg, len);
	free(msg);
}

/* --- the client, as ath10k's qmi.c ------------------------------------------------------- */

static struct qmi_handle qmi;
static struct sockaddr_qrtr server;
static int new_servers, del_servers, fw_ready, msa_ready;
static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;

static int
new_server(struct qmi_handle *h, struct qmi_service *svc)
{
	assert(h == &qmi && svc->service == WLFW_SERVICE_ID_V01 &&
	    svc->version == WLFW_SERVICE_VERS_V01 && svc->instance == 0);
	pthread_mutex_lock(&ev_lock);
	server.sq_family = AF_QIPCRTR;
	server.sq_node = svc->node;
	server.sq_port = svc->port;
	new_servers++;
	pthread_mutex_unlock(&ev_lock);
	return (0);
}

static void
del_server(struct qmi_handle *h, struct qmi_service *svc)
{
	pthread_mutex_lock(&ev_lock);
	del_servers++;
	pthread_mutex_unlock(&ev_lock);
}

static void
fw_ready_ind(struct qmi_handle *h, struct sockaddr_qrtr *sq,
    struct qmi_txn *txn, const void *data)
{
	assert(sq->sq_node == 5 && sq->sq_port == 7);
	pthread_mutex_lock(&ev_lock);
	fw_ready++;
	pthread_mutex_unlock(&ev_lock);
}

static void
msa_ready_ind(struct qmi_handle *h, struct sockaddr_qrtr *sq,
    struct qmi_txn *txn, const void *data)
{
	pthread_mutex_lock(&ev_lock);
	msa_ready++;
	pthread_mutex_unlock(&ev_lock);
}

static const struct qmi_msg_handler handlers[] = {
	{ QMI_INDICATION, QMI_WLFW_FW_READY_IND_V01, wlfw_fw_ready_ind_msg_v01_ei,
	  sizeof(struct wlfw_fw_ready_ind_msg_v01), fw_ready_ind },
	{ QMI_INDICATION, QMI_WLFW_MSA_READY_IND_V01, wlfw_msa_ready_ind_msg_v01_ei,
	  sizeof(struct wlfw_msa_ready_ind_msg_v01), msa_ready_ind },
	{ 0 }
};

static const struct qmi_ops ops = { .new_server = new_server,
    .del_server = del_server };

static int
counter(int *c)
{
	int v;

	pthread_mutex_lock(&ev_lock);
	v = *c;
	pthread_mutex_unlock(&ev_lock);
	return (v);
}

static void
wait_counter(int *c, int n)
{
	int i;

	for (i = 0; i < 400 && counter(c) < n; i++)
		usleep(5000);
	assert(counter(c) >= n);
}

/* A request from another thread (the test answers it meanwhile) */
struct call {
	struct qmi_txn	txn;
	int		msg_id;
	size_t		max;
	const struct qmi_elem_info *req_ei;
	const void	*req;
	unsigned long	timeout;
	int		ret;
};

static void *
call_thread(void *arg)
{
	struct call *c = arg;

	c->ret = (int)qmi_send_request(&qmi, &server, &c->txn, c->msg_id,
	    c->max, c->req_ei, c->req);
	if (c->ret < 0) {
		qmi_txn_cancel(&c->txn);
		return (NULL);
	}
	c->ret = qmi_txn_wait(&c->txn, c->timeout);
	return (NULL);
}

int
main(void)
{
	struct wlfw_ind_register_req_msg_v01 ind_req, ind_req_rx;
	struct wlfw_ind_register_resp_msg_v01 ind_resp, ind_resp_rx;
	struct wlfw_cap_resp_msg_v01 cap, cap_rx;
	struct wlfw_bdf_download_req_msg_v01 *bdf, *bdf_rx;
	struct wlfw_bdf_download_resp_msg_v01 bdf_resp, bdf_resp_rx;
	struct wlfw_cap_req_msg_v01 cap_req;
	struct qmi_header *qh;
	struct call call;
	uint32_t client_port;
	struct sent *s;
	pthread_t t;
	int i;

	assert(qrtr_init() == 0);
	node5 = qrtr_endpoint_register(xmit, NULL);
	assert(qmi_handle_init(&qmi, WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN,
	    &ops, handlers) == 0);

	/* 1. node 5's WLFW server, then our lookup: new_server with it */
	ctrl5(QRTR_TYPE_NEW_SERVER, WLFW_SERVICE_ID_V01,
	    WLFW_SERVICE_VERS_V01 | 0 << 8, 5, 7);
	assert(qmi_add_lookup(&qmi, WLFW_SERVICE_ID_V01, WLFW_SERVICE_VERS_V01,
	    0) == 0);
	wait_counter(&new_servers, 1);
	assert(server.sq_node == 5 && server.sq_port == 7);
	printf("lookup, new_server 5:7: ok\n");

	/* 2. a request with options, its response decoded into resp */
	memset(&ind_req, 0, sizeof(ind_req));
	ind_req.fw_ready_enable_valid = 1;
	ind_req.fw_ready_enable = 1;
	ind_req.msa_ready_enable_valid = 1;
	ind_req.msa_ready_enable = 1;
	ind_req.client_id_valid = 1;
	ind_req.client_id = 0x4b4e454c;
	memset(&call, 0, sizeof(call));
	memset(&ind_resp_rx, 0, sizeof(ind_resp_rx));
	assert(qmi_txn_init(&qmi, &call.txn, wlfw_ind_register_resp_msg_v01_ei,
	    &ind_resp_rx) == 0);
	call.msg_id = QMI_WLFW_IND_REGISTER_REQ_V01;
	call.max = WLFW_IND_REGISTER_REQ_MSG_V01_MAX_MSG_LEN;
	call.req_ei = wlfw_ind_register_req_msg_v01_ei;
	call.req = &ind_req;
	call.timeout = 2000;
	pthread_create(&t, NULL, call_thread, &call);
	s = next_data();
	assert(s->h.dst_node == 5 && s->h.dst_port == 7);
	qh = (struct qmi_header *)s->data;
	assert(qh->type == QMI_REQUEST && qh->msg_id == QMI_WLFW_IND_REGISTER_REQ_V01);
	memset(&ind_req_rx, 0, sizeof(ind_req_rx));
	assert(qmi_decode_message(s->data, s->h.size,
	    wlfw_ind_register_req_msg_v01_ei, &ind_req_rx) >= 0);
	assert(memcmp(&ind_req, &ind_req_rx, sizeof(ind_req)) == 0);
	memset(&ind_resp, 0, sizeof(ind_resp));
	ind_resp.resp.result = QMI_RESULT_SUCCESS_V01;
	ind_resp.fw_status_valid = 1;
	ind_resp.fw_status = 0x123456789abcULL;
	reply5(s->h.src_port, QMI_RESPONSE, QMI_WLFW_IND_REGISTER_REQ_V01,
	    qh->txn_id, WLFW_IND_REGISTER_RESP_MSG_V01_MAX_MSG_LEN,
	    wlfw_ind_register_resp_msg_v01_ei, &ind_resp);
	pthread_join(t, NULL);
	assert(call.ret >= 0);
	assert(memcmp(&ind_resp, &ind_resp_rx, sizeof(ind_resp)) == 0);
	client_port = s->h.src_port;
	free_sent(s);
	printf("request and response (options, 64 bit): ok\n");

	/* 3. capabilities: nested structures and strings */
	memset(&call, 0, sizeof(call));
	memset(&cap_rx, 0, sizeof(cap_rx));
	memset(&cap_req, 0, sizeof(cap_req));
	assert(qmi_txn_init(&qmi, &call.txn, wlfw_cap_resp_msg_v01_ei, &cap_rx) == 0);
	call.msg_id = QMI_WLFW_CAP_REQ_V01;
	call.max = WLFW_CAP_REQ_MSG_V01_MAX_MSG_LEN;
	call.req_ei = wlfw_cap_req_msg_v01_ei;
	call.req = &cap_req;
	call.timeout = 2000;
	pthread_create(&t, NULL, call_thread, &call);
	s = next_data();
	qh = (struct qmi_header *)s->data;
	memset(&cap, 0, sizeof(cap));
	cap.chip_info_valid = 1;
	cap.chip_info.chip_id = 0x30214;
	cap.chip_info.chip_family = 0x4001;
	cap.board_info_valid = 1;
	cap.board_info.board_id = 0xff;
	cap.soc_info_valid = 1;
	cap.soc_info.soc_id = 0x40670100;
	cap.fw_version_info_valid = 1;
	cap.fw_version_info.fw_version = 0x100204b2;
	strcpy(cap.fw_version_info.fw_build_timestamp, "2021-03-11 11:02");
	cap.fw_build_id_valid = 1;
	strcpy(cap.fw_build_id, "QC_IMAGE_VERSION_STRING=WLAN.HL.3.2.2.c10");
	reply5(s->h.src_port, QMI_RESPONSE, QMI_WLFW_CAP_REQ_V01, qh->txn_id,
	    WLFW_CAP_RESP_MSG_V01_MAX_MSG_LEN, wlfw_cap_resp_msg_v01_ei, &cap);
	pthread_join(t, NULL);
	assert(call.ret >= 0);
	assert(memcmp(&cap, &cap_rx, sizeof(cap)) == 0);
	free_sent(s);
	printf("capabilities (structures, strings): ok\n");

	/* 4. a board data segment of 6 KB */
	bdf = calloc(1, sizeof(*bdf));
	bdf_rx = calloc(1, sizeof(*bdf_rx));
	bdf->valid = 1;
	bdf->file_id_valid = 1;
	bdf->file_id = 0;
	bdf->total_size_valid = 1;
	bdf->total_size = 24000;
	bdf->seg_id_valid = 1;
	bdf->seg_id = 2;
	bdf->data_valid = 1;
	bdf->data_len = QMI_WLFW_MAX_DATA_SIZE_V01;
	for (i = 0; i < QMI_WLFW_MAX_DATA_SIZE_V01; i++)
		bdf->data[i] = (uint8_t)(i * 7);
	bdf->end_valid = 1;
	bdf->end = 0;
	memset(&call, 0, sizeof(call));
	memset(&bdf_resp_rx, 0, sizeof(bdf_resp_rx));
	assert(qmi_txn_init(&qmi, &call.txn, wlfw_bdf_download_resp_msg_v01_ei,
	    &bdf_resp_rx) == 0);
	call.msg_id = QMI_WLFW_BDF_DOWNLOAD_REQ_V01;
	call.max = WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN;
	call.req_ei = wlfw_bdf_download_req_msg_v01_ei;
	call.req = bdf;
	call.timeout = 2000;
	pthread_create(&t, NULL, call_thread, &call);
	s = next_data();
	qh = (struct qmi_header *)s->data;
	assert(s->h.size > QMI_WLFW_MAX_DATA_SIZE_V01);
	assert(qmi_decode_message(s->data, s->h.size,
	    wlfw_bdf_download_req_msg_v01_ei, bdf_rx) >= 0);
	assert(memcmp(bdf, bdf_rx, sizeof(*bdf)) == 0);
	memset(&bdf_resp, 0, sizeof(bdf_resp));
	reply5(s->h.src_port, QMI_RESPONSE, QMI_WLFW_BDF_DOWNLOAD_REQ_V01,
	    qh->txn_id, WLFW_BDF_DOWNLOAD_RESP_MSG_V01_MAX_MSG_LEN,
	    wlfw_bdf_download_resp_msg_v01_ei, &bdf_resp);
	pthread_join(t, NULL);
	assert(call.ret >= 0 && bdf_resp_rx.resp.result == QMI_RESULT_SUCCESS_V01);
	free_sent(s);
	free(bdf);
	free(bdf_rx);
	printf("board data segment of %d bytes: ok\n", QMI_WLFW_MAX_DATA_SIZE_V01);

	/* 5. indications to their handlers; a response nobody waits for */
	reply5(client_port, QMI_INDICATION, QMI_WLFW_MSA_READY_IND_V01, 0, 0,
	    wlfw_msa_ready_ind_msg_v01_ei, NULL);
	reply5(client_port, QMI_RESPONSE, QMI_WLFW_IND_REGISTER_REQ_V01, 999,
	    WLFW_IND_REGISTER_RESP_MSG_V01_MAX_MSG_LEN,
	    wlfw_ind_register_resp_msg_v01_ei, &ind_resp);
	reply5(client_port, QMI_INDICATION, QMI_WLFW_FW_READY_IND_V01, 0, 0,
	    wlfw_fw_ready_ind_msg_v01_ei, NULL);
	wait_counter(&msa_ready, 1);
	wait_counter(&fw_ready, 1);
	assert(counter(&msa_ready) == 1 && counter(&fw_ready) == 1);
	printf("indications, a response for nobody: ok\n");

	/* 6. no answer: -ETIMEDOUT, and the late response is dropped */
	memset(&cap_rx, 0, sizeof(cap_rx));
	assert(qmi_txn_init(&qmi, &call.txn, wlfw_cap_resp_msg_v01_ei, &cap_rx) == 0);
	assert(qmi_send_request(&qmi, &server, &call.txn, QMI_WLFW_CAP_REQ_V01,
	    WLFW_CAP_REQ_MSG_V01_MAX_MSG_LEN, wlfw_cap_req_msg_v01_ei, &cap_req) >= 0);
	s = next_data();
	qh = (struct qmi_header *)s->data;
	assert(qmi_txn_wait(&call.txn, 100) == -ETIMEDOUT);
	reply5(client_port, QMI_RESPONSE, QMI_WLFW_CAP_REQ_V01, qh->txn_id,
	    WLFW_CAP_RESP_MSG_V01_MAX_MSG_LEN, wlfw_cap_resp_msg_v01_ei, &cap);
	usleep(50000);
	assert(cap_rx.chip_info_valid == 0);
	free_sent(s);
	printf("timeout, late response dropped: ok\n");

	/*
	 * 6b. as ath10k's qmi.c: requests without an address once the
	 * handle's socket is connected to the server (kernel_connect), none
	 * before
	 */
	{
		struct qmi_handle qmi2;
		struct qmi_txn txn2;

		assert(qmi_handle_init(&qmi2, 64, NULL, NULL) == 0);
		assert(qmi2.sock == &qmi2);
		assert(qmi_txn_init(&qmi2, &txn2, wlfw_cap_resp_msg_v01_ei,
		    &cap_rx) == 0);
		assert(qmi_send_request(&qmi2, NULL, &txn2, QMI_WLFW_CAP_REQ_V01,
		    WLFW_CAP_REQ_MSG_V01_MAX_MSG_LEN, wlfw_cap_req_msg_v01_ei,
		    &cap_req) == -ENOTCONN);
		qmi_txn_cancel(&txn2);
		qmi_handle_release(&qmi2);
	}
	assert(qmi_connect(qmi.sock, &server, sizeof(server)) == 0);
	memset(&cap_rx, 0, sizeof(cap_rx));
	assert(qmi_txn_init(&qmi, &call.txn, wlfw_cap_resp_msg_v01_ei, &cap_rx) == 0);
	assert(qmi_send_request(&qmi, NULL, &call.txn, QMI_WLFW_CAP_REQ_V01,
	    WLFW_CAP_REQ_MSG_V01_MAX_MSG_LEN, wlfw_cap_req_msg_v01_ei, &cap_req) >= 0);
	s = next_data();
	assert(s->h.dst_node == 5 && s->h.dst_port == 7);
	qh = (struct qmi_header *)s->data;
	reply5(client_port, QMI_RESPONSE, QMI_WLFW_CAP_REQ_V01, qh->txn_id,
	    WLFW_CAP_RESP_MSG_V01_MAX_MSG_LEN, wlfw_cap_resp_msg_v01_ei, &cap);
	/* as Linux: the decoded length */
	assert(qmi_txn_wait(&call.txn, 1000) >= 0);
	assert(cap_rx.chip_info_valid == 1);
	free_sent(s);
	printf("connected handle: requests without an address to 5:7: ok\n");

	/* 7. node 5 says bye: its server goes */
	ctrl5(QRTR_TYPE_BYE, 0, 0, 0, 0);
	wait_counter(&del_servers, 1);
	assert(counter(&del_servers) == 1);
	printf("bye, del_server: ok\n");

	/* 8. release while a transaction waits: it ends with -ENETRESET */
	ctrl5(QRTR_TYPE_NEW_SERVER, WLFW_SERVICE_ID_V01, WLFW_SERVICE_VERS_V01, 5, 7);
	memset(&call, 0, sizeof(call));
	assert(qmi_txn_init(&qmi, &call.txn, wlfw_cap_resp_msg_v01_ei, &cap_rx) == 0);
	call.msg_id = QMI_WLFW_CAP_REQ_V01;
	call.max = WLFW_CAP_REQ_MSG_V01_MAX_MSG_LEN;
	call.req_ei = wlfw_cap_req_msg_v01_ei;
	call.req = &cap_req;
	call.timeout = 5000;
	pthread_create(&t, NULL, call_thread, &call);
	s = next_data();
	free_sent(s);
	usleep(20000);
	qmi_handle_release(&qmi);
	pthread_join(t, NULL);
	assert(call.ret == -ENETRESET);
	printf("release with a waiting transaction: ok\n");

	qrtr_endpoint_unregister(node5);
	qrtr_fini();
	while ((s = TAILQ_FIRST(&sent)) != NULL) {
		TAILQ_REMOVE(&sent, s, link);
		free_sent(s);
	}
	return (0);
}
