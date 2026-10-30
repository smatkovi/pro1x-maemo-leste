/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Registers of the SM6115's IPA v4.2 and its GSI, for the AP's execution
 * environment (EE 0). Offsets and fields are Linux 7.1's
 * drivers/net/ipa/reg/ipa_reg-v4.2.c and reg/gsi_reg-v4.0.c (gsi_reg.c
 * uses the v4.0 table for IPA v4.2), the values of the fields from
 * ipa_reg.h, gsi_reg.h and gsi_private.h; freebsd/data/test/test_regs.c
 * compares every offset and mask here with Linux's tables.
 */

#ifndef _IPA_REGS_H_
#define	_IPA_REGS_H_

#ifndef _KERNEL
#include <stdint.h>
#endif
#ifndef __packed
#define	__packed	__attribute__((__packed__))
#endif

#define	IPA_BIT(n)		(1U << (n))
#define	IPA_GENMASK(h, l)	(((~0U) << (l)) & (~0U >> (31 - (h))))
/* a value into / out of a field given by its mask */
#define	IPA_FIELD(mask, v)	(((uint32_t)(v) << (__builtin_ctz(mask))) & (mask))
#define	IPA_GET(mask, r)	(((uint32_t)(r) & (mask)) >> __builtin_ctz(mask))
#define	IPA_FIELD_MAX(mask)	((mask) >> __builtin_ctz(mask))

/* --- IPA ("ipa-reg", 0x05840000) ------------------------------------------------ */

#define	IPA_COMP_CFG			0x0000003c
#define	  COMP_GSI_MULTI_INORDER_RD_DIS	IPA_BIT(7)
#define	  COMP_GSI_MULTI_INORDER_WR_DIS	IPA_BIT(8)
#define	  COMP_GSI_MULTI_AXI_MASTERS_DIS IPA_BIT(15)
#define	IPA_CLKON_CFG			0x00000044
#define	  CLKON_GLOBAL			IPA_BIT(28)
#define	  CLKON_GLOBAL_2X_CLK		IPA_BIT(29)
#define	IPA_ROUTE			0x00000048
#define	  ROUTE_DIS			IPA_BIT(0)
#define	  ROUTE_DEF_PIPE		IPA_GENMASK(5, 1)
#define	  ROUTE_DEF_HDR_TABLE		IPA_BIT(6)
#define	  ROUTE_DEF_HDR_OFST		IPA_GENMASK(16, 7)
#define	  ROUTE_FRAG_DEF_PIPE		IPA_GENMASK(21, 17)
#define	  ROUTE_DEF_RETAIN_HDR		IPA_BIT(24)
#define	IPA_SHARED_MEM_SIZE		0x00000054
#define	  MEM_SIZE			IPA_GENMASK(15, 0)
#define	  MEM_BADDR			IPA_GENMASK(31, 16)
#define	IPA_QSB_MAX_WRITES		0x00000074
#define	  GEN_QMB_0_MAX_WRITES		IPA_GENMASK(3, 0)
#define	  GEN_QMB_1_MAX_WRITES		IPA_GENMASK(7, 4)
#define	IPA_QSB_MAX_READS		0x00000078
#define	  GEN_QMB_0_MAX_READS		IPA_GENMASK(3, 0)
#define	  GEN_QMB_1_MAX_READS		IPA_GENMASK(7, 4)
#define	  GEN_QMB_0_MAX_READS_BEATS	IPA_GENMASK(23, 16)
#define	  GEN_QMB_1_MAX_READS_BEATS	IPA_GENMASK(31, 24)
#define	IPA_STATE_AGGR_ACTIVE		0x000000b4	/* stride 4 */
#define	IPA_FILT_ROUT_HASH_EN		0x00000148
#define	IPA_BCR				0x000001d0
#define	IPA_LOCAL_PKT_PROC_CNTXT	0x000001e8
#define	  LOCAL_PKT_PROC_BASE_ADDR	IPA_GENMASK(16, 0)
#define	IPA_AGGR_FORCE_CLOSE		0x000001ec	/* stride 4 */
#define	IPA_COUNTER_CFG			0x000001f0
#define	  AGGR_GRANULARITY		IPA_GENMASK(8, 4)
#define	IPA_TX_CFG			0x000001fc
#define	  TX_CFG_PA_MASK_EN		IPA_BIT(12)
#define	IPA_FLAVOR_0			0x00000210
#define	  FLAVOR_MAX_PIPES		IPA_GENMASK(3, 0)
#define	  FLAVOR_MAX_CONS_PIPES		IPA_GENMASK(12, 8)
#define	  FLAVOR_MAX_PROD_PIPES		IPA_GENMASK(20, 16)
#define	  FLAVOR_PROD_LOWEST		IPA_GENMASK(27, 24)
#define	IPA_IDLE_INDICATION_CFG		0x00000240
#define	  ENTER_IDLE_DEBOUNCE_THRESH	IPA_GENMASK(15, 0)
#define	  CONST_NON_IDLE_ENABLE		IPA_BIT(16)
/* resource group limits, one register per resource type (stride 0x20) */
#define	IPA_SRC_RSRC_GRP_01_RSRC_TYPE(t) (0x00000400 + 0x20 * (t))
#define	IPA_SRC_RSRC_GRP_23_RSRC_TYPE(t) (0x00000404 + 0x20 * (t))
#define	IPA_DST_RSRC_GRP_01_RSRC_TYPE(t) (0x00000500 + 0x20 * (t))
#define	IPA_DST_RSRC_GRP_23_RSRC_TYPE(t) (0x00000504 + 0x20 * (t))
#define	  RSRC_X_MIN_LIM		IPA_GENMASK(5, 0)
#define	  RSRC_X_MAX_LIM		IPA_GENMASK(13, 8)
#define	  RSRC_Y_MIN_LIM		IPA_GENMASK(21, 16)
#define	  RSRC_Y_MAX_LIM		IPA_GENMASK(29, 24)

/* endpoint registers, stride 0x70 */
#define	IPA_ENDP(base, ep)		((base) + 0x70 * (ep))
#define	IPA_ENDP_INIT_CFG		0x00000808
#define	  CFG_FRAG_OFFLOAD_EN		IPA_BIT(0)
#define	  CFG_CS_OFFLOAD_EN		IPA_GENMASK(2, 1)
#define	  CFG_CS_METADATA_HDR_OFFSET	IPA_GENMASK(6, 3)
#define	  CFG_CS_GEN_QMB_MASTER_SEL	IPA_BIT(8)
#define	IPA_ENDP_INIT_NAT		0x0000080c
#define	  NAT_EN			IPA_GENMASK(1, 0)
#define	IPA_ENDP_INIT_HDR		0x00000810
#define	  HDR_LEN			IPA_GENMASK(5, 0)
#define	  HDR_OFST_METADATA_VALID	IPA_BIT(6)
#define	  HDR_OFST_METADATA		IPA_GENMASK(12, 7)
#define	  HDR_ADDITIONAL_CONST_LEN	IPA_GENMASK(18, 13)
#define	  HDR_OFST_PKT_SIZE_VALID	IPA_BIT(19)
#define	  HDR_OFST_PKT_SIZE		IPA_GENMASK(25, 20)
#define	  HDR_A5_MUX			IPA_BIT(26)
#define	  HDR_LEN_INC_DEAGG_HDR		IPA_BIT(27)
#define	  HDR_METADATA_REG_VALID	IPA_BIT(28)
#define	IPA_ENDP_INIT_HDR_EXT		0x00000814
#define	  HDR_ENDIANNESS		IPA_BIT(0)
#define	  HDR_TOTAL_LEN_OR_PAD_VALID	IPA_BIT(1)
#define	  HDR_TOTAL_LEN_OR_PAD		IPA_BIT(2)
#define	  HDR_PAYLOAD_LEN_INC_PADDING	IPA_BIT(3)
#define	  HDR_TOTAL_LEN_OR_PAD_OFFSET	IPA_GENMASK(9, 4)
#define	  HDR_PAD_TO_ALIGNMENT		IPA_GENMASK(13, 10)
#define	IPA_ENDP_INIT_HDR_METADATA_MASK	0x00000818
#define	IPA_ENDP_INIT_MODE		0x00000820
#define	  MODE_ENDP_MODE		IPA_GENMASK(2, 0)
#define	  MODE_DEST_PIPE_INDEX		IPA_GENMASK(8, 4)
#define	  MODE_BYTE_THRESHOLD		IPA_GENMASK(27, 12)
#define	  MODE_PIPE_REPLICATION_EN	IPA_BIT(28)
#define	  MODE_PAD_EN			IPA_BIT(29)
#define	  MODE_HDR_FTCH_DISABLE		IPA_BIT(30)
#define	IPA_ENDP_INIT_AGGR		0x00000824
#define	  AGGR_EN			IPA_GENMASK(1, 0)
#define	  AGGR_TYPE			IPA_GENMASK(4, 2)
#define	  AGGR_BYTE_LIMIT		IPA_GENMASK(9, 5)
#define	  AGGR_TIME_LIMIT		IPA_GENMASK(14, 10)
#define	  AGGR_PKT_LIMIT		IPA_GENMASK(20, 15)
#define	  AGGR_SW_EOF_ACTIVE		IPA_BIT(21)
#define	  AGGR_FORCE_CLOSE		IPA_BIT(22)
#define	  AGGR_HARD_BYTE_LIMIT_EN	IPA_BIT(24)
#define	IPA_ENDP_INIT_HOL_BLOCK_EN	0x0000082c
#define	  HOL_BLOCK_EN			IPA_BIT(0)
#define	IPA_ENDP_INIT_HOL_BLOCK_TIMER	0x00000830
#define	  HOL_TIMER_BASE_VALUE		IPA_GENMASK(4, 0)
#define	  HOL_TIMER_SCALE		IPA_GENMASK(12, 8)
#define	IPA_ENDP_INIT_DEAGGR		0x00000834
#define	IPA_ENDP_INIT_RSRC_GRP		0x00000838
#define	  ENDP_RSRC_GRP			IPA_BIT(0)
#define	IPA_ENDP_INIT_SEQ		0x0000083c
#define	  SEQ_TYPE			IPA_GENMASK(7, 0)
#define	  SEQ_REP_TYPE			IPA_GENMASK(15, 8)
#define	IPA_ENDP_STATUS			0x00000840
#define	  STATUS_EN			IPA_BIT(0)
#define	  STATUS_ENDP			IPA_GENMASK(5, 1)
#define	  STATUS_LOCATION		IPA_BIT(8)
#define	  STATUS_PKT_SUPPRESS		IPA_BIT(9)

/* interrupts of the AP's EE */
#define	IPA_IRQ_STTS			0x00003008
#define	IPA_IRQ_EN			0x0000300c
#define	IPA_IRQ_CLR			0x00003010
#define	IPA_IRQ_UC			0x0000301c
#define	  IRQ_UC_INTR			IPA_BIT(0)
#define	IPA_IRQ_SUSPEND_INFO		0x00003030	/* stride 4 */
#define	IPA_IRQ_SUSPEND_EN		0x00003034
#define	IPA_IRQ_SUSPEND_CLR		0x00003038

/* ipa_reg.h's values */
enum ipa_irq_id {
	IPA_IRQ_UC_0		= 0x2,	/* microcontroller event */
	IPA_IRQ_UC_1		= 0x3,	/* microcontroller response */
	IPA_IRQ_TX_SUSPEND	= 0xe,
};
#define	IPA_CS_OFFLOAD_NONE	0x0
#define	IPA_CS_OFFLOAD_UL	0x1	/* TX, before IPA v4.5 */
#define	IPA_CS_OFFLOAD_DL	0x2	/* RX, before IPA v4.5 */
#define	IPA_NAT_TYPE_BYPASS	0
#define	IPA_MODE_BASIC		0x0
#define	IPA_MODE_DMA		0x3
#define	IPA_BYPASS_AGGR		0x0
#define	IPA_ENABLE_AGGR		0x1	/* RX */
#define	IPA_ENABLE_DEAGGR	0x2	/* TX */
#define	IPA_AGGR_GENERIC	0x4
#define	IPA_AGGR_QCMAP		0x6
#define	IPA_SEQ_DMA		0x00
#define	IPA_SEQ_1_PASS_SKIP_LAST_UC 0x06
#define	IPA_SEQ_REP_DMA_PARSER	0x08

/* --- GSI ("gsi", 0x05804000), EE 0 ---------------------------------------------- */

#define	GSI_INTER_EE_SRC_CH_IRQ_MSK	0x0000c020
#define	GSI_INTER_EE_SRC_EV_CH_IRQ_MSK	0x0000c024
/* channel context, stride 0x80 */
#define	GSI_CH_C_CNTXT_0(c)		(0x0001c000 + 0x80 * (c))
#define	  CHTYPE_PROTOCOL		IPA_GENMASK(2, 0)
#define	  CHTYPE_DIR			IPA_BIT(3)
#define	  CH_EE				IPA_GENMASK(7, 4)
#define	  CHID				IPA_GENMASK(12, 8)
#define	  ERINDEX			IPA_GENMASK(18, 14)
#define	  CHSTATE			IPA_GENMASK(23, 20)
#define	  ELEMENT_SIZE			IPA_GENMASK(31, 24)
#define	GSI_CH_C_CNTXT_1(c)		(0x0001c004 + 0x80 * (c))
#define	  CH_R_LENGTH			IPA_GENMASK(15, 0)
#define	GSI_CH_C_CNTXT_2(c)		(0x0001c008 + 0x80 * (c))
#define	GSI_CH_C_CNTXT_3(c)		(0x0001c00c + 0x80 * (c))
#define	GSI_CH_C_QOS(c)			(0x0001c05c + 0x80 * (c))
#define	  WRR_WEIGHT			IPA_GENMASK(3, 0)
#define	  MAX_PREFETCH			IPA_BIT(8)
#define	  USE_DB_ENG			IPA_BIT(9)
#define	  USE_ESCAPE_BUF_ONLY		IPA_BIT(10)
#define	GSI_CH_C_SCRATCH_0(c)		(0x0001c060 + 0x80 * (c))
#define	GSI_CH_C_SCRATCH_1(c)		(0x0001c064 + 0x80 * (c))
#define	GSI_CH_C_SCRATCH_2(c)		(0x0001c068 + 0x80 * (c))
#define	GSI_CH_C_SCRATCH_3(c)		(0x0001c06c + 0x80 * (c))
/* event ring context, stride 0x80 */
#define	GSI_EV_CH_E_CNTXT_0(e)		(0x0001d000 + 0x80 * (e))
#define	  EV_CHTYPE			IPA_GENMASK(3, 0)
#define	  EV_EE				IPA_GENMASK(7, 4)
#define	  EV_EVCHID			IPA_GENMASK(15, 8)
#define	  EV_INTYPE			IPA_BIT(16)
#define	  EV_CHSTATE			IPA_GENMASK(23, 20)
#define	  EV_ELEMENT_SIZE		IPA_GENMASK(31, 24)
#define	GSI_EV_CH_E_CNTXT_1(e)		(0x0001d004 + 0x80 * (e))
#define	  EV_R_LENGTH			IPA_GENMASK(15, 0)
#define	GSI_EV_CH_E_CNTXT_2(e)		(0x0001d008 + 0x80 * (e))
#define	GSI_EV_CH_E_CNTXT_3(e)		(0x0001d00c + 0x80 * (e))
#define	GSI_EV_CH_E_CNTXT_4(e)		(0x0001d010 + 0x80 * (e))
#define	GSI_EV_CH_E_CNTXT_8(e)		(0x0001d020 + 0x80 * (e))
#define	  EV_MODT			IPA_GENMASK(15, 0)
#define	  EV_MODC			IPA_GENMASK(23, 16)
#define	  EV_MOD_CNT			IPA_GENMASK(31, 24)
#define	GSI_EV_CH_E_CNTXT_9(e)		(0x0001d024 + 0x80 * (e))
#define	GSI_EV_CH_E_CNTXT_10(e)		(0x0001d028 + 0x80 * (e))
#define	GSI_EV_CH_E_CNTXT_11(e)		(0x0001d02c + 0x80 * (e))
#define	GSI_EV_CH_E_CNTXT_12(e)		(0x0001d030 + 0x80 * (e))
#define	GSI_EV_CH_E_CNTXT_13(e)		(0x0001d034 + 0x80 * (e))
#define	GSI_EV_CH_E_SCRATCH_0(e)	(0x0001d048 + 0x80 * (e))
#define	GSI_EV_CH_E_SCRATCH_1(e)	(0x0001d04c + 0x80 * (e))
/* doorbells, stride 8 */
#define	GSI_CH_C_DOORBELL_0(c)		(0x0001e000 + 0x08 * (c))
#define	GSI_EV_CH_E_DOORBELL_0(e)	(0x0001e100 + 0x08 * (e))
#define	GSI_STATUS			0x0001f000
#define	  GSI_STATUS_ENABLED		IPA_BIT(0)
#define	GSI_CH_CMD			0x0001f008
#define	  CH_CMD_CHID			IPA_GENMASK(7, 0)
#define	  CH_CMD_OPCODE			IPA_GENMASK(31, 24)
#define	GSI_EV_CH_CMD			0x0001f010
#define	  EV_CMD_CHID			IPA_GENMASK(7, 0)
#define	  EV_CMD_OPCODE			IPA_GENMASK(31, 24)
#define	GSI_GENERIC_CMD			0x0001f018
#define	  GENERIC_OPCODE		IPA_GENMASK(4, 0)
#define	  GENERIC_CHID			IPA_GENMASK(9, 5)
#define	  GENERIC_EE			IPA_GENMASK(13, 10)
#define	GSI_HW_PARAM_2			0x0001f040
#define	  NUM_CH_PER_EE			IPA_GENMASK(7, 3)
#define	  NUM_EV_PER_EE			IPA_GENMASK(12, 8)
#define	GSI_CNTXT_TYPE_IRQ		0x0001f080
#define	GSI_CNTXT_TYPE_IRQ_MSK		0x0001f088
#define	GSI_CNTXT_SRC_CH_IRQ		0x0001f090
#define	GSI_CNTXT_SRC_EV_CH_IRQ		0x0001f094
#define	GSI_CNTXT_SRC_CH_IRQ_MSK	0x0001f098
#define	GSI_CNTXT_SRC_EV_CH_IRQ_MSK	0x0001f09c
#define	GSI_CNTXT_SRC_CH_IRQ_CLR	0x0001f0a0
#define	GSI_CNTXT_SRC_EV_CH_IRQ_CLR	0x0001f0a4
#define	GSI_CNTXT_SRC_IEOB_IRQ		0x0001f0b0
#define	GSI_CNTXT_SRC_IEOB_IRQ_MSK	0x0001f0b8
#define	GSI_CNTXT_SRC_IEOB_IRQ_CLR	0x0001f0c0
#define	GSI_CNTXT_GLOB_IRQ_STTS		0x0001f100
#define	GSI_CNTXT_GLOB_IRQ_EN		0x0001f108
#define	GSI_CNTXT_GLOB_IRQ_CLR		0x0001f110
#define	GSI_CNTXT_GSI_IRQ_STTS		0x0001f118
#define	GSI_CNTXT_GSI_IRQ_EN		0x0001f120
#define	GSI_CNTXT_GSI_IRQ_CLR		0x0001f128
#define	GSI_CNTXT_INTSET		0x0001f180
#define	  INTSET_INTYPE			IPA_BIT(0)
#define	GSI_ERROR_LOG			0x0001f200
#define	  ERR_ARG3			IPA_GENMASK(3, 0)
#define	  ERR_ARG2			IPA_GENMASK(7, 4)
#define	  ERR_ARG1			IPA_GENMASK(11, 8)
#define	  ERR_CODE			IPA_GENMASK(15, 12)
#define	  ERR_VIRT_IDX			IPA_GENMASK(23, 19)
#define	  ERR_TYPE			IPA_GENMASK(27, 24)
#define	  ERR_EE			IPA_GENMASK(31, 28)
#define	GSI_ERROR_LOG_CLR		0x0001f210
#define	GSI_CNTXT_SCRATCH_0		0x0001f400
#define	  INTER_EE_RESULT		IPA_GENMASK(2, 0)
#define	  GENERIC_EE_RESULT		IPA_GENMASK(7, 5)

/* gsi_reg.h's and gsi_private.h's values */
#define	GSI_CHANNEL_TYPE_GPI		0x2
#define	GSI_CH_ALLOCATE			0x0
#define	GSI_CH_START			0x1
#define	GSI_CH_STOP			0x2
#define	GSI_CH_RESET			0x9
#define	GSI_CH_DE_ALLOC			0xa
#define	GSI_EVT_ALLOCATE		0x0
#define	GSI_EVT_RESET			0x9
#define	GSI_EVT_DE_ALLOC		0xa
#define	GSI_GENERIC_HALT_CHANNEL	0x1
#define	GSI_GENERIC_ALLOCATE_CHANNEL	0x2
#define	GSI_GENERIC_ENABLE_FLOW_CONTROL	0x3
#define	GSI_GENERIC_DISABLE_FLOW_CONTROL 0x4
/* GSI_CNTXT_TYPE_IRQ bits */
#define	GSI_CH_CTRL			IPA_BIT(0)
#define	GSI_EV_CTRL			IPA_BIT(1)
#define	GSI_GLOB_EE			IPA_BIT(2)
#define	GSI_IEOB			IPA_BIT(3)
#define	GSI_INTER_EE_CH_CTRL		IPA_BIT(4)
#define	GSI_INTER_EE_EV_CTRL		IPA_BIT(5)
#define	GSI_GENERAL			IPA_BIT(6)
/* GSI_CNTXT_GLOB_IRQ_* bits */
#define	GSI_ERROR_INT			IPA_BIT(0)
#define	GSI_GP_INT1			IPA_BIT(1)
/* GSI_CNTXT_GSI_IRQ_* bits */
#define	GSI_BREAK_POINT			IPA_BIT(0)
#define	GSI_BUS_ERROR			IPA_BIT(1)
#define	GSI_CMD_FIFO_OVRFLOW		IPA_BIT(2)
#define	GSI_MCS_STACK_OVRFLOW		IPA_BIT(3)
/* channel and event ring states */
#define	GSI_CHANNEL_STATE_NOT_ALLOCATED	0x0
#define	GSI_CHANNEL_STATE_ALLOCATED	0x1
#define	GSI_CHANNEL_STATE_STARTED	0x2
#define	GSI_CHANNEL_STATE_STOPPED	0x3
#define	GSI_CHANNEL_STATE_STOP_IN_PROC	0x4
#define	GSI_CHANNEL_STATE_ERROR		0xf
#define	GSI_EVT_RING_STATE_NOT_ALLOCATED 0x0
#define	GSI_EVT_RING_STATE_ALLOCATED	0x1
#define	GSI_EVT_RING_STATE_ERROR	0xf
/* the error log */
#define	GSI_ERR_TYPE_GLOB		0x1
#define	GSI_ERR_TYPE_CHAN		0x2
#define	GSI_ERR_TYPE_EVT		0x3
#define	GSI_OUT_OF_RESOURCES		0x3
/* GENERIC_EE_RESULT */
#define	GENERIC_EE_SUCCESS		0x1
#define	GENERIC_EE_INCORRECT_CHANNEL_STATE 0x2
#define	GENERIC_EE_RETRY		0x6

/* --- ring elements (gsi.c, gsi_trans.c) -------------------------------------------- */

#define	GSI_RING_ELEMENT_SIZE		16

/* a transfer element in a channel ring */
struct gsi_tre {
	uint64_t	addr;		/* little endian */
	uint16_t	len_opcode;	/* length, or the immediate command */
	uint16_t	reserved;
	uint32_t	flags;
} __packed;
#define	TRE_FLAGS_CHAIN			IPA_BIT(0)
#define	TRE_FLAGS_IEOT			IPA_BIT(9)
#define	TRE_FLAGS_BEI			IPA_BIT(10)
#define	TRE_FLAGS_TYPE			IPA_GENMASK(23, 16)
#define	GSI_RE_XFER			0x2
#define	GSI_RE_IMMD_CMD			0x3

/* an element of an event ring */
struct gsi_event {
	uint64_t	xfer_ptr;	/* the TRE it completes */
	uint16_t	len;
	uint8_t		reserved1;
	uint8_t		code;
	uint16_t	reserved2;
	uint8_t		type;
	uint8_t		chid;
} __packed;

/* GPI channel scratch (gsi.c's gsi_channel_scratch_gpi) */
struct gsi_channel_scratch_gpi {
	uint64_t	reserved1;
	uint16_t	reserved2;
	uint16_t	max_outstanding_tre;
	uint16_t	reserved3;
	uint16_t	outstanding_threshold;
} __packed;

/* --- immediate commands (ipa_cmd.c, ipa_cmd.h) ------------------------------------- */

#define	IPA_CMD_NONE			0x0
#define	IPA_CMD_IP_V4_FILTER_INIT	0x3
#define	IPA_CMD_IP_V6_FILTER_INIT	0x4
#define	IPA_CMD_IP_V4_ROUTING_INIT	0x7
#define	IPA_CMD_IP_V6_ROUTING_INIT	0x8
#define	IPA_CMD_HDR_INIT_LOCAL		0x9
#define	IPA_CMD_REGISTER_WRITE		0xc
#define	IPA_CMD_IP_PACKET_INIT		0x10
#define	IPA_CMD_DMA_SHARED_MEM		0x13
#define	IPA_CMD_IP_PACKET_TAG_STATUS	0x14

struct ipa_cmd_hw_ip_fltrt_init {
	uint64_t	hash_rules_addr;
	uint64_t	flags;
	uint64_t	nhash_rules_addr;
} __packed;
#define	IP_FLTRT_FLAGS_HASH_SIZE_SHIFT	0	/* 12 bits */
#define	IP_FLTRT_FLAGS_HASH_ADDR_SHIFT	12	/* 16 bits */
#define	IP_FLTRT_FLAGS_NHASH_SIZE_SHIFT	28	/* 12 bits */
#define	IP_FLTRT_FLAGS_NHASH_ADDR_SHIFT	40	/* 16 bits */

struct ipa_cmd_hw_hdr_init_local {
	uint64_t	hdr_table_addr;
	uint32_t	flags;
	uint32_t	reserved;
} __packed;
#define	HDR_INIT_LOCAL_FLAGS_TABLE_SIZE	IPA_GENMASK(11, 0)
#define	HDR_INIT_LOCAL_FLAGS_HDR_ADDR	IPA_GENMASK(27, 12)

struct ipa_cmd_register_write {
	uint16_t	flags;
	uint16_t	offset;
	uint32_t	value;
	uint32_t	value_mask;
	uint32_t	clear_options;
} __packed;
#define	REGISTER_WRITE_OPCODE_CLEAR_OPTION IPA_GENMASK(10, 9)
#define	REGISTER_WRITE_FLAGS_OFFSET_HIGH IPA_GENMASK(14, 11)
#define	PIPELINE_CLEAR_HPS		0x0
#define	PIPELINE_CLEAR_FULL		0x2

struct ipa_cmd_ip_packet_init {
	uint8_t		dest_endpoint;
	uint8_t		reserved[7];
} __packed;
#define	IPA_PACKET_INIT_DEST_ENDPOINT	IPA_GENMASK(4, 0)

struct ipa_cmd_hw_dma_mem_mem {
	uint16_t	clear_after_read;
	uint16_t	size;
	uint16_t	local_addr;
	uint16_t	flags;
	uint64_t	system_addr;
} __packed;
#define	DMA_SHARED_MEM_FLAGS_DIRECTION	IPA_BIT(0)

struct ipa_cmd_ip_packet_tag_status {
	uint64_t	tag;
} __packed;

union ipa_cmd_payload {
	struct ipa_cmd_hw_ip_fltrt_init		table_init;
	struct ipa_cmd_hw_hdr_init_local	hdr_init_local;
	struct ipa_cmd_register_write		register_write;
	struct ipa_cmd_ip_packet_init		ip_packet_init;
	struct ipa_cmd_hw_dma_mem_mem		dma_shared_mem;
	struct ipa_cmd_ip_packet_tag_status	ip_packet_tag_status;
};

/* --- the packet status (ipa_endpoint.c) ------------------------------------------ */

#define	IPA_STATUS_SIZE			32	/* 8 little endian words */
#define	IPA_STATUS_OPCODE_PACKET	1
#define	IPA_STATUS_OPCODE_DROPPED_PACKET 4
#define	IPA_STATUS_OPCODE_SUSPENDED_PACKET 8
#define	IPA_STATUS_OPCODE_PACKET_2ND_PASS 64
#define	IPA_STATUS_EXCEPTION_DEAGGR	1
#define	IPA_STATUS_RULE_MISS		0x3ff

/* --- the microcontroller's shared area (ipa_uc.c) --------------------------------- */

#define	IPA_UC_OFF_COMMAND		0x00
#define	IPA_UC_OFF_COMMAND_PARAM	0x04
#define	IPA_UC_OFF_COMMAND_PARAM_HI	0x08
#define	IPA_UC_OFF_RESPONSE		0x0c
#define	IPA_UC_OFF_RESPONSE_PARAM	0x10
#define	IPA_UC_OFF_EVENT		0x14
#define	IPA_UC_OFF_EVENT_PARAM		0x18
#define	IPA_UC_OFF_HW_STATE		0x20
#define	IPA_UC_OFF_INTERFACE_VERSION	0x24
#define	IPA_UC_COMMAND_ERR_FATAL	0x4
#define	IPA_UC_RESPONSE_INIT_COMPLETED	0x1
#define	IPA_UC_EVENT_ERROR		0x1
#define	IPA_UC_EVENT_LOG_INFO		0x2

#endif /* _IPA_REGS_H_ */
