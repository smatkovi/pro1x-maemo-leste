/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The IPA's handshake with the modem, after Linux 7.1's drivers/net/ipa/
 * ipa_qmi.c, on freebsd/dsp's kernel QMI (qmi_core.c, Linux's API):
 *
 *  - the AP serves the IPA service 0x31 instance 1; the modem, once it
 *    runs, sends INDICATION_REGISTER (it wants the INIT_COMPLETE indication)
 *    and DRIVER_INIT_COMPLETE (its side, the microcontroller, is ready);
 *  - the AP looks up the modem's IPA service (0x31 instance 2) and sends it
 *    INIT_DRIVER: where in the IPA's memory the modem's headers, routes,
 *    filters and memory are, and the AP endpoint for its control packets;
 *  - with both done the AP sends INIT_COMPLETE (if asked for) and the data
 *    path starts (ipa_modem_start: the endpoints, the network interfaces).
 *
 * The INIT_DRIVER request waits for its response (a minute at most) on the
 * driver's own thread (ipa_os_schedule), not on the QMI handles' one, which
 * delivers that response. The modem's node saying BYE resets the handshake
 * (ipa_server_bye) and, with the data path up, runs what Linux's SSR notifier
 * runs before the modem shuts down (ipa_modem_crashed).
 */

#include "ipa_var.h"
#include "qrtr_sock.h"
#include "qcom_qmi.h"
#include "ipa_qmi_msg.h"

#define	IPA_HOST_SERVICE_SVC_ID		0x31
#define	IPA_HOST_SVC_VERS		1
#define	IPA_HOST_SERVICE_INS_ID		1
#define	IPA_MODEM_SERVICE_SVC_ID	0x31
#define	IPA_MODEM_SERVICE_INS_ID	2
#define	IPA_MODEM_SVC_VERS		1
#define	QMI_INIT_DRIVER_TIMEOUT_MS	60000

struct ipa_qmi {
	struct ipa		*ipa;
	struct qmi_handle	client_handle;
	struct qmi_handle	server_handle;
	struct sockaddr_qrtr	modem_sq;
	ipa_sleeplock_t		lock;		/* the state */
	struct ipa_qmi_state	st;
	struct ipa_init_modem_driver_req req;
	bool			handles;
};

/* ticks of qmi_txn_wait's timeout (Linux's jiffies) */
#ifdef _KERNEL
#define	IPA_QMI_HZ	hz
#else
#define	IPA_QMI_HZ	1000	/* glink_os.h on the PC: milliseconds */
#endif

/* one IPA per system */
static struct ipa_qmi ipa_qmi_s;

static struct ipa_qmi *
qmi_of(struct qmi_handle *qmi)
{
	(void)qmi;
	return (&ipa_qmi_s);
}

const struct ipa_qmi_state *
ipa_qmi_state(struct ipa *ipa)
{
	(void)ipa;
	return (&ipa_qmi_s.st);
}

/* --- readiness ------------------------------------------------------------------ */

static void
ipa_server_init_complete(struct ipa_qmi *q)
{
	struct ipa_init_complete_ind ind;
	ssize_t ret;

	memset(&ind, 0, sizeof(ind));
	ind.status.result = QMI_RESULT_SUCCESS_V01;
	ind.status.error = QMI_ERR_NONE_V01;
	ret = qmi_send_indication(&q->server_handle, &q->modem_sq,
	    IPA_QMI_INIT_COMPLETE, IPA_QMI_INIT_COMPLETE_IND_SZ,
	    ipa_init_complete_ind_ei, &ind);
	if (ret < 0)
		ipa_os_log(q->ipa, "error %d sending init complete indication\n",
		    (int)ret);
	else {
		q->st.indication_sent = true;
		ipa_os_log(q->ipa, "QMI: init complete indication sent\n");
	}
}

/* lock held */
static void
ipa_qmi_ready(struct ipa_qmi *q)
{
	int error;

	if (!q->st.modem_ready || !q->st.uc_ready)
		return;
	if (q->st.indication_requested && !q->st.indication_sent)
		ipa_server_init_complete(q);
	if (q->st.initial_boot) {
		if (!q->st.indication_sent)
			return;
		q->st.initial_boot = false;
	}
	error = ipa_modem_start(q->ipa);
	if (error != 0)
		ipa_os_log(q->ipa, "error %d starting modem\n", error);
}

/* --- the server (IPA service 0x31, instance 1) ------------------------------------- */

static void
ipa_server_bye(struct qmi_handle *qmi, unsigned int node)
{
	struct ipa_qmi *q = qmi_of(qmi);
	bool was;

	ipa_slock(&q->lock);
	if (q->modem_sq.sq_family == 0 || node != q->modem_sq.sq_node) {
		ipa_sunlock(&q->lock);
		return;
	}
	ipa_os_log(q->ipa, "QMI: the modem (node %u) is gone\n", node);
	q->st.n_bye++;
	memset(&q->modem_sq, 0, sizeof(q->modem_sq));
	was = q->st.modem_ready;
	q->st.modem_ready = false;
	q->st.indication_requested = false;
	q->st.indication_sent = false;
	ipa_sunlock(&q->lock);
	if (was || q->ipa->modem_state != IPA_MODEM_STATE_STOPPED)
		ipa_os_schedule(q->ipa, IPA_WORK_MODEM_GONE);
}

static const struct qmi_ops ipa_server_ops = {
	.bye = ipa_server_bye,
};

static void
ipa_server_indication_register(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
    struct qmi_txn *txn, const void *decoded)
{
	struct ipa_qmi *q = qmi_of(qmi);
	struct ipa_indication_register_rsp rsp;
	ssize_t ret;

	(void)decoded;
	memset(&rsp, 0, sizeof(rsp));
	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;
	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_INDICATION_REGISTER,
	    IPA_QMI_INDICATION_REGISTER_RSP_SZ, ipa_indication_register_rsp_ei,
	    &rsp);
	if (ret < 0) {
		ipa_os_log(q->ipa, "error %d sending register indication "
		    "response\n", (int)ret);
		return;
	}
	ipa_os_log(q->ipa, "QMI: the modem registered for the indication\n");
	ipa_slock(&q->lock);
	q->st.indication_requested = true;
	/* the modem's client and server come and go together */
	if (q->modem_sq.sq_family == 0) {
		q->modem_sq = *sq;
		q->st.modem_node = sq->sq_node;
	}
	ipa_qmi_ready(q);
	ipa_sunlock(&q->lock);
}

static void
ipa_server_driver_init_complete(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
    struct qmi_txn *txn, const void *decoded)
{
	struct ipa_qmi *q = qmi_of(qmi);
	struct ipa_driver_init_complete_rsp rsp;
	ssize_t ret;

	(void)decoded;
	memset(&rsp, 0, sizeof(rsp));
	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;
	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_DRIVER_INIT_COMPLETE,
	    IPA_QMI_DRIVER_INIT_COMPLETE_RSP_SZ, ipa_driver_init_complete_rsp_ei,
	    &rsp);
	if (ret < 0) {
		ipa_os_log(q->ipa, "error %d sending init complete response\n",
		    (int)ret);
		return;
	}
	ipa_os_log(q->ipa, "QMI: the modem's driver init is complete\n");
	ipa_slock(&q->lock);
	q->st.uc_ready = true;
	ipa_qmi_ready(q);
	ipa_sunlock(&q->lock);
}

static const struct qmi_msg_handler ipa_server_msg_handlers[] = {
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_INDICATION_REGISTER,
		.ei		= ipa_indication_register_req_ei,
		.decoded_size	= sizeof(struct ipa_indication_register_req),
		.fn		= ipa_server_indication_register,
	},
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_DRIVER_INIT_COMPLETE,
		.ei		= ipa_driver_init_complete_req_ei,
		.decoded_size	= sizeof(struct ipa_driver_init_complete_req),
		.fn		= ipa_server_driver_init_complete,
	},
	{ 0 },
};

/* --- the client (the modem's IPA service, instance 2) ----------------------------- */

/* init_modem_driver_req(): offsets in the IPA's memory, for the modem */
static const struct ipa_init_modem_driver_req *
init_modem_driver_req(struct ipa_qmi *q)
{
	struct ipa *ipa = q->ipa;
	struct ipa_init_modem_driver_req *req = &q->req;
	uint32_t routes = ipa->modem_route_count;
	const struct ipa_mem *mem;

	req->skip_uc_load_valid = 1;
	req->skip_uc_load = ipa->uc_loaded ? 1 : 0;
	if (req->platform_type_valid)
		return (req);
	req->platform_type_valid = 1;
	req->platform_type = IPA_QMI_PLATFORM_TYPE_MSM_ANDROID;
	mem = ipa_mem_find(ipa, IPA_MEM_MODEM_HEADER);
	if (mem->size) {
		req->hdr_tbl_info_valid = 1;
		req->hdr_tbl_info.start = ipa->mem_offset + mem->offset;
		req->hdr_tbl_info.end = req->hdr_tbl_info.start + mem->size - 1;
	}
	mem = ipa_mem_find(ipa, IPA_MEM_V4_ROUTE);
	req->v4_route_tbl_info_valid = 1;
	req->v4_route_tbl_info.start = ipa->mem_offset + mem->offset;
	req->v4_route_tbl_info.end = routes - 1;
	mem = ipa_mem_find(ipa, IPA_MEM_V6_ROUTE);
	req->v6_route_tbl_info_valid = 1;
	req->v6_route_tbl_info.start = ipa->mem_offset + mem->offset;
	req->v6_route_tbl_info.end = routes - 1;
	mem = ipa_mem_find(ipa, IPA_MEM_V4_FILTER);
	req->v4_filter_tbl_start_valid = 1;
	req->v4_filter_tbl_start = ipa->mem_offset + mem->offset;
	mem = ipa_mem_find(ipa, IPA_MEM_V6_FILTER);
	req->v6_filter_tbl_start_valid = 1;
	req->v6_filter_tbl_start = ipa->mem_offset + mem->offset;
	mem = ipa_mem_find(ipa, IPA_MEM_MODEM);
	if (mem->size) {
		req->modem_mem_info_valid = 1;
		req->modem_mem_info.start = ipa->mem_offset + mem->offset;
		req->modem_mem_info.size = mem->size;
	}
	req->ctrl_comm_dest_end_pt_valid = 1;
	req->ctrl_comm_dest_end_pt =
	    ipa->name_map[IPA_EP_AP_MODEM_RX]->endpoint_id;
	mem = ipa_mem_find(ipa, IPA_MEM_MODEM_PROC_CTX);
	if (mem->size) {
		req->hdr_proc_ctx_tbl_info_valid = 1;
		req->hdr_proc_ctx_tbl_info.start = ipa->mem_offset + mem->offset;
		req->hdr_proc_ctx_tbl_info.end =
		    req->hdr_proc_ctx_tbl_info.start + mem->size - 1;
	}
	/* the hashed tables have size 0 on IPA v4.2: not reported */
	mem = ipa_mem_find(ipa, IPA_MEM_V4_ROUTE_HASHED);
	if (mem->size) {
		req->v4_hash_route_tbl_info_valid = 1;
		req->v4_hash_route_tbl_info.start = ipa->mem_offset + mem->offset;
		req->v4_hash_route_tbl_info.end = routes - 1;
	}
	mem = ipa_mem_find(ipa, IPA_MEM_V6_ROUTE_HASHED);
	if (mem->size) {
		req->v6_hash_route_tbl_info_valid = 1;
		req->v6_hash_route_tbl_info.start = ipa->mem_offset + mem->offset;
		req->v6_hash_route_tbl_info.end = routes - 1;
	}
	mem = ipa_mem_find(ipa, IPA_MEM_V4_FILTER_HASHED);
	if (mem->size) {
		req->v4_hash_filter_tbl_start_valid = 1;
		req->v4_hash_filter_tbl_start = ipa->mem_offset + mem->offset;
	}
	mem = ipa_mem_find(ipa, IPA_MEM_V6_FILTER_HASHED);
	if (mem->size) {
		req->v6_hash_filter_tbl_start_valid = 1;
		req->v6_hash_filter_tbl_start = ipa->mem_offset + mem->offset;
	}
	/* IPA v4.0+: the modem's quota statistics (no DROP region on v4.2) */
	mem = ipa_mem_find(ipa, IPA_MEM_STATS_QUOTA_MODEM);
	if (mem->size) {
		req->hw_stats_quota_base_addr_valid = 1;
		req->hw_stats_quota_base_addr = ipa->mem_offset + mem->offset;
		req->hw_stats_quota_size_valid = 1;
		/* Linux's value, the offset added to the size */
		req->hw_stats_quota_size = ipa->mem_offset + mem->size;
	}
	return (req);
}

/* On the driver's thread: INIT_DRIVER and its response (a minute at most) */
void
ipa_qmi_init_driver_work(struct ipa *ipa)
{
	struct ipa_qmi *q = &ipa_qmi_s;
	struct ipa_init_modem_driver_rsp rsp;
	const struct ipa_init_modem_driver_req *req;
	struct sockaddr_qrtr sq;
	struct qmi_txn txn;
	int ret;

	(void)ipa;
	ipa_slock(&q->lock);
	if (!q->handles || q->modem_sq.sq_family == 0) {
		ipa_sunlock(&q->lock);
		return;
	}
	sq = q->modem_sq;
	req = init_modem_driver_req(q);
	ipa_sunlock(&q->lock);

	memset(&rsp, 0, sizeof(rsp));
	ret = qmi_txn_init(&q->client_handle, &txn, ipa_init_modem_driver_rsp_ei,
	    &rsp);
	if (ret < 0) {
		ipa_os_log(q->ipa, "error %d preparing init driver request\n", ret);
		return;
	}
	ipa_os_log(q->ipa, "QMI: init driver request to %u:%u (skip uC load "
	    "%u)\n", sq.sq_node, sq.sq_port, req->skip_uc_load);
	ret = (int)qmi_send_request(&q->client_handle, &sq, &txn,
	    IPA_QMI_INIT_DRIVER, IPA_QMI_INIT_DRIVER_REQ_SZ,
	    ipa_init_modem_driver_req_ei, req);
	if (ret < 0) {	/* qmi_core: the length sent */
		ipa_os_log(q->ipa, "error %d sending init driver request\n", ret);
		qmi_txn_cancel(&txn);
		q->st.init_driver_result = ret < 0 ? -ret : ret;
		return;
	}
	/* qmi_txn_wait's timeout is in ticks (Linux's jiffies) */
	ret = qmi_txn_wait(&txn, (unsigned long)QMI_INIT_DRIVER_TIMEOUT_MS *
	    IPA_QMI_HZ / 1000);
	if (ret != 0) {
		ipa_os_log(q->ipa, "error %d awaiting init driver response\n",
		    ret);
		q->st.init_driver_result = ret < 0 ? -ret : ret;
		return;
	}
	/* Linux takes any response as success; its result is logged here */
	ipa_os_log(q->ipa, "QMI: init driver response: result %u error %u%s%s\n",
	    rsp.rsp.result, rsp.rsp.error,
	    rsp.modem_driver_init_pending_valid &&
	    rsp.modem_driver_init_pending ? ", init pending" : "",
	    rsp.ctrl_comm_dest_end_pt_valid ? "" : ", no control endpoint");
	ipa_slock(&q->lock);
	q->st.init_driver_result = rsp.rsp.result;
	if (rsp.ctrl_comm_dest_end_pt_valid)
		q->st.rsp_ctrl_endpoint = rsp.ctrl_comm_dest_end_pt;
	if (rsp.default_end_pt_valid)
		q->st.rsp_default_endpoint = rsp.default_end_pt;
	q->st.modem_ready = true;
	ipa_qmi_ready(q);
	ipa_sunlock(&q->lock);
}

static int
ipa_client_new_server(struct qmi_handle *qmi, struct qmi_service *svc)
{
	struct ipa_qmi *q = qmi_of(qmi);

	ipa_os_log(q->ipa, "QMI: the modem's IPA service at %u:%u\n",
	    svc->node, svc->port);
	ipa_slock(&q->lock);
	memset(&q->modem_sq, 0, sizeof(q->modem_sq));
	q->modem_sq.sq_len = sizeof(q->modem_sq);
	q->modem_sq.sq_family = AF_QIPCRTR;
	q->modem_sq.sq_node = svc->node;
	q->modem_sq.sq_port = svc->port;
	q->st.modem_node = svc->node;
	q->st.modem_port = svc->port;
	ipa_sunlock(&q->lock);
	ipa_os_schedule(q->ipa, IPA_WORK_INIT_DRIVER);
	return (0);
}

static const struct qmi_ops ipa_client_ops = {
	.new_server = ipa_client_new_server,
};

/* The modem went away: ipa_modem_crashed() on the driver's thread */
void
ipa_qmi_modem_gone_work(struct ipa *ipa)
{
	ipa_os_log(ipa, "the modem is gone: its side of the IPA reset\n");
	ipa_modem_crashed(ipa);
}

void
ipa_work_run(struct ipa *ipa, enum ipa_work w)
{
	switch (w) {
	case IPA_WORK_INIT_DRIVER:
		ipa_qmi_init_driver_work(ipa);
		break;
	case IPA_WORK_MODEM_GONE:
		ipa_qmi_modem_gone_work(ipa);
		break;
	default:
		break;
	}
}

/* --- setup ---------------------------------------------------------------------------- */

int
ipa_qmi_setup(struct ipa *ipa)
{
	struct ipa_qmi *q = &ipa_qmi_s;
	int ret;

	if (q->ipa == NULL)
		ipa_sleeplock_init(&q->lock, "ipaqmi");
	q->ipa = ipa;
	memset(&q->st, 0, sizeof(q->st));
	memset(&q->req, 0, sizeof(q->req));
	memset(&q->modem_sq, 0, sizeof(q->modem_sq));
	q->st.initial_boot = true;
	q->st.init_driver_result = -1;
	ret = qmi_handle_init(&q->server_handle, IPA_QMI_SERVER_MAX_RCV_SZ,
	    &ipa_server_ops, ipa_server_msg_handlers);
	if (ret != 0)
		return (-ret);
	ret = qmi_add_server(&q->server_handle, IPA_HOST_SERVICE_SVC_ID,
	    IPA_HOST_SVC_VERS, IPA_HOST_SERVICE_INS_ID);
	if (ret != 0)
		goto fail_server;
	ret = qmi_handle_init(&q->client_handle, IPA_QMI_CLIENT_MAX_RCV_SZ,
	    &ipa_client_ops, NULL);
	if (ret != 0)
		goto fail_server;
	q->handles = true;
	ret = qmi_add_lookup(&q->client_handle, IPA_MODEM_SERVICE_SVC_ID,
	    IPA_MODEM_SVC_VERS, IPA_MODEM_SERVICE_INS_ID);
	if (ret != 0)
		goto fail_client;
	ipa_os_log(ipa, "QMI: IPA service %#x.%u served, the modem's (%#x.%u) "
	    "looked up\n", IPA_HOST_SERVICE_SVC_ID, IPA_HOST_SERVICE_INS_ID,
	    IPA_MODEM_SERVICE_SVC_ID, IPA_MODEM_SERVICE_INS_ID);
	return (0);
fail_client:
	q->handles = false;
	qmi_handle_release(&q->client_handle);
fail_server:
	qmi_handle_release(&q->server_handle);
	return (ret < 0 ? -ret : ret);
}

void
ipa_qmi_teardown(struct ipa *ipa)
{
	struct ipa_qmi *q = &ipa_qmi_s;

	(void)ipa;
	if (!q->handles)
		return;
	ipa_slock(&q->lock);
	q->handles = false;
	ipa_sunlock(&q->lock);
	qmi_handle_release(&q->client_handle);
	qmi_handle_release(&q->server_handle);
}
