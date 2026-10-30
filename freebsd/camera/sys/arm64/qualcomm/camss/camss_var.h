/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The camera module's helpers for what FreeBSD has no driver for on the
 * SM6115: the global clock controller's camera clocks, PLLs and power
 * domain (camss_gcc.c), the TLMM's pin functions (camss_tlmm.c), the SMMU
 * streams (camss_smmu.c). Shared by the CCI (qcom_cci.c) and the CAMSS
 * (qcom_camss.c) drivers.
 */
#ifndef _CAMSS_VAR_H_
#define	_CAMSS_VAR_H_

#include <sys/types.h>
#include <dev/ofw/openfirm.h>

#ifndef _KERNEL
#define	nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#endif

/* --- GCC (camss_gcc.c, tables in camss_gcc_sm6115.h) -------------------- */

/* register bits, from Linux's clk-branch.c, clk-rcg2.c, clk-alpha-pll.c,
 * gdsc.c */
#define	BRANCH_CLK_OFF		(1u << 31)	/* halt register */
#define	RCG_CMD			0x0
#define	 RCG_CMD_UPDATE		(1u << 0)
#define	 RCG_CMD_ROOT_EN	(1u << 1)
#define	 RCG_CMD_ROOT_OFF	(1u << 31)
#define	RCG_CFG			0x4
#define	 RCG_CFG_SRC_SEL_SHIFT	8
#define	 RCG_CFG_SRC_SEL_MASK	(0x7u << 8)
#define	 RCG_CFG_MODE_MASK	(0x3u << 12)
#define	 RCG_CFG_MODE_DUAL_EDGE	(0x2u << 12)
#define	 RCG_CFG_HW_CLK_CTRL	(1u << 20)
#define	RCG_M			0x8
#define	RCG_N			0xc
#define	RCG_D			0x10
#define	PLL_MODE		0x0
#define	 PLL_OUTCTRL		(1u << 0)
#define	 PLL_BYPASSNL		(1u << 1)
#define	 PLL_RESET_N		(1u << 2)
#define	 PLL_VOTE_FSM_ENA	(1u << 20)
#define	 PLL_ACTIVE_FLAG	(1u << 30)
#define	 PLL_LOCK_DET		(1u << 31)
#define	PLL_L_VAL		0x04		/* CLK_ALPHA_PLL_TYPE_DEFAULT_EVO */
#define	PLL_USER_CTL		0x18
#define	 PLL_POST_DIV_SHIFT	8
#define	 PLL_POST_DIV_MASK	(0xfu << 8)	/* .width = 4 */
#define	GDSC_PWR_ON		(1u << 31)
#define	GDSC_EN_REST_WAIT_MASK	(0xfu << 20)
#define	GDSC_EN_FEW_WAIT_MASK	(0xfu << 16)
#define	GDSC_CLK_DIS_WAIT_MASK	(0xfu << 12)
#define	GDSC_SW_OVERRIDE	(1u << 2)
#define	GDSC_HW_CONTROL		(1u << 1)
#define	GDSC_SW_COLLAPSE	(1u << 0)
#define	GDSC_WAITS		(0x2u << 20 | 0x8u << 16 | 0x2u << 12)

/* the M/N/D and CFG words clk_rcg2 writes for a frequency table entry */
struct camss_rcg_words {
	uint32_t	cfg, cfg_mask;
	int		mnd;		/* M/N/D are written */
	uint32_t	m, n, d, mnd_mask;
};

#ifdef _KERNEL
struct sbuf;

int	camss_gcc_attach(void);
int	camss_gdsc_get(uint32_t reg, const char *who);
void	camss_gdsc_put(uint32_t reg, const char *who);
int	camss_gdsc_vote(uint32_t reg, const char *who);
int	camss_clk_on(int branch, uint32_t hz, const char *who);
void	camss_clk_off(int branch, const char *who);
int	camss_clk_by_dt(phandle_t gcc, uint32_t dt);
uint64_t camss_clk_hz(int branch);
int	camss_clk_reparent_ok(int rcg, uint32_t hz);
void	camss_gcc_report(struct sbuf *sb);
int	camss_dt_clocks_on(phandle_t node, const char *who, int *ids, int max);
void	camss_dt_clocks_off(const int *ids, int n, const char *who);

int	camss_pinctrl(phandle_t node, int idx, const char *who);
int	camss_gpio_out(u_int pin, int value, const char *who);
int	camss_gpio_read(u_int pin, uint32_t *ctl, uint32_t *io);
int	camss_dt_gpio(phandle_t node, const char *prop, int *pin, int *active_low);

int	camss_smmu_bypass(phandle_t xref, uint32_t sid, uint32_t mask,
	    const char *who);
#endif

/* the encoding shared with the host tests (camss_gcc.c) */
int	camss_rcg_encode(u_int rcg, uint32_t hz, struct camss_rcg_words *w);
uint64_t camss_rcg_decode(u_int rcg, uint32_t cfg, uint32_t m, uint32_t n,
	    const uint64_t *src_hz);

#endif /* _CAMSS_VAR_H_ */
