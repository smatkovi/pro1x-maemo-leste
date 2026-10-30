/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Synopsys DWC_usb3 in device mode (dwc3dci.c): registers, TRBs, events
 * and endpoint command parameters, after Linux's drivers/usb/dwc3/core.h
 * and gadget.h (7.1). Plain C without kernel dependencies, so that the
 * host test (freebsd/pm/test/dwc3dci_test.c) checks the encodings.
 */
#ifndef _DWC3DCI_HW_H_
#define	_DWC3DCI_HW_H_

/* global registers (from the core's base) */
#define	DWC3_GCTL		0xc110
#define	DWC3_GCTL_PRTCAPDIR(n)	((uint32_t)(n) << 12)
#define	DWC3_GCTL_PRTCAP_MASK	(3u << 12)
#define	DWC3_GCTL_PRTCAP_DEVICE	2
#define	DWC3_GCTL_SCALEDOWN_MASK (3u << 4)
#define	DWC3_GCTL_DISSCRAMBLE	(1u << 3)
#define	DWC3_GSNPSID		0xc120
#define	DWC3_GUSB2PHYCFG(n)	(0xc200 + 4 * (n))
#define	DWC3_GUSB2PHYCFG_SUSPHY	(1u << 6)
#define	DWC3_GUSB2PHYCFG_ENBLSLPM (1u << 8)
#define	DWC3_GUSB3PIPECTL(n)	(0xc2c0 + 4 * (n))
#define	DWC3_GUSB3PIPECTL_SUSPHY (1u << 17)
#define	DWC3_GEVNTADRLO(n)	(0xc400 + 0x10 * (n))
#define	DWC3_GEVNTADRHI(n)	(0xc404 + 0x10 * (n))
#define	DWC3_GEVNTSIZ(n)	(0xc408 + 0x10 * (n))
#define	DWC3_GEVNTSIZ_INTMASK	(1u << 31)
#define	DWC3_GEVNTCOUNT(n)	(0xc40c + 0x10 * (n))
#define	DWC3_GEVNTCOUNT_MASK	0xfffc

/* device registers */
#define	DWC3_DCFG		0xc700
#define	DWC3_DCFG_SPEED_MASK	7u
#define	DWC3_DCFG_HIGHSPEED	0
#define	DWC3_DCFG_DEVADDR(a)	((uint32_t)(a) << 3)
#define	DWC3_DCFG_DEVADDR_MASK	(0x7fu << 3)
#define	DWC3_DCFG_NUMP(n)	((uint32_t)(n) << 17)
#define	DWC3_DCFG_NUMP_MASK	(0x1fu << 17)
#define	DWC3_DCFG_LPM_CAP	(1u << 22)
#define	DWC3_DCTL		0xc704
#define	DWC3_DCTL_RUN_STOP	(1u << 31)
#define	DWC3_DCTL_CSFTRST	(1u << 30)
#define	DWC3_DCTL_TSTCTRL_MASK	(0xfu << 1)
#define	DWC3_DCTL_ULSTCHNGREQ_MASK (0xfu << 5)
#define	DWC3_DCTL_ULSTCHNG_RECOVERY (8u << 5)
#define	DWC3_DEVTEN		0xc708
#define	DWC3_DEVTEN_DISCONNEVTEN (1u << 0)
#define	DWC3_DEVTEN_USBRSTEN	(1u << 1)
#define	DWC3_DEVTEN_CONNECTDONEEN (1u << 2)
#define	DWC3_DEVTEN_ULSTCNGEN	(1u << 3)
#define	DWC3_DEVTEN_EOPFEN	(1u << 6)
#define	DWC3_DSTS		0xc70c
#define	DWC3_DSTS_CONNECTSPD	7u
#define	DWC3_DSTS_HIGHSPEED	0
#define	DWC3_DSTS_FULLSPEED	1
#define	DWC3_DSTS_DEVCTRLHLT	(1u << 22)
#define	DWC3_DALEPENA		0xc720
#define	DWC3_DEPCMDPAR2(n)	(0xc800 + 0x10 * (n))
#define	DWC3_DEPCMDPAR1(n)	(0xc804 + 0x10 * (n))
#define	DWC3_DEPCMDPAR0(n)	(0xc808 + 0x10 * (n))
#define	DWC3_DEPCMD(n)		(0xc80c + 0x10 * (n))

/* endpoint commands */
#define	DWC3_DEPCMD_DEPCFG	0x01
#define	DWC3_DEPCMD_XFERCFG	0x02
#define	DWC3_DEPCMD_SETSTALL	0x04
#define	DWC3_DEPCMD_CLEARSTALL	0x05
#define	DWC3_DEPCMD_STARTXFER	0x06
#define	DWC3_DEPCMD_ENDXFER	0x08
#define	DWC3_DEPCMD_STARTCFG	0x09
#define	DWC3_DEPCMD_CMDIOC	(1u << 8)
#define	DWC3_DEPCMD_CMDACT	(1u << 10)
#define	DWC3_DEPCMD_FORCERM	(1u << 11)
#define	DWC3_DEPCMD_PARAM(x)	((uint32_t)(x) << 16)
#define	DWC3_DEPCMD_STATUS(x)	(((x) >> 12) & 0xf)
#define	DWC3_DEPCMD_RSCIDX(x)	(((x) >> 16) & 0x7f)

/* DEPCFG parameters */
#define	DWC3_DEPCFG_EP_TYPE(t)	((uint32_t)(t) << 1)
#define	DWC3_DEPCFG_MPS(m)	((uint32_t)(m) << 3)
#define	DWC3_DEPCFG_FIFO(n)	((uint32_t)(n) << 17)
#define	DWC3_DEPCFG_ACTION_INIT	0
#define	DWC3_DEPCFG_ACTION_MODIFY (2u << 30)
#define	DWC3_DEPCFG_XFERCOMPLETE_EN (1u << 8)
#define	DWC3_DEPCFG_XFERNOTREADY_EN (1u << 10)
#define	DWC3_DEPCFG_BINTERVAL_M1(n) ((uint32_t)(n) << 16)
#define	DWC3_DEPCFG_EP_NUMBER(n) ((uint32_t)(n) << 25)

/* transfer request blocks */
struct dwc3_trb {
	uint32_t	bpl;
	uint32_t	bph;
	uint32_t	size;
	uint32_t	ctrl;
};
#define	DWC3_TRB_SIZE_MASK	0x00ffffffu
#define	DWC3_TRB_SIZE_TRBSTS(x)	(((x) >> 28) & 0xf)
#define	DWC3_TRB_CTRL_HWO	(1u << 0)
#define	DWC3_TRB_CTRL_LST	(1u << 1)
#define	DWC3_TRB_CTRL_CHN	(1u << 2)
#define	DWC3_TRB_CTRL_CSP	(1u << 3)
#define	DWC3_TRB_CTRL_TRBCTL(n)	((uint32_t)(n) << 4)
#define	DWC3_TRB_CTRL_ISP_IMI	(1u << 10)
#define	DWC3_TRB_CTRL_IOC	(1u << 11)
#define	DWC3_TRBCTL_NORMAL	1
#define	DWC3_TRBCTL_SETUP	2
#define	DWC3_TRBCTL_STATUS2	3
#define	DWC3_TRBCTL_STATUS3	4
#define	DWC3_TRBCTL_DATA	5

/* events */
#define	DWC3_EV_IS_DEV(e)	(((e) & 1) != 0)
#define	DWC3_DEVT_TYPE(e)	(((e) >> 8) & 0xf)
#define	DWC3_DEVT_DISCONNECT	0
#define	DWC3_DEVT_RESET		1
#define	DWC3_DEVT_CONNECTDONE	2
#define	DWC3_DEVT_LINKSTS	3
#define	DWC3_DEVT_WAKEUP	4
#define	DWC3_DEVT_SUSPEND	6
#define	DWC3_DEPEVT_EP(e)	(((e) >> 1) & 0x1f)
#define	DWC3_DEPEVT_TYPE(e)	(((e) >> 6) & 0xf)
#define	DWC3_DEPEVT_STATUS(e)	(((e) >> 12) & 0xf)
#define	DWC3_DEPEVT_XFERCOMPLETE 1
#define	DWC3_DEPEVT_XFERINPROGRESS 2
#define	DWC3_DEPEVT_XFERNOTREADY 3
#define	DWC3_DEPEVT_EPCMDCMPLT	7
#define	DWC3_DEPEVT_STATUS_CTRL_DATA 1
#define	DWC3_DEPEVT_STATUS_CTRL_STATUS 2

/* USB endpoint types as DEPCFG takes them (the descriptor's bmAttributes) */
#define	DWC3_EP_CONTROL		0
#define	DWC3_EP_BULK		2
#define	DWC3_EP_INTERRUPT	3

/*
 * The physical endpoint of a logical one: 2 * number + direction (IN 1);
 * the control endpoint's OUT and IN are 0 and 1.
 */
static inline unsigned
dwc3_phys(unsigned ep_no, int in)
{
	return (2 * (ep_no & 0xf) + (in ? 1 : 0));
}

/* One TRB of a transfer of its own (LST, IOC), handed to the hardware */
static inline void
dwc3_trb_fill(volatile struct dwc3_trb *t, uint64_t pa, uint32_t len,
    unsigned trbctl, int out)
{
	t->bpl = (uint32_t)pa;
	t->bph = (uint32_t)(pa >> 32);
	t->size = len & DWC3_TRB_SIZE_MASK;
	t->ctrl = DWC3_TRB_CTRL_TRBCTL(trbctl) | DWC3_TRB_CTRL_LST |
	    DWC3_TRB_CTRL_IOC | (out ? DWC3_TRB_CTRL_ISP_IMI : 0) |
	    DWC3_TRB_CTRL_HWO;
}

/* The bytes moved of a completed TRB that asked for len */
static inline uint32_t
dwc3_trb_done(const volatile struct dwc3_trb *t, uint32_t len)
{
	uint32_t left = t->size & DWC3_TRB_SIZE_MASK;

	return (left > len ? 0 : len - left);
}

/* DEPCFG's parameters 0 and 1 (Linux's dwc3_gadget_set_ep_config) */
static inline void
dwc3_depcfg(unsigned phys, unsigned type, unsigned mps, unsigned binterval,
    int modify, uint32_t *p0, uint32_t *p1)
{
	unsigned m1;

	*p0 = DWC3_DEPCFG_EP_TYPE(type) | DWC3_DEPCFG_MPS(mps) |
	    (modify ? DWC3_DEPCFG_ACTION_MODIFY : DWC3_DEPCFG_ACTION_INIT);
	if (phys & 1)
		*p0 |= DWC3_DEPCFG_FIFO(phys >> 1);
	*p1 = DWC3_DEPCFG_XFERCOMPLETE_EN | DWC3_DEPCFG_XFERNOTREADY_EN |
	    DWC3_DEPCFG_EP_NUMBER(phys);
	if (binterval != 0 && type == DWC3_EP_INTERRUPT) {
		m1 = binterval - 1;
		if (m1 > 13)
			m1 = 13;
		*p1 |= DWC3_DEPCFG_BINTERVAL_M1(m1);
	}
}

/* An OUT transfer's TRB asks for whole packets */
static inline uint32_t
dwc3_out_len(uint32_t want, uint32_t mps, uint32_t bufsize)
{
	uint32_t n = want == 0 ? mps : ((want + mps - 1) / mps) * mps;

	return (n > bufsize ? (bufsize / mps) * mps : n);
}
#endif
