/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The ADSP's audio services for one PCM stream, as Linux sends them
 * (sound/soc/qcom/qdsp6/q6asm.c, q6asm-dai.c, q6adm.c, q6routing.c):
 *
 *   playback  map the buffer (ASM_CMD_SHARED_MEM_MAP_REGIONS, one
 *             contiguous region), OPEN_WRITE_V3 (PCM into the matrix, no
 *             post-processing), the ADM's COPP on the back end's AFE port
 *             (NULL topology, 48 kHz, 2 channels, 16 bit: the back end's
 *             hw_params, sm8250_be_hw_params_fixup) and the matrix routing
 *             session -> COPP, MEDIA_FMT_UPDATE_V2, the data writes, RUN;
 *   capture   the same with OPEN_READ_V3, the TX matrix, the encoder
 *             configuration (SET_ENCDEC_PARAM) and the reads queued;
 *   the end   CLOSE, unmap, the COPP closed.
 *
 * Every packet is laid out field by field as Linux's packed structures
 * (test/test_audiod.c checks them). The AFE's packets are freebsd/fm's
 * afe.c (built in); here only their transactions, through apr_txn.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "audiod.h"

void
q6_map_channels(uint8_t map[PCM_MAX_NUM_CHANNEL], unsigned ch)
{
	/* q6dsp_map_channels for the one and two channels used here */
	memset(map, 0, PCM_MAX_NUM_CHANNEL);
	if (ch == 1)
		map[0] = PCM_CHANNEL_FC;
	else {
		map[0] = PCM_CHANNEL_FL;
		map[1] = PCM_CHANNEL_FR;
	}
}

/* q6asm_add_hdr: session << 8 | stream id 1 on both ends, token session */
static size_t
asm_hdr(uint8_t *pkt, int session, uint32_t opcode, uint32_t token,
    size_t len)
{
	uint16_t port = (uint16_t)((session << 8 & 0xff00) | 1);

	return (apr_hdr_port(pkt, APR_SVC_ASM, port, port, opcode, token, len));
}

/* __q6asm_memory_map_regions: ports 0, token session << 8 | dir */
size_t
asm_pkt_map(uint8_t *pkt, int session, int dir, uint64_t addr, uint32_t size)
{
	size_t len = APR_HDR_SIZE + 8 + 12;
	uint8_t *p = pkt + APR_HDR_SIZE;

	apr_hdr_port(pkt, APR_SVC_ASM, 0, 0, ASM_CMD_SHARED_MEM_MAP_REGIONS,
	    (uint32_t)(session << 8 | dir), len);
	le16enc_(p, ADSP_MEMORY_MAP_SHMEM8_4K_POOL);	/* mem_pool_id */
	le16enc_(p + 2, 1);				/* num_regions */
	le32enc_(p + 4, 0);				/* property_flag */
	le32enc_(p + 8, (uint32_t)addr);		/* shm_addr_lsw */
	le32enc_(p + 12, (uint32_t)(addr >> 32));	/* shm_addr_msw */
	le32enc_(p + 16, (size + 4095) & ~4095u);	/* ALIGN(buf_sz, 4096) */
	return (len);
}

size_t
asm_pkt_unmap(uint8_t *pkt, int session, int dir, uint32_t handle)
{
	size_t len = APR_HDR_SIZE + 4;

	apr_hdr_port(pkt, APR_SVC_ASM, 0, 0, ASM_CMD_SHARED_MEM_UNMAP_REGIONS,
	    (uint32_t)(session << 8 | dir), len);
	le32enc_(pkt + APR_HDR_SIZE, handle);
	return (len);
}

/* struct asm_stream_cmd_open_write_v3 */
size_t
asm_pkt_open_write(uint8_t *pkt, int session, unsigned bits)
{
	size_t len = APR_HDR_SIZE + 16;
	uint8_t *p = pkt + APR_HDR_SIZE;

	asm_hdr(pkt, session, ASM_STREAM_CMD_OPEN_WRITE_V3, (uint32_t)session,
	    len);
	le32enc_(p, 0);				/* mode_flags: legacy */
	le16enc_(p + 4, ASM_END_POINT_DEVICE_MATRIX);
	le16enc_(p + 6, (uint16_t)bits);
	le32enc_(p + 8, ASM_NULL_POPP_TOPOLOGY);
	le32enc_(p + 12, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2);
	return (len);
}

/* struct asm_stream_cmd_open_read_v3 */
size_t
asm_pkt_open_read(uint8_t *pkt, int session, unsigned bits)
{
	size_t len = APR_HDR_SIZE + 20;
	uint8_t *p = pkt + APR_HDR_SIZE;

	asm_hdr(pkt, session, ASM_STREAM_CMD_OPEN_READ_V3, (uint32_t)session,
	    len);
	le32enc_(p, 0);				/* mode_flags */
	le32enc_(p + 4, ASM_END_POINT_DEVICE_MATRIX);
	le32enc_(p + 8, ASM_STREAM_POSTPROC_TOPO_ID_NONE);
	le32enc_(p + 12, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2);
	le16enc_(p + 16, (uint16_t)bits);
	le16enc_(p + 18, 0);
	return (len);
}

/* struct asm_multi_channel_pcm_fmt_blk_v2 */
size_t
asm_pkt_media_fmt(uint8_t *pkt, int session, unsigned rate,
    unsigned channels, unsigned bits)
{
	size_t len = APR_HDR_SIZE + 24;
	uint8_t *p = pkt + APR_HDR_SIZE;

	asm_hdr(pkt, session, ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2,
	    (uint32_t)session, len);
	le32enc_(p, 20);			/* fmt_blk_size */
	le16enc_(p + 4, (uint16_t)channels);
	le16enc_(p + 6, (uint16_t)bits);
	le32enc_(p + 8, rate);
	le16enc_(p + 12, 1);			/* is_signed */
	le16enc_(p + 14, 0);
	q6_map_channels(p + 16, channels);
	return (len);
}

/* struct asm_multi_channel_pcm_enc_cfg_v2 in SET_ENCDEC_PARAM */
size_t
asm_pkt_enc_cfg(uint8_t *pkt, int session, unsigned rate, unsigned channels,
    unsigned bits)
{
	size_t len = APR_HDR_SIZE + 36;
	uint8_t *p = pkt + APR_HDR_SIZE;

	asm_hdr(pkt, session, ASM_STREAM_CMD_SET_ENCDEC_PARAM, (uint32_t)session,
	    len);
	le32enc_(p, ASM_PARAM_ID_ENCDEC_ENC_CFG_BLK_V2);
	le32enc_(p + 4, 28);			/* param_size */
	le32enc_(p + 8, 0);			/* frames_per_buf */
	le32enc_(p + 12, 20);			/* enc_cfg_blk_size */
	le16enc_(p + 16, (uint16_t)channels);
	le16enc_(p + 18, (uint16_t)bits);
	le32enc_(p + 20, rate);
	le16enc_(p + 24, 1);			/* is_signed */
	le16enc_(p + 26, 0);
	q6_map_channels(p + 28, channels);
	return (len);
}

/* struct asm_session_cmd_run_v2: flags 0, no time */
size_t
asm_pkt_run(uint8_t *pkt, int session)
{
	size_t len = APR_HDR_SIZE + 12;

	asm_hdr(pkt, session, ASM_SESSION_CMD_RUN_V2, (uint32_t)session, len);
	memset(pkt + APR_HDR_SIZE, 0, 12);
	return (len);
}

/* __q6asm_cmd: CLOSE, EOS, FLUSH, PAUSE */
size_t
asm_pkt_cmd(uint8_t *pkt, int session, uint32_t opcode)
{
	return (asm_hdr(pkt, session, opcode, (uint32_t)session, APR_HDR_SIZE));
}

/* q6asm_write_async: token the block | len << 16, seq_id the block */
size_t
asm_pkt_write(uint8_t *pkt, int session, uint64_t addr, uint32_t handle,
    uint32_t len, uint32_t idx)
{
	size_t plen = APR_HDR_SIZE + 32;
	uint8_t *p = pkt + APR_HDR_SIZE;

	asm_hdr(pkt, session, ASM_DATA_CMD_WRITE_V2,
	    idx | len << ASM_WRITE_TOKEN_LEN_SHIFT, plen);
	le32enc_(p, (uint32_t)addr);
	le32enc_(p + 4, (uint32_t)(addr >> 32));
	le32enc_(p + 8, handle);
	le32enc_(p + 12, len);
	le32enc_(p + 16, idx);			/* seq_id */
	le32enc_(p + 20, 0);			/* timestamp */
	le32enc_(p + 24, 0);
	le32enc_(p + 28, 0);			/* flags */
	return (plen);
}

/* q6asm_read: token and seq_id the block */
size_t
asm_pkt_read(uint8_t *pkt, int session, uint64_t addr, uint32_t handle,
    uint32_t len, uint32_t idx)
{
	size_t plen = APR_HDR_SIZE + 20;
	uint8_t *p = pkt + APR_HDR_SIZE;

	asm_hdr(pkt, session, ASM_DATA_CMD_READ_V2, idx, plen);
	le32enc_(p, (uint32_t)addr);
	le32enc_(p + 4, (uint32_t)(addr >> 32));
	le32enc_(p + 8, handle);
	le32enc_(p + 12, len);
	le32enc_(p + 16, idx);
	return (plen);
}

/* q6adm_device_open: ports the AFE port, token port index << 16 | copp */
size_t
adm_pkt_open(uint8_t *pkt, uint16_t afe_port, unsigned port_idx,
    unsigned copp_idx, int path, unsigned rate, unsigned channels,
    unsigned bits)
{
	size_t len = APR_HDR_SIZE + 28;
	uint8_t *p = pkt + APR_HDR_SIZE;

	apr_hdr_port(pkt, APR_SVC_ADM, afe_port, afe_port,
	    ADM_CMD_DEVICE_OPEN_V5, port_idx << 16 | copp_idx, len);
	le16enc_(p, 0);				/* flags: legacy */
	le16enc_(p + 2, (uint16_t)path);	/* mode_of_operation */
	le16enc_(p + 4, afe_port);		/* endpoint_id_1 */
	le16enc_(p + 6, 0);			/* endpoint_id_2 */
	le32enc_(p + 8, NULL_COPP_TOPOLOGY);
	le16enc_(p + 12, (uint16_t)(channels & 0xff));
	le16enc_(p + 14, (uint16_t)bits);
	le32enc_(p + 16, rate);
	q6_map_channels(p + 20, channels);
	return (len);
}

/*
 * q6adm_matrix_map with one COPP: matrix, one session, the session's
 * node, the COPP ids as u16 in room for u32 each (Linux's pkt_size).
 * Ports 0, token 0.
 */
size_t
adm_pkt_map(uint8_t *pkt, int path, int session, uint16_t copp_id)
{
	size_t len = APR_HDR_SIZE + 8 + 4 + 4;
	uint8_t *p = pkt + APR_HDR_SIZE;

	apr_hdr_port(pkt, APR_SVC_ADM, 0, 0, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, 0,
	    len);
	le32enc_(p, path == ADM_PATH_PLAYBACK ? ADM_MATRIX_ID_AUDIO_RX :
	    ADM_MATRIX_ID_AUDIO_TX);
	le32enc_(p + 4, 1);			/* num_sessions */
	le16enc_(p + 8, (uint16_t)session);
	le16enc_(p + 10, 1);			/* num_copps */
	le16enc_(p + 12, copp_id);
	le16enc_(p + 14, 0);
	return (len);
}

/*
 * q6adm_device_close as q6adm_free_copp calls it: the source port is the
 * copp's afe_port, which q6adm_alloc_copp set to the port's index (not
 * its AFE id, unlike the open's ports), the destination the COPP
 */
size_t
adm_pkt_close(uint8_t *pkt, uint16_t afe_port, unsigned port_idx,
    unsigned copp_idx, uint16_t copp_id)
{
	(void)afe_port;
	return (apr_hdr_port(pkt, APR_SVC_ADM, (uint16_t)port_idx, copp_id,
	    ADM_CMD_DEVICE_CLOSE_V5, port_idx << 16 | copp_idx, APR_HDR_SIZE));
}

/* --- transactions ------------------------------------------------------- */

static int
result(const char *what, int r)
{
	if (r != 0)
		fma_log("q6: %s: %s", what, q6_err(r));
	else
		fma_dbg("q6: %s: done", what);
	return (r);
}

/* q6core_is_adsp_ready: AVCS_CMD_ADSP_EVENT_GET_STATE until it says 1 */
int
q6_core_ready(struct q6 *q, int timeout_ms)
{
	uint8_t pkt[APR_HDR_SIZE], rsp[APR_PKT_MAX];
	size_t rlen = 0;
	uint64_t until = fma_now_ms() + (uint64_t)timeout_ms;
	int r, saved = q->timeout_ms;

	q->timeout_ms = 200;
	do {
		apr_hdr_port(pkt, APR_SVC_ADSP_CORE, 0, 0,
		    AVCS_CMD_ADSP_EVENT_GET_STATE, 0, sizeof(pkt));
		r = apr_txn(q, pkt, sizeof(pkt), AVCS_CMDRSP_ADSP_EVENT_GET_STATE,
		    rsp, &rlen);
		if (r == 0 && rlen >= APR_HDR_SIZE + 4 &&
		    le32dec_(rsp + APR_HDR_SIZE) != 0)
			break;
		if (r > 0) {
			/* q6core: an ADSP without the command counts as up */
			fma_log("core: GET_STATE unsupported (%#x): taken as "
			    "ready", (unsigned)r);
			r = 0;
			break;
		}
		r = -ETIMEDOUT;
	} while (fma_now_ms() < until);
	q->timeout_ms = saved;
	if (r == 0) {
		q->core_ready = true;
		fma_log("core: ADSP ready");
	} else
		fma_log("core: the ADSP does not say it is ready");
	return (r);
}

/* the handle comes in ASM_CMDRSP_SHARED_MEM_MAP_REGIONS (payload[0]) */
int
asm_map(struct q6 *q, int session, int dir, uint64_t addr, uint32_t size,
    uint32_t *handle)
{
	uint8_t pkt[64], rsp[APR_PKT_MAX];
	size_t len, rlen = 0, h;
	int r;

	len = asm_pkt_map(pkt, session, dir, addr, size);
	r = apr_txn(q, pkt, len, ASM_CMDRSP_SHARED_MEM_MAP_REGIONS, rsp, &rlen);
	*handle = 0;
	if (r == 0) {
		h = apr_hlen(rsp, rlen);
		if (h == 0 || rlen < h + 4 || (*handle = le32dec_(rsp + h)) == 0)
			r = -EPROTO;
	}
	if (r == 0)
		fma_dbg("asm %d: %#jx+%u mapped, handle %#x", session,
		    (uintmax_t)addr, size, *handle);
	return (result("map", r));
}

int
asm_unmap(struct q6 *q, int session, int dir, uint32_t handle)
{
	uint8_t pkt[64];
	size_t len;

	if (handle == 0)
		return (0);
	len = asm_pkt_unmap(pkt, session, dir, handle);
	return (result("unmap", apr_txn(q, pkt, len, 0, NULL, NULL)));
}

int
asm_cmd(struct q6 *q, const uint8_t *pkt, size_t len, const char *what)
{
	return (result(what, apr_txn(q, pkt, len, 0, NULL, NULL)));
}

/* ADM_CMDRSP_DEVICE_OPEN_V5: {status, copp_id, reserved} */
int
adm_open(struct q6 *q, uint16_t afe_port, unsigned port_idx, int path,
    unsigned rate, unsigned channels, unsigned bits, uint16_t *copp_id)
{
	uint8_t pkt[64], rsp[APR_PKT_MAX];
	size_t len, rlen = 0, h;
	int r;

	len = adm_pkt_open(pkt, afe_port, port_idx, 0, path, rate, channels,
	    bits);
	r = apr_txn(q, pkt, len, ADM_CMDRSP_DEVICE_OPEN_V5, rsp, &rlen);
	*copp_id = INVALID_COPP_ID;
	if (r == 0) {
		h = apr_hlen(rsp, rlen);
		if (h == 0 || rlen < h + 6)
			r = -EPROTO;
		else if (le32dec_(rsp + h) != 0)
			r = (int)le32dec_(rsp + h);
		else if ((*copp_id = le16dec_(rsp + h + 4)) == INVALID_COPP_ID)
			r = -EPROTO;
	}
	if (r == 0)
		fma_dbg("adm: port %#x path %d: COPP %u", afe_port, path,
		    *copp_id);
	return (result("COPP open", r));
}

int
adm_map(struct q6 *q, int path, int session, uint16_t copp_id)
{
	uint8_t pkt[64];
	size_t len;

	len = adm_pkt_map(pkt, path, session, copp_id);
	return (result("matrix map", apr_txn(q, pkt, len, 0, NULL, NULL)));
}

int
adm_close(struct q6 *q, uint16_t afe_port, unsigned port_idx, uint16_t copp_id)
{
	uint8_t pkt[64];
	size_t len;

	if (copp_id == INVALID_COPP_ID)
		return (0);
	len = adm_pkt_close(pkt, afe_port, port_idx, 0, copp_id);
	return (result("COPP close", apr_txn(q, pkt, len, 0, NULL, NULL)));
}
