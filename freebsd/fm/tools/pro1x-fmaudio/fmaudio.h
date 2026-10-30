/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-fmaudio: the FM radio's sound path on FreeBSD, programmed from
 * user space (freebsd/fm/README.md, "Stage 3"):
 *
 *   ngd.c     the SLIMbus NGD of the LPASS (Linux's qcom-ngd-ctrl.c, with
 *             the register message path of the downstream slim-msm-ngd.c
 *             instead of BAM DMA), the bits of Linux's SLIMbus core the FM
 *             stream needs (logical addresses, value elements, the
 *             stream), and the WCN3950's FM ports (Leste's patch 0019,
 *             downstream btfm_slim)
 *   afe.c     the ADSP's AFE over APR (/dev/apr): port SLIMBUS_8_TX, the
 *             codec DMA port RX_CODEC_DMA_RX_0, the AFE loopback between
 *             them (Android's "play-fm"), the LPASS clock votes (Linux's
 *             q6afe.c, q6afe-clocks.c; the downstream q6afe.c's loopback)
 *   pro1x-fmaudio.c   the steps, the QMI client of the ADSP's SLIMbus
 *             service, the register windows (/dev/qcom_lpass), the daemon
 *
 * Register accesses and APR packets go through the small interfaces
 * below, so that the host tests (freebsd/fm/test) run the same code
 * against a simulated NGD and ADSP.
 */

#ifndef _FMAUDIO_H_
#define	_FMAUDIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- logging ---------------------------------------------------------- */

extern int fma_verbose;
void fma_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fma_dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint64_t fma_now_ms(void);
void fma_sleep_ms(unsigned ms);

/* --- register windows ------------------------------------------------- */

struct mmio {
	const char	*name;
	uint64_t	pa;
	size_t		len;
	volatile uint32_t *base;	/* the mapping (real hardware) */
	/* a simulation instead (host tests): both or neither */
	uint32_t	(*rd)(struct mmio *, uint32_t off);
	void		(*wr)(struct mmio *, uint32_t off, uint32_t val);
	void		*priv;
	int		trace;		/* log every access */
};

uint32_t mmio_rd(struct mmio *m, uint32_t off);
void mmio_wr(struct mmio *m, uint32_t off, uint32_t val);

/* --- SLIMbus: the NGD and the WCN3950 (ngd.c) ------------------------- */

/* include/linux/slimbus.h, drivers/slimbus/slimbus.h */
#define	SLIM_MSG_MT_CORE		0x0
#define	SLIM_MSG_MT_DEST_REFERRED_USER	0x2
#define	SLIM_MSG_MT_SRC_REFERRED_USER	0x6
#define	SLIM_MSG_MC_CONNECT_SOURCE	0x10
#define	SLIM_MSG_MC_REPLY_INFORMATION	0x24
#define	SLIM_MSG_MC_REQUEST_VALUE	0x60
#define	SLIM_MSG_MC_REPLY_VALUE		0x64
#define	SLIM_MSG_MC_CHANGE_VALUE	0x68
#define	SLIM_LA_MGR			0xff
/* qcom-ngd-ctrl.c: the messages to and from the ADSP's bus manager */
#define	SLIM_USR_MC_MASTER_CAPABILITY	0x00
#define	SLIM_USR_MC_REPORT_SATELLITE	0x01
#define	SLIM_USR_MC_ADDR_QUERY		0x0d
#define	SLIM_USR_MC_ADDR_REPLY		0x0e
#define	SLIM_USR_MC_DEF_ACT_CHAN	0x21
#define	SLIM_USR_MC_CHAN_CTRL		0x23
#define	SLIM_USR_MC_RECONFIG_NOW	0x24
#define	SLIM_USR_MC_GENERIC_ACK		0x25
#define	SLIM_USR_MC_CONNECT_SRC		0x2c
#define	SLIM_USR_MC_DISCONNECT_PORT	0x2e
/* slim_msg_asm: SLIM_MSG_ASM_FIRST_WORD(l, mt, mc, dt, ad) */
#define	SLIM_MSG_ASM_FIRST_WORD(l, mt, mc, dt, ad) \
	((uint32_t)(l) | (uint32_t)(mt) << 5 | (uint32_t)(mc) << 8 | \
	 (uint32_t)(dt) << 15 | (uint32_t)(ad) << 16)
#define	SLIM_MSGQ_BUF_LEN		40	/* the longest message */

/* the NGD's registers (qcom-ngd-ctrl.c; downstream slim-msm-ngd.c) */
#define	NGD_CFG			0x00
#define	NGD_CFG_ENABLE		(1u << 0)
#define	NGD_CFG_RX_MSGQ_EN	(1u << 1)
#define	NGD_CFG_TX_MSGQ_EN	(1u << 2)
#define	NGD_STATUS		0x04
#define	NGD_LADDR		(1u << 1)
#define	NGD_RX_MSGQ_CFG		0x08
#define	NGD_INT_EN		0x10
#define	NGD_INT_STAT		0x14
#define	NGD_INT_CLR		0x18
#define	NGD_TX_MSG		0x30	/* downstream: register TX, 16 words */
#define	NGD_RX_MSG		0x70	/* downstream: register RX */
#define	NGD_IE_STAT		0xf0
#define	NGD_VE_STAT		0x100
#define	NGD_INT_RECFG_DONE	(1u << 24)
#define	NGD_INT_TX_NACKED_2	(1u << 25)
#define	NGD_INT_MSG_BUF_CONTE	(1u << 26)
#define	NGD_INT_MSG_TX_INVAL	(1u << 27)
#define	NGD_INT_IE_VE_CHG	(1u << 28)
#define	NGD_INT_DEV_ERR		(1u << 29)
#define	NGD_INT_RX_MSG_RCVD	(1u << 30)
#define	NGD_INT_TX_MSG_SENT	(1u << 31)
#define	DEF_NGD_INT_MASK	(NGD_INT_TX_NACKED_2 | NGD_INT_MSG_BUF_CONTE | \
	NGD_INT_MSG_TX_INVAL | NGD_INT_IE_VE_CHG | NGD_INT_DEV_ERR | \
	NGD_INT_TX_MSG_SENT | NGD_INT_RX_MSG_RCVD)
#define	NGD_INT_ERRORS		(NGD_INT_TX_NACKED_2 | NGD_INT_MSG_BUF_CONTE | \
	NGD_INT_MSG_TX_INVAL | NGD_INT_DEV_ERR)
#define	SLIM_RX_MSGQ_TIMEOUT_VAL 0x10000
/* the NGD of the device tree's slim@1: ctrl->base + id * 0x1000 +
 * (id - 1) * 0x1000 (of_qcom_slim_ngd_register, ngd_v1_5_offset_info) */
#define	NGD_ID			1
#define	NGD_BASE		(NGD_ID * 0x1000 + (NGD_ID - 1) * 0x1000)
/* downstream slim-msm.h: the satellite's answer, a generic ACK's code */
#define	SAT_MAGIC_LSB		0xd9
#define	SAT_MAGIC_MSB		0xc5
#define	SAT_MSG_VER		0x1
#define	SAT_MSG_PROT		0x1
#define	MSM_SAT_SUCCSS		0x20

/* a SLIMbus enumeration address: Linux's struct slim_eaddr, packed */
struct slim_ea {
	uint8_t		instance;
	uint8_t		dev_index;
	uint16_t	prod_code;
	uint16_t	manf_id;
};

struct slim_rxmsg {
	uint8_t		len, mt, mc;
	uint8_t		buf[SLIM_MSGQ_BUF_LEN];
};

#define	NGD_RXQ		16

struct ngd {
	struct mmio	*m;		/* the window at the controller's base */
	uint32_t	ver;		/* the controller's version (base >> 16) */
	unsigned	timeout_ms;	/* per message (Linux: HZ) */
	uint8_t		tid;		/* the last transaction id */
	bool		up;		/* the capability exchange done */
	/* received, not yet consumed */
	struct slim_rxmsg rxq[NGD_RXQ];
	unsigned	rxn;
	unsigned	tx, rx, nacks, errors, dropped;
	uint32_t	last_err_stat;
};

int ngd_power_up(struct ngd *n);
void ngd_dump(struct ngd *n, const char *when);
int ngd_get_laddr(struct ngd *n, const struct slim_ea *ea, uint8_t *la);
int ngd_write_ve(struct ngd *n, uint8_t la, uint16_t addr, const uint8_t *val,
    unsigned count);
int ngd_read_ve(struct ngd *n, uint8_t la, uint16_t addr, uint8_t *val,
    unsigned count);
size_t ngd_build(uint8_t *msg, uint8_t mt, uint8_t mc, uint8_t la,
    const uint8_t *payload, size_t plen);

/* the FM stream on the bus: Linux's slim_stream_* with qcom-ngd-ctrl.c */
struct slim_fm {
	uint8_t		pgd_la, ifd_la;	/* the WCN3950's generic, interface devices */
	unsigned	rate, bps;	/* 48000, 16 */
	unsigned	nports;
	uint8_t		port[2];	/* WCN3950 FM TX1, TX2: 1, 2 */
	uint8_t		ch[2];		/* the channels: 160, 161 */
	bool		ports_on, connected, active;
};

/* the WCN3950: Leste's patch 0019, downstream btfm_slim_slave.[ch] */
#define	WCN399X_HW_REV_MSB		0x001
#define	WCN399X_HW_REV_LSB		0x002
#define	WCN399X_TX_PORT_CFG(p)		(0x050 + (p))
#define	WCN399X_TX_PORT_MULTI_CHNL_0(p)	(0x100 + 4 * (p))
#define	WCN399X_TX_PORT_OR_UR_CFG(p)	(0x1f0 + (p))
#define	WCN399X_PORT_ENABLE		(1u << 0)
#define	WCN399X_PORT_WM_L8		(0x8 << 1)
#define	WCN399X_OVERRUN_RECOVERY	(1u << 1)
#define	WCN399X_UNDERRUN_RECOVERY	(1u << 0)
/* the device tree's ifd@0,0 and codec@1,0, "slim217,220" */
#define	WCN3950_MANF_ID			0x217
#define	WCN3950_PROD_CODE		0x220

int slim_coef(unsigned ratem, int *exp);
int fm_ports(struct ngd *n, struct slim_fm *fm, bool on);
int fm_stream_enable(struct ngd *n, struct slim_fm *fm);
int fm_stream_disable(struct ngd *n, struct slim_fm *fm);

/* --- APR and the AFE (afe.c) ------------------------------------------ */

/* the transport: one whole APR packet per call */
struct apr_io {
	int	(*send)(void *ctx, const uint8_t *pkt, size_t len);
	/* the next packet within timeout_ms: its length, 0 none, < 0 error */
	int	(*recv)(void *ctx, uint8_t *buf, size_t size, int timeout_ms);
	void	*ctx;
};

/* include/dt-bindings/soc/qcom,apr.h, include/linux/soc/qcom/apr.h */
#define	APR_HDR_SIZE		20
#define	APR_SVC_ADSP_CORE	0x3
#define	APR_SVC_AFE		0x4
#define	APR_DOMAIN_ADSP		0x4
#define	APR_DOMAIN_APPS		0x5
#define	APR_BASIC_RSP_RESULT	0x000110e8

/* q6afe.c */
#define	AFE_PORT_CMD_DEVICE_START	0x000100e5
#define	AFE_PORT_CMD_DEVICE_STOP	0x000100e6
#define	AFE_PORT_CMD_SET_PARAM_V2	0x000100ef
#define	AFE_SVC_CMD_SET_PARAM		0x000100f3
#define	AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST	0x000100f4
#define	AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST	0x000100f5
#define	AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST	0x000100f6
#define	AFE_MODULE_AUDIO_DEV_INTERFACE	0x0001020c
#define	AFE_PARAM_ID_SLIMBUS_CONFIG	0x00010212
#define	AFE_PARAM_ID_CODEC_DMA_CONFIG	0x000102b8
#define	AFE_MODULE_CLOCK_SET		0x0001028f
#define	AFE_PARAM_ID_CLOCK_SET		0x00010290
#define	AFE_API_VERSION_SLIMBUS_CONFIG	0x1
#define	AFE_API_VERSION_CODEC_DMA_CONFIG 0x1
#define	AFE_API_VERSION_CLOCK_SET	1
#define	AFE_PORT_MAX_AUDIO_CHAN_CNT	8
#define	AFE_PORT_CONFIG_SIZE		36	/* sizeof(union afe_port_config) */
#define	AFE_CLK_TOKEN			1024
#define	AFE_PORT_ID_SLIMBUS_MULTI_CHAN_8_TX	0x4011
#define	AFE_PORT_ID_RX_CODEC_DMA_RX_0	0xb030
/* their port indices (Linux's dt-bindings qcom,q6dsp-lpass-ports.h with
 * Leste's patch 0018), the tokens q6afe.c sends */
#define	AFE_TOKEN_SLIMBUS_8_TX		152
#define	AFE_TOKEN_RX_CODEC_DMA_RX_0	113
/* the downstream q6afe.c (techpack/audio/include/dsp/apr_audio-v2.h) */
#define	AFE_MODULE_LOOPBACK		0x00010205
#define	AFE_PARAM_ID_LOOPBACK_GAIN_PER_PATH 0x00010206
#define	AFE_PARAM_ID_LOOPBACK_CONFIG	0x0001020b
#define	AFE_API_VERSION_LOOPBACK_CONFIG	0x1
#define	LB_MODE_DEFAULT			1
#define	AFE_LOOPBACK_GAIN_UNITY		0x2000	/* Q13; INT_RX_VOL_GAIN */
/* the LPASS clocks (q6afe.h) and the core blocks voted for */
#define	Q6AFE_LPASS_CLK_ID_TX_CORE_MCLK		0x30c
#define	Q6AFE_LPASS_CLK_ID_TX_CORE_NPL_MCLK	0x30d
#define	Q6AFE_LPASS_CLK_ID_RX_CORE_MCLK		0x30e
#define	Q6AFE_LPASS_CLK_ID_RX_CORE_NPL_MCLK	0x30f
#define	Q6AFE_LPASS_CORE_HW_MACRO_BLOCK		0x3
#define	Q6AFE_LPASS_CORE_HW_DCODEC_BLOCK	0x4
#define	LPASS_CLK_ATTRIBUTE_COUPLE_NO		0x1
#define	Q6AFE_LPASS_CLK_ROOT_DEFAULT		0
/* q6core.c */
#define	AVCS_CMD_ADSP_EVENT_GET_STATE		0x0001290c
#define	AVCS_CMDRSP_ADSP_EVENT_GET_STATE	0x0001290d

struct afe {
	struct apr_io	*io;
	int		timeout_ms;	/* q6afe.c: TIMEOUT_MS 3000 */
	uint32_t	last_status;
	unsigned	events;		/* packets nobody waited for */
};

size_t apr_hdr(uint8_t *pkt, uint8_t svc, uint32_t opcode, uint32_t token,
    size_t len);
size_t afe_pkt_port_set_param(uint8_t *pkt, uint16_t port, uint32_t token,
    uint32_t module, uint32_t param, const uint8_t *data, uint16_t psize);
size_t afe_pkt_svc_set_param(uint8_t *pkt, uint32_t token, uint32_t module,
    uint32_t param, const uint8_t *data, uint16_t psize);
size_t afe_pkt_port_cmd(uint8_t *pkt, uint32_t opcode, uint16_t port,
    uint32_t token);
size_t afe_cfg_slimbus(uint8_t *cfg, unsigned rate, unsigned bits,
    unsigned nch, const uint8_t *ch);
size_t afe_cfg_cdc_dma(uint8_t *cfg, unsigned rate, unsigned bits,
    unsigned nch);
size_t afe_cfg_loopback(uint8_t *cfg, uint16_t dst_port, bool enable);
size_t afe_cfg_loopback_gain(uint8_t *cfg, uint16_t port, uint16_t gain);
size_t afe_cfg_clock(uint8_t *cfg, uint32_t clk_id, uint32_t freq,
    uint16_t attri, uint16_t root);
size_t afe_pkt_vote(uint8_t *pkt, uint32_t block, const char *client,
    bool vote, uint32_t handle);

int adsp_ready(struct afe *a, int timeout_ms);
int afe_port_start(struct afe *a, uint16_t port, uint32_t token,
    uint32_t cfg_param, const uint8_t *cfg);
int afe_port_stop(struct afe *a, uint16_t port, uint32_t token);
int afe_loopback(struct afe *a, uint16_t tx_port, uint32_t token,
    uint16_t rx_port, bool enable);
int afe_loopback_gain(struct afe *a, uint16_t tx_port, uint32_t token,
    uint16_t gain);
int afe_clock(struct afe *a, uint32_t clk_id, uint32_t freq);
int afe_vote(struct afe *a, uint32_t block, const char *client,
    uint32_t *handle);
int afe_unvote(struct afe *a, uint32_t block, uint32_t handle);

/* little endian helpers */
static inline void
le16enc_(uint8_t *p, uint16_t v)
{
	p[0] = v & 0xff;
	p[1] = v >> 8;
}

static inline void
le32enc_(uint8_t *p, uint32_t v)
{
	le16enc_(p, v & 0xffff);
	le16enc_(p + 2, v >> 16);
}

static inline uint16_t
le16dec_(const uint8_t *p)
{
	return ((uint16_t)(p[0] | p[1] << 8));
}

static inline uint32_t
le32dec_(const uint8_t *p)
{
	return ((uint32_t)le16dec_(p) | (uint32_t)le16dec_(p + 2) << 16);
}

#endif /* _FMAUDIO_H_ */
