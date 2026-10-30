/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The IPA v4.2 itself, after Linux 7.1's drivers/net/ipa: its configuration
 * (data/ipa_data-v4.2.c), ipa_config() (ipa_main.c: the hardware, memory,
 * interrupts, microcontroller, endpoints, tables, resources), ipa_setup()
 * with the immediate commands of ipa_cmd.c, ipa_mem.c and ipa_table.c, the
 * endpoints (ipa_endpoint.c), the "ipa" interrupt (ipa_interrupt.c,
 * ipa_uc.c) and the modem's start and crash (ipa_modem.c). Linux's order and
 * values throughout; where this differs, the comment says why.
 */

#include "ipa_var.h"

/* --- data/ipa_data-v4.2.c ------------------------------------------------------- */

const struct ipa_ep_data ipa_ep_data_v4_2[IPA_EP_NAMES] = {
	[IPA_EP_AP_COMMAND_TX] = {
		.ee = GSI_EE_AP, .channel = 1, .endpoint = 6, .toward_ipa = true,
		.tre_count = 256, .event_count = 256, .tlv_count = 20,
		.resource_group = 0, .dma_mode = true,
		.dma_endpoint = IPA_EP_AP_LAN_RX, .seq_type = IPA_SEQ_DMA,
	},
	[IPA_EP_AP_LAN_RX] = {
		.ee = GSI_EE_AP, .channel = 2, .endpoint = 8, .toward_ipa = false,
		.tre_count = 256, .event_count = 256, .tlv_count = 6,
		.resource_group = 0, .aggregation = true, .status_enable = true,
		.buffer_size = 8192, .pad_align = 2 /* ilog2(sizeof(u32)) */,
		.aggr_time_limit = 500,
	},
	[IPA_EP_AP_MODEM_TX] = {
		.ee = GSI_EE_AP, .channel = 0, .endpoint = 1, .toward_ipa = true,
		.tre_count = 512, .event_count = 512, .tlv_count = 8,
		.filter_support = true, .resource_group = 0, .checksum = true,
		.qmap = true, .status_enable = true,
		.seq_type = IPA_SEQ_1_PASS_SKIP_LAST_UC,
		.seq_rep_type = IPA_SEQ_REP_DMA_PARSER,
		.status_endpoint = IPA_EP_MODEM_AP_RX,
	},
	[IPA_EP_AP_MODEM_RX] = {
		.ee = GSI_EE_AP, .channel = 3, .endpoint = 9, .toward_ipa = false,
		.tre_count = 256, .event_count = 256, .tlv_count = 6,
		.resource_group = 0, .checksum = true, .qmap = true,
		.aggregation = true, .buffer_size = 8192,
		.aggr_time_limit = 500, .aggr_close_eof = true,
	},
	[IPA_EP_MODEM_COMMAND_TX] = {
		.ee = GSI_EE_MODEM, .channel = 1, .endpoint = 5, .toward_ipa = true,
	},
	[IPA_EP_MODEM_LAN_RX] = {
		.ee = GSI_EE_MODEM, .channel = 3, .endpoint = 11, .toward_ipa = false,
	},
	[IPA_EP_MODEM_AP_TX] = {
		.ee = GSI_EE_MODEM, .channel = 0, .endpoint = 4, .toward_ipa = true,
		.filter_support = true,
	},
	[IPA_EP_MODEM_AP_RX] = {
		.ee = GSI_EE_MODEM, .channel = 2, .endpoint = 10, .toward_ipa = false,
	},
};

const struct ipa_mem ipa_mem_v4_2[IPA_MEM_COUNT] = {
	[IPA_MEM_UC_SHARED]		= { 0x0000, 0x0080, 0 },
	[IPA_MEM_UC_INFO]		= { 0x0080, 0x0200, 0 },
	[IPA_MEM_V4_FILTER_HASHED]	= { 0x0288, 0, 2 },
	[IPA_MEM_V4_FILTER]		= { 0x0290, 0x0078, 2 },
	[IPA_MEM_V6_FILTER_HASHED]	= { 0x0310, 0, 2 },
	[IPA_MEM_V6_FILTER]		= { 0x0318, 0x0078, 2 },
	[IPA_MEM_V4_ROUTE_HASHED]	= { 0x0398, 0, 2 },
	[IPA_MEM_V4_ROUTE]		= { 0x03a0, 0x0078, 2 },
	[IPA_MEM_V6_ROUTE_HASHED]	= { 0x0420, 0, 2 },
	[IPA_MEM_V6_ROUTE]		= { 0x0428, 0x0078, 2 },
	[IPA_MEM_MODEM_HEADER]		= { 0x04a8, 0x0140, 2 },
	[IPA_MEM_MODEM_PROC_CTX]	= { 0x05f0, 0x0200, 2 },
	[IPA_MEM_AP_PROC_CTX]		= { 0x07f0, 0x0200, 0 },
	[IPA_MEM_PDN_CONFIG]		= { 0x09f8, 0x0050, 2 },
	[IPA_MEM_STATS_QUOTA_MODEM]	= { 0x0a50, 0x0060, 2 },
	[IPA_MEM_STATS_TETHERING]	= { 0x0ab0, 0x0140, 0 },
	[IPA_MEM_MODEM]			= { 0x0bf0, 0x140c, 0 },
	[IPA_MEM_END_MARKER]		= { 0x2000, 0, 1 },
};

/* source and destination resource limits of group 0 (min, max) */
static const uint8_t ipa_rsrc_src_v4_2[][2] = {
	{ 3, 63 },	/* SRC_PKT_CONTEXTS */
	{ 3, 3 },	/* SRC_DESCRIPTOR_LISTS */
	{ 10, 10 },	/* SRC_DESCRIPTOR_BUFF */
	{ 1, 1 },	/* SRC_HPS_DMARS */
	{ 5, 5 },	/* SRC_ACK_ENTRIES */
};
static const uint8_t ipa_rsrc_dst_v4_2[][2] = {
	{ 3, 3 },	/* DST_DATA_SECTORS */
	{ 1, 63 },	/* DST_DPS_DMARS */
};

#define	IPA_QSB_MAX_WRITES_V4_2		8
#define	IPA_QSB_MAX_READS_V4_2		12
#define	IPA_MODEM_ROUTE_COUNT_V4_2	8
#define	IPA_TIMER_FREQUENCY		32000
#define	IPA_MEM_CANARY_VAL		0xdeadbeefU
#define	IPA_CMD_SLOT			32	/* bytes per command payload */
#define	QCOM_SMEM_HOST_MODEM		1
#define	SMEM_IPA_FILTER_TABLE		497

const struct ipa_mem *
ipa_mem_find(struct ipa *ipa, enum ipa_mem_id id)
{
	return (id < IPA_MEM_COUNT ? &ipa->mem[id] : NULL);
}

static struct ipa_endpoint *
ipa_ep(struct ipa *ipa, enum ipa_ep_name name)
{
	return (ipa->name_map[name]);
}

/* --- immediate commands (ipa_cmd.c) ------------------------------------------------ */

struct ipa_cmd_trans {
	struct ipa	*ipa;
	struct gsi_channel *ch;
	struct gsi_seg	seg[IPA_COMMAND_TRANS_TRE_MAX];
	int		n;
	uint32_t	first;		/* the TRE (and payload slot) of seg[0] */
};

static union ipa_cmd_payload *
ipa_cmd_slot(struct ipa *ipa, uint32_t tre, uint64_t *pa)
{
	struct gsi_channel *ch = &ipa->channel[ipa_ep(ipa,
	    IPA_EP_AP_COMMAND_TX)->channel_id];
	uint32_t i = tre % ch->tre_count;

	*pa = ipa->cmd_payload.pa + (uint64_t)i * IPA_CMD_SLOT;
	return ((union ipa_cmd_payload *)((char *)ipa->cmd_payload.va +
	    i * IPA_CMD_SLOT));
}

/* A command transaction (ipa_cmd_trans_alloc): the imm_lock is taken */
static void
ipa_cmd_begin(struct ipa *ipa, struct ipa_cmd_trans *t)
{
	memset(t, 0, sizeof(*t));
	t->ipa = ipa;
	t->ch = &ipa->channel[ipa_ep(ipa, IPA_EP_AP_COMMAND_TX)->channel_id];
	ipa_slock(&ipa->imm_lock);
	ipa_lock(&t->ch->lock);
	t->first = t->ch->tre_index;
	ipa_unlock(&t->ch->lock);
}

/* The next payload of the transaction, zeroed (gsi_trans_cmd_add) */
static union ipa_cmd_payload *
ipa_cmd_add(struct ipa_cmd_trans *t, uint16_t opcode, uint32_t len)
{
	union ipa_cmd_payload *p;
	uint64_t pa;

	static union ipa_cmd_payload overflow;

	if (t->n >= IPA_COMMAND_TRANS_TRE_MAX) {
		/* not with the transactions here; never sent */
		ipa_os_log(t->ipa, "too many immediate commands\n");
		return (&overflow);
	}
	p = ipa_cmd_slot(t->ipa, t->first + t->n, &pa);
	memset(p, 0, IPA_CMD_SLOT);
	t->seg[t->n].pa = pa;
	t->seg[t->n].len = len;
	t->seg[t->n].opcode = opcode;
	t->n++;
	return (p);
}

/*
 * Commit and wait (gsi_trans_commit_wait). The command channel's TREs have
 * BEI set, as on Linux; its events are looked at here as well as on the
 * interrupt, so a missing interrupt costs time, not the command.
 */
static int
ipa_cmd_commit_wait(struct ipa_cmd_trans *t)
{
	struct ipa *ipa = t->ipa;
	int error = 0, ms;

	if (t->n == 0) {
		ipa_sunlock(&ipa->imm_lock);
		return (0);
	}
	ipa_wmb();
	error = gsi_submit(t->ch, t->seg, t->n, NULL, NULL, true);
	if (error != 0) {
		ipa_os_log(ipa, "immediate commands not submitted: %d\n", error);
		ipa_sunlock(&ipa->imm_lock);
		return (error);
	}
	for (ms = 0; ms < 1000; ms++) {
		gsi_channel_poll(t->ch);
		if (gsi_channel_idle(t->ch))
			break;
		ipa_msleep(1);
	}
	if (!gsi_channel_idle(t->ch)) {
		ipa_os_log(ipa, "immediate commands (%d, opcode %#x) not done "
		    "after 1 s\n", t->n, t->seg[0].opcode);
		ipa->n_cmd_timeouts++;
		error = ETIMEDOUT;
	} else if (ms > 0)
		ipa->n_polled_done++;
	ipa_sunlock(&ipa->imm_lock);
	return (error);
}

/* IPA_CMD_IP_V{4,6}_{FILTER,ROUTING}_INIT; hash_size 0: no hashed table */
static void
ipa_cmd_table_init_add(struct ipa_cmd_trans *t, uint16_t opcode, uint16_t size,
    uint32_t offset, uint64_t addr, uint16_t hash_size, uint32_t hash_offset,
    uint64_t hash_addr)
{
	struct ipa *ipa = t->ipa;
	union ipa_cmd_payload *p;
	uint64_t val;

	p = ipa_cmd_add(t, opcode, sizeof(p->table_init));
	offset += ipa->mem_offset;
	val = (uint64_t)(offset & 0xffff) << IP_FLTRT_FLAGS_NHASH_ADDR_SHIFT;
	val |= (uint64_t)(size & 0xfff) << IP_FLTRT_FLAGS_NHASH_SIZE_SHIFT;
	if (hash_size != 0) {
		hash_offset += ipa->mem_offset;
		val |= (uint64_t)(hash_offset & 0xffff) <<
		    IP_FLTRT_FLAGS_HASH_ADDR_SHIFT;
		val |= (uint64_t)(hash_size & 0xfff) <<
		    IP_FLTRT_FLAGS_HASH_SIZE_SHIFT;
		p->table_init.hash_rules_addr = ipa_le64(hash_addr);
	}
	p->table_init.flags = ipa_le64(val);
	p->table_init.nhash_rules_addr = ipa_le64(addr);
}

/* IPA_CMD_HDR_INIT_LOCAL: where the header tables are; written from addr */
static void
ipa_cmd_hdr_init_local_add(struct ipa_cmd_trans *t, uint32_t offset,
    uint16_t size, uint64_t addr)
{
	union ipa_cmd_payload *p;

	p = ipa_cmd_add(t, IPA_CMD_HDR_INIT_LOCAL, sizeof(p->hdr_init_local));
	offset += t->ipa->mem_offset;
	p->hdr_init_local.hdr_table_addr = ipa_le64(addr);
	p->hdr_init_local.flags = ipa_le32(IPA_FIELD(HDR_INIT_LOCAL_FLAGS_TABLE_SIZE,
	    size) | IPA_FIELD(HDR_INIT_LOCAL_FLAGS_HDR_ADDR, offset));
}

/* IPA_CMD_REGISTER_WRITE; IPA v4.0+ has the pipeline option in the opcode */
static void
ipa_cmd_register_write_add(struct ipa_cmd_trans *t, uint32_t offset,
    uint32_t value, uint32_t mask, bool clear_full)
{
	union ipa_cmd_payload *p;
	uint16_t opcode;

	opcode = IPA_CMD_REGISTER_WRITE | IPA_FIELD(REGISTER_WRITE_OPCODE_CLEAR_OPTION,
	    clear_full ? PIPELINE_CLEAR_FULL : PIPELINE_CLEAR_HPS);
	p = ipa_cmd_add(t, opcode, sizeof(p->register_write));
	p->register_write.flags = ipa_le16(IPA_FIELD(REGISTER_WRITE_FLAGS_OFFSET_HIGH,
	    (offset >> 16) & 0xf));
	p->register_write.offset = ipa_le16((uint16_t)offset);
	p->register_write.value = ipa_le32(value);
	p->register_write.value_mask = ipa_le32(mask);
	p->register_write.clear_options = 0;
}

static void
ipa_cmd_ip_packet_init_add(struct ipa_cmd_trans *t, uint8_t endpoint_id)
{
	union ipa_cmd_payload *p;

	p = ipa_cmd_add(t, IPA_CMD_IP_PACKET_INIT, sizeof(p->ip_packet_init));
	p->ip_packet_init.dest_endpoint = IPA_FIELD(IPA_PACKET_INIT_DEST_ENDPOINT,
	    endpoint_id);
}

/* IPA_CMD_DMA_SHARED_MEM: a block of IPA memory written from (or to) addr */
static void
ipa_cmd_dma_shared_mem_add(struct ipa_cmd_trans *t, uint32_t offset,
    uint16_t size, uint64_t addr, bool toward_ipa)
{
	union ipa_cmd_payload *p;

	p = ipa_cmd_add(t, IPA_CMD_DMA_SHARED_MEM, sizeof(p->dma_shared_mem));
	offset += t->ipa->mem_offset;
	p->dma_shared_mem.size = ipa_le16(size);
	p->dma_shared_mem.local_addr = ipa_le16((uint16_t)offset);
	p->dma_shared_mem.flags = ipa_le16(toward_ipa ? 0 :
	    DMA_SHARED_MEM_FLAGS_DIRECTION);
	p->dma_shared_mem.system_addr = ipa_le64(addr);
}

/* The pipeline clear (ipa_cmd_pipeline_clear_add): four commands */
static void
ipa_cmd_pipeline_clear_add(struct ipa_cmd_trans *t)
{
	union ipa_cmd_payload *p;

	ipa_completion_reinit(&t->ipa->pipeline_done);
	/* a no-op register write (mask 0), the full pipeline clear */
	ipa_cmd_register_write_add(t, 0, 0, 0, true);
	/* the next packet straight to the exception endpoint, tagged */
	ipa_cmd_ip_packet_init_add(t, ipa_ep(t->ipa, IPA_EP_AP_LAN_RX)->endpoint_id);
	p = ipa_cmd_add(t, IPA_CMD_IP_PACKET_TAG_STATUS,
	    sizeof(p->ip_packet_tag_status));
	p->ip_packet_tag_status.tag = 0;
	/* and that packet: a zeroed payload */
	(void)ipa_cmd_add(t, IPA_CMD_NONE, sizeof(union ipa_cmd_payload));
}

/* --- memory (ipa_mem.c) -------------------------------------------------------------- */

static void
ipa_mem_zero_region_add(struct ipa_cmd_trans *t, enum ipa_mem_id id)
{
	const struct ipa_mem *mem = ipa_mem_find(t->ipa, id);

	if (mem->size == 0)
		return;
	ipa_cmd_dma_shared_mem_add(t, mem->offset, mem->size, t->ipa->zero.pa,
	    true);
}

/* Header memory, the processing contexts, the modem's memory zeroed */
static int
ipa_mem_setup(struct ipa *ipa)
{
	struct ipa_cmd_trans t;
	const struct ipa_mem *mem;
	int error;

	ipa_cmd_begin(ipa, &t);
	/* no AP header region on IPA v4.2: the modem's alone */
	mem = ipa_mem_find(ipa, IPA_MEM_MODEM_HEADER);
	ipa_cmd_hdr_init_local_add(&t, mem->offset, mem->size, ipa->zero.pa);
	ipa_mem_zero_region_add(&t, IPA_MEM_MODEM_PROC_CTX);
	ipa_mem_zero_region_add(&t, IPA_MEM_AP_PROC_CTX);
	ipa_mem_zero_region_add(&t, IPA_MEM_MODEM);
	if ((error = ipa_cmd_commit_wait(&t)) != 0)
		return (error);
	/* where the processing context area is */
	mem = ipa_mem_find(ipa, IPA_MEM_MODEM_PROC_CTX);
	ipa_wr(ipa, IPA_LOCAL_PKT_PROC_CNTXT, IPA_FIELD(LOCAL_PKT_PROC_BASE_ADDR,
	    ipa->mem_offset + mem->offset));
	return (0);
}

int
ipa_mem_zero_modem(struct ipa *ipa)
{
	struct ipa_cmd_trans t;

	ipa_cmd_begin(ipa, &t);
	ipa_mem_zero_region_add(&t, IPA_MEM_MODEM_HEADER);
	ipa_mem_zero_region_add(&t, IPA_MEM_MODEM_PROC_CTX);
	ipa_mem_zero_region_add(&t, IPA_MEM_MODEM);
	return (ipa_cmd_commit_wait(&t));
}

/* SHARED_MEM_SIZE, then the canaries before the regions (ipa_mem_config) */
static int
ipa_mem_config(struct ipa *ipa)
{
	uint32_t val, size, off, mapped = ipa->mem_size;
	int i, n;

	val = ipa_rd(ipa, IPA_SHARED_MEM_SIZE);
	ipa->mem_offset = 8 * IPA_GET(MEM_BADDR, val);
	size = 8 * IPA_GET(MEM_SIZE, val);
	ipa_os_log(ipa, "shared memory: offset %#x, size %#x (mapped %#x)\n",
	    ipa->mem_offset, size, mapped);
	if (ipa->mem_offset + size < ipa->mem_size)
		ipa->mem_size = size;
	for (i = 0; i < IPA_MEM_COUNT; i++) {
		const struct ipa_mem *mem = &ipa->mem[i];

		if (mem->offset + mem->size > ipa->mem_size) {
			ipa_os_log(ipa, "region %d ends beyond the memory "
			    "limit (%#x)\n", i, ipa->mem_size);
			return (EINVAL);
		}
		/* Linux writes the canaries at mem_offset + offset - 4n */
		if (ipa->mem_offset + mem->offset > mapped) {
			ipa_os_log(ipa, "region %d is beyond the mapping\n", i);
			return (EINVAL);
		}
	}
	for (i = 0; i < IPA_MEM_COUNT; i++) {
		off = ipa->mem_offset + ipa->mem[i].offset;
		for (n = 1; n <= ipa->mem[i].canary_count; n++)
			ipa_os_write(ipa, IPA_REGION_SHARED, off - 4 * n,
			    IPA_MEM_CANARY_VAL);
	}
	return (0);
}

/* --- tables (ipa_table.c) ---------------------------------------------------------- */

/* the zero rule, the filter bitmap, then the zero rule's address */
static uint64_t
ipa_table_addr(struct ipa *ipa, bool filter_mask, uint16_t count)
{
	if (count == 0)
		return (0);
	return (ipa->table.pa + (filter_mask ? 1 : 2) * sizeof(uint64_t));
}

static const struct ipa_mem *
ipa_table_mem(struct ipa *ipa, bool filter, bool hashed, bool ipv6)
{
	enum ipa_mem_id id;

	id = filter ? (hashed ? (ipv6 ? IPA_MEM_V6_FILTER_HASHED :
	    IPA_MEM_V4_FILTER_HASHED) : (ipv6 ? IPA_MEM_V6_FILTER :
	    IPA_MEM_V4_FILTER)) : (hashed ? (ipv6 ? IPA_MEM_V6_ROUTE_HASHED :
	    IPA_MEM_V4_ROUTE_HASHED) : (ipv6 ? IPA_MEM_V6_ROUTE :
	    IPA_MEM_V4_ROUTE));
	return (ipa_mem_find(ipa, id));
}

static int
ipa_table_init(struct ipa *ipa)
{
	uint64_t *v;
	uint32_t count;
	size_t size;
	int error;

	count = MAX(ipa->filter_count, ipa->route_count);
	size = 8 + (1 + count) * sizeof(uint64_t);
	error = ipa_os_dma_alloc(ipa, &ipa->table, size, 128, IPA_DMA_UNCACHED);
	if (error != 0)
		return (error);
	v = ipa->table.va;
	*v++ = 0;				/* the zero rule */
	*v++ = ipa_le64((uint64_t)ipa->filtered << 1); /* bit 0: global */
	while (count--)
		*v++ = ipa_le64(ipa->table.pa);
	ipa_os_dma_sync(ipa, &ipa->table, IPA_SYNC_TODEV);
	return (0);
}

static void
ipa_table_init_add(struct ipa_cmd_trans *t, bool filter, bool ipv6)
{
	struct ipa *ipa = t->ipa;
	const struct ipa_mem *mem, *hash_mem;
	uint16_t count, hash_count, size, hash_size, opcode;
	uint32_t hash_offset;

	opcode = filter ? (ipv6 ? IPA_CMD_IP_V6_FILTER_INIT :
	    IPA_CMD_IP_V4_FILTER_INIT) : (ipv6 ? IPA_CMD_IP_V6_ROUTING_INIT :
	    IPA_CMD_IP_V4_ROUTING_INIT);
	mem = ipa_table_mem(ipa, filter, false, ipv6);
	hash_mem = ipa_table_mem(ipa, filter, true, ipv6);
	hash_offset = hash_mem != NULL ? hash_mem->offset : 0;
	if (filter) {
		count = 1 + __builtin_popcount(ipa->filtered);
		hash_count = hash_mem != NULL && hash_mem->size ? count : 0;
	} else {
		count = mem->size / sizeof(uint64_t);
		hash_count = hash_mem != NULL ? hash_mem->size / sizeof(uint64_t) : 0;
	}
	size = count * sizeof(uint64_t);
	hash_size = hash_count * sizeof(uint64_t);
	ipa_cmd_table_init_add(t, opcode, size, mem->offset,
	    ipa_table_addr(ipa, filter, count), hash_size, hash_offset,
	    ipa_table_addr(ipa, filter, hash_count));
	if (!filter)
		return;
	/* the filter table's unused rest zeroed */
	ipa_cmd_dma_shared_mem_add(t, mem->offset + size, mem->size - size,
	    ipa->zero.pa, true);
	if (hash_size == 0)
		return;
	ipa_cmd_dma_shared_mem_add(t, hash_offset + hash_size,
	    hash_mem->size - hash_size, ipa->zero.pa, true);
}

static int
ipa_table_setup(struct ipa *ipa)
{
	struct ipa_cmd_trans t;

	ipa_cmd_begin(ipa, &t);
	ipa_table_init_add(&t, false, false);
	ipa_table_init_add(&t, false, true);
	ipa_table_init_add(&t, true, false);
	ipa_table_init_add(&t, true, true);
	return (ipa_cmd_commit_wait(&t));
}

/* Entries of the AP's or the modem's back to the zero rule (no hashing) */
static void
ipa_table_reset_add(struct ipa_cmd_trans *t, bool filter, bool ipv6,
    uint16_t first, uint16_t count)
{
	const struct ipa_mem *mem = ipa_table_mem(t->ipa, filter, false, ipv6);

	if (mem == NULL || mem->size == 0)
		return;
	if (filter)
		first++;	/* over the bitmap */
	ipa_cmd_dma_shared_mem_add(t, mem->offset + first * sizeof(uint64_t),
	    count * sizeof(uint64_t), ipa_table_addr(t->ipa, false, count), true);
}

static void
ipa_table_reset(struct ipa *ipa, bool modem)
{
	struct ipa_cmd_trans t;
	uint32_t mask, id;
	uint8_t ee = modem ? GSI_EE_MODEM : GSI_EE_AP;
	int v6, error;
	uint16_t first, count;

	for (v6 = 0; v6 <= 1; v6++) {
		ipa_cmd_begin(ipa, &t);
		for (mask = ipa->filtered; mask != 0; mask &= ~IPA_BIT(id)) {
			id = __builtin_ctz(mask);
			if (ipa->endpoint[id].ee == ee)
				ipa_table_reset_add(&t, true, v6, id, 1);
		}
		if ((error = ipa_cmd_commit_wait(&t)) != 0)
			ipa_os_log(ipa, "error %d resetting the %s filter table "
			    "for the %s\n", error, v6 ? "IPv6" : "IPv4",
			    modem ? "modem" : "AP");
	}
	if (modem) {
		first = 0;
		count = ipa->modem_route_count;
	} else {
		first = ipa->modem_route_count;
		count = ipa->route_count - ipa->modem_route_count;
	}
	ipa_cmd_begin(ipa, &t);
	ipa_table_reset_add(&t, false, false, first, count);
	ipa_table_reset_add(&t, false, true, first, count);
	if ((error = ipa_cmd_commit_wait(&t)) != 0)
		ipa_os_log(ipa, "error %d resetting the route table for the %s\n",
		    error, modem ? "modem" : "AP");
}

/* --- interrupts (ipa_interrupt.c) and the microcontroller (ipa_uc.c) ------------- */

static void
ipa_interrupt_enable(struct ipa *ipa, enum ipa_irq_id id)
{
	ipa->irq_enabled |= IPA_BIT(id);
	ipa_wr(ipa, IPA_IRQ_EN, ipa->irq_enabled);
}

static void
ipa_interrupt_disable(struct ipa *ipa, enum ipa_irq_id id)
{
	ipa->irq_enabled &= ~IPA_BIT(id);
	ipa_wr(ipa, IPA_IRQ_EN, ipa->irq_enabled);
}

static void
ipa_interrupt_suspend_control(struct ipa *ipa, uint32_t endpoint_id,
    bool enable)
{
	uint32_t was = ipa->suspend_enabled, val;

	if (was == IPA_BIT(endpoint_id) && !enable)
		ipa_interrupt_disable(ipa, IPA_IRQ_TX_SUSPEND);
	val = ipa_rd(ipa, IPA_IRQ_SUSPEND_EN);
	if (enable) {
		val |= IPA_BIT(endpoint_id);
		ipa->suspend_enabled |= IPA_BIT(endpoint_id);
	} else {
		val &= ~IPA_BIT(endpoint_id);
		ipa->suspend_enabled &= ~IPA_BIT(endpoint_id);
	}
	ipa_wr(ipa, IPA_IRQ_SUSPEND_EN, val);
	if (was == 0 && enable)
		ipa_interrupt_enable(ipa, IPA_IRQ_TX_SUSPEND);
}

static void
ipa_uc_event_handler(struct ipa *ipa)
{
	uint32_t base = ipa->mem_offset + ipa->mem[IPA_MEM_UC_SHARED].offset;
	uint32_t ev = ipa_os_read(ipa, IPA_REGION_SHARED, base +
	    IPA_UC_OFF_EVENT) & 0xff;

	if (ev == IPA_UC_EVENT_ERROR)
		ipa_os_log(ipa, "microcontroller error event\n");
	else if (ev != IPA_UC_EVENT_LOG_INFO)
		ipa_os_log(ipa, "unsupported microcontroller event %u\n", ev);
}

static void
ipa_uc_response_handler(struct ipa *ipa)
{
	uint32_t base = ipa->mem_offset + ipa->mem[IPA_MEM_UC_SHARED].offset;
	uint32_t rsp = ipa_os_read(ipa, IPA_REGION_SHARED, base +
	    IPA_UC_OFF_RESPONSE) & 0xff;

	/*
	 * INIT_COMPLETED once it runs (the modem loads it). Linux drops the
	 * proxy power vote of the modem's start here; the IPA is powered all
	 * the time on FreeBSD.
	 */
	if (rsp == IPA_UC_RESPONSE_INIT_COMPLETED) {
		if (!ipa->uc_loaded)
			ipa_os_log(ipa, "microcontroller running\n");
		ipa->uc_loaded = true;
	} else
		ipa_os_log(ipa, "unsupported microcontroller response %u\n", rsp);
}

/* The "ipa" interrupt (ipa_isr_thread) */
void
ipa_interrupt(struct ipa *ipa)
{
	uint32_t pending, mask, id;

	if (!ipa->configured)
		return;
	pending = ipa_rd(ipa, IPA_IRQ_STTS);
	while ((mask = pending & ipa->irq_enabled) != 0) {
		do {
			id = __builtin_ctz(mask);
			mask &= ~IPA_BIT(id);
			switch (id) {
			case IPA_IRQ_UC_0:
			case IPA_IRQ_UC_1:
				ipa_wr(ipa, IPA_IRQ_CLR, IPA_BIT(id));
				if (id == IPA_IRQ_UC_0)
					ipa_uc_event_handler(ipa);
				else
					ipa_uc_response_handler(ipa);
				break;
			case IPA_IRQ_TX_SUSPEND: {
				uint32_t v = ipa_rd(ipa, IPA_IRQ_SUSPEND_INFO);

				if (v != 0)
					ipa_wr(ipa, IPA_IRQ_SUSPEND_CLR, v);
				ipa_wr(ipa, IPA_IRQ_CLR, IPA_BIT(id));
				break;
			}
			default:
				ipa_wr(ipa, IPA_IRQ_CLR, IPA_BIT(id));
				break;
			}
		} while (mask != 0);
		pending = ipa_rd(ipa, IPA_IRQ_STTS);
	}
	if (pending != 0)
		ipa_wr(ipa, IPA_IRQ_CLR, pending);
}

/* --- endpoints (ipa_endpoint.c) ------------------------------------------------------ */

#define	ENDP(ep, reg)	IPA_ENDP(reg, (ep)->endpoint_id)

static void
ipa_endpoint_init_cfg(struct ipa_endpoint *ep)
{
	uint32_t val = 0, enabled = IPA_CS_OFFLOAD_NONE;

	if (ep->data->checksum) {
		if (ep->toward_ipa) {
			/* the checksum header's offset in 4-byte units: after MAP */
			val |= IPA_FIELD(CFG_CS_METADATA_HDR_OFFSET, 4 / 4);
			enabled = IPA_CS_OFFLOAD_UL;
		} else
			enabled = IPA_CS_OFFLOAD_DL;
	}
	val |= IPA_FIELD(CFG_CS_OFFLOAD_EN, enabled);
	ipa_wr(ep->ipa, ENDP(ep, IPA_ENDP_INIT_CFG), val);
}

static void
ipa_endpoint_init_nat(struct ipa_endpoint *ep)
{
	if (!ep->toward_ipa)
		return;
	ipa_wr(ep->ipa, ENDP(ep, IPA_ENDP_INIT_NAT), IPA_FIELD(NAT_EN,
	    IPA_NAT_TYPE_BYPASS));
}

/*
 * QMAP endpoints: a MAP header before each packet (with the MAPv4 checksum
 * header on TX); on RX the IPA writes the mux id (metadata, offset 1) and
 * the length (offset 2) into it
 */
static void
ipa_endpoint_init_hdr(struct ipa_endpoint *ep)
{
	uint32_t val = 0, header_size;

	if (ep->data->qmap) {
		header_size = 4;			/* rmnet_map_header */
		if (ep->data->checksum && ep->toward_ipa)
			header_size += 4;		/* rmnet_map_ul_csum_header */
		val = IPA_FIELD(HDR_LEN, header_size);
		if (!ep->toward_ipa) {
			val |= IPA_FIELD(HDR_OFST_METADATA, 1);	/* mux_id */
			val |= HDR_OFST_PKT_SIZE_VALID;
			val |= IPA_FIELD(HDR_OFST_PKT_SIZE, 2);	/* pkt_len */
		}
		val |= HDR_OFST_METADATA_VALID;
	}
	ipa_wr(ep->ipa, ENDP(ep, IPA_ENDP_INIT_HDR), val);
}

static void
ipa_endpoint_init_hdr_ext(struct ipa_endpoint *ep)
{
	uint32_t val = 0;

	if (ep->data->qmap) {
		val |= HDR_ENDIANNESS;			/* big endian */
		if (!ep->toward_ipa) {
			val |= HDR_TOTAL_LEN_OR_PAD_VALID;
			val |= HDR_PAYLOAD_LEN_INC_PADDING;
		}
	}
	if (!ep->toward_ipa)
		val |= IPA_FIELD(HDR_PAD_TO_ALIGNMENT, ep->data->pad_align);
	ipa_wr(ep->ipa, ENDP(ep, IPA_ENDP_INIT_HDR_EXT), val);
}

static void
ipa_endpoint_init_hdr_metadata_mask(struct ipa_endpoint *ep)
{
	uint32_t val = 0;

	if (ep->toward_ipa)
		return;
	/* the mux id in the metadata's low byte: cpu_to_be32(0xff) */
	if (ep->data->qmap)
		val = 0xff000000U;
	ipa_wr(ep->ipa, ENDP(ep, IPA_ENDP_INIT_HDR_METADATA_MASK), val);
}

static void
ipa_endpoint_init_mode(struct ipa_endpoint *ep)
{
	uint32_t val;

	if (!ep->toward_ipa)
		return;
	if (ep->data->dma_mode)
		val = IPA_FIELD(MODE_ENDP_MODE, IPA_MODE_DMA) |
		    IPA_FIELD(MODE_DEST_PIPE_INDEX,
		    ipa_ep(ep->ipa, ep->data->dma_endpoint)->endpoint_id);
	else
		val = IPA_FIELD(MODE_ENDP_MODE, IPA_MODE_BASIC);
	ipa_wr(ep->ipa, ENDP(ep, IPA_ENDP_INIT_MODE), val);
}

/* ipa_aggr_size_kb() with a soft limit: room for an MTU after it */
static uint32_t
ipa_aggr_size_kb(uint32_t rx_buffer_size)
{
	/* IPA_RX_BUFFER_OVERHEAD on Linux arm64: skb_shared_info + NET_SKB_PAD */
	return ((rx_buffer_size - (IPA_MTU + 320 + IPA_NET_SKB_PAD)) / 1024);
}

static void
ipa_endpoint_init_aggr(struct ipa_endpoint *ep)
{
	uint32_t val = 0, ticks;

	if (ep->data->aggregation) {
		if (!ep->toward_ipa) {
			val |= IPA_FIELD(AGGR_EN, IPA_ENABLE_AGGR);
			val |= IPA_FIELD(AGGR_TYPE, IPA_AGGR_GENERIC);
			val |= IPA_FIELD(AGGR_BYTE_LIMIT, ipa_aggr_size_kb(
			    ep->data->buffer_size - IPA_NET_SKB_PAD));
			ticks = (ep->data->aggr_time_limit +
			    IPA_AGGR_GRANULARITY / 2) / IPA_AGGR_GRANULARITY;
			if (ep->data->aggr_time_limit)
				val |= IPA_FIELD(AGGR_TIME_LIMIT, ticks);
			if (ep->data->aggr_close_eof)
				val |= AGGR_SW_EOF_ACTIVE;
		} else {
			val |= IPA_FIELD(AGGR_EN, IPA_ENABLE_DEAGGR);
			val |= IPA_FIELD(AGGR_TYPE, IPA_AGGR_QCMAP);
		}
	} else
		val |= IPA_FIELD(AGGR_EN, IPA_BYPASS_AGGR);
	ipa_wr(ep->ipa, ENDP(ep, IPA_ENDP_INIT_AGGR), val);
}

static void
ipa_endpoint_init_hol_block_en(struct ipa_endpoint *ep, bool enable)
{
	ipa_wr(ep->ipa, ENDP(ep, IPA_ENDP_INIT_HOL_BLOCK_EN), enable ?
	    HOL_BLOCK_EN : 0);
}

/* a 0 us timer encodes as 0 (hol_block_timer_encode) */
static void
ipa_endpoint_init_hol_block_enable(struct ipa_endpoint *ep)
{
	ipa_wr(ep->ipa, ENDP(ep, IPA_ENDP_INIT_HOL_BLOCK_TIMER), 0);
	ipa_endpoint_init_hol_block_en(ep, true);
}

static void
ipa_endpoint_init_deaggr(struct ipa_endpoint *ep)
{
	if (!ep->toward_ipa)
		return;
	ipa_wr(ep->ipa, ENDP(ep, IPA_ENDP_INIT_DEAGGR), 0);
}

static void
ipa_endpoint_init_rsrc_grp(struct ipa_endpoint *ep)
{
	ipa_wr(ep->ipa, ENDP(ep, IPA_ENDP_INIT_RSRC_GRP), IPA_FIELD(ENDP_RSRC_GRP,
	    ep->data->resource_group));
}

static void
ipa_endpoint_init_seq(struct ipa_endpoint *ep)
{
	if (!ep->toward_ipa)
		return;
	ipa_wr(ep->ipa, ENDP(ep, IPA_ENDP_INIT_SEQ), IPA_FIELD(SEQ_TYPE,
	    ep->data->seq_type) | IPA_FIELD(SEQ_REP_TYPE, ep->data->seq_rep_type));
}

static void
ipa_endpoint_status(struct ipa_endpoint *ep)
{
	uint32_t val = 0;

	if (ep->data->status_enable) {
		val |= STATUS_EN;
		if (ep->toward_ipa)
			val |= IPA_FIELD(STATUS_ENDP, ipa_ep(ep->ipa,
			    ep->data->status_endpoint)->endpoint_id);
		/* the status before the packet (STATUS_LOCATION 0) */
	}
	ipa_wr(ep->ipa, ENDP(ep, IPA_ENDP_STATUS), val);
}

/* ipa_endpoint_program(): on IPA v4.2 no DELAY or SUSPEND modes */
static void
ipa_endpoint_program(struct ipa_endpoint *ep)
{
	ipa_endpoint_init_cfg(ep);
	ipa_endpoint_init_nat(ep);
	ipa_endpoint_init_hdr(ep);
	ipa_endpoint_init_hdr_ext(ep);
	ipa_endpoint_init_hdr_metadata_mask(ep);
	ipa_endpoint_init_mode(ep);
	ipa_endpoint_init_aggr(ep);
	if (!ep->toward_ipa)
		ipa_endpoint_init_hol_block_en(ep, false);	/* no holb_drop */
	ipa_endpoint_init_deaggr(ep);
	ipa_endpoint_init_rsrc_grp(ep);
	ipa_endpoint_init_seq(ep);
	ipa_endpoint_status(ep);
}

/* --- receive buffers ------------------------------------------------------------------ */

static int
ipa_endpoint_post_one(struct ipa_endpoint *ep, uint32_t i, bool doorbell)
{
	struct gsi_channel *ch = &ep->ipa->channel[ep->channel_id];
	struct gsi_seg seg;

	ipa_os_dma_sync(ep->ipa, &ep->rxbuf[i], IPA_SYNC_TODEV);
	seg.pa = ep->rxbuf[i].pa;
	seg.len = ep->rx_post_len;
	seg.opcode = IPA_CMD_NONE;
	return (gsi_submit(ch, &seg, 1, (void *)(uintptr_t)(i + 1), NULL,
	    doorbell));
}

/* All buffers to the hardware (replenish, with the endpoint enabled) */
static void
ipa_endpoint_replenish_all(struct ipa_endpoint *ep)
{
	uint32_t i;
	int error;

	for (i = 0; i < ep->rxbuf_count; i++)
		if ((error = ipa_endpoint_post_one(ep, i, false)) != 0) {
			ipa_os_log(ep->ipa, "endpoint %u: buffer %u not posted "
			    "(%d)\n", ep->endpoint_id, i, error);
			break;
		}
	gsi_doorbell(&ep->ipa->channel[ep->channel_id]);
}

static uint32_t
le32_bits(const uint8_t *s, int word, int h, int l)
{
	uint32_t w;

	memcpy(&w, s + 4 * word, 4);
	return (IPA_GET(IPA_GENMASK(h, l), ipa_from_le32(w)));
}

/*
 * The exception endpoint's buffer: status entries, each before its packet
 * (ipa_endpoint_status_parse). Nothing consumes the packets (Linux has no
 * network device on this endpoint either); a tagged one from the command
 * endpoint completes a pipeline clear.
 */
static void
ipa_endpoint_status_parse(struct ipa_endpoint *ep, const uint8_t *data,
    uint32_t resid)
{
	struct ipa *ipa = ep->ipa;
	uint32_t opcode, length, align, len, dst, mask, src;

	align = ep->data->pad_align ? (1U << ep->data->pad_align) : 1;
	while (resid > 0) {
		if (resid < IPA_STATUS_SIZE) {
			ipa_os_log(ipa, "short message (%u bytes < %u byte "
			    "status)\n", resid, IPA_STATUS_SIZE);
			break;
		}
		opcode = le32_bits(data, 0, 7, 0);
		length = le32_bits(data, 1, 15, 0);
		dst = le32_bits(data, 1, 28, 24);
		if (length == 0 || dst != ep->endpoint_id ||
		    (opcode != IPA_STATUS_OPCODE_PACKET &&
		    opcode != IPA_STATUS_OPCODE_DROPPED_PACKET &&
		    opcode != IPA_STATUS_OPCODE_SUSPENDED_PACKET &&
		    opcode != IPA_STATUS_OPCODE_PACKET_2ND_PASS)) {
			data += IPA_STATUS_SIZE;
			resid -= IPA_STATUS_SIZE;
			continue;
		}
		len = IPA_STATUS_SIZE + roundup2(length, align);
		if (ep->data->checksum)
			len += 8;	/* rmnet_map_dl_csum_trailer */
		mask = le32_bits(data, 0, 31, 16);
		if (mask != 0) {
			src = le32_bits(data, 1, 20, 16);
			if (src == ipa_ep(ipa, IPA_EP_AP_COMMAND_TX)->endpoint_id)
				ipa_complete(&ipa->pipeline_done);
			else
				ipa_os_log(ipa, "unexpected tagged packet from "
				    "endpoint %u\n", src);
		} else
			ep->rx_dropped++;
		if (len > resid)
			break;
		data += len;
		resid -= len;
	}
}

/* A transaction of the endpoint is done (gsi.c's poll) */
void
ipa_endpoint_trans_complete(struct ipa_endpoint *ep, struct gsi_trans *t)
{
	struct ipa *ipa = ep->ipa;
	uint32_t i;

	if (ep->toward_ipa) {
		if (ep->name == IPA_EP_AP_MODEM_TX && t->cookie != NULL)
			ipa_os_tx_done(ipa, ep, t->cookie);
		return;
	}
	i = (uint32_t)(uintptr_t)t->cookie - 1;
	if (i >= ep->rxbuf_count)
		return;
	if (!t->cancelled && t->len > 0) {
		ipa_os_dma_sync(ipa, &ep->rxbuf[i], IPA_SYNC_FROMDEV);
		ep->rx_buffers++;
		ep->rx_bytes += t->len;
		if (ep->data->status_enable)
			ipa_endpoint_status_parse(ep, ep->rxbuf[i].va,
			    MIN(t->len, ep->rx_post_len));
		else
			ipa_os_rx(ipa, ep, ep->rxbuf[i].va,
			    MIN(t->len, ep->rx_post_len));
	}
	if (ep->replenish && ep->enabled)
		(void)ipa_endpoint_post_one(ep, i, true);
}

/* --- transmit ------------------------------------------------------------------------ */

int
ipa_modem_tx(struct ipa *ipa, const struct gsi_seg *seg, int nseg, void *cookie,
    bool more)
{
	struct ipa_endpoint *ep = ipa_ep(ipa, IPA_EP_AP_MODEM_TX);
	int error;

	if (!ep->enabled)
		return (ENXIO);
	error = gsi_submit(&ipa->channel[ep->channel_id], seg, nseg, cookie,
	    NULL, !more);
	if (error == 0)
		ep->tx_packets++;
	return (error);
}

/* --- enable, disable ---------------------------------------------------------------- */

int
ipa_endpoint_enable_one(struct ipa_endpoint *ep)
{
	struct ipa *ipa = ep->ipa;
	int error;

	if (ep->enabled)
		return (0);
	error = gsi_channel_start(ipa, ep->channel_id);
	if (error != 0) {
		ipa_os_log(ipa, "error %d starting %cX channel %u for endpoint "
		    "%u\n", error, ep->toward_ipa ? 'T' : 'R', ep->channel_id,
		    ep->endpoint_id);
		return (error);
	}
	ep->enabled = true;
	if (!ep->toward_ipa) {
		ipa_interrupt_suspend_control(ipa, ep->endpoint_id, true);
		ep->replenish = true;
		ipa_endpoint_replenish_all(ep);
	}
	return (0);
}

void
ipa_endpoint_disable_one(struct ipa_endpoint *ep)
{
	struct ipa *ipa = ep->ipa;
	int error;

	if (!ep->enabled)
		return;
	if (!ep->toward_ipa) {
		ep->replenish = false;
		ipa_interrupt_suspend_control(ipa, ep->endpoint_id, false);
	}
	error = gsi_channel_stop(ipa, ep->channel_id);
	if (error != 0)
		ipa_os_log(ipa, "error %d attempting to stop endpoint %u\n",
		    error, ep->endpoint_id);
	ep->enabled = false;
}

static void
ipa_endpoint_default_route_set(struct ipa *ipa, uint32_t endpoint_id)
{
	ipa_wr(ipa, IPA_ROUTE, IPA_FIELD(ROUTE_DEF_PIPE, endpoint_id) |
	    ROUTE_DEF_HDR_TABLE | IPA_FIELD(ROUTE_FRAG_DEF_PIPE, endpoint_id) |
	    ROUTE_DEF_RETAIN_HDR);
}

/* --- the modem (ipa_modem.c, ipa_endpoint.c's modem parts) ----------------------- */

/* the modem's TX channels flow controlled (IPA v4.2: no DELAY mode) */
static void
ipa_endpoint_modem_pause_all(struct ipa *ipa, bool enable)
{
	uint32_t i;

	for (i = 0; i < IPA_ENDPOINT_MAX; i++) {
		struct ipa_endpoint *ep = &ipa->endpoint[i];

		if (ep->data == NULL || ep->ee != GSI_EE_MODEM || !ep->toward_ipa)
			continue;
		gsi_modem_channel_flow_control(ipa, ep->channel_id, enable);
	}
}

static void
ipa_endpoint_modem_hol_block_clear_all(struct ipa *ipa)
{
	uint32_t i;

	for (i = 0; i < IPA_ENDPOINT_MAX; i++) {
		struct ipa_endpoint *ep = &ipa->endpoint[i];

		if (ep->data == NULL || ep->toward_ipa || ep->ee != GSI_EE_MODEM)
			continue;
		ipa_endpoint_init_hol_block_en(ep, false);
		ipa_endpoint_init_hol_block_enable(ep);
	}
}

/* the modem's TX endpoints without status, then a pipeline clear */
static int
ipa_endpoint_modem_exception_reset_all(struct ipa *ipa)
{
	struct ipa_cmd_trans t;
	uint32_t i;
	int error;

	ipa_cmd_begin(ipa, &t);
	for (i = 0; i < IPA_ENDPOINT_MAX; i++) {
		struct ipa_endpoint *ep = &ipa->endpoint[i];

		if (ep->data == NULL || ep->ee != GSI_EE_MODEM || !ep->toward_ipa)
			continue;
		ipa_cmd_register_write_add(&t, ENDP(ep, IPA_ENDP_STATUS), 0, ~0U,
		    false);
	}
	ipa_cmd_pipeline_clear_add(&t);
	if ((error = ipa_cmd_commit_wait(&t)) != 0)
		return (error);
	if (!ipa_completion_wait(&ipa->pipeline_done, 1000)) {
		ipa_os_log(ipa, "pipeline clear: no tagged packet\n");
		return (ETIMEDOUT);
	}
	return (0);
}

/* The handshake is complete: the modem's data path up (ipa_modem_start) */
int
ipa_modem_start(struct ipa *ipa)
{
	int error;

	if (ipa->modem_state != IPA_MODEM_STATE_STOPPED)
		return (0);
	ipa->modem_state = IPA_MODEM_STATE_STARTING;
	/* Linux enables them when rmnet_ipa0 is opened; here at once */
	error = ipa_endpoint_enable_one(ipa_ep(ipa, IPA_EP_AP_MODEM_TX));
	if (error == 0) {
		error = ipa_endpoint_enable_one(ipa_ep(ipa, IPA_EP_AP_MODEM_RX));
		if (error != 0)
			ipa_endpoint_disable_one(ipa_ep(ipa, IPA_EP_AP_MODEM_TX));
	}
	if (error != 0) {
		ipa->modem_state = IPA_MODEM_STATE_STOPPED;
		return (error);
	}
	ipa->modem_state = IPA_MODEM_STATE_RUNNING;
	ipa_os_log(ipa, "modem data path up (endpoints %u TX, %u RX)\n",
	    ipa_ep(ipa, IPA_EP_AP_MODEM_TX)->endpoint_id,
	    ipa_ep(ipa, IPA_EP_AP_MODEM_RX)->endpoint_id);
	ipa_os_modem_up(ipa);
	return (0);
}

int
ipa_modem_stop(struct ipa *ipa)
{
	if (ipa->modem_state == IPA_MODEM_STATE_STOPPED)
		return (0);
	if (ipa->modem_state != IPA_MODEM_STATE_RUNNING)
		return (EBUSY);
	ipa->modem_state = IPA_MODEM_STATE_STOPPING;
	ipa_os_modem_down(ipa);
	ipa_endpoint_disable_one(ipa_ep(ipa, IPA_EP_AP_MODEM_RX));
	ipa_endpoint_disable_one(ipa_ep(ipa, IPA_EP_AP_MODEM_TX));
	ipa->modem_state = IPA_MODEM_STATE_STOPPED;
	return (0);
}

/*
 * The modem went away (crashed or stopped: QRTR's BYE; Linux's SSR
 * notifier's BEFORE_SHUTDOWN): what ipa_modem_crashed() does, so that the
 * next start of the modem finds the IPA as on a first boot
 */
void
ipa_modem_crashed(struct ipa *ipa)
{
	int error;

	if (!ipa->setup_complete)
		return;
	ipa_endpoint_modem_pause_all(ipa, true);
	ipa_endpoint_modem_hol_block_clear_all(ipa);
	ipa_table_reset(ipa, true);
	/* no hashed tables on IPA v4.2: nothing to flush */
	if ((error = ipa_endpoint_modem_exception_reset_all(ipa)) != 0)
		ipa_os_log(ipa, "error %d resetting exception endpoint\n", error);
	ipa_endpoint_modem_pause_all(ipa, false);
	if ((error = ipa_modem_stop(ipa)) != 0)
		ipa_os_log(ipa, "error %d stopping modem\n", error);
	if ((error = ipa_mem_zero_modem(ipa)) != 0)
		ipa_os_log(ipa, "error %d zeroing modem memory regions\n", error);
	/* Linux does this when the modem starts again (BEFORE_POWERUP) */
	ipa_os_modem_gone(ipa);
}

/* --- init, config, setup ------------------------------------------------------------- */

/* No hardware access: endpoints, rings, buffers, tables (ipa_probe's init) */
int
ipa_init(struct ipa *ipa)
{
	const struct ipa_ep_data *d;
	struct ipa_endpoint *ep;
	uint32_t i, j, n;
	int error;

	ipa->version = 0x42;
	ipa->mem = ipa_mem_v4_2;
	ipa->modem_route_count = IPA_MODEM_ROUTE_COUNT_V4_2;
	/* ipa_table_mem_valid(): entries of the non-hashed tables */
	ipa->filter_count = ipa->mem[IPA_MEM_V4_FILTER].size / 8 - 1;
	ipa->route_count = ipa->mem[IPA_MEM_V4_ROUTE].size / 8;
	ipa_completion_init(&ipa->pipeline_done);
	ipa_sleeplock_init(&ipa->imm_lock, "ipaimm");
	for (i = 0; i < IPA_EP_NAMES; i++) {
		d = &ipa_ep_data_v4_2[i];
		ep = &ipa->endpoint[d->endpoint];
		ep->ipa = ipa;
		ep->data = d;
		ep->name = i;
		ep->ee = d->ee;
		ep->channel_id = d->channel;
		ep->endpoint_id = d->endpoint;
		ep->toward_ipa = d->toward_ipa;
		ipa->name_map[i] = ep;
		if (d->ee == GSI_EE_AP)
			ipa->channel_map[d->channel] = ep;
		if (d->filter_support)
			ipa->filtered |= IPA_BIT(d->endpoint);
		if (d->ee == GSI_EE_MODEM && d->toward_ipa)
			ipa->modem_tx_count++;
	}
	if ((error = gsi_init(ipa)) != 0)
		goto fail;
	error = ipa_os_dma_alloc(ipa, &ipa->zero, IPA_MEM_MAX, 4096,
	    IPA_DMA_UNCACHED);
	if (error != 0)
		goto fail_gsi;
	memset(ipa->zero.va, 0, IPA_MEM_MAX);
	ipa_os_dma_sync(ipa, &ipa->zero, IPA_SYNC_TODEV);
	if ((error = ipa_table_init(ipa)) != 0)
		goto fail_zero;
	/* the command payloads: one slot per TRE of the command channel */
	n = ipa->channel[ipa->name_map[IPA_EP_AP_COMMAND_TX]->channel_id].tre_count;
	error = ipa_os_dma_alloc(ipa, &ipa->cmd_payload, (size_t)n * IPA_CMD_SLOT,
	    4096, IPA_DMA_UNCACHED);
	if (error != 0)
		goto fail_table;
	/* receive buffers: recycled, so a fixed set per RX endpoint */
	for (i = 0; i < IPA_EP_NAMES; i++) {
		struct gsi_channel *ch;

		ep = ipa->name_map[i];
		if (ep->ee != GSI_EE_AP || ep->toward_ipa)
			continue;
		ch = &ipa->channel[ep->channel_id];
		ep->rxbuf_count = MIN(ch->tre_max, ipa->rx_buffers_max ?
		    ipa->rx_buffers_max : 128);
		ep->rx_post_len = ep->data->buffer_size - IPA_NET_SKB_PAD;
		ep->rxbuf = ipa_zalloc(ep->rxbuf_count * sizeof(ep->rxbuf[0]));
		for (j = 0; j < ep->rxbuf_count; j++) {
			error = ipa_os_dma_alloc(ipa, &ep->rxbuf[j],
			    ep->data->buffer_size, 128, 0);
			if (error != 0) {
				ipa_os_log(ipa, "endpoint %u: %u of %u receive "
				    "buffers\n", ep->endpoint_id, j,
				    ep->rxbuf_count);
				ep->rxbuf_count = j;
				if (j == 0)
					goto fail_buffers;
				break;
			}
		}
	}
	return (0);
fail_buffers:
	ipa_exit(ipa);
	return (ENOMEM);
fail_table:
	ipa_os_dma_free(ipa, &ipa->table);
fail_zero:
	ipa_os_dma_free(ipa, &ipa->zero);
fail_gsi:
	gsi_exit(ipa);
fail:
	ipa_sleeplock_destroy(&ipa->imm_lock);
	ipa_completion_destroy(&ipa->pipeline_done);
	return (error);
}

void
ipa_exit(struct ipa *ipa)
{
	uint32_t i, j;

	for (i = 0; i < IPA_EP_NAMES; i++) {
		struct ipa_endpoint *ep = ipa->name_map[i];

		if (ep == NULL || ep->rxbuf == NULL)
			continue;
		for (j = 0; j < ep->rxbuf_count; j++)
			ipa_os_dma_free(ipa, &ep->rxbuf[j]);
		ipa_free(ep->rxbuf);
		ep->rxbuf = NULL;
	}
	ipa_os_dma_free(ipa, &ipa->cmd_payload);
	ipa_os_dma_free(ipa, &ipa->table);
	ipa_os_dma_free(ipa, &ipa->zero);
	gsi_exit(ipa);
	ipa_sleeplock_destroy(&ipa->imm_lock);
	ipa_completion_destroy(&ipa->pipeline_done);
}

/* FLAVOR_0: the endpoints the hardware has, in their directions */
static int
ipa_endpoint_config(struct ipa *ipa)
{
	uint32_t val, tx_count, rx_count, rx_base, limit, i;

	val = ipa_rd(ipa, IPA_FLAVOR_0);
	tx_count = IPA_GET(FLAVOR_MAX_CONS_PIPES, val);
	rx_count = IPA_GET(FLAVOR_MAX_PROD_PIPES, val);
	rx_base = IPA_GET(FLAVOR_PROD_LOWEST, val);
	limit = rx_base + rx_count;
	ipa_os_log(ipa, "endpoints: %u TX from 0, %u RX from %u (FLAVOR_0 "
	    "%#x)\n", tx_count, rx_count, rx_base, val);
	if (limit > IPA_ENDPOINT_MAX) {
		ipa_os_log(ipa, "too many endpoints, %u > %u\n", limit,
		    IPA_ENDPOINT_MAX);
		return (EINVAL);
	}
	ipa->available = (tx_count ? IPA_GENMASK(tx_count - 1, 0) : 0) |
	    (rx_count ? IPA_GENMASK(rx_base + rx_count - 1, rx_base) : 0);
	for (i = 0; i < IPA_ENDPOINT_MAX; i++) {
		struct ipa_endpoint *ep = &ipa->endpoint[i];

		if (ep->data == NULL)
			continue;
		if (i >= limit || (ipa->available & IPA_BIT(i)) == 0) {
			ipa_os_log(ipa, "unavailable endpoint id %u\n", i);
			return (EINVAL);
		}
		if (ep->toward_ipa ? i < tx_count : i >= rx_base)
			continue;
		ipa_os_log(ipa, "endpoint id %u wrong direction\n", i);
		return (EINVAL);
	}
	return (0);
}

static void
ipa_resource_config(struct ipa *ipa)
{
	uint32_t i;

	/* one source and one destination group: the X limits of 01 only */
	for (i = 0; i < nitems(ipa_rsrc_src_v4_2); i++)
		ipa_wr(ipa, IPA_SRC_RSRC_GRP_01_RSRC_TYPE(i),
		    IPA_FIELD(RSRC_X_MIN_LIM, ipa_rsrc_src_v4_2[i][0]) |
		    IPA_FIELD(RSRC_X_MAX_LIM, ipa_rsrc_src_v4_2[i][1]));
	for (i = 0; i < nitems(ipa_rsrc_dst_v4_2); i++)
		ipa_wr(ipa, IPA_DST_RSRC_GRP_01_RSRC_TYPE(i),
		    IPA_FIELD(RSRC_X_MIN_LIM, ipa_rsrc_dst_v4_2[i][0]) |
		    IPA_FIELD(RSRC_X_MAX_LIM, ipa_rsrc_dst_v4_2[i][1]));
}

/* ipa_hardware_config() for IPA v4.2 */
static void
ipa_hardware_config(struct ipa *ipa)
{
	uint32_t val, gran;

	ipa_wr(ipa, IPA_BCR, 0);			/* backward_compat 0 */
	/* no PA mask: HOLB drops allowed (IPA v4.1 to v4.4) */
	val = ipa_rd(ipa, IPA_TX_CFG);
	ipa_wr(ipa, IPA_TX_CFG, val & ~TX_CFG_PA_MASK_EN);
	/* open global clocks (IPA v4.0 to v4.4) */
	ipa_wr(ipa, IPA_CLKON_CFG, CLKON_GLOBAL | CLKON_GLOBAL_2X_CLK);
	val = ipa_rd(ipa, IPA_COMP_CFG);
	val |= COMP_GSI_MULTI_AXI_MASTERS_DIS;
	val |= COMP_GSI_MULTI_INORDER_RD_DIS | COMP_GSI_MULTI_INORDER_WR_DIS;
	ipa_wr(ipa, IPA_COMP_CFG, val);
	/* the DDR master only; no read beat limit */
	ipa_wr(ipa, IPA_QSB_MAX_WRITES, IPA_FIELD(GEN_QMB_0_MAX_WRITES,
	    IPA_QSB_MAX_WRITES_V4_2));
	ipa_wr(ipa, IPA_QSB_MAX_READS, IPA_FIELD(GEN_QMB_0_MAX_READS,
	    IPA_QSB_MAX_READS_V4_2));
	/* the aggregation timer: 500 us of the 32 kHz clock, less one */
	gran = (IPA_AGGR_GRANULARITY * IPA_TIMER_FREQUENCY + 500000) / 1000000 - 1;
	ipa_wr(ipa, IPA_COUNTER_CFG, IPA_FIELD(AGGR_GRANULARITY, gran));
	/* IPA v4.2 has no hashed tables */
	ipa_wr(ipa, IPA_FILT_ROUT_HASH_EN, 0);
	/* dynamic clock division: idle after 256 */
	ipa_wr(ipa, IPA_IDLE_INDICATION_CFG, IPA_FIELD(ENTER_IDLE_DEBOUNCE_THRESH,
	    256));
}

/* Registers only (ipa_config); the IPA's power must be on */
int
ipa_config(struct ipa *ipa)
{
	int error;

	ipa_hardware_config(ipa);
	if ((error = ipa_mem_config(ipa)) != 0)
		return (error);
	/* ipa_interrupt_config: all off */
	ipa->irq_enabled = 0;
	ipa->suspend_enabled = 0;
	ipa_wr(ipa, IPA_IRQ_EN, 0);
	ipa->configured = true;
	/* ipa_uc_config */
	ipa->uc_loaded = false;
	ipa_interrupt_enable(ipa, IPA_IRQ_UC_0);
	ipa_interrupt_enable(ipa, IPA_IRQ_UC_1);
	if ((error = ipa_endpoint_config(ipa)) != 0)
		goto fail;
	/* ipa_table_config: nothing without hashing */
	ipa_resource_config(ipa);
	return (0);
fail:
	ipa_deconfig(ipa);
	return (error);
}

void
ipa_deconfig(struct ipa *ipa)
{
	if (!ipa->configured)
		return;
	ipa_interrupt_disable(ipa, IPA_IRQ_UC_1);
	ipa_interrupt_disable(ipa, IPA_IRQ_UC_0);
	/* ipa_hardware_dcd_deconfig: the power-on values */
	ipa_wr(ipa, IPA_IDLE_INDICATION_CFG, CONST_NON_IDLE_ENABLE);
	ipa->configured = false;
}

/* After the GSI firmware runs: GSI, endpoints, commands, the QMI side */
int
ipa_setup(struct ipa *ipa)
{
	struct ipa_endpoint *cmd, *lan;
	uint32_t i;
	int error;

	if ((error = gsi_setup(ipa)) != 0)
		return (error);
	for (i = 0; i < IPA_ENDPOINT_MAX; i++) {
		struct ipa_endpoint *ep = &ipa->endpoint[i];

		if (ep->data == NULL || ep->ee != GSI_EE_AP)
			continue;
		ipa_endpoint_program(ep);
		ep->set_up = true;
	}
	cmd = ipa_ep(ipa, IPA_EP_AP_COMMAND_TX);
	if ((error = ipa_endpoint_enable_one(cmd)) != 0)
		goto fail_teardown;
	if ((error = ipa_mem_setup(ipa)) != 0) {
		ipa_os_log(ipa, "memory setup: %d\n", error);
		goto fail_cmd;
	}
	if ((error = ipa_table_setup(ipa)) != 0) {
		ipa_os_log(ipa, "table setup: %d\n", error);
		goto fail_cmd;
	}
	lan = ipa_ep(ipa, IPA_EP_AP_LAN_RX);
	if ((error = ipa_endpoint_enable_one(lan)) != 0)
		goto fail_cmd;
	ipa_endpoint_default_route_set(ipa, lan->endpoint_id);
	if ((error = ipa_qmi_setup(ipa)) != 0) {
		ipa_os_log(ipa, "QMI setup: %d\n", error);
		goto fail_route;
	}
	ipa->setup_complete = true;
	ipa_os_log(ipa, "IPA driver setup completed successfully\n");
	return (0);
fail_route:
	ipa_endpoint_default_route_set(ipa, 0);
	ipa_endpoint_disable_one(lan);
fail_cmd:
	ipa_endpoint_disable_one(cmd);
fail_teardown:
	for (i = 0; i < IPA_ENDPOINT_MAX; i++)
		if (ipa->endpoint[i].set_up) {
			ipa->endpoint[i].set_up = false;
			gsi_channel_reset(ipa, ipa->endpoint[i].channel_id, true);
		}
	gsi_teardown(ipa);
	return (error);
}

void
ipa_teardown(struct ipa *ipa)
{
	uint32_t i;

	if (!ipa->setup_complete)
		return;
	(void)ipa_modem_stop(ipa);
	ipa->setup_complete = false;
	ipa_qmi_teardown(ipa);
	ipa_endpoint_default_route_set(ipa, 0);
	ipa_endpoint_disable_one(ipa_ep(ipa, IPA_EP_AP_LAN_RX));
	ipa_endpoint_disable_one(ipa_ep(ipa, IPA_EP_AP_COMMAND_TX));
	for (i = 0; i < IPA_ENDPOINT_MAX; i++)
		if (ipa->endpoint[i].set_up) {
			ipa->endpoint[i].set_up = false;
			gsi_channel_reset(ipa, ipa->endpoint[i].channel_id, true);
		}
	gsi_teardown(ipa);
}
