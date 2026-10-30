/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-audiod: FreeBSD's sound on the Pro1X in user space
 * (freebsd/audio/README.md). It owns every packet to the ADSP's audio
 * services (/dev/apr) and the codec's registers (/dev/qcom_lpass, the
 * SoundWire masters behind them, the AW87339s on I2C):
 *
 *   apr.c     APR packets and transactions; packets nobody waits for go
 *             to the dispatcher (the ASM data events, voice)
 *   q6.c      the ADSP's services as Linux's q6asm.c, q6adm.c, q6routing.c
 *             send them (the AFE's packets are freebsd/fm's afe.c)
 *   pcm.c     snd_pro1x.ko's channels (/dev/pro1x_pcm) to ASM sessions
 *   codec.c   the replay sequences (freebsd/audio/seq): register writes of
 *             the macros, the WCD9370, the AW87339s, GPIOs, rails, clocks
 *   swr.c     the SoundWire masters (Linux's drivers/soundwire/qcom.c)
 *   voice.c   the voice session for calls (q6voice: MVM, CVP)
 *   main.c    the loop, the back ends, the control socket
 *
 * As in freebsd/fm, register windows and the APR transport are small
 * interfaces (fmaudio.h's struct mmio and struct apr_io), so that the host
 * tests (freebsd/audio/test) run the same code against simulations.
 */

#ifndef _AUDIOD_H_
#define	_AUDIOD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../../fm/tools/pro1x-fmaudio/fmaudio.h"
#include "../../sys/arm64/qualcomm/audio/pro1x_pcm.h"

/* --- APR ---------------------------------------------------------------- */

#define	APR_SVC_ASM		0x7
#define	APR_SVC_ADM		0x8
#define	APR_SVC_MVM		0x9
#define	APR_SVC_CVS		0xa
#define	APR_SVC_CVP		0xb
#define	APR_PKT_MAX		4096

struct q6;
typedef void (*q6_async_t)(struct q6 *, const uint8_t *pkt, size_t len);

struct q6 {
	struct apr_io	*io;
	q6_async_t	async;		/* packets no transaction waits for */
	void		*priv;		/* the daemon's */
	int		timeout_ms;	/* per command (Linux: 1-5 s) */
	uint32_t	last_status;	/* the ADSP's, of the last refusal */
	unsigned	sent, received, async_pkts, timeouts, refused;
	bool		core_ready;
	struct afe	afe;		/* fm's afe.c on the same transport */
};

size_t apr_hdr_port(uint8_t *pkt, uint8_t svc, uint16_t src_port,
    uint16_t dest_port, uint32_t opcode, uint32_t token, size_t len);
/* the parts of a received packet */
static inline uint8_t apr_src_svc(const uint8_t *p) { return (p[4]); }
static inline uint16_t apr_dest_port(const uint8_t *p) { return (le16dec_(p + 10)); }
static inline uint16_t apr_src_port(const uint8_t *p) { return (le16dec_(p + 6)); }
static inline uint32_t apr_token(const uint8_t *p) { return (le32dec_(p + 12)); }
static inline uint32_t apr_opcode(const uint8_t *p) { return (le32dec_(p + 16)); }
size_t apr_hlen(const uint8_t *p, size_t n);	/* 0: not a packet */
/*
 * Send and wait: APR_BASIC_RSP_RESULT for the command's opcode and token,
 * or (want != 0) the packet with opcode want from the same service (into
 * rsp; a basic result then ends the wait only if it reports an error).
 * Other packets go to q->async meanwhile. 0 done, > 0 the ADSP's status,
 * < 0 -errno.
 */
int apr_txn(struct q6 *q, const uint8_t *pkt, size_t len, uint32_t want,
    uint8_t *rsp, size_t *rsplen);
int apr_send(struct q6 *q, const uint8_t *pkt, size_t len);
/* read what is there within timeout_ms and dispatch it: packets handled */
int apr_poll(struct q6 *q, int timeout_ms);
const char *q6_err(int r);	/* r as text: "done", "ADSP status 0x..", errno */

/* --- the ADSP's services (q6.c) --------------------------------------- */

/* q6asm.c */
#define	ASM_STREAM_CMD_CLOSE			0x00010bcd
#define	ASM_STREAM_CMD_FLUSH			0x00010bce
#define	ASM_SESSION_CMD_PAUSE			0x00010bd3
#define	ASM_DATA_CMD_EOS			0x00010bdb
#define	ASM_DATA_EVENT_RENDERED_EOS		0x00010c1c
#define	ASM_NULL_POPP_TOPOLOGY			0x00010c68
#define	ASM_STREAM_CMD_FLUSH_READBUFS		0x00010c09
#define	ASM_STREAM_CMD_SET_ENCDEC_PARAM		0x00010c10
#define	ASM_STREAM_POSTPROC_TOPO_ID_NONE	0x00010c68
#define	ASM_CMD_SHARED_MEM_MAP_REGIONS		0x00010d92
#define	ASM_CMDRSP_SHARED_MEM_MAP_REGIONS	0x00010d93
#define	ASM_CMD_SHARED_MEM_UNMAP_REGIONS	0x00010d94
#define	ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2	0x00010d98
#define	ASM_DATA_EVENT_WRITE_DONE_V2		0x00010d99
#define	ASM_DATA_EVENT_READ_DONE_V2		0x00010d9a
#define	ASM_PARAM_ID_ENCDEC_ENC_CFG_BLK_V2	0x00010da3
#define	ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2	0x00010da5
#define	ASM_SESSION_CMD_RUN_V2			0x00010daa
#define	ASM_DATA_CMD_WRITE_V2			0x00010dab
#define	ASM_DATA_CMD_READ_V2			0x00010dac
#define	ASM_STREAM_CMD_OPEN_WRITE_V3		0x00010db3
#define	ASM_STREAM_CMD_OPEN_READ_V3		0x00010db4
#define	ASM_END_POINT_DEVICE_MATRIX		0
#define	ADSP_MEMORY_MAP_SHMEM8_4K_POOL		3
#define	ASM_WRITE_TOKEN_LEN_SHIFT		16
#define	PCM_MAX_NUM_CHANNEL			8
#define	PCM_CHANNEL_FL				1
#define	PCM_CHANNEL_FR				2
#define	PCM_CHANNEL_FC				3
/* q6adm.c */
#define	ADM_CMD_MATRIX_MAP_ROUTINGS_V5		0x00010325
#define	ADM_CMD_DEVICE_OPEN_V5			0x00010326
#define	ADM_CMD_DEVICE_CLOSE_V5			0x00010327
#define	ADM_CMDRSP_DEVICE_OPEN_V5		0x00010329
#define	ADM_PATH_PLAYBACK			0x1
#define	ADM_PATH_LIVE_REC			0x2
#define	ADM_MATRIX_ID_AUDIO_RX			0
#define	ADM_MATRIX_ID_AUDIO_TX			1
#define	NULL_COPP_TOPOLOGY			0x00010312
#define	INVALID_COPP_ID				0xff
/* the codec DMA ports of the device tree's dai links (q6afe.c), their
 * indices in dt-bindings/sound/qcom,q6dsp-lpass-ports.h */
#define	AFE_PORT_ID_VA_CODEC_DMA_TX_0		0xb021
#define	AFE_TOKEN_VA_CODEC_DMA_TX_0		110
#define	AFE_PORT_ID_TX_CODEC_DMA_TX_3		0xb037
#define	AFE_TOKEN_TX_CODEC_DMA_TX_3		120
/* (RX_CODEC_DMA_RX_0 0xb030, token 113, and SLIMBUS_8_TX: fmaudio.h) */

void q6_map_channels(uint8_t map[PCM_MAX_NUM_CHANNEL], unsigned ch);
/* packet builders (host tested against Linux's structures) */
size_t asm_pkt_map(uint8_t *pkt, int session, int dir, uint64_t addr,
    uint32_t size);
size_t asm_pkt_unmap(uint8_t *pkt, int session, int dir, uint32_t handle);
size_t asm_pkt_open_write(uint8_t *pkt, int session, unsigned bits);
size_t asm_pkt_open_read(uint8_t *pkt, int session, unsigned bits);
size_t asm_pkt_media_fmt(uint8_t *pkt, int session, unsigned rate,
    unsigned channels, unsigned bits);
size_t asm_pkt_enc_cfg(uint8_t *pkt, int session, unsigned rate,
    unsigned channels, unsigned bits);
size_t asm_pkt_run(uint8_t *pkt, int session);
size_t asm_pkt_cmd(uint8_t *pkt, int session, uint32_t opcode);
size_t asm_pkt_write(uint8_t *pkt, int session, uint64_t addr,
    uint32_t handle, uint32_t len, uint32_t idx);
size_t asm_pkt_read(uint8_t *pkt, int session, uint64_t addr,
    uint32_t handle, uint32_t len, uint32_t idx);
size_t adm_pkt_open(uint8_t *pkt, uint16_t afe_port, unsigned port_idx,
    unsigned copp_idx, int path, unsigned rate, unsigned channels,
    unsigned bits);
size_t adm_pkt_map(uint8_t *pkt, int path, int session, uint16_t copp_id);
size_t adm_pkt_close(uint8_t *pkt, uint16_t afe_port, unsigned port_idx,
    unsigned copp_idx, uint16_t copp_id);

int q6_core_ready(struct q6 *q, int timeout_ms);
int asm_map(struct q6 *q, int session, int dir, uint64_t addr, uint32_t size,
    uint32_t *handle);
int asm_unmap(struct q6 *q, int session, int dir, uint32_t handle);
int asm_cmd(struct q6 *q, const uint8_t *pkt, size_t len, const char *what);
int adm_open(struct q6 *q, uint16_t afe_port, unsigned port_idx, int path,
    unsigned rate, unsigned channels, unsigned bits, uint16_t *copp_id);
int adm_map(struct q6 *q, int path, int session, uint16_t copp_id);
int adm_close(struct q6 *q, uint16_t afe_port, unsigned port_idx,
    uint16_t copp_id);

/* --- the PCM streams (pcm.c) ------------------------------------------ */

enum { ST_IDLE, ST_RUNNING, ST_FAILED };

struct stream {
	int		chan;		/* PRO1X_PCM_PLAY, PRO1X_PCM_REC */
	int		session;	/* ASM: 1 (MultiMedia1), 2 (MultiMedia2) */
	int		state;
	uint32_t	gen;
	uint32_t	rate, channels, bits, blksz, blkcnt;
	uint64_t	dspaddr;
	uint32_t	maphandle;
	uint16_t	copp;
	uint16_t	afe_port;	/* the back end's */
	unsigned	afe_idx;
	int		be;		/* BE_*, as of the start */
	uint32_t	hw;		/* the kernel's count of blocks done */
	uint32_t	ready;		/* playback: blocks filled from hw on */
	uint32_t	submitted;	/* blocks handed to the ADSP */
	unsigned	dones, errors, starts;
	bool		running;	/* RUN sent */
};

/* the kernel's side: /dev/pro1x_pcm or the host test's */
struct pcm_io {
	int	(*read_events)(void *ctx, struct pro1x_pcm_event *ev,
		    unsigned max);	/* > 0 events, 0 none, < 0 error */
	int	(*advance)(void *ctx, struct pro1x_pcm_advance *a);
	int	(*fail)(void *ctx, struct pro1x_pcm_advance *a);
	void	*ctx;
};

struct audiod;
void pcm_event(struct audiod *d, const struct pro1x_pcm_event *ev);
void pcm_asm_event(struct audiod *d, const uint8_t *pkt, size_t len);
void pcm_stop_all(struct audiod *d);

/* --- the codec: replay sequences (codec.c) ----------------------------- */

#define	SEQ_MAXSECT	64
#define	SEQ_MAXLINES	8192

enum seq_op {
	SEQ_W, SEQ_U, SEQ_R, SEQ_POLL, SEQ_DELAY, SEQ_GPIO, SEQ_RAIL,
	SEQ_CLOCK, SEQ_VOTE, SEQ_AFE, SEQ_SWR, SEQ_MARK, SEQ_CALL
};

struct seq_line {
	enum seq_op	op;
	int		target;		/* TGT_* */
	uint32_t	reg, mask, val, arg;
	char		*text;		/* names, marks; the variable of $... */
	int		lineno;
};

struct seq_sect {
	char		*name;
	unsigned	first, count;
};

struct seq {
	struct seq_line	*lines;
	unsigned	nlines;
	struct seq_sect	sect[SEQ_MAXSECT];
	unsigned	nsect;
	char		*file;
};

/* targets of register lines (README, "The sequences") */
enum {
	TGT_RXMACRO, TGT_TXMACRO, TGT_VAMACRO, TGT_SWR0, TGT_SWR1,
	TGT_LPASS_TLMM, TGT_LPASS_TLMM_SLEW, TGT_LPASS_AUDIOCC, TGT_LPASSCC,
	TGT_NMMIO,
	TGT_WCD = TGT_NMMIO,	/* the WCD9370's registers (SoundWire, swr0) */
	TGT_WCDRX,		/* its RX device's (swr1): SCP, DPn */
	TGT_AW0, TGT_AW1,	/* the AW87339s, I2C 0x59, 0x5a */
	TGT_N
};

int seq_load(struct seq *s, const char *path, char *err, size_t errlen);
int seq_parse(struct seq *s, const char *text, const char *name, char *err,
    size_t errlen);
void seq_free(struct seq *s);
const struct seq_sect *seq_find(const struct seq *s, const char *name);
const char *tgt_name(int t);

/* what the sequences act on: the daemon's, or the host test's */
struct codec_io {
	struct mmio	*win[TGT_NMMIO];	/* NULL: not mapped */
	int	(*sdw_write)(void *ctx, int tgt, uint32_t reg, uint8_t val);
	int	(*sdw_read)(void *ctx, int tgt, uint32_t reg, uint8_t *val);
	int	(*i2c_write)(void *ctx, int tgt, uint8_t reg, uint8_t val);
	int	(*i2c_read)(void *ctx, int tgt, uint8_t reg, uint8_t *val);
	int	(*gpio)(void *ctx, unsigned pin, int val);
	int	(*rail)(void *ctx, const char *name, bool on, uint32_t uv);
	int	(*clock)(void *ctx, uint32_t id, uint32_t hz);
	int	(*vote)(void *ctx, uint32_t block, bool on);
	int	(*afe)(void *ctx, const char *what);	/* "start", "stop" */
	int	(*swr)(void *ctx, const char *what);	/* "init swr0", ... */
	void	(*delay_us)(void *ctx, uint32_t us);
	void	*ctx;
	int	trace;		/* log every action */
	/* a line to leave out (the jack holds its block): true */
	bool	(*skip)(void *ctx, const struct seq_line *l);
	/* variables for $name values: the volume's */
	int	(*var)(void *ctx, const char *name, uint32_t *val);
};

int seq_run(const struct seq *s, const char *section, struct codec_io *io);

/* --- SoundWire masters (swr.c) ----------------------------------------- */

#define	SWR_NPORTS	8

struct swr_port {	/* the device tree's qcom,ports-* for one port */
	uint8_t		si, off1, off2, bp_mode, hstart, hstop, word_length,
			blk_group_count, lane_control;
};

struct swr {
	const char	*name;		/* "swr0", "swr1" */
	struct mmio	*m;
	uint32_t	version;
	unsigned	din, dout;	/* data ports (device tree) */
	struct swr_port	port[SWR_NPORTS];
	unsigned	nports;
	unsigned	wr_fifo_depth, rd_fifo_depth;
	uint8_t		cmd_id;		/* the command FIFO's last id */
	uint8_t		devnum;		/* the WCD9370's device number, 0: none */
	uint64_t	slave_id;	/* its enumeration id (the 48-bit one) */
	unsigned	timeout_ms;
	unsigned	writes, reads, errors;
	bool		up;
};

int swr_init(struct swr *w);
int swr_enumerate(struct swr *w, uint8_t unique_id, unsigned wait_ms);
int swr_write(struct swr *w, uint8_t dev, uint16_t reg, uint8_t val);
int swr_read(struct swr *w, uint8_t dev, uint16_t reg, uint8_t *val);
/* the stream's ports on master and slave, then the bank switch */
int swr_stream(struct swr *w, const uint8_t *ports, unsigned nports,
    const uint8_t *chmask, bool on);
void swr_dump(struct swr *w, const char *why);

/* --- the headset jack (mbhc.c): the WCD9370's MBHC ---------------------- */

enum { MBHC_PLUG_NONE, MBHC_PLUG_HEADPHONE, MBHC_PLUG_HEADSET,
	MBHC_PLUG_HIGH_HPH, MBHC_PLUG_GND_MIC_SWAP };

struct mbhc_io {
	int	(*read)(void *ctx, uint16_t reg, uint8_t *val);	/* the WCD */
	int	(*write)(void *ctx, uint16_t reg, uint8_t val);
	void	(*sleep_ms)(void *ctx, unsigned ms);
	uint64_t (*now_ms)(void *ctx);
	void	(*report)(void *ctx, int plug);
	void	(*button)(void *ctx, unsigned mask, bool pressed);
	void	*ctx;
};

struct mbhc {
	const struct mbhc_io *io;
	bool		up, in_swch, is_btn_press, extn_cable_hph_rem;
	bool		correcting, c_spl_hs, long_reported;
	int		plug;		/* MBHC_PLUG_* reported */
	int		c_plug, c_hs_thr, c_micbias_mv, c_swap_cnt, last_mv;
	uint8_t		irq_enabled;	/* DIGITAL_INTR_STATUS_0 bits watched */
	unsigned	micb_ref, buttons;	/* bit n: SND_JACK_BTN_n */
	uint64_t	t_report, c_until, c_next, long_at;
	unsigned	errors, polls, irqs, reports, adc_failures;
};

const char *mbhc_plug_name(int plug);
int mbhc_adc_mv(unsigned val, int micbias_mv);
int mbhc_plug_from_mv(int mv, int hph_thr, int hs_thr);
int mbhc_init(struct mbhc *m);
int mbhc_tick(struct mbhc *m);	/* < 0 error, 1 an interrupt handled */

/* --- the daemon (backend.c, main.c) ----------------------------------- */

#ifndef nitems
#define	nitems(x)	(sizeof(x) / sizeof((x)[0]))
#endif

/* the back ends: an AFE codec DMA port and the codec path behind it */
enum { BE_RX, BE_VA, BE_TX, BE_N };

struct backend {
	const char	*name;		/* "rx", "va", "tx" */
	uint16_t	afe_port;
	unsigned	afe_idx;	/* the port's index: its token */
	unsigned	channels;	/* the port's (sm8250_be_hw_params_fixup) */
	int		users;
	bool		up, port_started;
	bool		codec;		/* its <backend>-up ran: the macro clocked */
	uint64_t	idle_at;	/* power down then; 0: not pending */
	char		route[16];	/* the codec path it is up for */
};

struct audiod {
	struct q6	q;
	struct pcm_io	*pcm;
	struct stream	st[PRO1X_PCM_NCHAN];
	struct backend	be[BE_N];
	struct backend	*cur;		/* whose sequence runs ("afe start") */
	struct seq	seq;
	bool		have_seq;
	struct codec_io	cio;
	struct swr	swr[2];		/* swr0 (TX, the WCD's registers), swr1 */
	char		route[16];	/* playback: spk, hph, ear */
	char		recroute[16];	/* capture: mic, amic */
	unsigned	idle_ms;	/* back end power-down delay */
	bool		powered;	/* the "power" section ran */
	unsigned	mix[25];	/* OSS mixer levels (left), SOUND_MIXER_* */
	uint32_t	recsrc;
	bool		fm_on;
	uint16_t	fm_gain;
	unsigned	blocks_out, blocks_in;
	struct voice	*voice;
	/* the jack (backend.c's jack_*) */
	struct mbhc	mbhc;
	struct mbhc_io	mbio;
	bool		jack_enabled;	/* -J turns it off */
	bool		jack_on;	/* [jack-up] ran, MBHC polled */
	bool		in_jack_seq;	/* [jack-up]/[jack-down] running */
	uint32_t	jack_regs[256];	/* target << 24 | reg the jack holds */
	unsigned	njack_regs;
	uint8_t		jack_swr;	/* 1 << master the jack holds */
	int		voice_be;	/* the voice session's mic back end */
	unsigned	clk_ref[0x20], vote_ref[8];
	/* events for watchers, devd and evdev (main.c's) */
	void		(*notify)(struct audiod *, const char *event);
};

const char *eff_route(struct audiod *d);
const char *eff_recroute(struct audiod *d);
int jack_start(struct audiod *d);
void jack_stop(struct audiod *d);
void jack_report(void *ctx, int plug);
void jack_button(void *ctx, unsigned mask, bool pressed);
int voice_up(struct audiod *d, struct voice *v);
int voice_down(struct audiod *d, struct voice *v);

int backend_get(struct audiod *d, int be);
void backend_put(struct audiod *d, int be);
void backend_tick(struct audiod *d);
int backend_route(struct audiod *d, const char *route);
void backend_mixer(struct audiod *d, unsigned dev, unsigned left);
int backend_fm(struct audiod *d, bool on);
void backend_init(struct audiod *d);
int afe_cmd(struct q6 *q, const uint8_t *pkt, size_t len, const char *what);

/* --- voice (voice.c): Linux's q6voice as KolibriOS runs it --------------- */

enum { VP_IDLE, VP_UP, VP_FAILED };

struct voice {
	int		state;
	uint16_t	mvm, cvp;	/* the sessions' handles (ports) */
	uint16_t	rx_port, tx_port;
	uint32_t	token;
	int		failed_step;
	uint32_t	failed_status;
};

size_t voice_pkt(uint8_t *pkt, int step, const struct voice *v);
int voice_start(struct q6 *q, struct voice *v);
int voice_stop(struct q6 *q, struct voice *v);

#endif /* _AUDIOD_H_ */
