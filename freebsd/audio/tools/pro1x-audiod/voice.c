/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The voice session of a call (for the modem agent, freebsd/modem): the
 * modem hands the voice to the ADSP, which runs it through its voice
 * processing out to the codec DMA ports and from the microphone back;
 * the application processor only sets the session up. As Linux's
 * q6voice (Leste's sound/soc/qcom/qdsp6/q6mvm.c, q6cvp.c,
 * q6voice-common.c) starts it when the VoiceMMode1 PCM opens, and as
 * KolibriOS runs it on the same phone (~/ps/kolibrios-arm
 * kernel/modem/apr.c, docs/pro1x-audio.md), one command at a time, each
 * answered by APR_BASIC_RSP_RESULT (Linux waits 300 ms):
 *
 *   up    MVM CREATE_PASSIVE_CONTROL_SESSION "11C05000" (the session's
 *         handle is the answer's source port), MVM SET_POLICY_DUAL_CONTROL
 *         1, CVP CREATE_FULL_CONTROL_SESSION_V2 (RX+TX, tx port
 *         VA_CODEC_DMA_TX_0 with no topology, rx port RX_CODEC_DMA_RX_0
 *         with RX_DEFAULT, no network, EC_INT_MIXING, no echo reference;
 *         the device tree asks for no CVD 2.3), CVP ENABLE, MVM
 *         ATTACH_VOCPROC, MVM START_VOICE
 *   down  MVM STOP_VOICE, MVM DETACH_VOCPROC, CVP DISABLE, DESTROY_SESSION
 *         for CVP, then MVM
 *
 * Headers: from the apps domain to the ADSP, the same service on both
 * ends, source port the path (VoiceMMode1, 6), destination the session's
 * handle (0 before it exists), token 0. The back ends (the AFE ports, the
 * codec's earpiece and microphone paths) come after the session, as
 * Linux's back ends prepare after the front end's startup; the daemon's
 * "voice start" does both (main.c).
 */

#include <errno.h>
#include <string.h>

#include "audiod.h"

#define	APRV2_IBASIC_CMD_DESTROY_SESSION		0x0001003c
#define	VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION	0x000110ff
#define	VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL		0x00011327
#define	VSS_IMVM_CMD_ATTACH_VOCPROC			0x0001123e
#define	VSS_IMVM_CMD_DETACH_VOCPROC			0x0001123f
#define	VSS_IMVM_CMD_START_VOICE			0x00011190
#define	VSS_IMVM_CMD_STOP_VOICE				0x00011192
#define	VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2	0x000112bf
#define	VSS_IVOCPROC_CMD_ENABLE				0x000100c6
#define	VSS_IVOCPROC_CMD_DISABLE			0x000110e1
#define	VSS_IVOCPROC_DIRECTION_RX_TX			2
#define	VSS_IVOCPROC_PORT_ID_NONE			0xffff
#define	VSS_IVOCPROC_TOPOLOGY_ID_NONE			0x00010f70
#define	VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT		0x00010f77
#define	VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING		0x00010f7c
#define	VSS_ICOMMON_CAL_NETWORK_ID_NONE			0x0001135e
#define	VOICE_PATH_VOICEMMODE1				6
#define	VOICE_SESSION_VOICEMMODE1			"11C05000"

enum {
	V_MVM_CREATE, V_MVM_DUAL, V_CVP_CREATE, V_CVP_ENABLE, V_MVM_ATTACH,
	V_MVM_START,
	V_MVM_STOP, V_MVM_DETACH, V_CVP_DISABLE, V_CVP_DESTROY, V_MVM_DESTROY,
	V_N
};

static const char *const vnames[V_N] = {
	"MVM create", "MVM dual control", "CVP create", "CVP enable",
	"MVM attach", "MVM start", "MVM stop", "MVM detach", "CVP disable",
	"CVP destroy", "MVM destroy",
};

static size_t
vhdr(uint8_t *p, uint8_t svc, uint16_t handle, uint32_t op, size_t len)
{
	return (apr_hdr_port(p, svc, VOICE_PATH_VOICEMMODE1, handle, op, 0, len));
}

/* the command of a step, as Linux's packed structures lay it out */
size_t
voice_pkt(uint8_t *p, int step, const struct voice *v)
{
	uint8_t *c = p + APR_HDR_SIZE;
	size_t len;

	switch (step) {
	case V_MVM_CREATE:
		/* struct vss_imvm_cmd_create_control_session_cmd: name[20] */
		len = APR_HDR_SIZE + 20;
		memset(p, 0, len);
		vhdr(p, APR_SVC_MVM, 0,
		    VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION, len);
		strncpy((char *)c, VOICE_SESSION_VOICEMMODE1, 19);
		return (len);
	case V_MVM_DUAL:
		len = APR_HDR_SIZE + 1;		/* bool enable */
		vhdr(p, APR_SVC_MVM, v->mvm, VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL,
		    len);
		c[0] = 1;
		return (len);
	case V_CVP_CREATE:
		/* vss_ivocproc_cmd_create_full_control_session_v2_cmd */
		len = APR_HDR_SIZE + 44;
		memset(p, 0, len);
		vhdr(p, APR_SVC_CVP, 0,
		    VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2, len);
		le16enc_(c, VSS_IVOCPROC_DIRECTION_RX_TX);
		le16enc_(c + 2, v->tx_port);
		le32enc_(c + 4, VSS_IVOCPROC_TOPOLOGY_ID_NONE);
		le16enc_(c + 8, v->rx_port);
		le32enc_(c + 10, VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT);
		le32enc_(c + 14, VSS_ICOMMON_CAL_NETWORK_ID_NONE);
		le32enc_(c + 18, VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING);
		le16enc_(c + 22, VSS_IVOCPROC_PORT_ID_NONE);
		return (len);
	case V_CVP_ENABLE:
	case V_CVP_DISABLE:
		return (vhdr(p, APR_SVC_CVP, v->cvp, step == V_CVP_ENABLE ?
		    VSS_IVOCPROC_CMD_ENABLE : VSS_IVOCPROC_CMD_DISABLE,
		    APR_HDR_SIZE));
	case V_MVM_ATTACH:
	case V_MVM_DETACH:
		len = APR_HDR_SIZE + 2;
		vhdr(p, APR_SVC_MVM, v->mvm, step == V_MVM_ATTACH ?
		    VSS_IMVM_CMD_ATTACH_VOCPROC : VSS_IMVM_CMD_DETACH_VOCPROC, len);
		le16enc_(c, v->cvp);
		return (len);
	case V_MVM_START:
	case V_MVM_STOP:
		return (vhdr(p, APR_SVC_MVM, v->mvm, step == V_MVM_START ?
		    VSS_IMVM_CMD_START_VOICE : VSS_IMVM_CMD_STOP_VOICE,
		    APR_HDR_SIZE));
	case V_CVP_DESTROY:
		return (vhdr(p, APR_SVC_CVP, v->cvp,
		    APRV2_IBASIC_CMD_DESTROY_SESSION, APR_HDR_SIZE));
	case V_MVM_DESTROY:
		return (vhdr(p, APR_SVC_MVM, v->mvm,
		    APRV2_IBASIC_CMD_DESTROY_SESSION, APR_HDR_SIZE));
	}
	return (0);
}

static int
vstep(struct q6 *q, struct voice *v, int step)
{
	uint8_t pkt[128], rsp[APR_PKT_MAX];
	size_t len, rlen = 0;
	int r, saved = q->timeout_ms;

	len = voice_pkt(pkt, step, v);
	q->timeout_ms = 300;		/* q6voice-common's TIMEOUT_MS */
	r = apr_txn(q, pkt, len, 0, rsp, &rlen);
	q->timeout_ms = saved;
	if (r == 0 && (step == V_MVM_CREATE || step == V_CVP_CREATE)) {
		/* q6voice_session_callback: the handle is the source port */
		uint16_t h = rlen >= APR_HDR_SIZE ? apr_src_port(rsp) : 0;

		if (h == 0)
			r = -EPROTO;
		else if (step == V_MVM_CREATE)
			v->mvm = h;
		else
			v->cvp = h;
	}
	if (r != 0) {
		v->failed_step = step;
		v->failed_status = r > 0 ? (uint32_t)r : 0;
		fma_log("voice: %s: %s", vnames[step], q6_err(r));
	} else
		fma_dbg("voice: %s: done", vnames[step]);
	return (r);
}

int
voice_stop(struct q6 *q, struct voice *v)
{
	int r = 0;

	/* every step that has its session, whatever the one before said */
	if (v->mvm != 0) {
		r |= vstep(q, v, V_MVM_STOP);
		if (v->cvp != 0)
			r |= vstep(q, v, V_MVM_DETACH);
	}
	if (v->cvp != 0) {
		r |= vstep(q, v, V_CVP_DISABLE);
		r |= vstep(q, v, V_CVP_DESTROY);
	}
	if (v->mvm != 0)
		r |= vstep(q, v, V_MVM_DESTROY);
	v->mvm = v->cvp = 0;
	v->state = VP_IDLE;
	return (r != 0 ? -EIO : 0);
}

int
voice_start(struct q6 *q, struct voice *v)
{
	int step;

	if (v->state == VP_UP)
		return (0);
	if (v->rx_port == 0)
		v->rx_port = AFE_PORT_ID_RX_CODEC_DMA_RX_0;
	if (v->tx_port == 0)
		v->tx_port = AFE_PORT_ID_VA_CODEC_DMA_TX_0;
	v->mvm = v->cvp = 0;
	v->failed_step = -1;
	for (step = V_MVM_CREATE; step <= V_MVM_START; step++)
		if (vstep(q, v, step) != 0) {
			int fs = v->failed_step;
			uint32_t st = v->failed_status;

			voice_stop(q, v);
			v->failed_step = fs;
			v->failed_status = st;
			v->state = VP_FAILED;
			return (-EIO);
		}
	v->state = VP_UP;
	fma_log("voice: up (MVM %#x, CVP %#x)", v->mvm, v->cvp);
	return (0);
}
