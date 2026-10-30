/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The ADSP's AFE (audio front end) over APR, as far as the FM radio needs
 * it (freebsd/fm/README.md):
 *
 * - the ports: their configuration with AFE_PORT_CMD_SET_PARAM_V2 and
 *   AFE_PORT_CMD_DEVICE_START/STOP, as Linux's q6afe.c sends them
 *   (q6afe_port_set_param_v2, q6afe_port_start, q6afe_port_stop): the
 *   configuration is the whole union afe_port_config (36 bytes) whatever
 *   the port's type, the token the port's index in the device tree
 *   bindings;
 * - the AFE loopback from a TX port to an RX port and its gain, which
 *   mainline lacks: the downstream q6afe.c (afe_loopback,
 *   afe_loopback_gain; Android's "play-fm" mixer path: "RX_CDC_DMA_RX_0
 *   Port Mixer SLIM_8_TX" and "SLIMBUS_8 LOOPBACK Volume"), sent in
 *   SET_PARAM_V2 as mainline sends its parameters;
 * - the LPASS clocks (AFE_SVC_CMD_SET_PARAM, AFE_PARAM_ID_CLOCK_SET,
 *   q6afe_set_lpass_clock) and the core block votes
 *   (q6afe_vote_lpass_core_hw; the devote as the downstream q6afe.c).
 *
 * Every command is answered by APR_BASIC_RSP_RESULT {opcode, status}
 * with the command's token (q6afe_callback); status 0 is success.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "fmaudio.h"

#define	APR_MSG_TYPE_SEQ_CMD	0x2
#define	APR_HDR_FIELD(mt, len, ver) \
	((uint16_t)(((mt) & 0x3) << 8 | (((len) / 4) & 0xf) << 4 | ((ver) & 0xf)))

/* struct apr_hdr as apr_send_pkt completes it for a service of the ADSP */
size_t
apr_hdr(uint8_t *pkt, uint8_t svc, uint32_t opcode, uint32_t token,
    size_t len)
{
	le16enc_(pkt, APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, APR_HDR_SIZE, 0));
	le16enc_(pkt + 2, (uint16_t)len);
	pkt[4] = svc;			/* src_svc: the same service */
	pkt[5] = APR_DOMAIN_APPS;
	le16enc_(pkt + 6, 0);		/* src_port */
	pkt[8] = svc;
	pkt[9] = APR_DOMAIN_ADSP;
	le16enc_(pkt + 10, 0);		/* dest_port */
	le32enc_(pkt + 12, token);
	le32enc_(pkt + 16, opcode);
	return (APR_HDR_SIZE);
}

/* struct afe_port_param_data_v2: module, param, size, reserved */
static size_t
afe_pdata(uint8_t *p, uint32_t module, uint32_t param, uint16_t psize)
{
	le32enc_(p, module);
	le32enc_(p + 4, param);
	le16enc_(p + 8, psize);
	le16enc_(p + 10, 0);
	return (12);
}

/* q6afe_port_set_param_v2: struct afe_port_cmd_set_param_v2 in band */
size_t
afe_pkt_port_set_param(uint8_t *pkt, uint16_t port, uint32_t token,
    uint32_t module, uint32_t param, const uint8_t *data, uint16_t psize)
{
	size_t len = APR_HDR_SIZE + 16 + 12 + psize;
	uint8_t *p = pkt + APR_HDR_SIZE;

	apr_hdr(pkt, APR_SVC_AFE, AFE_PORT_CMD_SET_PARAM_V2, token, len);
	le16enc_(p, port);
	le16enc_(p + 2, (uint16_t)(12 + psize));	/* payload_size */
	le32enc_(p + 4, 0);		/* payload_address_lsw */
	le32enc_(p + 8, 0);		/* payload_address_msw */
	le32enc_(p + 12, 0);		/* mem_map_handle */
	afe_pdata(p + 16, module, param, psize);
	memcpy(p + 28, data, psize);
	return (len);
}

/* q6afe_set_param: struct afe_svc_cmd_set_param in band */
size_t
afe_pkt_svc_set_param(uint8_t *pkt, uint32_t token, uint32_t module,
    uint32_t param, const uint8_t *data, uint16_t psize)
{
	size_t len = APR_HDR_SIZE + 16 + 12 + psize;
	uint8_t *p = pkt + APR_HDR_SIZE;

	apr_hdr(pkt, APR_SVC_AFE, AFE_SVC_CMD_SET_PARAM, token, len);
	le32enc_(p, 12 + psize);	/* payload_size */
	le32enc_(p + 4, 0);
	le32enc_(p + 8, 0);
	le32enc_(p + 12, 0);
	afe_pdata(p + 16, module, param, psize);
	memcpy(p + 28, data, psize);
	return (len);
}

/* struct afe_port_cmd_device_start/stop: port_id, reserved */
size_t
afe_pkt_port_cmd(uint8_t *pkt, uint32_t opcode, uint16_t port, uint32_t token)
{
	size_t len = APR_HDR_SIZE + 4;

	apr_hdr(pkt, APR_SVC_AFE, opcode, token, len);
	le16enc_(pkt + APR_HDR_SIZE, port);
	le16enc_(pkt + APR_HDR_SIZE + 2, 0);
	return (len);
}

/*
 * union afe_port_config holding struct afe_param_id_slimbus_cfg, as
 * q6afe_slim_port_prepare fills it: minor version 1, device 0
 * (AFE_SLIMBUS_DEVICE_1; downstream's qcom,msm-dai-q6-slim-dev-id = <0>
 * for SLIMBUS_8_TX), bit width, data format 0, the channels and their
 * shared channel numbers, the rate.
 */
size_t
afe_cfg_slimbus(uint8_t *cfg, unsigned rate, unsigned bits, unsigned nch,
    const uint8_t *ch)
{
	unsigned i;

	memset(cfg, 0, AFE_PORT_CONFIG_SIZE);
	le32enc_(cfg, AFE_API_VERSION_SLIMBUS_CONFIG);
	le16enc_(cfg + 4, 0);			/* slimbus_dev_id */
	le16enc_(cfg + 6, (uint16_t)bits);
	le16enc_(cfg + 8, 0);			/* data_format */
	le16enc_(cfg + 10, (uint16_t)nch);
	/* q6afe_slim_port_prepare copies the first four */
	for (i = 0; i < nch && i < 4; i++)
		cfg[12 + i] = ch[i];
	le32enc_(cfg + 20, rate);
	return (AFE_PORT_CONFIG_SIZE);
}

/* struct afe_param_id_cdc_dma_cfg (q6afe_cdc_dma_port_prepare) */
size_t
afe_cfg_cdc_dma(uint8_t *cfg, unsigned rate, unsigned bits, unsigned nch)
{
	memset(cfg, 0, AFE_PORT_CONFIG_SIZE);
	le32enc_(cfg, AFE_API_VERSION_CODEC_DMA_CONFIG);
	le32enc_(cfg + 4, rate);
	le16enc_(cfg + 8, (uint16_t)bits);
	le16enc_(cfg + 10, 0);			/* data_format */
	le16enc_(cfg + 12, (uint16_t)nch);
	le16enc_(cfg + 14, (uint16_t)((1u << nch) - 1));
	return (AFE_PORT_CONFIG_SIZE);
}

/* struct afe_loopback_cfg_v1 (downstream apr_audio-v2.h) */
size_t
afe_cfg_loopback(uint8_t *cfg, uint16_t dst_port, bool enable)
{
	le32enc_(cfg, AFE_API_VERSION_LOOPBACK_CONFIG);
	le16enc_(cfg + 4, dst_port);
	le16enc_(cfg + 6, LB_MODE_DEFAULT);
	le16enc_(cfg + 8, enable ? 1 : 0);
	le16enc_(cfg + 10, 0);
	return (12);
}

/* struct afe_loopback_gain_per_path_param: rx_port_id (afe_loopback_gain
 * puts the TX port there), gain in Q13 */
size_t
afe_cfg_loopback_gain(uint8_t *cfg, uint16_t port, uint16_t gain)
{
	le16enc_(cfg, port);
	le16enc_(cfg + 2, gain);
	return (4);
}

/* struct afe_clk_set (q6afe_set_lpass_clock): enable = freq != 0 */
size_t
afe_cfg_clock(uint8_t *cfg, uint32_t clk_id, uint32_t freq, uint16_t attri,
    uint16_t root)
{
	le32enc_(cfg, AFE_API_VERSION_CLOCK_SET);
	le32enc_(cfg + 4, clk_id);
	le32enc_(cfg + 8, freq);
	le16enc_(cfg + 12, attri);
	le16enc_(cfg + 14, root);
	le32enc_(cfg + 16, freq != 0);
	return (20);		/* sizeof(struct afe_clk_set): no padding */
}

/*
 * struct afe_cmd_remote_lpass_core_hw_vote_request: the block, the
 * client's name in 8 bytes (strscpy: at most 7 and a NUL); the token is
 * the block. The devote: block and handle, token 0 (downstream).
 */
size_t
afe_pkt_vote(uint8_t *pkt, uint32_t block, const char *client, bool vote,
    uint32_t handle)
{
	size_t len = APR_HDR_SIZE + (vote ? 12 : 8);
	uint8_t *p = pkt + APR_HDR_SIZE;

	apr_hdr(pkt, APR_SVC_AFE, vote ? AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST :
	    AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST, vote ? block : 0, len);
	le32enc_(p, block);
	if (vote) {
		memset(p + 4, 0, 8);
		strncpy((char *)p + 4, client, 7);
	} else
		le32enc_(p + 4, handle);
	return (len);
}

/* --- transactions ----------------------------------------------------- */

/*
 * Send pkt and wait for the answer: APR_BASIC_RSP_RESULT for its opcode
 * and token, or, if want_opcode is set, the packet with that opcode (then
 * *rsp, *rsplen hold it; a basic result for the command then only ends
 * the wait if it reports an error: both kernels wait for the answer
 * itself, e.g. the vote's handle). 0 done; > 0 the ADSP's status; < 0 an
 * errno (-ETIMEDOUT, or -ENOMSG: only a basic success, no answer).
 */
static int
afe_txn(struct afe *a, const uint8_t *pkt, size_t len, uint32_t want_opcode,
    uint8_t *rsp, size_t *rsplen)
{
	uint8_t buf[4096];
	uint32_t opcode = le32dec_(pkt + 16), token = le32dec_(pkt + 12);
	uint64_t until;
	int n, hlen;
	bool basic_ok = false;

	if (a->io->send(a->io->ctx, pkt, len) != 0)
		return (-EIO);
	until = fma_now_ms() + (uint64_t)a->timeout_ms;
	while (fma_now_ms() < until) {
		n = a->io->recv(a->io->ctx, buf, sizeof(buf),
		    (int)(until - fma_now_ms()));
		if (n < 0)
			return (-EIO);
		if (n == 0)
			break;
		if (n < APR_HDR_SIZE)
			continue;
		hlen = ((le16dec_(buf) >> 4) & 0xf) * 4;
		if (hlen < APR_HDR_SIZE || hlen > n)
			continue;
		if (le32dec_(buf + 16) == APR_BASIC_RSP_RESULT &&
		    n >= hlen + 8 && le32dec_(buf + hlen) == opcode &&
		    le32dec_(buf + 12) == token) {
			a->last_status = le32dec_(buf + hlen + 4);
			if (want_opcode == 0 || a->last_status != 0)
				return ((int)a->last_status);
			basic_ok = true;
			continue;
		}
		if (want_opcode != 0 && le32dec_(buf + 16) == want_opcode) {
			if (rsp != NULL) {
				memcpy(rsp, buf, (size_t)n);
				*rsplen = (size_t)n;
			}
			return (0);
		}
		a->events++;
		fma_log("afe: from service %u: opcode %#x token %#x (%d bytes)",
		    buf[4], le32dec_(buf + 16), le32dec_(buf + 12), n);
	}
	return (basic_ok ? -ENOMSG : -ETIMEDOUT);
}

static int
afe_result(const char *what, int r)
{
	if (r < 0)
		fma_log("afe: %s: %s", what, strerror(-r));
	else if (r > 0)
		fma_log("afe: %s: ADSP status %#x", what, (unsigned)r);
	else
		fma_dbg("afe: %s: done", what);
	return (r);
}

/* q6core_is_adsp_ready: AVCS_CMD_ADSP_EVENT_GET_STATE until it says 1 */
int
adsp_ready(struct afe *a, int timeout_ms)
{
	uint8_t pkt[APR_HDR_SIZE], rsp[256];
	size_t rlen = 0;
	uint64_t until = fma_now_ms() + (uint64_t)timeout_ms;
	int r, saved = a->timeout_ms;

	a->timeout_ms = 200;
	do {
		apr_hdr(pkt, APR_SVC_ADSP_CORE, AVCS_CMD_ADSP_EVENT_GET_STATE, 0,
		    sizeof(pkt));
		r = afe_txn(a, pkt, sizeof(pkt), AVCS_CMDRSP_ADSP_EVENT_GET_STATE,
		    rsp, &rlen);
		if (r == 0 && rlen >= APR_HDR_SIZE + 4 &&
		    le32dec_(rsp + APR_HDR_SIZE) != 0) {
			a->timeout_ms = saved;
			fma_log("core: ADSP ready");
			return (0);
		}
		if (r > 0) {
			/* q6core: an ADSP without the command counts as up */
			a->timeout_ms = saved;
			fma_log("core: GET_STATE unsupported (%#x): taken as "
			    "ready", (unsigned)r);
			return (0);
		}
	} while (fma_now_ms() < until);
	a->timeout_ms = saved;
	fma_log("core: the ADSP does not say it is ready");
	return (-ETIMEDOUT);
}

/* q6afe_port_start: the configuration, then DEVICE_START */
int
afe_port_start(struct afe *a, uint16_t port, uint32_t token,
    uint32_t cfg_param, const uint8_t *cfg)
{
	uint8_t pkt[256];
	char what[64];
	size_t len;
	int r;

	len = afe_pkt_port_set_param(pkt, port, token,
	    AFE_MODULE_AUDIO_DEV_INTERFACE, cfg_param, cfg,
	    AFE_PORT_CONFIG_SIZE);
	snprintf(what, sizeof(what), "port %#x configuration", port);
	if ((r = afe_result(what, afe_txn(a, pkt, len, 0, NULL, NULL))) != 0)
		return (r);
	len = afe_pkt_port_cmd(pkt, AFE_PORT_CMD_DEVICE_START, port, token);
	snprintf(what, sizeof(what), "port %#x start", port);
	r = afe_result(what, afe_txn(a, pkt, len, 0, NULL, NULL));
	if (r == 0)
		fma_log("afe: port %#x started", port);
	return (r);
}

int
afe_port_stop(struct afe *a, uint16_t port, uint32_t token)
{
	uint8_t pkt[64];
	char what[64];
	size_t len;

	len = afe_pkt_port_cmd(pkt, AFE_PORT_CMD_DEVICE_STOP, port, token);
	snprintf(what, sizeof(what), "port %#x stop", port);
	return (afe_result(what, afe_txn(a, pkt, len, 0, NULL, NULL)));
}

/* afe_loopback(enable, rx_port, tx_port): set on the TX (source) port */
int
afe_loopback(struct afe *a, uint16_t tx_port, uint32_t token,
    uint16_t rx_port, bool enable)
{
	uint8_t pkt[128], cfg[12];
	char what[64];
	size_t len;

	afe_cfg_loopback(cfg, rx_port, enable);
	len = afe_pkt_port_set_param(pkt, tx_port, token, AFE_MODULE_LOOPBACK,
	    AFE_PARAM_ID_LOOPBACK_CONFIG, cfg, sizeof(cfg));
	snprintf(what, sizeof(what), "loopback %#x -> %#x %s", tx_port,
	    rx_port, enable ? "on" : "off");
	return (afe_result(what, afe_txn(a, pkt, len, 0, NULL, NULL)));
}

int
afe_loopback_gain(struct afe *a, uint16_t tx_port, uint32_t token,
    uint16_t gain)
{
	uint8_t pkt[128], cfg[4];
	char what[64];
	size_t len;

	afe_cfg_loopback_gain(cfg, tx_port, gain);
	len = afe_pkt_port_set_param(pkt, tx_port, token, AFE_MODULE_LOOPBACK,
	    AFE_PARAM_ID_LOOPBACK_GAIN_PER_PATH, cfg, sizeof(cfg));
	snprintf(what, sizeof(what), "loopback gain %#x on %#x", gain, tx_port);
	return (afe_result(what, afe_txn(a, pkt, len, 0, NULL, NULL)));
}

/* q6afe_set_lpass_clock: attribute COUPLE_NO, root default; 0 Hz is off */
int
afe_clock(struct afe *a, uint32_t clk_id, uint32_t freq)
{
	uint8_t pkt[128], cfg[20];
	char what[64];
	size_t len;

	afe_cfg_clock(cfg, clk_id, freq, LPASS_CLK_ATTRIBUTE_COUPLE_NO,
	    Q6AFE_LPASS_CLK_ROOT_DEFAULT);
	len = afe_pkt_svc_set_param(pkt, AFE_CLK_TOKEN, AFE_MODULE_CLOCK_SET,
	    AFE_PARAM_ID_CLOCK_SET, cfg, sizeof(cfg));
	snprintf(what, sizeof(what), "clock %#x at %u Hz", clk_id, freq);
	return (afe_result(what, afe_txn(a, pkt, len, 0, NULL, NULL)));
}

/* the answer carries the client's handle (downstream q6afe.c) */
int
afe_vote(struct afe *a, uint32_t block, const char *client, uint32_t *handle)
{
	uint8_t pkt[64], rsp[256];
	size_t len, rlen = 0;
	char what[64];
	int r;

	len = afe_pkt_vote(pkt, block, client, true, 0);
	snprintf(what, sizeof(what), "vote for block %u", block);
	*handle = 0;
	r = afe_txn(a, pkt, len, AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST,
	    rsp, &rlen);
	if (r == -ENOMSG) {
		fma_log("afe: block %u: the vote acknowledged, but no handle "
		    "came", block);
		return (0);
	}
	if (r == 0) {
		*handle = rlen >= APR_HDR_SIZE + 4 ?
		    le32dec_(rsp + APR_HDR_SIZE) : 0;
		fma_log("afe: block %u voted on, handle %u", block, *handle);
	}
	return (afe_result(what, r));
}

int
afe_unvote(struct afe *a, uint32_t block, uint32_t handle)
{
	uint8_t pkt[64];
	char what[64];
	size_t len;

	/* the downstream q6afe.c sends no devote without a valid handle */
	if (handle == 0) {
		fma_log("afe: block %u: no handle, no devote", block);
		return (0);
	}
	len = afe_pkt_vote(pkt, block, NULL, false, handle);
	snprintf(what, sizeof(what), "devote block %u", block);
	return (afe_result(what, afe_txn(a, pkt, len, 0, NULL, NULL)));
}
