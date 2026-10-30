/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The LPASS SoundWire masters (soundwire-v1.6.0: swr1 at a610000 for the
 * WCD9370's RX device, swr0 at a740000 for its TX device, which also
 * carries the codec's registers), polled from user space through
 * /dev/qcom_lpass, as Linux's drivers/soundwire/qcom.c and the SoundWire
 * core (stream.c, bus.c) drive them:
 *
 *   swr_init       qcom_swrm_probe's register part and qcom_swrm_init: the
 *                  version, frame shape 50 x 16 (swrm_v1_6_data), the
 *                  hardware's auto enumeration, no-ping count, command
 *                  retries, the component enable, the frame generator;
 *                  the CGCR reset of lpass_audiocc/lpasscc comes before it
 *                  (a sequence line, codec.c)
 *   swr_enumerate  qcom_swrm_enumerate: the device numbers the hardware
 *                  assigned (MCP_SLV_STATUS), their 48-bit ids, the one
 *                  with the device tree's unique id
 *   swr_write/read the command FIFO (qcom_swrm_cmd_fifo_wr_cmd/_rd_cmd),
 *                  one byte per command; a broadcast waits for
 *                  SPECIAL_CMD_ID_FINISHED by polling the interrupt status
 *   swr_stream     sdw_prepare_stream + sdw_enable_stream for one stream:
 *                  the master's and the slave's DPn transport and port
 *                  parameters from the device tree's qcom,ports-* tables
 *                  (qcom_swrm_compute_params, qcom_swrm_transport_params,
 *                  sdw_program_slave_port_params for the WCD937x's SIMPLE
 *                  ports with lane control, simple_ch_prep_sm: no prepare
 *                  handshake), the channels enabled, the bank switch
 *                  (qcom_swrm_pre_bank_switch, then SCP_FrameCtrl of the
 *                  next bank by broadcast); both banks are programmed alike
 *                  (Linux's prepare and enable each program the next bank,
 *                  so both end up the same)
 *
 *   swr_irq        qcom_swrm_irq_handler: the master's interrupt status
 *                  (with qcom_swr.ko's /dev/qcom_swr0 the interrupt line;
 *                  else the status read when the caller looks): a
 *                  slave's alert (SLAVE_PEND_IRQ: the device whose ping
 *                  status is ALERT; sdw_handle_slave_alerts: SCP_INT1,
 *                  the implementation-defined bit to the caller's
 *                  callback, acknowledged; again while it is set), slave
 *                  status changes (a device back after a clock stop or a
 *                  lost sync: sdw_initialize_slave again), command
 *                  errors (the FIFO flushed), clashes (masked, as Linux)
 *   swr_slave_init sdw_initialize_slave: SCP_IntMask1 with the WCD937x's
 *                  scp_int1_mask (implementation-defined, bus clash,
 *                  parity), without which it never alerts
 *   swr_clock_stop swrm_runtime_suspend with clock stop: the write FIFO
 *                  drained, the clash interrupt masked, the slaves
 *                  (simple_clk_stop_capable: no SCP_SystemCtrl prepare)
 *                  told ClockStopNow by broadcast; the caller then stops
 *                  the master's clock (the TX macro's) and arms the wake
 *                  interrupt
 *   swr_clock_start swrm_runtime_resume with clock stop, after the caller
 *                  has the clock back and the CGCR reset done: the bus
 *                  clock started, the clash cleared and unmasked, the
 *                  frame generator awaited (not a new init: the master's
 *                  registers and the slaves' device numbers are kept)
 *
 * The interrupt line in level mode (irq_level: COMP_CFG without the pulse
 * bit Linux sets, which then asks for a rising edge): qcom_swr.ko masks
 * the line in the GIC when it fires and the daemon arms it again after
 * INTERRUPT_CLEAR, so a level that stays up until it is cleared is what
 * fits, and the device tree says level high. Every access is 32 bits in
 * the master's window.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "audiod.h"

#define	SWRM_COMP_HW_VERSION			0x00
#define	SWRM_COMP_CFG_ADDR			0x04
#define	SWRM_COMP_CFG_IRQ_LEVEL_OR_PULSE_MSK	(1u << 1)
#define	SWRM_COMP_CFG_ENABLE_MSK		(1u << 0)
#define	SWRM_COMP_SW_RESET			0x008
#define	SWRM_COMP_STATUS			0x014
#define	SWRM_FRM_GEN_ENABLED			(1u << 0)
#define	SWRM_COMP_PARAMS			0x100
#define	SWRM_COMP_MASTER_ID			0x104
#define	SWRM_INTERRUPT_STATUS			0x200
#define	SWRM_INTERRUPT_STATUS_RMSK		0x1ffff
#define	SWRM_INTERRUPT_SLAVE_PEND_IRQ		(1u << 0)
#define	SWRM_INTERRUPT_NEW_SLAVE_ATTACHED	(1u << 1)
#define	SWRM_INTERRUPT_CHANGE_ENUM_SLAVE_STATUS	(1u << 2)
#define	SWRM_INTERRUPT_MASTER_CLASH_DET		(1u << 3)
#define	SWRM_INTERRUPT_RD_FIFO_OVERFLOW		(1u << 4)
#define	SWRM_INTERRUPT_RD_FIFO_UNDERFLOW	(1u << 5)
#define	SWRM_INTERRUPT_WR_CMD_FIFO_OVERFLOW	(1u << 6)
#define	SWRM_INTERRUPT_CMD_ERROR		(1u << 7)
#define	SWRM_INTERRUPT_DOUT_PORT_COLLISION	(1u << 8)
#define	SWRM_INTERRUPT_READ_EN_RD_VALID_MISMATCH (1u << 9)
#define	SWRM_INTERRUPT_SPECIAL_CMD_ID_FINISHED	(1u << 10)
#define	SWRM_INTERRUPT_MASK_ADDR		0x204
#define	SWRM_INTERRUPT_CLEAR			0x208
#define	SWRM_INTERRUPT_CPU_EN			0x210
#define	SWRM_CMD_FIFO_WR_CMD			0x300
#define	SWRM_CMD_FIFO_RD_CMD			0x304
#define	SWRM_CMD_FIFO_CMD			0x308
#define	SWRM_CMD_FIFO_FLUSH			0x1
#define	SWRM_CMD_FIFO_STATUS			0x30c
#define	SWRM_RD_CMD_FIFO_CNT(v)			(((v) >> 16) & 0x1f)
#define	SWRM_WR_CMD_FIFO_CNT(v)			(((v) >> 8) & 0x1f)
#define	SWRM_CMD_FIFO_CFG_ADDR			0x314
#define	SWRM_CONTINUE_EXEC_ON_CMD_IGNORE	(1u << 31)
#define	SWRM_RD_WR_CMD_RETRIES			0x7
#define	SWRM_CMD_FIFO_RD_FIFO_ADDR		0x318
#define	SWRM_RD_FIFO_CMD_ID(v)			(((v) >> 8) & 0xf)
#define	SWRM_ENUMERATOR_CFG_ADDR		0x500
#define	SWRM_ENUMERATOR_SLAVE_DEV_ID_1(m)	(0x530 + 0x8 * (m))
#define	SWRM_ENUMERATOR_SLAVE_DEV_ID_2(m)	(0x534 + 0x8 * (m))
#define	SWRM_MCP_FRAME_CTRL_BANK_ADDR(m)	(0x101c + 0x40 * (m))
#define	SWRM_MCP_BUS_CTRL			0x1044
#define	SWRM_MCP_BUS_CLK_START			(1u << 1)
#define	SWRM_MCP_CFG_ADDR			0x1048
#define	SWRM_MCP_CFG_NO_PINGS_SHIFT		17
#define	SWRM_MCP_CFG_NO_PINGS_MASK		(0x1fu << 17)
#define	SWRM_DEF_CMD_NO_PINGS			0x1f
#define	SWRM_MCP_STATUS				0x104c
#define	SWRM_MCP_SLV_STATUS			0x1090
#define	SWRM_VERSION_1_5_1			0x01050001
/* the v1.3 layout's DPn offsets (swrm_v1_3_reg_layout) */
#define	DP_PORT_CTRL_BANK(n, b)			(0x1124 + 0x100 * ((n) - 1) + 0x40 * (b))
#define	DP_PORT_CTRL_2_BANK(n, b)		(0x1128 + 0x100 * ((n) - 1) + 0x40 * (b))
#define	DP_BLOCK_CTRL_1(n)			(0x112c + 0x100 * ((n) - 1))
#define	DP_BLOCK_CTRL2_BANK(n, b)		(0x1130 + 0x100 * ((n) - 1) + 0x40 * (b))
#define	DP_PORT_HCTRL_BANK(n, b)		(0x1134 + 0x100 * ((n) - 1) + 0x40 * (b))
#define	DP_BLOCK_CTRL3_BANK(n, b)		(0x1138 + 0x100 * ((n) - 1) + 0x40 * (b))
#define	DP_SAMPLECTRL2_BANK(n, b)		(0x113c + 0x100 * ((n) - 1) + 0x40 * (b))
#define	SWRM_DP_PORT_CTRL_EN_CHAN_SHFT		24
#define	SWR_INVALID_PARAM			0xff
#define	SWR_MAX_CMD_ID				14
#define	SWR_BROADCAST_CMD_ID			0x0f
#define	SDW_BROADCAST_DEV_NUM			15
#define	SWRM_REG_VAL_PACK(data, dev, id, reg) \
	((uint32_t)(reg) | (uint32_t)(id) << 16 | (uint32_t)(dev) << 20 | \
	 (uint32_t)(data) << 24)
/* the SoundWire slave registers (include/linux/soundwire/sdw_registers.h) */
#define	SDW_SCP_INT1				0x40
#define	SDW_SCP_INTMASK1			0x41
#define	SDW_SCP_INT1_PARITY			(1u << 0)
#define	SDW_SCP_INT1_BUS_CLASH			(1u << 1)
#define	SDW_SCP_INT1_IMPL_DEF			(1u << 2)
#define	SDW_SCP_INT1_PORT0_3			(0xfu << 3)
#define	SDW_SCP_INT1_SCP2_CASCADE		(1u << 7)
#define	SDW_SCP_CTRL				0x44
#define	SDW_SCP_CTRL_CLK_STP_NOW		(1u << 1)
#define	SDW_SCP_SYSTEMCTRL			0x45
#define	SDW_SCP_SYSTEMCTRL_WAKE_UP_EN		(1u << 3)
#define	SDW_SLAVE_ATTACHED			1
#define	SDW_SLAVE_ALERT				2
#define	SDW_READ_INTR_CLEAR_RETRY		10
/* wcd937x-sdw.c: prop.scp_int1_mask */
#define	WCD_SCP_INT1_MASK	(SDW_SCP_INT1_IMPL_DEF | SDW_SCP_INT1_BUS_CLASH | \
				 SDW_SCP_INT1_PARITY)
#define	SDW_SCP_FRAMECTRL_B(b)			(0x60 + 0x10 * (b))
#define	SDW_DPN_PORTCTRL(n)			(0x02 + 0x100 * (n))
#define	SDW_DPN_BLOCKCTRL1(n)			(0x03 + 0x100 * (n))
#define	SDW_DPN_CHANNELEN_B(n, b)		(0x20 + 0x10 * (b) + 0x100 * (n))
#define	SDW_DPN_SAMPLECTRL1_B(n, b)		(0x22 + 0x10 * (b) + 0x100 * (n))
#define	SDW_DPN_OFFSETCTRL1_B(n, b)		(0x24 + 0x10 * (b) + 0x100 * (n))
#define	SDW_DPN_LANECTRL_B(n, b)		(0x28 + 0x10 * (b) + 0x100 * (n))
/* swrm_v1_6_data: 50 rows, 16 columns; sdw_rows[] index 1, sdw_cols[] 7 */
#define	SWR_ROWS_INDEX				1
#define	SWR_COLS_INDEX				7
#define	SWR_FRAMECTRL				(SWR_COLS_INDEX | SWR_ROWS_INDEX << 3)

static uint32_t
rd(struct swr *w, uint32_t off)
{
	return (mmio_rd(w->m, off));
}

static void
wr(struct swr *w, uint32_t off, uint32_t val)
{
	mmio_wr(w->m, off, val);
}

/* qcom_swrm_init (the probe's order), after the CGCR reset */
int
swr_init(struct swr *w)
{
	uint32_t val;
	unsigned i;

	if (w->timeout_ms == 0)
		w->timeout_ms = 100;		/* TIMEOUT_MS */
	w->version = rd(w, SWRM_COMP_HW_VERSION);
	wr(w, SWRM_MCP_FRAME_CTRL_BANK_ADDR(0),
	    SWR_ROWS_INDEX << 3 | SWR_COLS_INDEX);
	wr(w, SWRM_ENUMERATOR_CFG_ADDR, 1);	/* auto enumeration */
	wr(w, SWRM_INTERRUPT_MASK_ADDR, SWRM_INTERRUPT_STATUS_RMSK);
	val = rd(w, SWRM_MCP_CFG_ADDR);
	val = (val & ~SWRM_MCP_CFG_NO_PINGS_MASK) |
	    SWRM_DEF_CMD_NO_PINGS << SWRM_MCP_CFG_NO_PINGS_SHIFT;
	wr(w, SWRM_MCP_CFG_ADDR, val);
	wr(w, SWRM_MCP_BUS_CTRL, SWRM_MCP_BUS_CLK_START);
	wr(w, SWRM_CMD_FIFO_CFG_ADDR, w->version >= SWRM_VERSION_1_5_1 ?
	    SWRM_RD_WR_CMD_RETRIES | SWRM_CONTINUE_EXEC_ON_CMD_IGNORE :
	    SWRM_RD_WR_CMD_RETRIES);
	wr(w, SWRM_COMP_CFG_ADDR, SWRM_COMP_CFG_ENABLE_MSK);
	wr(w, SWRM_COMP_CFG_ADDR, SWRM_COMP_CFG_IRQ_LEVEL_OR_PULSE_MSK);
	wr(w, SWRM_INTERRUPT_CLEAR, 0xffffffff);
	wr(w, SWRM_INTERRUPT_CPU_EN, SWRM_INTERRUPT_STATUS_RMSK);
	/* Linux: pulse (with a rising edge asked of the GIC); level here */
	wr(w, SWRM_COMP_CFG_ADDR, (w->irq_level ? 0 :
	    SWRM_COMP_CFG_IRQ_LEVEL_OR_PULSE_MSK) | SWRM_COMP_CFG_ENABLE_MSK);
	w->intr_mask = SWRM_INTERRUPT_STATUS_RMSK;
	w->slave_status = 0;
	w->clk_stopped = false;
	/* swrm_wait_for_frame_gen_enabled: 100 tries of 500 us */
	for (i = 0; i <= 100; i++) {
		if (rd(w, SWRM_COMP_STATUS) & SWRM_FRM_GEN_ENABLED)
			break;
		fma_sleep_ms(1);
	}
	val = rd(w, SWRM_COMP_PARAMS);
	w->wr_fifo_depth = (val >> 10) & 0x1f;
	w->rd_fifo_depth = (val >> 15) & 0x1f;
	w->dout = val & 0x1f;
	w->din = (val >> 5) & 0x1f;
	w->up = i <= 100;
	w->cmd_id = 0;
	fma_log("%s: v%u.%u.%u, master id %u, %u out and %u in ports, FIFOs "
	    "%u/%u, frame generator %s", w->name, w->version >> 24,
	    (w->version >> 16) & 0xff, w->version & 0xffff,
	    rd(w, SWRM_COMP_MASTER_ID), w->dout, w->din, w->wr_fifo_depth,
	    w->rd_fifo_depth, w->up ? "running" : "not running");
	if (w->wr_fifo_depth == 0)
		w->wr_fifo_depth = 1;
	return (w->up ? 0 : -EIO);
}

/*
 * The hardware enumerated the slaves (SWRM_ENUMERATOR_CFG_ADDR): device
 * numbers 1-11 with their status in MCP_SLV_STATUS, 2 bits each, their
 * ids in the enumerator's table (qcom_swrm_enumerate's byte order).
 */
int
swr_enumerate(struct swr *w, uint8_t unique_id, unsigned wait_ms)
{
	uint64_t until = fma_now_ms() + wait_ms, addr;
	uint32_t st, v1, v2;
	unsigned i;

	w->devnum = 0;
	do {
		st = rd(w, SWRM_MCP_SLV_STATUS);
		for (i = 1; i <= 11; i++) {
			if (((st >> (i * 2)) & 3) == 0)
				continue;
			v1 = rd(w, SWRM_ENUMERATOR_SLAVE_DEV_ID_1(i));
			v2 = rd(w, SWRM_ENUMERATOR_SLAVE_DEV_ID_2(i));
			if (v1 == 0 && v2 == 0)
				break;
			/* buf2[1] | buf2[0] << 8 | buf1[3] << 16 | ... */
			addr = (uint64_t)((v2 >> 8) & 0xff) |
			    (uint64_t)(v2 & 0xff) << 8 |
			    (uint64_t)((v1 >> 24) & 0xff) << 16 |
			    (uint64_t)((v1 >> 16) & 0xff) << 24 |
			    (uint64_t)((v1 >> 8) & 0xff) << 32 |
			    (uint64_t)(v1 & 0xff) << 40;
			fma_log("%s: device %u: id %#014jx (version %u, unique "
			    "%u, manufacturer %#06x, part %#06x, class %#04x), "
			    "status %u", w->name, i, (uintmax_t)addr,
			    (unsigned)(addr >> 44) & 0xf,
			    (unsigned)(addr >> 40) & 0xf,
			    (unsigned)(addr >> 24) & 0xffff,
			    (unsigned)(addr >> 8) & 0xffff,
			    (unsigned)addr & 0xff, (st >> (i * 2)) & 3);
			if (((addr >> 40) & 0xf) == unique_id) {
				w->devnum = (uint8_t)i;
				w->slave_id = addr;
			}
		}
		if (w->devnum != 0)
			return (0);
		fma_sleep_ms(5);
	} while (fma_now_ms() < until);
	fma_log("%s: no device with unique id %u (slave status %#x)", w->name,
	    unique_id, st);
	return (-ENODEV);
}

/* swrm_get_packed_reg_val's ids: 1-14, then 0; the broadcast's is 15 */
static uint32_t
pack(struct swr *w, uint8_t data, uint8_t dev, uint16_t reg)
{
	uint8_t id;

	if (dev == SDW_BROADCAST_DEV_NUM)
		id = SWR_BROADCAST_CMD_ID;
	else {
		w->cmd_id = w->cmd_id < SWR_MAX_CMD_ID ? w->cmd_id + 1 : 0;
		id = w->cmd_id;
	}
	return (SWRM_REG_VAL_PACK(data, dev, id, reg));
}

static int
wait_wr_fifo(struct swr *w)
{
	unsigned i;

	for (i = 0; i <= 30; i++) {	/* SWR_OVERFLOW_RETRY_COUNT */
		if (SWRM_WR_CMD_FIFO_CNT(rd(w, SWRM_CMD_FIFO_STATUS)) <
		    w->wr_fifo_depth)
			return (0);
		fma_sleep_ms(1);
	}
	return (-EIO);
}

int
swr_write(struct swr *w, uint8_t dev, uint16_t reg, uint8_t val)
{
	uint64_t until;
	uint32_t st;

	if (wait_wr_fifo(w) != 0) {
		w->errors++;
		fma_log("%s: write %u:%#x: command FIFO full", w->name, dev, reg);
		return (-EIO);
	}
	if (dev == SDW_BROADCAST_DEV_NUM)
		wr(w, SWRM_INTERRUPT_CLEAR, SWRM_INTERRUPT_SPECIAL_CMD_ID_FINISHED);
	wr(w, SWRM_CMD_FIFO_WR_CMD, pack(w, val, dev, reg));
	w->writes++;
	if (dev != SDW_BROADCAST_DEV_NUM)
		return (0);
	/* the broadcast's completion (Linux: its interrupt, 100 ms) */
	until = fma_now_ms() + w->timeout_ms;
	do {
		st = rd(w, SWRM_INTERRUPT_STATUS);
		if (st & SWRM_INTERRUPT_SPECIAL_CMD_ID_FINISHED) {
			wr(w, SWRM_INTERRUPT_CLEAR,
			    SWRM_INTERRUPT_SPECIAL_CMD_ID_FINISHED);
			return (0);
		}
		fma_sleep_ms(0);
	} while (fma_now_ms() < until);
	w->errors++;
	fma_log("%s: broadcast %#x <- %#x: no completion (interrupt status "
	    "%#x)", w->name, reg, val, st);
	return (-ETIMEDOUT);
}

int
swr_read(struct swr *w, uint8_t dev, uint16_t reg, uint8_t *val)
{
	uint32_t cmd, data = 0;
	unsigned i, try;

	cmd = pack(w, 1, dev, reg);	/* the length in the data field */
	(void)wait_wr_fifo(w);
	for (try = 0; try < 3; try++) {	/* MAX_FIFO_RD_RETRY */
		if (try > 0)
			wr(w, SWRM_CMD_FIFO_CMD, SWRM_CMD_FIFO_FLUSH);
		wr(w, SWRM_CMD_FIFO_RD_CMD, cmd);
		for (i = 0; i <= 30; i++) {
			if (SWRM_RD_CMD_FIFO_CNT(rd(w, SWRM_CMD_FIFO_STATUS)) > 0)
				break;
			fma_sleep_ms(1);
		}
		if (i > 30)
			continue;
		data = rd(w, SWRM_CMD_FIFO_RD_FIFO_ADDR);
		if (SWRM_RD_FIFO_CMD_ID(data) == w->cmd_id) {
			*val = data & 0xff;
			w->reads++;
			return (0);
		}
	}
	w->errors++;
	fma_log("%s: read %u:%#x: no answer (FIFO %#x, id %u)", w->name, dev,
	    reg, data, w->cmd_id);
	return (-EIO);
}

static int
swr_update(struct swr *w, uint8_t dev, uint16_t reg, uint8_t mask, uint8_t val)
{
	uint8_t old = 0;
	int r;

	if ((r = swr_read(w, dev, reg, &old)) != 0)
		return (r);
	return (swr_write(w, dev, reg, (uint8_t)((old & ~mask) | (val & mask))));
}

/*
 * One stream: ports[i] the master's (and, through the device tree's
 * qcom,rx/tx-port-mapping, which is 1:1 for the ports used here, the
 * slave's) port numbers, chmask[i] their channels.
 */
int
swr_stream(struct swr *w, const uint8_t *ports, unsigned nports,
    const uint8_t *chmask, bool on)
{
	const struct swr_port *pc;
	unsigned i, b, n, bps;
	uint32_t v;
	int r = 0;

	if (w->devnum == 0)
		return (-ENODEV);
	for (i = 0; i < nports; i++) {
		n = ports[i];
		if (n < 1 || n > w->nports)
			return (-EINVAL);
		pc = &w->port[n - 1];
		bps = pc->word_length != SWR_INVALID_PARAM ?
		    pc->word_length + 1u : 1u;
		for (b = 0; b < 2 && on; b++) {
			/* the slave: sdw_program_slave_port_params (SIMPLE) */
			r |= swr_update(w, w->devnum, SDW_DPN_PORTCTRL(n), 0xf, 0);
			r |= swr_write(w, w->devnum, SDW_DPN_BLOCKCTRL1(n),
			    (uint8_t)(bps - 1));
			r |= swr_write(w, w->devnum, SDW_DPN_SAMPLECTRL1_B(n, b),
			    pc->si);		/* sample_interval - 1 */
			r |= swr_write(w, w->devnum, SDW_DPN_OFFSETCTRL1_B(n, b),
			    pc->off1);
			r |= swr_write(w, w->devnum, SDW_DPN_LANECTRL_B(n, b),
			    pc->lane_control);
			/* the master: qcom_swrm_transport_params, _port_params */
			wr(w, DP_PORT_CTRL_BANK(n, b), (uint32_t)pc->off1 << 8 |
			    (uint32_t)pc->off2 << 16 | pc->si);
			if (pc->lane_control != SWR_INVALID_PARAM)
				wr(w, DP_PORT_CTRL_2_BANK(n, b), pc->lane_control);
			if (pc->blk_group_count != SWR_INVALID_PARAM)
				wr(w, DP_BLOCK_CTRL2_BANK(n, b),
				    pc->blk_group_count);
			wr(w, DP_PORT_HCTRL_BANK(n, b),
			    pc->hstart != SWR_INVALID_PARAM &&
			    pc->hstop != SWR_INVALID_PARAM ?
			    (uint32_t)pc->hstop << 4 | pc->hstart : 0xf0);
			if (pc->bp_mode != SWR_INVALID_PARAM)
				wr(w, DP_BLOCK_CTRL3_BANK(n, b), pc->bp_mode);
			wr(w, DP_BLOCK_CTRL_1(n), bps - 1);
		}
		/* the channels: sdw_enable_disable_ports, both banks */
		for (b = 0; b < 2; b++) {
			r |= swr_write(w, w->devnum, SDW_DPN_CHANNELEN_B(n, b),
			    on ? chmask[i] : 0);
			v = rd(w, DP_PORT_CTRL_BANK(n, b));
			if (on)
				v |= (uint32_t)chmask[i] << SWRM_DP_PORT_CTRL_EN_CHAN_SHFT;
			else
				v &= ~(0xffu << SWRM_DP_PORT_CTRL_EN_CHAN_SHFT);
			wr(w, DP_PORT_CTRL_BANK(n, b), v);
		}
	}
	/* the bank switch: the next bank's frame shape, then SCP_FrameCtrl */
	b = !(rd(w, SWRM_MCP_STATUS) & 1);
	v = rd(w, SWRM_MCP_FRAME_CTRL_BANK_ADDR(b));
	v = (v & ~0xffu) | SWR_ROWS_INDEX << 3 | SWR_COLS_INDEX;
	wr(w, SWRM_MCP_FRAME_CTRL_BANK_ADDR(b), v);
	r |= swr_write(w, SDW_BROADCAST_DEV_NUM, SDW_SCP_FRAMECTRL_B(b),
	    SWR_FRAMECTRL);
	fma_log("%s: stream %s on %u port(s), bank %u now (MCP status %#x)%s",
	    w->name, on ? "on" : "off", nports, b, rd(w, SWRM_MCP_STATUS),
	    r ? ": with errors" : "");
	return (r ? -EIO : 0);
}

void
swr_dump(struct swr *w, const char *why)
{
	fma_log("%s (%s): version %#x, status %#x, interrupts %#x, MCP %#x, "
	    "slaves %#x, FIFO %#x, frame %#x/%#x, %u writes, %u reads, %u "
	    "errors", w->name, why, rd(w, SWRM_COMP_HW_VERSION),
	    rd(w, SWRM_COMP_STATUS), rd(w, SWRM_INTERRUPT_STATUS),
	    rd(w, SWRM_MCP_STATUS), rd(w, SWRM_MCP_SLV_STATUS),
	    rd(w, SWRM_CMD_FIFO_STATUS), rd(w, SWRM_MCP_FRAME_CTRL_BANK_ADDR(0)),
	    rd(w, SWRM_MCP_FRAME_CTRL_BANK_ADDR(1)), w->writes, w->reads,
	    w->errors);
}

/* --- interrupts, alerts, clock stop --------------------------------------- */

/* the ping status of device dev (MCP_SLV_STATUS, two bits each) */
static unsigned
slv_status(uint32_t st, unsigned dev)
{
	return ((st >> (dev * 2)) & 3);
}

/*
 * sdw_initialize_slave: the slave's SCP interrupts unmasked (its
 * prop.scp_int1_mask, the WCD937x's), whatever it latched before
 * acknowledged; without this it never raises an alert
 */
int
swr_slave_init(struct swr *w, uint8_t dev)
{
	uint8_t v = 0;
	int r;

	if (dev == 0)
		return (-ENODEV);
	if (swr_read(w, dev, SDW_SCP_INT1, &v) == 0 && (v & WCD_SCP_INT1_MASK))
		(void)swr_write(w, dev, SDW_SCP_INT1, v & WCD_SCP_INT1_MASK);
	r = swr_update(w, dev, SDW_SCP_INTMASK1, WCD_SCP_INT1_MASK,
	    WCD_SCP_INT1_MASK);
	if (r == 0)
		w->slave_inited = dev;
	fma_log("%s: device %u: SCP interrupts %s (IntMask1 %#x)", w->name,
	    dev, r == 0 ? "unmasked" : "not unmasked", WCD_SCP_INT1_MASK);
	return (r);
}

/*
 * sdw_handle_slave_alerts for one device: SCP_INT1; its implementation-
 * defined interrupt to the callback (the WCD9370's: wcd_interrupt_callback,
 * its DIGITAL_INTR_STATUS_*); parity and bus clash noted; the bits
 * acknowledged (written back), and again while new ones came, at most
 * SDW_READ_INTR_CLEAR_RETRY times
 */
static int
swr_slave_alert(struct swr *w, uint8_t dev)
{
	uint8_t int1 = 0, clear;
	unsigned count = 0;

	w->alerts++;
	if (swr_read(w, dev, SDW_SCP_INT1, &int1) != 0)
		return (-EIO);
	do {
		clear = int1 & WCD_SCP_INT1_MASK;
		if (int1 & SDW_SCP_INT1_PARITY)
			fma_log("%s: device %u: parity error", w->name, dev);
		if (int1 & SDW_SCP_INT1_BUS_CLASH)
			fma_log("%s: device %u: bus clash", w->name, dev);
		if (int1 & (SDW_SCP_INT1_PORT0_3 | SDW_SCP_INT1_SCP2_CASCADE))
			fma_log("%s: device %u: port interrupts %#x (not "
			    "handled)", w->name, dev, int1 &
			    (SDW_SCP_INT1_PORT0_3 | SDW_SCP_INT1_SCP2_CASCADE));
		if ((int1 & SDW_SCP_INT1_IMPL_DEF) && w->alert != NULL) {
			w->impl_alerts++;
			w->alert(w->alert_ctx, w, dev);
		}
		if (clear != 0 && swr_write(w, dev, SDW_SCP_INT1, clear) != 0)
			return (-EIO);
		if (swr_read(w, dev, SDW_SCP_INT1, &int1) != 0)
			return (-EIO);
		int1 &= WCD_SCP_INT1_MASK;
	} while (int1 != 0 && ++count < SDW_READ_INTR_CLEAR_RETRY);
	if (count == SDW_READ_INTR_CLEAR_RETRY)
		fma_log("%s: device %u: still alerting after %u rounds",
		    w->name, dev, count);
	return (0);
}

/* qcom_swrm_get_device_status for the device we drive: back, gone */
static void
swr_status_changed(struct swr *w, uint32_t st)
{
	unsigned s = slv_status(st, w->devnum), was;

	was = slv_status(w->slave_status, w->devnum);
	w->slave_status = st;
	if (w->devnum == 0 || s == was)
		return;
	fma_log("%s: device %u: status %u -> %u", w->name, w->devnum, was, s);
	if (s == 0)
		w->slave_inited = 0;
	else if (was == 0 || w->slave_inited != w->devnum)
		(void)swr_slave_init(w, w->devnum);
	if (s == SDW_SLAVE_ALERT)
		(void)swr_slave_alert(w, w->devnum);
}

/*
 * qcom_swrm_irq_handler: the status, each bit as Linux handles it, the
 * status cleared, again while new bits came. The master must be clocked.
 * The result: the bits seen (0: nothing was pending, a spurious event).
 */
uint32_t
swr_irq(struct swr *w)
{
	uint32_t st, stm, seen = 0, slv, v;
	unsigned dev, rounds = 0;

	if (w->clk_stopped)
		return (0);
	st = rd(w, SWRM_INTERRUPT_STATUS);
	stm = st & w->intr_mask;
	while (stm != 0 && rounds++ < 8) {
		seen |= stm;
		if (stm & SWRM_INTERRUPT_SLAVE_PEND_IRQ) {
			slv = rd(w, SWRM_MCP_SLV_STATUS);
			for (dev = 1; dev <= 11; dev++)
				if (slv_status(slv, dev) == SDW_SLAVE_ALERT)
					(void)swr_slave_alert(w, (uint8_t)dev);
		}
		if (stm & (SWRM_INTERRUPT_NEW_SLAVE_ATTACHED |
		    SWRM_INTERRUPT_CHANGE_ENUM_SLAVE_STATUS)) {
			slv = rd(w, SWRM_MCP_SLV_STATUS);
			if (slv != w->slave_status)
				swr_status_changed(w, slv);
		}
		if (stm & SWRM_INTERRUPT_MASTER_CLASH_DET) {
			fma_log("%s: bus clash: its interrupt masked", w->name);
			w->intr_mask &= ~SWRM_INTERRUPT_MASTER_CLASH_DET;
			wr(w, SWRM_INTERRUPT_CPU_EN, w->intr_mask);
		}
		if (stm & (SWRM_INTERRUPT_RD_FIFO_OVERFLOW |
		    SWRM_INTERRUPT_RD_FIFO_UNDERFLOW))
			fma_log("%s: read FIFO %s (FIFO %#x)", w->name,
			    (stm & SWRM_INTERRUPT_RD_FIFO_OVERFLOW) ? "overflow" :
			    "underflow", rd(w, SWRM_CMD_FIFO_STATUS));
		if (stm & (SWRM_INTERRUPT_WR_CMD_FIFO_OVERFLOW |
		    SWRM_INTERRUPT_CMD_ERROR)) {
			v = rd(w, SWRM_CMD_FIFO_STATUS);
			fma_log("%s: command %s (FIFO %#x): flushed", w->name,
			    (stm & SWRM_INTERRUPT_CMD_ERROR) ? "error" :
			    "FIFO overflow", v);
			wr(w, SWRM_CMD_FIFO_CMD, SWRM_CMD_FIFO_FLUSH);
		}
		if (stm & (SWRM_INTERRUPT_DOUT_PORT_COLLISION |
		    SWRM_INTERRUPT_READ_EN_RD_VALID_MISMATCH)) {
			fma_log("%s: %s: its interrupt masked", w->name,
			    (stm & SWRM_INTERRUPT_DOUT_PORT_COLLISION) ?
			    "port collision" : "read enable mismatch");
			w->intr_mask &= ~(stm & (SWRM_INTERRUPT_DOUT_PORT_COLLISION |
			    SWRM_INTERRUPT_READ_EN_RD_VALID_MISMATCH));
			wr(w, SWRM_INTERRUPT_CPU_EN, w->intr_mask);
		}
		wr(w, SWRM_INTERRUPT_CLEAR, st);
		st = rd(w, SWRM_INTERRUPT_STATUS);
		stm = st & w->intr_mask;
	}
	w->irqs++;
	if (seen == 0)
		w->spurious++;
	return (seen);
}

uint32_t
swr_status(struct swr *w)
{
	return (w->clk_stopped ? 0 : rd(w, SWRM_INTERRUPT_STATUS));
}

/* an alert waiting at the master: its status, or the device's ping */
bool
swr_alert_pending(struct swr *w)
{
	if (w->clk_stopped || w->devnum == 0)
		return (false);
	return ((rd(w, SWRM_INTERRUPT_STATUS) & SWRM_INTERRUPT_SLAVE_PEND_IRQ) ||
	    slv_status(rd(w, SWRM_MCP_SLV_STATUS), w->devnum) == SDW_SLAVE_ALERT);
}

/* swrm_wait_for_wr_fifo_done: what is queued has gone out */
static void
wait_wr_fifo_done(struct swr *w)
{
	unsigned i;

	for (i = 0; i <= 30; i++) {
		if (SWRM_WR_CMD_FIFO_CNT(rd(w, SWRM_CMD_FIFO_STATUS)) == 0)
			return;
		fma_sleep_ms(1);
	}
	fma_log("%s: the write FIFO did not drain", w->name);
}

/*
 * swrm_runtime_suspend (clock stop supported): the clash masked, no
 * prepare for simple_clk_stop_capable slaves (wake_en: SCP_SystemCtrl's
 * WakeUpEnable written anyway, which Linux does only for slaves that
 * prepare), ClockStopNow broadcast. The caller stops the master's clock.
 */
int
swr_clock_stop(struct swr *w)
{
	int r;

	if (w->clk_stopped)
		return (0);
	wait_wr_fifo_done(w);
	w->intr_mask &= ~SWRM_INTERRUPT_MASTER_CLASH_DET;
	wr(w, SWRM_INTERRUPT_MASK_ADDR, w->intr_mask);
	wr(w, SWRM_INTERRUPT_CPU_EN, w->intr_mask);
	if (w->wake_en && w->devnum != 0)
		(void)swr_write(w, w->devnum, SDW_SCP_SYSTEMCTRL,
		    SDW_SCP_SYSTEMCTRL_WAKE_UP_EN);
	r = swr_write(w, SDW_BROADCAST_DEV_NUM, SDW_SCP_CTRL,
	    SDW_SCP_CTRL_CLK_STP_NOW);
	if (r != 0) {
		/* nobody acknowledged it: the bus stays as it is */
		w->intr_mask |= SWRM_INTERRUPT_MASTER_CLASH_DET;
		wr(w, SWRM_INTERRUPT_MASK_ADDR, w->intr_mask);
		wr(w, SWRM_INTERRUPT_CPU_EN, w->intr_mask);
		fma_log("%s: clock stop refused (%d)", w->name, r);
		return (r);
	}
	w->clk_stopped = true;
	w->clkstops++;
	return (0);
}

/*
 * swrm_runtime_resume (clock stop supported), after the master's clock is
 * back and its CGCR reset done: the bus clock started, the clash cleared
 * and unmasked, the frame generator awaited. The slaves (simple clock
 * stop) need nothing; one that lost sync re-attaches, which swr_irq sees.
 */
int
swr_clock_start(struct swr *w)
{
	unsigned i;

	wr(w, SWRM_MCP_BUS_CTRL, SWRM_MCP_BUS_CLK_START);
	wr(w, SWRM_INTERRUPT_CLEAR, SWRM_INTERRUPT_MASTER_CLASH_DET);
	w->intr_mask |= SWRM_INTERRUPT_MASTER_CLASH_DET;
	wr(w, SWRM_INTERRUPT_MASK_ADDR, w->intr_mask);
	wr(w, SWRM_INTERRUPT_CPU_EN, w->intr_mask);
	for (i = 0; i <= 100; i++) {
		if (rd(w, SWRM_COMP_STATUS) & SWRM_FRM_GEN_ENABLED)
			break;
		fma_sleep_ms(1);
	}
	w->clk_stopped = false;
	w->clkstarts++;
	if (i > 100) {
		fma_log("%s: the frame generator did not come back after the "
		    "clock stop", w->name);
		return (-EIO);
	}
	return (0);
}
