/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The arithmetic of the Qualcomm GENI UART (uart_dev_qcom_geni.c), without
 * kernel dependencies, so that the host test (freebsd/fm/test) checks it:
 *
 * - the serial engine's clock: Linux's geni_se_clk_freq_match over the
 *   frequency table of the SM6115's QUP v3 serial engine clocks
 *   (drivers/clk/qcom/gcc-sm6115.c, ftbl_gcc_qupv3_wrap0_s0_clk_src, which
 *   every wrap0 serial engine uses), the divider of geni_serial_set_rate,
 *   and the register values clk-rcg2.c writes for an entry (M, N, D, CFG);
 * - the FIFO packing of geni_se_config_packing (8 bits, 4 per word).
 */

#ifndef _UART_QCOM_GENI_CALC_H_
#define	_UART_QCOM_GENI_CALC_H_

/* clk-rcg2.c */
#define	RCG_CMD			0x0
#define	 RCG_CMD_UPDATE		(1u << 0)
#define	 RCG_CMD_ROOT_OFF	(1u << 31)
#define	RCG_CFG			0x4
#define	 RCG_CFG_SRC_DIV_MASK	0x1fu		/* hid_width 5 */
#define	 RCG_CFG_SRC_SEL_SHIFT	8
#define	 RCG_CFG_SRC_SEL_MASK	(0x7u << 8)
#define	 RCG_CFG_MODE_MASK	(0x3u << 12)
#define	 RCG_CFG_MODE_DUAL_EDGE	(0x2u << 12)
#define	 RCG_CFG_HW_CLK_CTRL	(1u << 20)
#define	RCG_M			0x8
#define	RCG_N			0xc
#define	RCG_D			0x10
#define	RCG_MND_MASK		0xffffu		/* mnd_width 16 */

/*
 * The parents (gcc_parent_map_1 of gcc-sm6115.c) with the rates the table
 * below implies: F(75000000, GPLL0_OUT_AUX2, 4) is 300 MHz,
 * F(128000000, GPLL6_OUT_MAIN, 3) 384 MHz.
 */
#define	GCC_SRC_TCXO		0	/* bi_tcxo, 19.2 MHz */
#define	GCC_SRC_GPLL0_AUX2	2	/* 300 MHz */
#define	GCC_SRC_GPLL6_MAIN	4	/* 384 MHz */

struct geni_clk_freq {
	uint32_t	rate;
	uint8_t		src;		/* the parent's CFG source select */
	uint8_t		pre_div;	/* as F() stores it: 2 * div - 1 */
	uint16_t	m, n;
};

/* ftbl_gcc_qupv3_wrap0_s0_clk_src */
static const struct geni_clk_freq geni_clk_table[] = {
	{   7372800, GCC_SRC_GPLL0_AUX2, 1,  384, 15625 },
	{  14745600, GCC_SRC_GPLL0_AUX2, 1,  768, 15625 },
	{  19200000, GCC_SRC_TCXO,       1,    0,     0 },
	{  29491200, GCC_SRC_GPLL0_AUX2, 1, 1536, 15625 },
	{  32000000, GCC_SRC_GPLL0_AUX2, 1,    8,    75 },
	{  48000000, GCC_SRC_GPLL0_AUX2, 1,    4,    25 },
	{  64000000, GCC_SRC_GPLL0_AUX2, 1,   16,    75 },
	{  75000000, GCC_SRC_GPLL0_AUX2, 7,    0,     0 },
	{  80000000, GCC_SRC_GPLL0_AUX2, 1,    4,    15 },
	{  96000000, GCC_SRC_GPLL0_AUX2, 1,    8,    25 },
	{ 100000000, GCC_SRC_GPLL0_AUX2, 5,    0,     0 },
	{ 102400000, GCC_SRC_GPLL0_AUX2, 1,  128,   375 },
	{ 112000000, GCC_SRC_GPLL0_AUX2, 1,   28,    75 },
	{ 117964800, GCC_SRC_GPLL0_AUX2, 1, 6144, 15625 },
	{ 120000000, GCC_SRC_GPLL0_AUX2, 4,    0,     0 },
	{ 128000000, GCC_SRC_GPLL6_MAIN, 5,    0,     0 },
};
#define	GENI_CLK_LEVELS	(sizeof(geni_clk_table) / sizeof(geni_clk_table[0]))

/* GENI_SER_M/S_CLK_CFG, SE_GENI_CLK_SEL */
#define	GENI_SER_CLK_EN		(1u << 0)
#define	GENI_CLK_DIV_SHIFT	4
#define	GENI_CLK_DIV_MAX	0xfffu		/* GENMASK(15, 4) */
#define	GENI_CLK_SEL_MASK	0x7u
#define	GENI_UART_OVERSAMPLING	32
#define	QUP_SE_VERSION_2_5	0x20050000u

/*
 * geni_se_clk_freq_match (exact false) and geni_serial_set_rate: the
 * table entry, the divider. 0, or -1 when no entry gives the rate.
 */
static inline int
geni_uart_clk_match(uint32_t baud, uint32_t qup_version, unsigned int *index,
    uint32_t *div)
{
	uint32_t sampling, req, best = 0xffffffffu, delta, d;
	unsigned int i;
	int found = -1;

	sampling = GENI_UART_OVERSAMPLING;
	/* halved for QUP v2.5 and later */
	if (qup_version >= QUP_SE_VERSION_2_5)
		sampling /= 2;
	req = baud * sampling;
	if (baud == 0 || req / sampling != baud)
		return (-1);
	for (i = 0; i < GENI_CLK_LEVELS; i++) {
		d = (geni_clk_table[i].rate + req - 1) / req;
		delta = req - geni_clk_table[i].rate / d;
		if (delta < best) {
			*index = i;
			best = delta;
			found = 0;
			if (delta == 0)
				break;
		}
	}
	if (found != 0)
		return (-1);
	*div = (geni_clk_table[*index].rate + req - 1) / req;
	if (*div > GENI_CLK_DIV_MAX)
		return (-1);
	return (0);
}

/* __clk_rcg2_configure for one entry: M, N, D and the CFG bits it sets */
struct geni_rcg_regs {
	bool		mnd;		/* M, N, D written */
	uint32_t	m, n, d;
	uint32_t	cfg_mask, cfg;
};

static inline void
geni_rcg_regs(const struct geni_clk_freq *f, struct geni_rcg_regs *r)
{
	uint32_t d_val, n_minus_m2;

	r->mnd = f->n != 0;
	r->m = r->n = r->d = 0;
	if (r->mnd) {
		r->m = f->m & RCG_MND_MASK;
		r->n = ~(uint32_t)(f->n - f->m) & RCG_MND_MASK;
		/* the 2d value, clamped to [m, 2 (n - m)] */
		d_val = f->n;
		n_minus_m2 = 2u * (uint32_t)(f->n - f->m);
		if (d_val < f->m)
			d_val = f->m;
		if (d_val > n_minus_m2)
			d_val = n_minus_m2;
		r->d = ~d_val & RCG_MND_MASK;
	}
	r->cfg_mask = RCG_CFG_SRC_DIV_MASK | RCG_CFG_SRC_SEL_MASK |
	    RCG_CFG_MODE_MASK | RCG_CFG_HW_CLK_CTRL;
	r->cfg = f->pre_div | (uint32_t)f->src << RCG_CFG_SRC_SEL_SHIFT;
	if (r->mnd && f->m != f->n)
		r->cfg |= RCG_CFG_MODE_DUAL_EDGE;
}

/* The rate an RCG runs at, from its registers (for the report) */
static inline uint64_t
geni_rcg_rate(uint32_t cfg, uint32_t m, uint32_t n_reg)
{
	uint64_t parent, rate;
	uint32_t hid = cfg & RCG_CFG_SRC_DIV_MASK, n;

	switch ((cfg & RCG_CFG_SRC_SEL_MASK) >> RCG_CFG_SRC_SEL_SHIFT) {
	case GCC_SRC_TCXO:	parent = 19200000; break;
	case GCC_SRC_GPLL0_AUX2: parent = 300000000; break;
	case GCC_SRC_GPLL6_MAIN: parent = 384000000; break;
	default:		return (0);
	}
	/* the divider: (hid + 1) / 2 */
	rate = hid != 0 ? parent * 2 / (hid + 1) : parent;
	if ((cfg & RCG_CFG_MODE_MASK) != 0) {
		/* n = ~(n - m) */
		n = (~n_reg & RCG_MND_MASK) + (m & RCG_MND_MASK);
		if (n != 0)
			rate = rate * (m & RCG_MND_MASK) / n;
	}
	return (rate);
}

/* geni_se_config_packing(bpw, pack_words, msb_to_lsb): cfg0, cfg1 */
static inline void
geni_packing(int bpw, int pack_words, bool msb_to_lsb, uint32_t *cfg0,
    uint32_t *cfg1)
{
	uint32_t cfg[4] = { 0, 0, 0, 0 };
	int len, temp_bpw = bpw, idx_start = msb_to_lsb ? bpw - 1 : 0;
	int idx = idx_start, idx_delta = msb_to_lsb ? -8 : 8;
	int ceil_bpw = (bpw + 7) / 8 * 8, iter = ceil_bpw * pack_words / 8, i;

	*cfg0 = *cfg1 = 0;
	if (iter <= 0 || iter > 4)
		return;
	for (i = 0; i < iter; i++) {
		len = (temp_bpw < 8 ? temp_bpw : 8) - 1;
		cfg[i] = (uint32_t)idx << 5 | (uint32_t)msb_to_lsb << 4 |
		    (uint32_t)len << 1;
		if (temp_bpw <= 8) {
			idx = (i + 1) * 8 + idx_start;
			temp_bpw = bpw;
		} else {
			idx += idx_delta;
			temp_bpw -= 8;
		}
	}
	cfg[iter - 1] |= 1;		/* stop */
	*cfg0 = cfg[0] | cfg[1] << 10;
	*cfg1 = cfg[2] | cfg[3] << 10;
}

#endif /* _UART_QCOM_GENI_CALC_H_ */
