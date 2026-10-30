/*
 * Host test of dwc3dci_hw.h (freebsd/pm/README.md, stage 4): the TRB, the
 * endpoint configuration and the event encodings against Linux 7.1's
 * dwc3 (core.h, gadget.h; the DEPCFG values its trace prints for a
 * high-speed gadget).
 *
 *   cc -I freebsd/pm/sys -o /tmp/dwc3t freebsd/pm/test/dwc3dci_test.c && /tmp/dwc3t
 */
#include <stdint.h>
#include <stdio.h>
#include "dev/usb/controller/dwc3dci_hw.h"

static int fails;
#define	EQ(a, b) do { unsigned long long _a = (a), _b = (b); if (_a != _b) { \
	printf("FAIL %s:%d: %s = %#llx, want %#llx\n", __FILE__, __LINE__, #a, _a, _b); \
	fails++; } } while (0)

int
main(void)
{
	struct dwc3_trb t;
	uint32_t p0, p1;

	/* physical endpoints */
	EQ(dwc3_phys(0, 0), 0); EQ(dwc3_phys(0, 1), 1);
	EQ(dwc3_phys(1, 1), 3); EQ(dwc3_phys(2, 0), 4);
	/* DEPCFG: control 64 (Linux: 00000200 00000500 / 00000200 02000500) */
	dwc3_depcfg(0, DWC3_EP_CONTROL, 64, 0, 0, &p0, &p1);
	EQ(p0, 0x200); EQ(p1, 0x500);
	dwc3_depcfg(1, DWC3_EP_CONTROL, 64, 0, 0, &p0, &p1);
	EQ(p0, 0x200); EQ(p1, 0x2000500);
	/* modify at connect-done */
	dwc3_depcfg(1, DWC3_EP_CONTROL, 64, 0, 1, &p0, &p1);
	EQ(p0, 0x80000200);
	/* bulk IN ep1 (phys 3) 512: type 2, FIFO 1, number 3 */
	dwc3_depcfg(3, DWC3_EP_BULK, 512, 0, 0, &p0, &p1);
	EQ(p0, 0x21004); EQ(p1, 0x6000500);
	/* bulk OUT ep2 (phys 4): no FIFO number */
	dwc3_depcfg(4, DWC3_EP_BULK, 512, 0, 0, &p0, &p1);
	EQ(p0, 0x1004); EQ(p1, 0x8000500);
	/* interrupt IN ep3 (phys 7) 16 bytes, bInterval 8: m1 7 */
	dwc3_depcfg(7, DWC3_EP_INTERRUPT, 16, 8, 0, &p0, &p1);
	EQ(p0, 0x60086); EQ(p1, 0xe070500);
	dwc3_depcfg(7, DWC3_EP_INTERRUPT, 16, 32, 0, &p0, &p1);
	EQ((p1 >> 16) & 0xff, 13);
	/* TRBs: SETUP 8 bytes, OUT with ISP_IMI */
	dwc3_trb_fill(&t, 0x12345678abcULL, 8, DWC3_TRBCTL_SETUP, 1);
	EQ(t.bpl, 0x45678abc); EQ(t.bph, 0x123); EQ(t.size, 8);
	EQ(t.ctrl, 0x400 | 0x800 | (2 << 4) | 2 | 1);
	dwc3_trb_fill(&t, 0x1000, 0, DWC3_TRBCTL_STATUS2, 0);
	EQ(t.ctrl, 0x800 | (3 << 4) | 2 | 1);
	/* completion: size holds what was left */
	t.size = 512 - 100;
	EQ(dwc3_trb_done(&t, 512), 100);
	t.size = 0;
	EQ(dwc3_trb_done(&t, 64), 64);
	t.size = (1u << 28) | 5;
	EQ(DWC3_TRB_SIZE_TRBSTS(t.size), 1);
	EQ(dwc3_trb_done(&t, 4), 0);
	/* OUT lengths: whole packets, the bounce buffer at most */
	EQ(dwc3_out_len(0, 512, 16384), 512);
	EQ(dwc3_out_len(1, 64, 4096), 64);
	EQ(dwc3_out_len(1514, 512, 16384), 1536);
	EQ(dwc3_out_len(20000, 512, 16384), 16384);
	EQ(dwc3_out_len(100000, 64, 4000), 3968);
	/* events (Linux's dwc3_event_devt, dwc3_event_depevt) */
	EQ(DWC3_EV_IS_DEV(0x101), 1); EQ(DWC3_DEVT_TYPE(0x101), DWC3_DEVT_RESET);
	EQ(DWC3_DEVT_TYPE(0x201), DWC3_DEVT_CONNECTDONE);
	EQ(DWC3_DEVT_TYPE(0x001), DWC3_DEVT_DISCONNECT);
	EQ(DWC3_EV_IS_DEV(0x20c2), 0);
	EQ(DWC3_DEPEVT_EP(0x20c2), 1);
	EQ(DWC3_DEPEVT_TYPE(0x20c2), DWC3_DEPEVT_XFERNOTREADY);
	EQ(DWC3_DEPEVT_STATUS(0x20c2), DWC3_DEPEVT_STATUS_CTRL_STATUS);
	EQ(DWC3_DEPEVT_TYPE(0xc040), DWC3_DEPEVT_XFERCOMPLETE);
	EQ(DWC3_DEPEVT_EP(0xc040), 0);
	/* endpoint command words */
	EQ(DWC3_DEPCMD_STARTCFG | DWC3_DEPCMD_PARAM(2), 0x20009);
	EQ(DWC3_DEPCMD_RSCIDX(0x00030406u), 3);
	EQ(DWC3_DEPCMD_STATUS(0x1000u), 1);
	EQ(DWC3_DCFG_DEVADDR(5), 0x28);
	printf("%s: %d failures\n", fails ? "FAILED" : "OK", fails);
	return (fails != 0);
}
