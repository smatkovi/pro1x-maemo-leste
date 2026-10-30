/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The SM6115's IPA v4.2 (IP Accelerator) and its GSI: the modem's data path
 * (freebsd/data/README.md). A port of Linux 7.1's drivers/net/ipa for this
 * one version: the configuration of ipa_data-v4.2.c, the register sequences
 * of ipa_main.c, gsi.c, gsi_trans.c, ipa_endpoint.c, ipa_cmd.c, ipa_mem.c,
 * ipa_table.c, ipa_resource.c, ipa_uc.c, ipa_smp2p.c, ipa_qmi.c and
 * ipa_modem.c, with Linux's names where a function does what Linux's does.
 *
 *   gsi.c        rings, channels, event rings, commands, the GSI interrupt,
 *                transactions
 *   ipa_hw.c     the IPA's configuration (ipa_config), its setup with
 *                immediate commands (ipa_setup), endpoints, receive buffers,
 *                transmit, the microcontroller, the modem's start and crash
 *   ipa_qmi.c    the QMI handshake with the modem (IPA service 0x31)
 *   qmap.c       QMAP (de)aggregation and the MAPv4 checksum offload
 *   ipa_fbsd.c   FreeBSD: newbus, RPM votes, SMMU, SMP2P, the firmware's
 *                device, the network interfaces, sysctls
 *
 * The core (gsi.c, ipa_hw.c, qmap.c) uses the system only through the
 * ipa_os_* functions below and ipa_os.h, so that the tests on the PC run it
 * against a model of the hardware.
 */

#ifndef _IPA_VAR_H_
#define	_IPA_VAR_H_

#include "ipa_os.h"
#include "ipa_regs.h"

/* ipa_endpoint.h's names of the endpoints */
enum ipa_ep_name {
	IPA_EP_AP_COMMAND_TX,
	IPA_EP_AP_LAN_RX,
	IPA_EP_AP_MODEM_TX,
	IPA_EP_AP_MODEM_RX,
	IPA_EP_MODEM_COMMAND_TX,
	IPA_EP_MODEM_LAN_RX,
	IPA_EP_MODEM_AP_TX,
	IPA_EP_MODEM_AP_RX,
	IPA_EP_NAMES,
};

#define	GSI_EE_AP		0
#define	GSI_EE_MODEM		1
#define	GSI_CHANNEL_COUNT_MAX	28	/* gsi.h */
#define	GSI_EVT_RING_COUNT_MAX	28
#define	GSI_MHI_EVENT_ID_START	10	/* gsi.c: reserved event ids */
#define	GSI_MHI_EVENT_ID_END	16
#define	IPA_ENDPOINT_MAX	32	/* endpoint ids before IPA v5.0 */
#define	IPA_COMMAND_TRANS_TRE_MAX 8	/* gsi_trans.h */
#define	IPA_MTU			1500	/* ipa_endpoint.h: ETH_DATA_LEN */
#define	IPA_AGGR_GRANULARITY	500	/* microseconds */
#define	IPA_NET_SKB_PAD		64	/* Linux arm64's NET_SKB_PAD */
#define	IPA_MEM_MAX		8192	/* ipa_mem.h: 2 * PAGE_SIZE */
#define	IPA_PAS_ID		15	/* ipa_main.c */

/* One endpoint of ipa_data-v4.2.c */
struct ipa_ep_data {
	uint8_t		ee;
	uint8_t		channel;
	uint8_t		endpoint;
	bool		toward_ipa;
	/* channel (AP endpoints) */
	uint16_t	tre_count;
	uint16_t	event_count;
	uint8_t		tlv_count;
	/* endpoint */
	bool		filter_support;
	uint8_t		resource_group;
	bool		checksum;
	bool		qmap;
	bool		aggregation;
	bool		status_enable;
	bool		dma_mode;
	enum ipa_ep_name dma_endpoint;
	/* TX */
	uint8_t		seq_type;
	uint8_t		seq_rep_type;
	enum ipa_ep_name status_endpoint;
	/* RX */
	uint32_t	buffer_size;
	uint32_t	pad_align;
	uint32_t	aggr_time_limit;	/* microseconds */
	bool		aggr_close_eof;
};
extern const struct ipa_ep_data ipa_ep_data_v4_2[IPA_EP_NAMES];

/* IPA-resident memory regions (ipa_mem.h's ids, ipa_data-v4.2.c's layout) */
enum ipa_mem_id {
	IPA_MEM_UC_SHARED,
	IPA_MEM_UC_INFO,
	IPA_MEM_V4_FILTER_HASHED,
	IPA_MEM_V4_FILTER,
	IPA_MEM_V6_FILTER_HASHED,
	IPA_MEM_V6_FILTER,
	IPA_MEM_V4_ROUTE_HASHED,
	IPA_MEM_V4_ROUTE,
	IPA_MEM_V6_ROUTE_HASHED,
	IPA_MEM_V6_ROUTE,
	IPA_MEM_MODEM_HEADER,
	IPA_MEM_MODEM_PROC_CTX,
	IPA_MEM_AP_PROC_CTX,
	IPA_MEM_PDN_CONFIG,
	IPA_MEM_STATS_QUOTA_MODEM,
	IPA_MEM_STATS_TETHERING,
	IPA_MEM_MODEM,
	IPA_MEM_END_MARKER,
	IPA_MEM_COUNT,
};
struct ipa_mem {
	uint32_t	offset;
	uint16_t	size;
	uint16_t	canary_count;
};
extern const struct ipa_mem ipa_mem_v4_2[IPA_MEM_COUNT];

/* DMA memory: one contiguous block the hardware reaches at pa */
struct ipa_dma {
	void		*va;
	uint64_t	pa;
	size_t		size;
	void		*os;		/* the system's (bus_dma tag and map) */
};
#define	IPA_DMA_UNCACHED	0x1	/* rings, commands, tables */
#define	IPA_SYNC_TODEV		1	/* before the hardware reads or writes */
#define	IPA_SYNC_FROMDEV	2	/* after the hardware wrote */

/* A GSI transaction: one to IPA_COMMAND_TRANS_TRE_MAX TREs */
struct gsi_trans {
	uint32_t	first;		/* its first TRE (ring index, monotonic) */
	uint8_t		ntre;
	bool		done;
	bool		cancelled;
	uint32_t	len;		/* RX: bytes received */
	void		*cookie;	/* TX: the system's packet; RX: buffer */
};

struct ipa;

/* An AP channel and its event ring */
struct gsi_channel {
	struct ipa	*ipa;
	bool		used;
	bool		toward_ipa;
	bool		command;
	uint8_t		id;
	uint8_t		evt_ring_id;
	uint16_t	tre_count;
	uint16_t	event_count;
	uint8_t		tlv_count;	/* TREs in a transaction at most */
	uint32_t	tre_max;	/* TREs outstanding at most */
	struct ipa_dma	ring;		/* the TREs */
	struct ipa_dma	evring;		/* its event ring */
	ipa_mutex_t	lock;		/* all below */
	uint32_t	tre_index;	/* the next TRE to fill (monotonic) */
	uint32_t	evt_index;	/* the next event to look at (monotonic) */
	uint32_t	tre_avail;
	struct gsi_trans *trans;	/* tre_count of them, a FIFO: */
	uint32_t	trans_head;	/* the oldest outstanding (monotonic) */
	uint32_t	trans_tail;	/* the next to allocate (monotonic) */
	uint16_t	*map;		/* last TRE -> transaction index + 1 */
	bool		started;
	/* counters */
	uint64_t	n_trans, n_events, n_polls, n_cancelled;
};

/* An endpoint (ipa_endpoint.h's struct ipa_endpoint) */
struct ipa_endpoint {
	struct ipa	*ipa;
	const struct ipa_ep_data *data;
	enum ipa_ep_name name;
	uint8_t		ee;
	uint8_t		channel_id;
	uint8_t		endpoint_id;
	bool		toward_ipa;
	bool		set_up;
	bool		enabled;
	/* RX: the receive buffers, recycled (the system copies out) */
	struct ipa_dma	*rxbuf;
	uint32_t	rxbuf_count;
	uint32_t	rx_post_len;	/* buffer_size - NET_SKB_PAD, as Linux */
	bool		replenish;
	/* counters */
	uint64_t	rx_buffers, rx_bytes, rx_dropped, tx_packets, tx_bytes;
};

/* the modem's state (ipa_modem.c) */
enum ipa_modem_state {
	IPA_MODEM_STATE_STOPPED,
	IPA_MODEM_STATE_STARTING,
	IPA_MODEM_STATE_RUNNING,
	IPA_MODEM_STATE_STOPPING,
};

struct ipa {
	void		*os;		/* the system's softc */
	uint32_t	version;	/* 0x42 */
	/* memory (ipa_mem.c) */
	const struct ipa_mem *mem;
	uint32_t	mem_offset;	/* SHARED_MEM_SIZE's base */
	uint32_t	mem_size;	/* the "ipa-shared" region */
	struct ipa_dma	zero;		/* IPA_MEM_MAX zeros */
	/* tables (ipa_table.c) */
	struct ipa_dma	table;
	uint32_t	filter_count, route_count, modem_route_count;
	uint32_t	filtered;	/* endpoints with filtering */
	/* GSI (gsi.c) */
	uint32_t	channel_count, evt_ring_count;
	uint32_t	event_bitmap;
	uint32_t	modem_channel_bitmap;
	uint32_t	type_enabled_bitmap;
	uint32_t	ieob_enabled_bitmap;
	ipa_mutex_t	irq_lock;	/* the two bitmaps above */
	ipa_sleeplock_t	cmd_lock;	/* one GSI command at a time */
	ipa_sleeplock_t	imm_lock;	/* one immediate command transaction */
	struct ipa_completion gsi_done;	/* a GSI command completed */
	int		generic_result;
	struct gsi_channel channel[GSI_CHANNEL_COUNT_MAX];
	struct gsi_channel *evt_channel[GSI_EVT_RING_COUNT_MAX];
	struct ipa_dma	cmd_payload;	/* one per TRE of the command channel */
	/* endpoints */
	struct ipa_endpoint endpoint[IPA_ENDPOINT_MAX];
	struct ipa_endpoint *name_map[IPA_EP_NAMES];
	struct ipa_endpoint *channel_map[GSI_CHANNEL_COUNT_MAX];
	uint32_t	available;	/* FLAVOR_0's endpoints */
	uint32_t	modem_tx_count;
	struct ipa_completion pipeline_done; /* the tagged packet came back */
	/* interrupts (ipa_interrupt.c) */
	uint32_t	irq_enabled;
	uint32_t	suspend_enabled;
	/* the microcontroller (ipa_uc.c) */
	bool		uc_loaded;
	/* state */
	bool		configured;	/* ipa_config done */
	bool		setup_complete;	/* ipa_setup done */
	int		modem_state;
	uint32_t	rx_buffers_max;	/* receive buffers per RX endpoint */
	/* errors seen (for the device runs' logs) */
	uint64_t	n_gsi_errors, n_gsi_general, n_cmd_timeouts, n_polled_done;
};

#define	IPA_REGION_REG		0	/* "ipa-reg" */
#define	IPA_REGION_SHARED	1	/* "ipa-shared" */
#define	IPA_REGION_GSI		2	/* "gsi" */

/* --- the system's side (ipa_fbsd.c; the tests' model) ------------------------------- */
uint32_t ipa_os_read(struct ipa *, int region, uint32_t off);
void	ipa_os_write(struct ipa *, int region, uint32_t off, uint32_t val);
int	ipa_os_dma_alloc(struct ipa *, struct ipa_dma *, size_t size,
	    size_t align, int flags);
void	ipa_os_dma_free(struct ipa *, struct ipa_dma *);
void	ipa_os_dma_sync(struct ipa *, struct ipa_dma *, int how);
void	ipa_os_log(struct ipa *, const char *fmt, ...)
	    __attribute__((__format__(__printf__, 2, 3)));
/* the channel has events: poll it (gsi_channel_poll) from a task */
void	ipa_os_poll_schedule(struct ipa *, struct gsi_channel *);
/* a receive buffer of the modem's RX endpoint: aggregated QMAP frames */
void	ipa_os_rx(struct ipa *, struct ipa_endpoint *, const uint8_t *buf,
	    uint32_t len);
/* a transmitted packet (cookie of gsi_tx) is done with */
void	ipa_os_tx_done(struct ipa *, struct ipa_endpoint *, void *cookie);
/* the modem's data path is up (the QMI handshake), or gone */
void	ipa_os_modem_up(struct ipa *);
void	ipa_os_modem_down(struct ipa *);
/* the modem is gone: its next boot gets a fresh SMP2P answer
 * (ipa_smp2p_notify_reset) */
void	ipa_os_modem_gone(struct ipa *);
/* is the IPA powered (the SMP2P clock query's answer) */
bool	ipa_os_powered(struct ipa *);
/* work that may sleep, on the driver's own thread: ipa_work_run(ipa, w) */
enum ipa_work {
	IPA_WORK_INIT_DRIVER,		/* the QMI INIT_DRIVER request */
	IPA_WORK_MODEM_GONE,		/* the modem's node said BYE */
	IPA_WORK_COUNT,
};
void	ipa_os_schedule(struct ipa *, enum ipa_work);
void	ipa_work_run(struct ipa *, enum ipa_work);

static inline uint32_t
ipa_rd(struct ipa *ipa, uint32_t off)
{
	return (ipa_os_read(ipa, IPA_REGION_REG, off));
}
static inline void
ipa_wr(struct ipa *ipa, uint32_t off, uint32_t v)
{
	ipa_os_write(ipa, IPA_REGION_REG, off, v);
}
static inline uint32_t
gsi_rd(struct ipa *ipa, uint32_t off)
{
	return (ipa_os_read(ipa, IPA_REGION_GSI, off));
}
static inline void
gsi_wr(struct ipa *ipa, uint32_t off, uint32_t v)
{
	ipa_os_write(ipa, IPA_REGION_GSI, off, v);
}

/* --- gsi.c ----------------------------------------------------------------------- */
int	gsi_init(struct ipa *);
void	gsi_exit(struct ipa *);
int	gsi_setup(struct ipa *);
void	gsi_teardown(struct ipa *);
void	gsi_isr(struct ipa *);
int	gsi_channel_start(struct ipa *, uint32_t channel_id);
int	gsi_channel_stop(struct ipa *, uint32_t channel_id);
void	gsi_channel_reset(struct ipa *, uint32_t channel_id, bool doorbell);
void	gsi_modem_channel_flow_control(struct ipa *, uint32_t channel_id,
	    bool enable);
/* the channel's completed transactions to their endpoint; IEOB on again */
void	gsi_channel_poll(struct gsi_channel *);
/* transactions */
struct gsi_seg {
	uint64_t	pa;
	uint32_t	len;
	uint16_t	opcode;		/* command channel: IPA_CMD_* (with options) */
};
int	gsi_submit(struct gsi_channel *, const struct gsi_seg *, int nseg,
	    void *cookie, uint32_t *first, bool doorbell);
void	gsi_doorbell(struct gsi_channel *);
uint32_t gsi_channel_tre_free(struct gsi_channel *);
bool	gsi_channel_idle(struct gsi_channel *);

/* --- ipa_hw.c ---------------------------------------------------------------------- */
int	ipa_init(struct ipa *);		/* no hardware access */
void	ipa_exit(struct ipa *);
int	ipa_config(struct ipa *);	/* registers; power must be on */
void	ipa_deconfig(struct ipa *);
int	ipa_setup(struct ipa *);	/* after the GSI firmware runs */
void	ipa_teardown(struct ipa *);
void	ipa_interrupt(struct ipa *);	/* the "ipa" interrupt */
const struct ipa_mem *ipa_mem_find(struct ipa *, enum ipa_mem_id);
int	ipa_endpoint_enable_one(struct ipa_endpoint *);
void	ipa_endpoint_disable_one(struct ipa_endpoint *);
/* the transaction's endpoint side (gsi.c calls these) */
void	ipa_endpoint_trans_complete(struct ipa_endpoint *, struct gsi_trans *);
/* TX of a QMAP frame on AP_MODEM_TX: segments, cookie for ipa_os_tx_done */
int	ipa_modem_tx(struct ipa *, const struct gsi_seg *, int nseg,
	    void *cookie, bool more);
int	ipa_modem_start(struct ipa *);
int	ipa_modem_stop(struct ipa *);
void	ipa_modem_crashed(struct ipa *);
/* the commands of the modem's start (ipa_mem_zero_modem) */
int	ipa_mem_zero_modem(struct ipa *);

/* --- ipa_qmi.c --------------------------------------------------------------------- */
int	ipa_qmi_setup(struct ipa *);
void	ipa_qmi_teardown(struct ipa *);
void	ipa_qmi_init_driver_work(struct ipa *);
void	ipa_qmi_modem_gone_work(struct ipa *);
/* the handshake's state, for sysctls and the device runs' logs */
struct ipa_qmi_state {
	bool		initial_boot, modem_ready, uc_ready;
	bool		indication_requested, indication_sent;
	uint32_t	modem_node, modem_port;	/* the modem's IPA server */
	int		init_driver_result;	/* -1: not yet */
	uint32_t	rsp_ctrl_endpoint, rsp_default_endpoint;
	uint32_t	n_bye;
};
const struct ipa_qmi_state *ipa_qmi_state(struct ipa *);

#endif /* _IPA_VAR_H_ */
