/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The SM6115's QMP USB3 PHY ("qcom,sm6115-qmp-usb3-phy" at 0x1615000):
 * Linux 7.1's phy-qcom-qmp-usbc.c, its QCM2290/SM6115 configuration
 * (qcm2290_usb3phy_cfg, qmp_usbc_offsets_v3_qcm2290), the register names
 * resolved through Linux's phy-qcom-qmp-qserdes-com.h, -qserdes-txrx-v3.h
 * and -pcs-v3.h. The tables below are generated from Linux's (each entry
 * names its register); dwc3dci_host.c runs Linux's qmp_usbc_com_init and
 * qmp_usbc_usb_power_on with them.
 */
#ifndef _DWC3DCI_QMP_H_
#define	_DWC3DCI_QMP_H_

struct qmp_init {
	uint16_t	off;
	uint8_t		val;
};

/* the blocks in the PHY's window */
#define	QMP_SERDES		0x000
#define	QMP_TX			0x200
#define	QMP_RX			0x400
#define	QMP_TX2			0x600
#define	QMP_RX2			0x800
#define	QMP_PCS_MISC		0xa00	/* the lane select (SW_PORTSELECT) */
#define	QMP_PCS			0xc00
/* PCS v3 */
#define	QMP_PCS_SW_RESET	0x000
#define	QMP_PCS_POWER_DOWN	0x004
#define	QMP_PCS_START		0x008
#define	QMP_PCS_STATUS		0x174
#define	QMP_SW_RESET		(1u << 0)
#define	QMP_SW_PWRDN		(1u << 0)
#define	QMP_SERDES_START	(1u << 0)
#define	QMP_PCS_START_BIT	(1u << 1)
#define	QMP_PHYSTATUS		(1u << 6)
#define	QMP_SW_PORTSELECT_VAL	(1u << 0)	/* the reversed plug: CC2 */
#define	QMP_SW_PORTSELECT_MUX	(1u << 1)

static const struct qmp_init qmp_serdes_tbl[] = {
	{ 0x0ac, 0x14 },	/* QSERDES_COM_SYSCLK_EN_SEL */
	{ 0x034, 0x08 },	/* QSERDES_COM_BIAS_EN_CLKBUFLR_EN */
	{ 0x174, 0x30 },	/* QSERDES_COM_CLK_SELECT */
	{ 0x03c, 0x06 },	/* QSERDES_COM_SYS_CLK_CTRL */
	{ 0x0b4, 0x00 },	/* QSERDES_COM_RESETSM_CNTRL */
	{ 0x0b8, 0x08 },	/* QSERDES_COM_RESETSM_CNTRL2 */
	{ 0x070, 0x0f },	/* QSERDES_COM_BG_TRIM */
	{ 0x19c, 0x01 },	/* QSERDES_COM_SVS_MODE_CLK_SEL */
	{ 0x178, 0x00 },	/* QSERDES_COM_HSCLK_SEL */
	{ 0x0d0, 0x82 },	/* QSERDES_COM_DEC_START_MODE0 */
	{ 0x0dc, 0x55 },	/* QSERDES_COM_DIV_FRAC_START1_MODE0 */
	{ 0x0e0, 0x55 },	/* QSERDES_COM_DIV_FRAC_START2_MODE0 */
	{ 0x0e4, 0x03 },	/* QSERDES_COM_DIV_FRAC_START3_MODE0 */
	{ 0x078, 0x0b },	/* QSERDES_COM_CP_CTRL_MODE0 */
	{ 0x084, 0x16 },	/* QSERDES_COM_PLL_RCTRL_MODE0 */
	{ 0x090, 0x28 },	/* QSERDES_COM_PLL_CCTRL_MODE0 */
	{ 0x108, 0x80 },	/* QSERDES_COM_INTEGLOOP_GAIN0_MODE0 */
	{ 0x10c, 0x00 },	/* QSERDES_COM_INTEGLOOP_GAIN1_MODE0 */
	{ 0x184, 0x0a },	/* QSERDES_COM_CORECLK_DIV */
	{ 0x04c, 0x15 },	/* QSERDES_COM_LOCK_CMP1_MODE0 */
	{ 0x050, 0x34 },	/* QSERDES_COM_LOCK_CMP2_MODE0 */
	{ 0x054, 0x00 },	/* QSERDES_COM_LOCK_CMP3_MODE0 */
	{ 0x0c8, 0x00 },	/* QSERDES_COM_LOCK_CMP_EN */
	{ 0x18c, 0x00 },	/* QSERDES_COM_CORE_CLK_EN */
	{ 0x0cc, 0x00 },	/* QSERDES_COM_LOCK_CMP_CFG */
	{ 0x128, 0x00 },	/* QSERDES_COM_VCO_TUNE_MAP */
	{ 0x00c, 0x0a },	/* QSERDES_COM_BG_TIMER */
	{ 0x010, 0x01 },	/* QSERDES_COM_SSC_EN_CENTER */
	{ 0x01c, 0x31 },	/* QSERDES_COM_SSC_PER1 */
	{ 0x020, 0x01 },	/* QSERDES_COM_SSC_PER2 */
	{ 0x014, 0x00 },	/* QSERDES_COM_SSC_ADJ_PER1 */
	{ 0x018, 0x00 },	/* QSERDES_COM_SSC_ADJ_PER2 */
	{ 0x024, 0xde },	/* QSERDES_COM_SSC_STEP_SIZE1 */
	{ 0x028, 0x07 },	/* QSERDES_COM_SSC_STEP_SIZE2 */
	{ 0x048, 0x0f },	/* QSERDES_COM_PLL_IVCO */
	{ 0x194, 0x06 },	/* QSERDES_COM_CMN_CONFIG */
	{ 0x100, 0x80 },	/* QSERDES_COM_INTEGLOOP_INITVAL */
	{ 0x0a8, 0x01 },	/* QSERDES_COM_BIAS_EN_CTRL_BY_PSM */
};
static const struct qmp_init qmp_tx_tbl[] = {
	{ 0x060, 0x10 },	/* QSERDES_V3_TX_HIGHZ_DRVR_EN */
	{ 0x0a4, 0x12 },	/* QSERDES_V3_TX_RCV_DETECT_LVL_2 */
	{ 0x08c, 0xc6 },	/* QSERDES_V3_TX_LANE_MODE_1 */
	{ 0x044, 0x00 },	/* QSERDES_V3_TX_RES_CODE_LANE_OFFSET_TX */
	{ 0x048, 0x00 },	/* QSERDES_V3_TX_RES_CODE_LANE_OFFSET_RX */
};
static const struct qmp_init qmp_rx_tbl[] = {
	{ 0x030, 0x0b },	/* QSERDES_V3_RX_UCDR_FASTLOCK_FO_GAIN */
	{ 0x044, 0x80 },	/* QSERDES_V3_RX_UCDR_PI_CONTROLS */
	{ 0x03c, 0x00 },	/* QSERDES_V3_RX_UCDR_FASTLOCK_COUNT_LOW */
	{ 0x040, 0x00 },	/* QSERDES_V3_RX_UCDR_FASTLOCK_COUNT_HIGH */
	{ 0x008, 0x0a },	/* QSERDES_V3_RX_UCDR_FO_GAIN */
	{ 0x014, 0x06 },	/* QSERDES_V3_RX_UCDR_SO_GAIN */
	{ 0x034, 0x75 },	/* QSERDES_V3_RX_UCDR_SO_SATURATION_AND_ENABLE */
	{ 0x0d4, 0x02 },	/* QSERDES_V3_RX_RX_EQU_ADAPTOR_CNTRL2 */
	{ 0x0d8, 0x4e },	/* QSERDES_V3_RX_RX_EQU_ADAPTOR_CNTRL3 */
	{ 0x0dc, 0x18 },	/* QSERDES_V3_RX_RX_EQU_ADAPTOR_CNTRL4 */
	{ 0x0f8, 0x77 },	/* QSERDES_V3_RX_RX_EQ_OFFSET_ADAPTOR_CNTRL1 */
	{ 0x0fc, 0x80 },	/* QSERDES_V3_RX_RX_OFFSET_ADAPTOR_CNTRL2 */
	{ 0x0c0, 0x0a },	/* QSERDES_V3_RX_VGA_CAL_CNTRL2 */
	{ 0x104, 0x03 },	/* QSERDES_V3_RX_SIGDET_CNTRL */
	{ 0x10c, 0x16 },	/* QSERDES_V3_RX_SIGDET_DEGLITCH_CNTRL */
	{ 0x100, 0x00 },	/* QSERDES_V3_RX_SIGDET_ENABLES */
	{ 0x164, 0x00 },	/* QSERDES_V3_RX_RX_MODE_00 */
};
static const struct qmp_init qmp_pcs_tbl[] = {
	{ 0x00c, 0x9f },	/* QPHY_V3_PCS_TXMGN_V0 */
	{ 0x024, 0x17 },	/* QPHY_V3_PCS_TXDEEMPH_M6DB_V0 */
	{ 0x028, 0x0f },	/* QPHY_V3_PCS_TXDEEMPH_M3P5DB_V0 */
	{ 0x0c8, 0x83 },	/* QPHY_V3_PCS_FLL_CNTRL2 */
	{ 0x0c4, 0x02 },	/* QPHY_V3_PCS_FLL_CNTRL1 */
	{ 0x0cc, 0x09 },	/* QPHY_V3_PCS_FLL_CNT_VAL_L */
	{ 0x0d0, 0xa2 },	/* QPHY_V3_PCS_FLL_CNT_VAL_H_TOL */
	{ 0x0d4, 0x85 },	/* QPHY_V3_PCS_FLL_MAN_CODE */
	{ 0x080, 0xd1 },	/* QPHY_V3_PCS_LOCK_DETECT_CONFIG1 */
	{ 0x084, 0x1f },	/* QPHY_V3_PCS_LOCK_DETECT_CONFIG2 */
	{ 0x088, 0x47 },	/* QPHY_V3_PCS_LOCK_DETECT_CONFIG3 */
	{ 0x0b8, 0x75 },	/* QPHY_V3_PCS_RXEQTRAINING_WAIT_TIME */
	{ 0x0bc, 0x13 },	/* QPHY_V3_PCS_RXEQTRAINING_RUN_TIME */
	{ 0x0b0, 0x86 },	/* QPHY_V3_PCS_LFPS_TX_ECSTART_EQTLOCK */
	{ 0x0a0, 0x04 },	/* QPHY_V3_PCS_PWRUP_RESET_DLY_TIME_AUXCLK */
	{ 0x08c, 0x44 },	/* QPHY_V3_PCS_TSYNC_RSYNC_TIME */
	{ 0x070, 0xe7 },	/* QPHY_V3_PCS_RCVR_DTCT_DLY_P1U2_L */
	{ 0x074, 0x03 },	/* QPHY_V3_PCS_RCVR_DTCT_DLY_P1U2_H */
	{ 0x078, 0x40 },	/* QPHY_V3_PCS_RCVR_DTCT_DLY_U3_L */
	{ 0x07c, 0x00 },	/* QPHY_V3_PCS_RCVR_DTCT_DLY_U3_H */
	{ 0x1d8, 0x88 },	/* QPHY_V3_PCS_RX_SIGDET_LVL */
};

#endif
