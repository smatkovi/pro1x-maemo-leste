/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The IPA's GSI (generic software interface) for IPA v4.2, after Linux 7.1's
 * drivers/net/ipa/gsi.c and gsi_trans.c: per AP endpoint a channel (a ring
 * of 16-byte transfer elements, TREs, filled by the AP) and an event ring
 * (16-byte completions written by the GSI); channel and event ring commands
 * (allocate, start, stop, reset) completed by the GSI interrupt; GENERIC
 * commands for the modem's channels (on IPA v4.2 the AP allocates them, a
 * hardware quirk Linux works around the same way).
 *
 * Transactions are simpler than Linux's: per channel a FIFO of them, each
 * one to tlv_count TREs, the last with IEOT (an event per transaction) and,
 * toward the IPA, BEI as Linux sets it. An event names the TRE that
 * finished; the map gives its transaction, which is done, and the FIFO's
 * done head goes to the endpoint (ipa_endpoint_trans_complete).
 */

#include "ipa_var.h"

#define	GSI_EVT_RING_INT_MODT	(32 * 1)	/* 1 ms of the 32 kHz timer */
#define	GSI_CMD_TIMEOUT		50		/* milliseconds */
#define	GSI_CHANNEL_STOP_RETRIES 10
#define	GSI_CHANNEL_MODEM_HALT_RETRIES 10
#define	GSI_ISR_MAX_ITER	50
#define	GSI_POLL_BUDGET		64

/* --- interrupt masks --------------------------------------------------------------- */

static void
gsi_irq_type_update(struct ipa *ipa, uint32_t val)
{
	ipa->type_enabled_bitmap = val;
	gsi_wr(ipa, GSI_CNTXT_TYPE_IRQ_MSK, val);
}

static void
gsi_irq_type_enable(struct ipa *ipa, uint32_t type)
{
	gsi_irq_type_update(ipa, ipa->type_enabled_bitmap | type);
}

static void
gsi_irq_type_disable(struct ipa *ipa, uint32_t type)
{
	gsi_irq_type_update(ipa, ipa->type_enabled_bitmap & ~type);
}

static void
gsi_irq_ev_ctrl_enable(struct ipa *ipa, uint32_t evt_ring_id)
{
	ipa_lock(&ipa->irq_lock);
	gsi_wr(ipa, GSI_CNTXT_SRC_EV_CH_IRQ_CLR, ~0U);
	gsi_wr(ipa, GSI_CNTXT_SRC_EV_CH_IRQ_MSK, IPA_BIT(evt_ring_id));
	gsi_irq_type_enable(ipa, GSI_EV_CTRL);
	ipa_unlock(&ipa->irq_lock);
}

static void
gsi_irq_ev_ctrl_disable(struct ipa *ipa)
{
	ipa_lock(&ipa->irq_lock);
	gsi_irq_type_disable(ipa, GSI_EV_CTRL);
	gsi_wr(ipa, GSI_CNTXT_SRC_EV_CH_IRQ_MSK, 0);
	ipa_unlock(&ipa->irq_lock);
}

static void
gsi_irq_ch_ctrl_enable(struct ipa *ipa, uint32_t channel_id)
{
	ipa_lock(&ipa->irq_lock);
	gsi_wr(ipa, GSI_CNTXT_SRC_CH_IRQ_CLR, ~0U);
	gsi_wr(ipa, GSI_CNTXT_SRC_CH_IRQ_MSK, IPA_BIT(channel_id));
	gsi_irq_type_enable(ipa, GSI_CH_CTRL);
	ipa_unlock(&ipa->irq_lock);
}

static void
gsi_irq_ch_ctrl_disable(struct ipa *ipa)
{
	ipa_lock(&ipa->irq_lock);
	gsi_irq_type_disable(ipa, GSI_CH_CTRL);
	gsi_wr(ipa, GSI_CNTXT_SRC_CH_IRQ_MSK, 0);
	ipa_unlock(&ipa->irq_lock);
}

static void
gsi_irq_ieob_enable_one(struct ipa *ipa, uint32_t evt_ring_id)
{
	bool first;

	ipa_lock(&ipa->irq_lock);
	first = ipa->ieob_enabled_bitmap == 0;
	ipa->ieob_enabled_bitmap |= IPA_BIT(evt_ring_id);
	gsi_wr(ipa, GSI_CNTXT_SRC_IEOB_IRQ_MSK, ipa->ieob_enabled_bitmap);
	if (first)
		gsi_irq_type_enable(ipa, GSI_IEOB);
	ipa_unlock(&ipa->irq_lock);
}

/* irq_lock held */
static void
gsi_irq_ieob_disable_locked(struct ipa *ipa, uint32_t mask)
{
	ipa->ieob_enabled_bitmap &= ~mask;
	if (ipa->ieob_enabled_bitmap == 0)
		gsi_irq_type_disable(ipa, GSI_IEOB);
	gsi_wr(ipa, GSI_CNTXT_SRC_IEOB_IRQ_MSK, ipa->ieob_enabled_bitmap);
}

static void
gsi_irq_ieob_disable_one(struct ipa *ipa, uint32_t evt_ring_id)
{
	ipa_lock(&ipa->irq_lock);
	gsi_irq_ieob_disable_locked(ipa, IPA_BIT(evt_ring_id));
	ipa_unlock(&ipa->irq_lock);
}

/* errors and the general interrupts, so they are at least reported */
static void
gsi_irq_enable(struct ipa *ipa)
{
	ipa_lock(&ipa->irq_lock);
	gsi_wr(ipa, GSI_CNTXT_GLOB_IRQ_EN, GSI_ERROR_INT);
	gsi_irq_type_update(ipa, ipa->type_enabled_bitmap | GSI_GLOB_EE);
	gsi_wr(ipa, GSI_CNTXT_GSI_IRQ_EN, GSI_BUS_ERROR | GSI_CMD_FIFO_OVRFLOW |
	    GSI_MCS_STACK_OVRFLOW);
	gsi_irq_type_update(ipa, ipa->type_enabled_bitmap | GSI_GENERAL);
	ipa_unlock(&ipa->irq_lock);
}

static void
gsi_irq_disable(struct ipa *ipa)
{
	ipa_lock(&ipa->irq_lock);
	gsi_irq_type_update(ipa, 0);
	gsi_wr(ipa, GSI_CNTXT_GSI_IRQ_EN, 0);
	gsi_wr(ipa, GSI_CNTXT_GLOB_IRQ_EN, 0);
	ipa_unlock(&ipa->irq_lock);
}

/* --- rings --------------------------------------------------------------------------- */

static inline uint32_t
ring_addr(const struct ipa_dma *r, uint32_t index)
{
	return ((uint32_t)r->pa + index * GSI_RING_ELEMENT_SIZE);
}

static inline uint32_t
ring_index(const struct ipa_dma *r, uint32_t offset)
{
	return ((offset - (uint32_t)r->pa) / GSI_RING_ELEMENT_SIZE);
}

/* --- commands ------------------------------------------------------------------------ */

/* Write a command register and wait for its interrupt; false on timeout */
static bool
gsi_command(struct ipa *ipa, uint32_t reg, uint32_t val)
{
	bool done;

	ipa_completion_reinit(&ipa->gsi_done);
	ipa_wmb();
	gsi_wr(ipa, reg, val);
	done = ipa_completion_wait(&ipa->gsi_done, GSI_CMD_TIMEOUT);
	if (!done)
		ipa->n_cmd_timeouts++;
	return (done);
}

static uint32_t
gsi_evt_ring_state(struct ipa *ipa, uint32_t evt_ring_id)
{
	return (IPA_GET(EV_CHSTATE, gsi_rd(ipa, GSI_EV_CH_E_CNTXT_0(evt_ring_id))));
}

static uint32_t
gsi_channel_state(struct ipa *ipa, uint32_t channel_id)
{
	return (IPA_GET(CHSTATE, gsi_rd(ipa, GSI_CH_C_CNTXT_0(channel_id))));
}

static void
gsi_evt_ring_command(struct ipa *ipa, uint32_t evt_ring_id, uint32_t opcode)
{
	bool ok;

	gsi_irq_ev_ctrl_enable(ipa, evt_ring_id);
	ok = gsi_command(ipa, GSI_EV_CH_CMD, IPA_FIELD(EV_CMD_CHID, evt_ring_id) |
	    IPA_FIELD(EV_CMD_OPCODE, opcode));
	gsi_irq_ev_ctrl_disable(ipa);
	if (!ok)
		ipa_os_log(ipa, "GSI command %u for event ring %u timed out, "
		    "state %u\n", opcode, evt_ring_id,
		    gsi_evt_ring_state(ipa, evt_ring_id));
}

static int
gsi_evt_ring_alloc_command(struct ipa *ipa, uint32_t evt_ring_id)
{
	uint32_t state;

	state = gsi_evt_ring_state(ipa, evt_ring_id);
	if (state != GSI_EVT_RING_STATE_NOT_ALLOCATED) {
		ipa_os_log(ipa, "event ring %u bad state %u before alloc\n",
		    evt_ring_id, state);
		return (EINVAL);
	}
	gsi_evt_ring_command(ipa, evt_ring_id, GSI_EVT_ALLOCATE);
	state = gsi_evt_ring_state(ipa, evt_ring_id);
	if (state == GSI_EVT_RING_STATE_ALLOCATED)
		return (0);
	ipa_os_log(ipa, "event ring %u bad state %u after alloc\n", evt_ring_id,
	    state);
	return (EIO);
}

static void
gsi_evt_ring_reset_command(struct ipa *ipa, uint32_t evt_ring_id)
{
	uint32_t state;

	state = gsi_evt_ring_state(ipa, evt_ring_id);
	if (state != GSI_EVT_RING_STATE_ALLOCATED &&
	    state != GSI_EVT_RING_STATE_ERROR) {
		ipa_os_log(ipa, "event ring %u bad state %u before reset\n",
		    evt_ring_id, state);
		return;
	}
	gsi_evt_ring_command(ipa, evt_ring_id, GSI_EVT_RESET);
	state = gsi_evt_ring_state(ipa, evt_ring_id);
	if (state != GSI_EVT_RING_STATE_ALLOCATED)
		ipa_os_log(ipa, "event ring %u bad state %u after reset\n",
		    evt_ring_id, state);
}

static void
gsi_evt_ring_de_alloc_command(struct ipa *ipa, uint32_t evt_ring_id)
{
	uint32_t state;

	state = gsi_evt_ring_state(ipa, evt_ring_id);
	if (state != GSI_EVT_RING_STATE_ALLOCATED) {
		ipa_os_log(ipa, "event ring %u state %u before dealloc\n",
		    evt_ring_id, state);
		return;
	}
	gsi_evt_ring_command(ipa, evt_ring_id, GSI_EVT_DE_ALLOC);
	state = gsi_evt_ring_state(ipa, evt_ring_id);
	if (state != GSI_EVT_RING_STATE_NOT_ALLOCATED)
		ipa_os_log(ipa, "event ring %u bad state %u after dealloc\n",
		    evt_ring_id, state);
}

static void
gsi_channel_command(struct ipa *ipa, uint32_t channel_id, uint32_t opcode)
{
	bool ok;

	gsi_irq_ch_ctrl_enable(ipa, channel_id);
	ok = gsi_command(ipa, GSI_CH_CMD, IPA_FIELD(CH_CMD_CHID, channel_id) |
	    IPA_FIELD(CH_CMD_OPCODE, opcode));
	gsi_irq_ch_ctrl_disable(ipa);
	if (!ok)
		ipa_os_log(ipa, "GSI command %u for channel %u timed out, "
		    "state %u\n", opcode, channel_id,
		    gsi_channel_state(ipa, channel_id));
}

static int
gsi_channel_alloc_command(struct ipa *ipa, uint32_t channel_id)
{
	uint32_t state;

	state = gsi_channel_state(ipa, channel_id);
	if (state != GSI_CHANNEL_STATE_NOT_ALLOCATED) {
		ipa_os_log(ipa, "channel %u bad state %u before alloc\n",
		    channel_id, state);
		return (EINVAL);
	}
	gsi_channel_command(ipa, channel_id, GSI_CH_ALLOCATE);
	state = gsi_channel_state(ipa, channel_id);
	if (state == GSI_CHANNEL_STATE_ALLOCATED)
		return (0);
	ipa_os_log(ipa, "channel %u bad state %u after alloc\n", channel_id,
	    state);
	return (EIO);
}

static int
gsi_channel_start_command(struct ipa *ipa, uint32_t channel_id)
{
	uint32_t state;

	state = gsi_channel_state(ipa, channel_id);
	if (state != GSI_CHANNEL_STATE_ALLOCATED &&
	    state != GSI_CHANNEL_STATE_STOPPED) {
		ipa_os_log(ipa, "channel %u bad state %u before start\n",
		    channel_id, state);
		return (EINVAL);
	}
	gsi_channel_command(ipa, channel_id, GSI_CH_START);
	state = gsi_channel_state(ipa, channel_id);
	if (state == GSI_CHANNEL_STATE_STARTED)
		return (0);
	ipa_os_log(ipa, "channel %u bad state %u after start\n", channel_id,
	    state);
	return (EIO);
}

static int
gsi_channel_stop_command(struct ipa *ipa, uint32_t channel_id)
{
	uint32_t state;

	state = gsi_channel_state(ipa, channel_id);
	if (state == GSI_CHANNEL_STATE_STOPPED)
		return (0);
	if (state != GSI_CHANNEL_STATE_STARTED &&
	    state != GSI_CHANNEL_STATE_STOP_IN_PROC) {
		ipa_os_log(ipa, "channel %u bad state %u before stop\n",
		    channel_id, state);
		return (EINVAL);
	}
	gsi_channel_command(ipa, channel_id, GSI_CH_STOP);
	state = gsi_channel_state(ipa, channel_id);
	if (state == GSI_CHANNEL_STATE_STOPPED)
		return (0);
	if (state == GSI_CHANNEL_STATE_STOP_IN_PROC)
		return (EAGAIN);
	ipa_os_log(ipa, "channel %u bad state %u after stop\n", channel_id,
	    state);
	return (EIO);
}

static void
gsi_channel_reset_command(struct ipa *ipa, uint32_t channel_id)
{
	uint32_t state;

	/* a short delay is required before a RESET command */
	ipa_msleep(1);
	state = gsi_channel_state(ipa, channel_id);
	if (state != GSI_CHANNEL_STATE_STOPPED &&
	    state != GSI_CHANNEL_STATE_ERROR) {
		if (state != GSI_CHANNEL_STATE_ALLOCATED)
			ipa_os_log(ipa, "channel %u bad state %u before reset\n",
			    channel_id, state);
		return;
	}
	gsi_channel_command(ipa, channel_id, GSI_CH_RESET);
	state = gsi_channel_state(ipa, channel_id);
	if (state != GSI_CHANNEL_STATE_ALLOCATED)
		ipa_os_log(ipa, "channel %u bad state %u after reset\n",
		    channel_id, state);
}

static void
gsi_channel_de_alloc_command(struct ipa *ipa, uint32_t channel_id)
{
	uint32_t state;

	state = gsi_channel_state(ipa, channel_id);
	if (state != GSI_CHANNEL_STATE_ALLOCATED) {
		ipa_os_log(ipa, "channel %u bad state %u before dealloc\n",
		    channel_id, state);
		return;
	}
	gsi_channel_command(ipa, channel_id, GSI_CH_DE_ALLOC);
	state = gsi_channel_state(ipa, channel_id);
	if (state != GSI_CHANNEL_STATE_NOT_ALLOCATED)
		ipa_os_log(ipa, "channel %u bad state %u after dealloc\n",
		    channel_id, state);
}

/*
 * GENERIC commands, for the modem's channels: completed by the global
 * interrupt GP_INT1, enabled for the command only
 */
static int
gsi_generic_command(struct ipa *ipa, uint32_t channel_id, uint32_t opcode)
{
	uint32_t val;
	bool ok;

	ipa_lock(&ipa->irq_lock);
	gsi_wr(ipa, GSI_CNTXT_GLOB_IRQ_EN, GSI_ERROR_INT | GSI_GP_INT1);
	ipa_unlock(&ipa->irq_lock);
	val = gsi_rd(ipa, GSI_CNTXT_SCRATCH_0);
	gsi_wr(ipa, GSI_CNTXT_SCRATCH_0, val & ~GENERIC_EE_RESULT);
	ipa->generic_result = EIO;
	ok = gsi_command(ipa, GSI_GENERIC_CMD, IPA_FIELD(GENERIC_OPCODE, opcode) |
	    IPA_FIELD(GENERIC_CHID, channel_id) |
	    IPA_FIELD(GENERIC_EE, GSI_EE_MODEM));
	ipa_lock(&ipa->irq_lock);
	gsi_wr(ipa, GSI_CNTXT_GLOB_IRQ_EN, GSI_ERROR_INT);
	ipa_unlock(&ipa->irq_lock);
	if (ok)
		return (ipa->generic_result);
	ipa_os_log(ipa, "GSI generic command %u to channel %u timed out\n",
	    opcode, channel_id);
	return (ETIMEDOUT);
}

static int
gsi_modem_channel_alloc(struct ipa *ipa, uint32_t channel_id)
{
	return (gsi_generic_command(ipa, channel_id,
	    GSI_GENERIC_ALLOCATE_CHANNEL));
}

static void
gsi_modem_channel_halt(struct ipa *ipa, uint32_t channel_id)
{
	int retries = GSI_CHANNEL_MODEM_HALT_RETRIES, error;

	do
		error = gsi_generic_command(ipa, channel_id,
		    GSI_GENERIC_HALT_CHANNEL);
	while (error == EAGAIN && retries--);
	if (error != 0)
		ipa_os_log(ipa, "error %d halting modem channel %u\n", error,
		    channel_id);
}

void
gsi_modem_channel_flow_control(struct ipa *ipa, uint32_t channel_id,
    bool enable)
{
	int error;

	ipa_slock(&ipa->cmd_lock);
	error = gsi_generic_command(ipa, channel_id, enable ?
	    GSI_GENERIC_ENABLE_FLOW_CONTROL : GSI_GENERIC_DISABLE_FLOW_CONTROL);
	ipa_sunlock(&ipa->cmd_lock);
	if (error != 0)
		ipa_os_log(ipa, "error %d %sabling modem channel %u flow "
		    "control\n", error, enable ? "en" : "dis", channel_id);
}

/* --- programming ---------------------------------------------------------------- */

/* Tell the hardware the last event processed: the one before index */
static void
gsi_evt_ring_doorbell(struct ipa *ipa, struct gsi_channel *ch, uint32_t index)
{
	ch->evt_index = index;
	gsi_wr(ipa, GSI_EV_CH_E_DOORBELL_0(ch->evt_ring_id),
	    ring_addr(&ch->evring, (index - 1) % ch->event_count));
}

static void
gsi_evt_ring_program(struct ipa *ipa, struct gsi_channel *ch)
{
	uint32_t e = ch->evt_ring_id;

	gsi_wr(ipa, GSI_EV_CH_E_CNTXT_0(e), IPA_FIELD(EV_CHTYPE,
	    GSI_CHANNEL_TYPE_GPI) | EV_INTYPE |
	    IPA_FIELD(EV_ELEMENT_SIZE, GSI_RING_ELEMENT_SIZE));
	gsi_wr(ipa, GSI_EV_CH_E_CNTXT_1(e), IPA_FIELD(EV_R_LENGTH,
	    ch->event_count * GSI_RING_ELEMENT_SIZE));
	gsi_wr(ipa, GSI_EV_CH_E_CNTXT_2(e), (uint32_t)ch->evring.pa);
	gsi_wr(ipa, GSI_EV_CH_E_CNTXT_3(e), (uint32_t)(ch->evring.pa >> 32));
	/* interrupt moderation: 1 ms, the count from the channel */
	gsi_wr(ipa, GSI_EV_CH_E_CNTXT_8(e), IPA_FIELD(EV_MODT,
	    GSI_EVT_RING_INT_MODT) | IPA_FIELD(EV_MODC, 1));
	gsi_wr(ipa, GSI_EV_CH_E_CNTXT_9(e), 0);
	gsi_wr(ipa, GSI_EV_CH_E_CNTXT_10(e), 0);
	gsi_wr(ipa, GSI_EV_CH_E_CNTXT_11(e), 0);
	gsi_wr(ipa, GSI_EV_CH_E_CNTXT_12(e), 0);
	gsi_wr(ipa, GSI_EV_CH_E_CNTXT_13(e), 0);
	/* the "last processed" event: arbitrary */
	gsi_evt_ring_doorbell(ipa, ch, ch->evt_index);
}

static void
gsi_channel_program(struct ipa *ipa, struct gsi_channel *ch)
{
	struct gsi_channel_scratch_gpi gpi;
	uint32_t c = ch->id, val, words[4];

	val = IPA_FIELD(CHTYPE_PROTOCOL, GSI_CHANNEL_TYPE_GPI);
	if (ch->toward_ipa)
		val |= CHTYPE_DIR;
	val |= IPA_FIELD(ERINDEX, ch->evt_ring_id);
	val |= IPA_FIELD(ELEMENT_SIZE, GSI_RING_ELEMENT_SIZE);
	gsi_wr(ipa, GSI_CH_C_CNTXT_0(c), val);
	gsi_wr(ipa, GSI_CH_C_CNTXT_1(c), IPA_FIELD(CH_R_LENGTH,
	    ch->tre_count * GSI_RING_ELEMENT_SIZE));
	gsi_wr(ipa, GSI_CH_C_CNTXT_2(c), (uint32_t)ch->ring.pa);
	gsi_wr(ipa, GSI_CH_C_CNTXT_3(c), (uint32_t)(ch->ring.pa >> 32));
	/*
	 * The command channel gets the lowest round-robin weight; no doorbell
	 * engine from IPA v4.0; the escape buffer for all but the command
	 * channel (IPA v4.0 to v4.2)
	 */
	val = IPA_FIELD(WRR_WEIGHT, ch->command ? IPA_FIELD_MAX(WRR_WEIGHT) : 0);
	if (!ch->command)
		val |= USE_ESCAPE_BUF_ONLY;
	gsi_wr(ipa, GSI_CH_C_QOS(c), val);
	/* GPI scratch: the TLV FIFO's size, a threshold of two TREs */
	memset(&gpi, 0, sizeof(gpi));
	gpi.max_outstanding_tre = ch->tlv_count * GSI_RING_ELEMENT_SIZE;
	gpi.outstanding_threshold = 2 * GSI_RING_ELEMENT_SIZE;
	memcpy(words, &gpi, sizeof(words));
	gsi_wr(ipa, GSI_CH_C_SCRATCH_0(c), words[0]);
	gsi_wr(ipa, GSI_CH_C_SCRATCH_1(c), words[1]);
	gsi_wr(ipa, GSI_CH_C_SCRATCH_2(c), words[2]);
	/* the upper 16 bits of the last word only */
	val = gsi_rd(ipa, GSI_CH_C_SCRATCH_3(c));
	val = (words[3] & 0xffff0000U) | (val & 0x0000ffffU);
	gsi_wr(ipa, GSI_CH_C_SCRATCH_3(c), val);
}

/* --- transactions ------------------------------------------------------------------- */

uint32_t
gsi_channel_tre_free(struct gsi_channel *ch)
{
	uint32_t n;

	ipa_lock(&ch->lock);
	n = ch->tre_avail;
	ipa_unlock(&ch->lock);
	return (n);
}

bool
gsi_channel_idle(struct gsi_channel *ch)
{
	bool idle;

	ipa_lock(&ch->lock);
	idle = ch->trans_head == ch->trans_tail;
	ipa_unlock(&ch->lock);
	return (idle);
}

/* channel lock held */
static void
gsi_doorbell_locked(struct gsi_channel *ch)
{
	ipa_wmb();
	gsi_wr(ch->ipa, GSI_CH_C_DOORBELL_0(ch->id),
	    ring_addr(&ch->ring, ch->tre_index % ch->tre_count));
}

void
gsi_doorbell(struct gsi_channel *ch)
{
	ipa_lock(&ch->lock);
	gsi_doorbell_locked(ch);
	ipa_unlock(&ch->lock);
}

/*
 * A transaction of nseg TREs (gsi_trans_commit): EBUSY when the ring has no
 * room. The first TRE's index in *first (the command payloads' slots).
 */
int
gsi_submit(struct gsi_channel *ch, const struct gsi_seg *seg, int nseg,
    void *cookie, uint32_t *first, bool doorbell)
{
	struct gsi_tre *tre;
	struct gsi_trans *t;
	uint32_t idx, flags, tid;
	int i;

	if (nseg < 1 || nseg > ch->tlv_count)
		return (EINVAL);
	ipa_lock(&ch->lock);
	if (!ch->started) {
		ipa_unlock(&ch->lock);
		return (ENXIO);
	}
	if (ch->tre_avail < (uint32_t)nseg) {
		ipa_unlock(&ch->lock);
		return (EBUSY);
	}
	ch->tre_avail -= nseg;
	tid = ch->trans_tail++;
	t = &ch->trans[tid % ch->tre_count];
	memset(t, 0, sizeof(*t));
	t->first = ch->tre_index;
	t->ntre = nseg;
	t->cookie = cookie;
	if (first != NULL)
		*first = ch->tre_index;
	for (i = 0; i < nseg; i++) {
		idx = ch->tre_index + i;
		tre = (struct gsi_tre *)ch->ring.va + idx % ch->tre_count;
		flags = IPA_FIELD(TRE_FLAGS_TYPE, seg[i].opcode == IPA_CMD_NONE ?
		    GSI_RE_XFER : GSI_RE_IMMD_CMD);
		if (i == nseg - 1) {
			/* every transaction ends with a completion event */
			flags |= TRE_FLAGS_IEOT;
			/* no interrupt when outbound ones are acknowledged */
			if (ch->toward_ipa)
				flags |= TRE_FLAGS_BEI;
		} else
			flags |= TRE_FLAGS_CHAIN;
		tre->addr = ipa_le64(seg[i].pa);
		tre->len_opcode = ipa_le16(seg[i].opcode == IPA_CMD_NONE ?
		    (uint16_t)seg[i].len : seg[i].opcode);
		tre->reserved = 0;
		tre->flags = ipa_le32(flags);
	}
	/* the completion names the last TRE */
	ch->map[(ch->tre_index + nseg - 1) % ch->tre_count] =
	    (uint16_t)(tid % ch->tre_count) + 1;
	ch->tre_index += nseg;
	ch->n_trans++;
	if (doorbell || ch->tre_avail == 0)
		gsi_doorbell_locked(ch);
	ipa_unlock(&ch->lock);
	return (0);
}

/*
 * The events the GSI wrote since the last look: their transactions are done
 * (gsi_channel_update, gsi_evt_ring_update). Channel lock held.
 */
static void
gsi_channel_update_locked(struct ipa *ipa, struct gsi_channel *ch)
{
	struct gsi_event *ev;
	struct gsi_trans *t;
	uint32_t hw, tre, n;
	uint16_t m;

	ipa_os_dma_sync(ipa, &ch->evring, IPA_SYNC_FROMDEV);
	hw = ring_index(&ch->evring, gsi_rd(ipa,
	    GSI_EV_CH_E_CNTXT_4(ch->evt_ring_id)));
	if (hw >= ch->event_count) {
		ipa_os_log(ipa, "channel %u: event pointer out of its ring\n",
		    ch->id);
		return;
	}
	n = (hw - ch->evt_index) % ch->event_count;
	if (n == 0)
		return;
	ipa_rmb();
	while (n-- > 0) {
		ev = (struct gsi_event *)ch->evring.va +
		    ch->evt_index % ch->event_count;
		ch->n_events++;
		if (ev->chid != ch->id)
			ipa_os_log(ipa, "channel %u: event for channel %u\n",
			    ch->id, ev->chid);
		tre = ring_index(&ch->ring, (uint32_t)ipa_from_le64(ev->xfer_ptr));
		m = ch->map[tre % ch->tre_count];
		if (m == 0) {
			ipa_os_log(ipa, "channel %u: event with no "
			    "transaction (TRE %u)\n", ch->id, tre);
		} else {
			ch->map[tre % ch->tre_count] = 0;
			t = &ch->trans[m - 1];
			t->done = true;
			if (!ch->toward_ipa)
				t->len = ipa_from_le16(ev->len);
		}
		ch->evt_index++;
	}
	ipa_os_dma_sync(ipa, &ch->evring, IPA_SYNC_TODEV);
	gsi_evt_ring_doorbell(ipa, ch, ch->evt_index);
}

/* The oldest done transaction off the FIFO, into *out; false: none */
static bool
gsi_trans_pop_locked(struct gsi_channel *ch, struct gsi_trans *out)
{
	struct gsi_trans *t;

	if (ch->trans_head == ch->trans_tail)
		return (false);
	t = &ch->trans[ch->trans_head % ch->tre_count];
	if (!t->done && !t->cancelled)
		return (false);
	*out = *t;
	t->done = false;
	ch->trans_head++;
	ch->tre_avail += t->ntre;
	return (true);
}

/*
 * NAPI's poll (gsi_channel_poll): the completed transactions to the
 * endpoint, then the completion interrupt on again
 */
void
gsi_channel_poll(struct gsi_channel *ch)
{
	struct ipa *ipa = ch->ipa;
	struct ipa_endpoint *ep = ipa->channel_map[ch->id];
	struct gsi_trans t;
	int count = 0;
	bool more;

	ch->n_polls++;
	for (;;) {
		ipa_lock(&ch->lock);
		if (!gsi_trans_pop_locked(ch, &t)) {
			gsi_channel_update_locked(ipa, ch);
			more = gsi_trans_pop_locked(ch, &t);
		} else
			more = true;
		ipa_unlock(&ch->lock);
		if (!more)
			break;
		if (ep != NULL)
			ipa_endpoint_trans_complete(ep, &t);
		if (++count >= GSI_POLL_BUDGET) {
			ipa_os_poll_schedule(ipa, ch);
			return;
		}
	}
	if (ch->started)
		gsi_irq_ieob_enable_one(ipa, ch->evt_ring_id);
}

/* After a reset: what was outstanding is cancelled (channel lock held) */
static void
gsi_channel_trans_cancel_pending(struct gsi_channel *ch)
{
	uint32_t i;

	for (i = ch->trans_head; i != ch->trans_tail; i++) {
		struct gsi_trans *t = &ch->trans[i % ch->tre_count];

		if (!t->done) {
			t->cancelled = true;
			ch->n_cancelled++;
		}
	}
	memset(ch->map, 0, ch->tre_count * sizeof(ch->map[0]));
}

/* --- channels ------------------------------------------------------------------------ */

int
gsi_channel_start(struct ipa *ipa, uint32_t channel_id)
{
	struct gsi_channel *ch = &ipa->channel[channel_id];
	int error;

	ipa_lock(&ch->lock);
	ch->started = true;
	ipa_unlock(&ch->lock);
	gsi_irq_ieob_enable_one(ipa, ch->evt_ring_id);
	ipa_slock(&ipa->cmd_lock);
	error = gsi_channel_start_command(ipa, channel_id);
	ipa_sunlock(&ipa->cmd_lock);
	if (error != 0) {
		gsi_irq_ieob_disable_one(ipa, ch->evt_ring_id);
		ipa_lock(&ch->lock);
		ch->started = false;
		ipa_unlock(&ch->lock);
	}
	return (error);
}

int
gsi_channel_stop(struct ipa *ipa, uint32_t channel_id)
{
	struct gsi_channel *ch = &ipa->channel[channel_id];
	int retries = GSI_CHANNEL_STOP_RETRIES, i, error;

	/* what is on its way toward the IPA first (gsi_channel_trans_quiesce) */
	for (i = 0; ch->toward_ipa && !gsi_channel_idle(ch) && i < 100; i++) {
		gsi_channel_poll(ch);
		ipa_msleep(1);
	}
	ipa_slock(&ipa->cmd_lock);
	do {
		error = gsi_channel_stop_command(ipa, channel_id);
		if (error != EAGAIN)
			break;
		ipa_msleep(4);
	} while (retries--);
	ipa_sunlock(&ipa->cmd_lock);
	if (error != 0)
		return (error);
	gsi_irq_ieob_disable_one(ipa, ch->evt_ring_id);
	ipa_lock(&ch->lock);
	ch->started = false;
	ipa_unlock(&ch->lock);
	return (0);
}

void
gsi_channel_reset(struct ipa *ipa, uint32_t channel_id, bool doorbell)
{
	struct gsi_channel *ch = &ipa->channel[channel_id];

	(void)doorbell;		/* no doorbell engine from IPA v4.0 */
	ipa_slock(&ipa->cmd_lock);
	gsi_channel_reset_command(ipa, channel_id);
	ipa_lock(&ch->lock);
	/* the hardware's ring index is 0 after a reset */
	ch->tre_index = 0;
	gsi_channel_program(ipa, ch);
	gsi_channel_trans_cancel_pending(ch);
	ipa_unlock(&ch->lock);
	ipa_sunlock(&ipa->cmd_lock);
	/* the cancelled ones to their endpoint */
	gsi_channel_poll(ch);
}

/* --- the interrupt --------------------------------------------------------------- */

static void
gsi_isr_chan_ctrl(struct ipa *ipa)
{
	uint32_t mask = gsi_rd(ipa, GSI_CNTXT_SRC_CH_IRQ);

	gsi_wr(ipa, GSI_CNTXT_SRC_CH_IRQ_CLR, mask);
	if (mask != 0)
		ipa_complete(&ipa->gsi_done);
}

static void
gsi_isr_evt_ctrl(struct ipa *ipa)
{
	uint32_t mask = gsi_rd(ipa, GSI_CNTXT_SRC_EV_CH_IRQ);

	gsi_wr(ipa, GSI_CNTXT_SRC_EV_CH_IRQ_CLR, mask);
	if (mask != 0)
		ipa_complete(&ipa->gsi_done);
}

static void
gsi_isr_glob_err(struct ipa *ipa)
{
	uint32_t val, ee, type, which, code;

	val = gsi_rd(ipa, GSI_ERROR_LOG);
	gsi_wr(ipa, GSI_ERROR_LOG, 0);
	gsi_wr(ipa, GSI_ERROR_LOG_CLR, ~0U);
	ee = IPA_GET(ERR_EE, val);
	type = IPA_GET(ERR_TYPE, val);
	which = IPA_GET(ERR_VIRT_IDX, val);
	code = IPA_GET(ERR_CODE, val);
	ipa->n_gsi_errors++;
	if (code == GSI_OUT_OF_RESOURCES &&
	    (type == GSI_ERR_TYPE_CHAN || type == GSI_ERR_TYPE_EVT)) {
		ipa_os_log(ipa, "%s %u out of resources\n",
		    type == GSI_ERR_TYPE_CHAN ? "channel" : "event ring", which);
		ipa_complete(&ipa->gsi_done);
		return;
	}
	if (type == GSI_ERR_TYPE_CHAN || type == GSI_ERR_TYPE_EVT)
		ipa_os_log(ipa, "%s %u global error ee %u code %#x\n",
		    type == GSI_ERR_TYPE_CHAN ? "channel" : "event ring", which,
		    ee, code);
	else
		ipa_os_log(ipa, "unexpected global error %#x (log %#x)\n", type,
		    val);
}

static void
gsi_isr_gp_int1(struct ipa *ipa)
{
	uint32_t result;

	result = IPA_GET(GENERIC_EE_RESULT, gsi_rd(ipa, GSI_CNTXT_SCRATCH_0));
	switch (result) {
	case GENERIC_EE_SUCCESS:
	case GENERIC_EE_INCORRECT_CHANNEL_STATE:
		ipa->generic_result = 0;
		break;
	case GENERIC_EE_RETRY:
		ipa->generic_result = EAGAIN;
		break;
	default:
		ipa_os_log(ipa, "global INT1 generic result %u\n", result);
		ipa->generic_result = EIO;
		break;
	}
	ipa_complete(&ipa->gsi_done);
}

static void
gsi_isr_glob_ee(struct ipa *ipa)
{
	uint32_t val = gsi_rd(ipa, GSI_CNTXT_GLOB_IRQ_STTS);

	if (val & GSI_ERROR_INT)
		gsi_isr_glob_err(ipa);
	gsi_wr(ipa, GSI_CNTXT_GLOB_IRQ_CLR, val);
	val &= ~GSI_ERROR_INT;
	if (val & GSI_GP_INT1) {
		val &= ~GSI_GP_INT1;
		gsi_isr_gp_int1(ipa);
	}
	if (val != 0)
		ipa_os_log(ipa, "unexpected global interrupt %#x\n", val);
}

static void
gsi_isr_ieob(struct ipa *ipa)
{
	uint32_t mask, e;

	ipa_lock(&ipa->irq_lock);
	mask = gsi_rd(ipa, GSI_CNTXT_SRC_IEOB_IRQ);
	gsi_irq_ieob_disable_locked(ipa, mask);
	gsi_wr(ipa, GSI_CNTXT_SRC_IEOB_IRQ_CLR, mask);
	ipa_unlock(&ipa->irq_lock);
	while (mask != 0) {
		e = __builtin_ctz(mask);
		mask &= ~IPA_BIT(e);
		if (e < GSI_EVT_RING_COUNT_MAX && ipa->evt_channel[e] != NULL)
			ipa_os_poll_schedule(ipa, ipa->evt_channel[e]);
	}
}

static void
gsi_isr_general(struct ipa *ipa)
{
	uint32_t val = gsi_rd(ipa, GSI_CNTXT_GSI_IRQ_STTS);

	gsi_wr(ipa, GSI_CNTXT_GSI_IRQ_CLR, val);
	ipa->n_gsi_general++;
	ipa_os_log(ipa, "unexpected general interrupt %#x\n", val);
}

/* The "gsi" interrupt (gsi_isr) */
void
gsi_isr(struct ipa *ipa)
{
	uint32_t pending, bit;
	int cnt = 0;

	while ((pending = gsi_rd(ipa, GSI_CNTXT_TYPE_IRQ)) != 0) {
		do {
			bit = IPA_BIT(__builtin_ctz(pending));
			pending &= ~bit;
			switch (bit) {
			case GSI_CH_CTRL:
				gsi_isr_chan_ctrl(ipa);
				break;
			case GSI_EV_CTRL:
				gsi_isr_evt_ctrl(ipa);
				break;
			case GSI_GLOB_EE:
				gsi_isr_glob_ee(ipa);
				break;
			case GSI_IEOB:
				gsi_isr_ieob(ipa);
				break;
			case GSI_GENERAL:
				gsi_isr_general(ipa);
				break;
			default:
				ipa_os_log(ipa, "unrecognized interrupt type "
				    "%#x\n", bit);
				break;
			}
		} while (pending != 0);
		if (++cnt > GSI_ISR_MAX_ITER) {
			ipa_os_log(ipa, "interrupt flood\n");
			break;
		}
	}
}

/* --- setup and teardown ---------------------------------------------------------- */

/* all off, interrupts (not MSI) */
static void
gsi_irq_setup(struct ipa *ipa)
{
	gsi_wr(ipa, GSI_CNTXT_INTSET, INTSET_INTYPE);
	ipa_lock(&ipa->irq_lock);
	gsi_irq_type_update(ipa, 0);
	ipa->ieob_enabled_bitmap = 0;
	ipa_unlock(&ipa->irq_lock);
	gsi_wr(ipa, GSI_CNTXT_SRC_CH_IRQ_MSK, 0);
	gsi_wr(ipa, GSI_CNTXT_SRC_EV_CH_IRQ_MSK, 0);
	gsi_wr(ipa, GSI_CNTXT_GLOB_IRQ_EN, 0);
	gsi_wr(ipa, GSI_CNTXT_SRC_IEOB_IRQ_MSK, 0);
	gsi_wr(ipa, GSI_INTER_EE_SRC_CH_IRQ_MSK, 0);
	gsi_wr(ipa, GSI_INTER_EE_SRC_EV_CH_IRQ_MSK, 0);
	gsi_wr(ipa, GSI_CNTXT_GSI_IRQ_EN, 0);
}

static int
gsi_ring_setup(struct ipa *ipa)
{
	uint32_t val = gsi_rd(ipa, GSI_HW_PARAM_2), count;

	count = IPA_GET(NUM_CH_PER_EE, val);
	if (count == 0) {
		ipa_os_log(ipa, "GSI reports zero channels supported\n");
		return (EINVAL);
	}
	ipa->channel_count = MIN(count, GSI_CHANNEL_COUNT_MAX);
	count = IPA_GET(NUM_EV_PER_EE, val);
	if (count == 0) {
		ipa_os_log(ipa, "GSI reports zero event rings supported\n");
		return (EINVAL);
	}
	ipa->evt_ring_count = MIN(count, GSI_EVT_RING_COUNT_MAX);
	return (0);
}

static int
gsi_channel_setup_one(struct ipa *ipa, struct gsi_channel *ch)
{
	int error;

	if (ch->evt_ring_id >= ipa->evt_ring_count) {
		ipa_os_log(ipa, "event ring %u not supported by hardware\n",
		    ch->evt_ring_id);
		return (EINVAL);
	}
	error = gsi_evt_ring_alloc_command(ipa, ch->evt_ring_id);
	if (error != 0)
		return (error);
	gsi_evt_ring_program(ipa, ch);
	error = gsi_channel_alloc_command(ipa, ch->id);
	if (error != 0) {
		gsi_evt_ring_de_alloc_command(ipa, ch->evt_ring_id);
		return (error);
	}
	gsi_channel_program(ipa, ch);
	return (0);
}

static void
gsi_channel_teardown_one(struct ipa *ipa, struct gsi_channel *ch)
{
	gsi_channel_de_alloc_command(ipa, ch->id);
	gsi_evt_ring_reset_command(ipa, ch->evt_ring_id);
	gsi_evt_ring_de_alloc_command(ipa, ch->evt_ring_id);
}

/* GSI firmware loaded and running (gsi_setup) */
int
gsi_setup(struct ipa *ipa)
{
	uint32_t mask, id;
	int error, i, done = 0;

	if ((gsi_rd(ipa, GSI_STATUS) & GSI_STATUS_ENABLED) == 0) {
		ipa_os_log(ipa, "GSI has not been enabled\n");
		return (EIO);
	}
	gsi_irq_setup(ipa);
	if ((error = gsi_ring_setup(ipa)) != 0)
		return (error);
	gsi_wr(ipa, GSI_ERROR_LOG, 0);
	gsi_irq_enable(ipa);

	ipa_slock(&ipa->cmd_lock);
	for (i = 0; i < GSI_CHANNEL_COUNT_MAX; i++) {
		struct gsi_channel *ch = &ipa->channel[i];

		if (!ch->used)
			continue;
		if ((uint32_t)i >= ipa->channel_count) {
			ipa_os_log(ipa, "channel %d not supported by hardware\n",
			    i);
			error = EINVAL;
			goto unwind;
		}
		if ((error = gsi_channel_setup_one(ipa, ch)) != 0)
			goto unwind;
		done |= 1 << i;
	}
	/* the modem's channels: the AP allocates them on IPA v4.2 */
	mask = ipa->modem_channel_bitmap;
	while (mask != 0) {
		id = __builtin_ctz(mask);
		error = gsi_modem_channel_alloc(ipa, id);
		if (error != 0) {
			ipa_os_log(ipa, "error %d allocating modem channel %u\n",
			    error, id);
			mask ^= ipa->modem_channel_bitmap;
			while (mask != 0) {
				id = 31 - __builtin_clz(mask);
				mask &= ~IPA_BIT(id);
				gsi_modem_channel_halt(ipa, id);
			}
			goto unwind;
		}
		mask &= ~IPA_BIT(id);
	}
	ipa_sunlock(&ipa->cmd_lock);
	return (0);
unwind:
	for (i = GSI_CHANNEL_COUNT_MAX - 1; i >= 0; i--)
		if (done & (1 << i))
			gsi_channel_teardown_one(ipa, &ipa->channel[i]);
	ipa_sunlock(&ipa->cmd_lock);
	gsi_irq_disable(ipa);
	return (error);
}

void
gsi_teardown(struct ipa *ipa)
{
	uint32_t mask = ipa->modem_channel_bitmap, id;
	int i;

	ipa_slock(&ipa->cmd_lock);
	while (mask != 0) {
		id = 31 - __builtin_clz(mask);
		mask &= ~IPA_BIT(id);
		gsi_modem_channel_halt(ipa, id);
	}
	for (i = GSI_CHANNEL_COUNT_MAX - 1; i >= 0; i--)
		if (ipa->channel[i].used)
			gsi_channel_teardown_one(ipa, &ipa->channel[i]);
	ipa_sunlock(&ipa->cmd_lock);
	gsi_irq_disable(ipa);
}

/* --- init ------------------------------------------------------------------------------- */

static int
gsi_evt_ring_id_alloc(struct ipa *ipa)
{
	uint32_t id;

	if (ipa->event_bitmap == ~0U)
		return (-1);
	id = __builtin_ctz(~ipa->event_bitmap);
	ipa->event_bitmap |= IPA_BIT(id);
	return ((int)id);
}

static void
gsi_channel_exit_one(struct ipa *ipa, struct gsi_channel *ch)
{
	if (!ch->used)
		return;
	ipa_os_dma_free(ipa, &ch->ring);
	ipa_os_dma_free(ipa, &ch->evring);
	ipa_free(ch->trans);
	ipa_free(ch->map);
	ipa_mutex_destroy(&ch->lock);
	ipa->evt_channel[ch->evt_ring_id] = NULL;
	ch->used = false;
}

static int
gsi_channel_init_one(struct ipa *ipa, const struct ipa_ep_data *d,
    bool command)
{
	struct gsi_channel *ch = &ipa->channel[d->channel];
	size_t size;
	int e, error;

	if (d->channel >= GSI_CHANNEL_COUNT_MAX || d->tlv_count == 0 ||
	    d->tre_count < 2 * d->tlv_count - 1 ||
	    (d->tre_count & (d->tre_count - 1)) != 0 ||
	    (d->event_count & (d->event_count - 1)) != 0 ||
	    (command && d->tlv_count < IPA_COMMAND_TRANS_TRE_MAX)) {
		ipa_os_log(ipa, "channel %u: bad configuration\n", d->channel);
		return (EINVAL);
	}
	memset(ch, 0, sizeof(*ch));
	ch->ipa = ipa;
	ch->id = d->channel;
	ch->toward_ipa = d->toward_ipa;
	ch->command = command;
	ch->tlv_count = d->tlv_count;
	/* an event for every outstanding TRE at worst */
	ch->tre_count = MIN(d->tre_count, d->event_count);
	ch->event_count = d->event_count;
	/* gsi_channel_tre_max() */
	ch->tre_max = ch->tre_count - (ch->tlv_count - 1);
	ch->tre_avail = ch->tre_max;
	if ((e = gsi_evt_ring_id_alloc(ipa)) < 0) {
		ipa_os_log(ipa, "event rings exhausted\n");
		return (ENOSPC);
	}
	ch->evt_ring_id = e;
	/* 2^n elements, aligned to their size (the hardware's requirement) */
	size = (size_t)ch->tre_count * GSI_RING_ELEMENT_SIZE;
	error = ipa_os_dma_alloc(ipa, &ch->ring, size, size, IPA_DMA_UNCACHED);
	if (error != 0)
		goto fail;
	size = (size_t)ch->event_count * GSI_RING_ELEMENT_SIZE;
	error = ipa_os_dma_alloc(ipa, &ch->evring, size, size, IPA_DMA_UNCACHED);
	if (error != 0) {
		ipa_os_dma_free(ipa, &ch->ring);
		goto fail;
	}
	ch->trans = ipa_zalloc(ch->tre_count * sizeof(ch->trans[0]));
	ch->map = ipa_zalloc(ch->tre_count * sizeof(ch->map[0]));
	ipa_mutex_init(&ch->lock, "gsichan");
	ipa->evt_channel[e] = ch;
	ch->used = true;
	return (0);
fail:
	ipa_os_log(ipa, "error %d allocating channel %u's rings\n", error,
	    d->channel);
	ipa->event_bitmap &= ~IPA_BIT(e);
	return (error);
}

/* The channels of the AP's endpoints (gsi_init, gsi_channel_init) */
int
gsi_init(struct ipa *ipa)
{
	const struct ipa_ep_data *d;
	int i, error;

	/* event ids from 28 up and the MHI ones (10-16) are not ours */
	ipa->event_bitmap = ~IPA_GENMASK(GSI_EVT_RING_COUNT_MAX - 1, 0) |
	    IPA_GENMASK(GSI_MHI_EVENT_ID_END, GSI_MHI_EVENT_ID_START);
	ipa->ieob_enabled_bitmap = 0;
	ipa->modem_channel_bitmap = 0;
	ipa_mutex_init(&ipa->irq_lock, "gsiirq");
	ipa_sleeplock_init(&ipa->cmd_lock, "gsicmd");
	ipa_completion_init(&ipa->gsi_done);
	for (i = 0; i < IPA_EP_NAMES; i++) {
		d = &ipa_ep_data_v4_2[i];
		if (d->ee == GSI_EE_MODEM) {
			/* IPA v4.2: the AP allocates the modem's channels */
			ipa->modem_channel_bitmap |= IPA_BIT(d->channel);
			continue;
		}
		error = gsi_channel_init_one(ipa, d, i == IPA_EP_AP_COMMAND_TX);
		if (error != 0) {
			gsi_exit(ipa);
			return (error);
		}
	}
	return (0);
}

void
gsi_exit(struct ipa *ipa)
{
	int i;

	for (i = 0; i < GSI_CHANNEL_COUNT_MAX; i++)
		gsi_channel_exit_one(ipa, &ipa->channel[i]);
	ipa->modem_channel_bitmap = 0;
	ipa_completion_destroy(&ipa->gsi_done);
	ipa_sleeplock_destroy(&ipa->cmd_lock);
	ipa_mutex_destroy(&ipa->irq_lock);
}
