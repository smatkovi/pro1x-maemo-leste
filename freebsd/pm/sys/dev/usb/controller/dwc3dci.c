/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2009 Hans Petter Selasky. All rights reserved.
 * (the device controller framework of atmegadci.c: the transfer
 * descriptor chains, the root hub, the transfer setup, kept as they are)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * dwc3dci: the Synopsys DWC_usb3 controller in device mode, for FreeBSD's
 * USB device side (usb_template: CDC Ethernet, serial), which so far has
 * device controller drivers for the DWC2 (dwc_otg), MUSB and a few small
 * chips only; FreeBSD's own dwc3 driver hands the core to xhci (host).
 *
 * The framework is atmegadci.c's: a transfer is a chain of transfer
 * descriptors whose functions are polled until they are done. Here each
 * one hands a TRB to the hardware (one TRB per transfer, LST and IOC, on
 * an uncached bounce buffer per physical endpoint) and is done when the
 * hardware gives it back (HWO clear). The control endpoint follows Linux's
 * dwc3 ep0.c: a SETUP TRB on physical endpoint 0 whenever it is idle, the
 * data stage as CONTROL_DATA TRBs, the status stage as STATUS2/STATUS3
 * after the core's XferNotReady for it; SET_ADDRESS goes into DCFG at the
 * setup stage. Endpoints are configured (DEPCFG, XFERCFG, DALEPENA) when
 * the stack resets them (clear_stall), after one STARTCFG(2) as Linux
 * does it once a configuration is set. High speed only.
 *
 * The SoC glue (power, clocks, the PHY, the SMMU stream) is
 * dwc3dci_pro1x.c's.
 */

#ifdef USB_GLOBAL_INCLUDE_FILE
#include USB_GLOBAL_INCLUDE_FILE
#else
#include <sys/stdint.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/sx.h>
#include <sys/unistd.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <sys/priv.h>
#include <sys/rman.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>

#include <machine/bus.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#define	USB_DEBUG_VAR dwc3dci_debug

#include <dev/usb/usb_core.h>
#include <dev/usb/usb_debug.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_transfer.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_hub.h>
#include <dev/usb/usb_util.h>

#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#endif			/* USB_GLOBAL_INCLUDE_FILE */

#include <dev/usb/controller/dwc3dci.h>

#define	DWC3DCI_PC2SC(pc) \
   DWC3DCI_BUS2SC(USB_DMATAG_TO_XROOT((pc)->tag_parent)->bus)

#define	DWC3DCI_INTR_ENDPT 1

#ifdef USB_DEBUG
static int dwc3dci_debug = 0;

static SYSCTL_NODE(_hw_usb, OID_AUTO, dwc3dci, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "USB DWC3 device controller");
SYSCTL_INT(_hw_usb_dwc3dci, OID_AUTO, debug, CTLFLAG_RWTUN,
    &dwc3dci_debug, 0, "dwc3dci debug level");
#endif

/* prototypes */

static const struct usb_bus_methods dwc3dci_bus_methods;
static const struct usb_pipe_methods dwc3dci_device_non_isoc_methods;

static dwc3dci_cmd_t dwc3dci_setup_rx;
static dwc3dci_cmd_t dwc3dci_data_rx;
static dwc3dci_cmd_t dwc3dci_data_tx;
static dwc3dci_cmd_t dwc3dci_ctrl_status;
static void dwc3dci_device_done(struct usb_xfer *, usb_error_t);
static void dwc3dci_do_poll(struct usb_bus *);
static void dwc3dci_standard_done(struct usb_xfer *);
static void dwc3dci_root_intr(struct dwc3dci_softc *sc);

/* --- the hardware ------------------------------------------------------ */

static uint32_t
dwc3_rd(struct dwc3dci_softc *sc, bus_size_t off)
{
	return (bus_read_4(sc->sc_io_res, off));
}

static void
dwc3_wr(struct dwc3dci_softc *sc, bus_size_t off, uint32_t v)
{
	bus_write_4(sc->sc_io_res, off, v);
}

/* An endpoint command; its status (0 done), or -1 if it did not finish */
static int
dwc3dci_depcmd(struct dwc3dci_softc *sc, u_int phys, uint32_t cmd,
    uint32_t p0, uint32_t p1, uint32_t p2)
{
	uint32_t v;
	int i;

	dwc3_wr(sc, DWC3_DEPCMDPAR2(phys), p2);
	dwc3_wr(sc, DWC3_DEPCMDPAR1(phys), p1);
	dwc3_wr(sc, DWC3_DEPCMDPAR0(phys), p0);
	dwc3_wr(sc, DWC3_DEPCMD(phys), cmd | DWC3_DEPCMD_CMDACT);
	for (i = 0; i < 5000; i++) {
		v = dwc3_rd(sc, DWC3_DEPCMD(phys));
		if ((v & DWC3_DEPCMD_CMDACT) == 0) {
			if (DWC3_DEPCMD_STATUS(v) != 0)
				sc->sc_n_cmderr++;
			if ((cmd & 0xf) == DWC3_DEPCMD_STARTXFER)
				sc->sc_ep[phys].rsc = DWC3_DEPCMD_RSCIDX(v);
			return (DWC3_DEPCMD_STATUS(v));
		}
		DELAY(1);
	}
	sc->sc_n_cmderr++;
	device_printf(sc->sc_dev, "endpoint %u: command %#x timed out\n",
	    phys, cmd & 0xff);
	return (-1);
}

/* One TRB on the endpoint's bounce buffer to the hardware */
static int
dwc3dci_start(struct dwc3dci_softc *sc, u_int phys, uint32_t len,
    u_int trbctl)
{
	struct dwc3dci_ep *ep = &sc->sc_ep[phys];

	dwc3_trb_fill(ep->trb, ep->buf_pa, len, trbctl, (phys & 1) == 0);
	wmb();
	ep->busy = 1;
	sc->sc_n_trbs++;
	if (dwc3dci_depcmd(sc, phys, DWC3_DEPCMD_STARTXFER,
	    (uint32_t)(ep->trb_pa >> 32), (uint32_t)ep->trb_pa, 0) != 0) {
		ep->trb->ctrl &= ~DWC3_TRB_CTRL_HWO;
		ep->busy = 0;
		return (EIO);
	}
	return (0);
}

/* The transfer on an endpoint ended (cancelled, reset) */
static void
dwc3dci_end(struct dwc3dci_softc *sc, u_int phys)
{
	struct dwc3dci_ep *ep = &sc->sc_ep[phys];

	if (!ep->busy)
		return;
	dwc3dci_depcmd(sc, phys, DWC3_DEPCMD_ENDXFER | DWC3_DEPCMD_FORCERM |
	    DWC3_DEPCMD_CMDIOC | DWC3_DEPCMD_PARAM(ep->rsc), 0, 0, 0);
	DELAY(100);
	ep->trb->ctrl &= ~DWC3_TRB_CTRL_HWO;
	ep->busy = 0;
}

static void
dwc3dci_ep_config(struct dwc3dci_softc *sc, u_int phys, u_int type,
    u_int mps, u_int binterval, int modify)
{
	uint32_t p0, p1;

	dwc3_depcfg(phys, type, mps, binterval, modify, &p0, &p1);
	dwc3dci_depcmd(sc, phys, DWC3_DEPCMD_DEPCFG, p0, p1, 0);
	if (!modify)
		dwc3dci_depcmd(sc, phys, DWC3_DEPCMD_XFERCFG, 1, 0, 0);
	dwc3_wr(sc, DWC3_DALEPENA, dwc3_rd(sc, DWC3_DALEPENA) | (1u << phys));
	sc->sc_ep[phys].mps = mps;
	sc->sc_ep[phys].configured = 1;
}

/* The control endpoint waits for the next SETUP */
static void
dwc3dci_ep0_arm(struct dwc3dci_softc *sc)
{
	if (sc->sc_setup_armed || sc->sc_ep[0].busy)
		return;
	if (dwc3dci_start(sc, 0, 8, DWC3_TRBCTL_SETUP) == 0)
		sc->sc_setup_armed = 1;
}

/* A SETUP that came: kept for setup_rx, the endpoint free again */
static void
dwc3dci_ep0_check(struct dwc3dci_softc *sc)
{
	struct dwc3dci_ep *ep = &sc->sc_ep[0];

	if (!sc->sc_setup_armed || (ep->trb->ctrl & DWC3_TRB_CTRL_HWO) != 0)
		return;
	rmb();
	memcpy(sc->sc_setup, ep->buf, sizeof(sc->sc_setup));
	sc->sc_setup_armed = 0;
	sc->sc_setup_ready = 1;
	ep->busy = 0;
	sc->sc_n_setups++;
}

static void
dwc3dci_get_hw_ep_profile(struct usb_device *udev,
    const struct usb_hw_ep_profile **ppf, uint8_t ep_addr)
{
	static const struct usb_hw_ep_profile ep0 = {
		.max_in_frame_size = 64,
		.max_out_frame_size = 64,
		.is_simplex = 1,
		.support_control = 1,
	};
	static const struct usb_hw_ep_profile epn = {
		.max_in_frame_size = 1024,
		.max_out_frame_size = 1024,
		.is_simplex = 0,
		.support_interrupt = 1,
		.support_bulk = 1,
		.support_in = 1,
		.support_out = 1,
	};

	if (ep_addr == 0)
		*ppf = &ep0;
	else if (ep_addr < DWC3DCI_EP_MAX)
		*ppf = &epn;
	else
		*ppf = NULL;
}

static void
dwc3dci_clocks_on(struct dwc3dci_softc *sc)
{
	sc->sc_flags.clocks_off = 0;
}

static void
dwc3dci_clocks_off(struct dwc3dci_softc *sc)
{
	sc->sc_flags.clocks_off = 1;
}

/* The pull-up: the core's run/stop bit */
static void
dwc3dci_pull_up(struct dwc3dci_softc *sc)
{
	if (sc->sc_flags.d_pulled_up || !sc->sc_flags.port_powered)
		return;
	sc->sc_flags.d_pulled_up = 1;
	dwc3_wr(sc, DWC3_DCTL, dwc3_rd(sc, DWC3_DCTL) | DWC3_DCTL_RUN_STOP);
	device_printf(sc->sc_dev, "connected (run)\n");
}

static void
dwc3dci_pull_down(struct dwc3dci_softc *sc)
{
	int i;

	if (!sc->sc_flags.d_pulled_up)
		return;
	sc->sc_flags.d_pulled_up = 0;
	dwc3_wr(sc, DWC3_DCTL, dwc3_rd(sc, DWC3_DCTL) & ~DWC3_DCTL_RUN_STOP);
	for (i = 0; i < 1000 &&
	    (dwc3_rd(sc, DWC3_DSTS) & DWC3_DSTS_DEVCTRLHLT) == 0; i++)
		DELAY(10);
}

static void
dwc3dci_wakeup_peer(struct dwc3dci_softc *sc)
{
	uint32_t v;

	/* the link to recovery (remote wake-up), as Linux's wakeup */
	v = dwc3_rd(sc, DWC3_DCTL) & ~DWC3_DCTL_ULSTCHNGREQ_MASK;
	dwc3_wr(sc, DWC3_DCTL, v | DWC3_DCTL_ULSTCHNG_RECOVERY);
	DELAY(8000);
	dwc3_wr(sc, DWC3_DCTL, v);
}

/* --- the transfer descriptors ---------------------------------------- */

static uint8_t
dwc3dci_setup_rx(struct dwc3dci_td *td)
{
	struct dwc3dci_softc *sc = DWC3DCI_PC2SC(td->pc);
	struct usb_device_request req;

	dwc3dci_ep0_check(sc);
	if (!sc->sc_setup_ready) {
		dwc3dci_ep0_arm(sc);
		return (1);		/* busy */
	}
	if (td->remainder != sizeof(req)) {
		/* a SETUP needs 8 bytes */
		td->error = 1;
		sc->sc_setup_ready = 0;
		return (0);
	}
	memcpy(&req, sc->sc_setup, sizeof(req));
	usbd_copy_in(td->pc, 0, &req, sizeof(req));
	td->offset = sizeof(req);
	td->remainder = 0;
	sc->sc_setup_ready = 0;
	sc->sc_status_ready = 0;
	sc->sc_three_stage = UGETW(req.wLength) != 0;
	/* the core takes the address at the setup stage (Linux's ep0.c) */
	if (req.bmRequestType == UT_WRITE_DEVICE &&
	    req.bRequest == UR_SET_ADDRESS) {
		sc->sc_dv_addr = req.wValue[0] & 0x7f;
		dwc3_wr(sc, DWC3_DCFG, (dwc3_rd(sc, DWC3_DCFG) &
		    ~DWC3_DCFG_DEVADDR_MASK) | DWC3_DCFG_DEVADDR(sc->sc_dv_addr));
	}
	return (0);			/* complete */
}

/* Data, any endpoint, in chunks of the bounce buffer */
static uint8_t
dwc3dci_data_xfer(struct dwc3dci_td *td, int in)
{
	struct dwc3dci_softc *sc = DWC3DCI_PC2SC(td->pc);
	struct dwc3dci_ep *ep;
	u_int phys;
	uint32_t got;

	phys = dwc3_phys(td->ep_no, in);
	ep = &sc->sc_ep[phys];
	if (td->ep_no == 0)
		dwc3dci_ep0_check(sc);
	for (;;) {
		if (!td->started) {
			if (ep->busy)
				return (1);	/* the SETUP TRB is still out */
			td->chunk = MIN(td->remainder, ep->buf_size);
			if (in) {
				usbd_copy_out(td->pc, td->offset, ep->buf,
				    td->chunk);
				td->req = td->chunk;
			} else
				td->req = dwc3_out_len(td->chunk,
				    td->max_packet_size, ep->buf_size);
			if (dwc3dci_start(sc, phys, td->req, td->ep_no == 0 ?
			    DWC3_TRBCTL_DATA : DWC3_TRBCTL_NORMAL) != 0) {
				td->error = 1;
				return (0);
			}
			td->started = 1;
			return (1);
		}
		if ((ep->trb->ctrl & DWC3_TRB_CTRL_HWO) != 0)
			return (1);	/* with the hardware */
		rmb();
		td->started = 0;
		ep->busy = 0;
		if (DWC3_TRB_SIZE_TRBSTS(ep->trb->size) != 0) {
			td->error = 1;
			return (0);
		}
		got = dwc3_trb_done(ep->trb, td->req);
		if (!in) {
			got = MIN(got, td->remainder);
			usbd_copy_in(td->pc, td->offset, ep->buf, got);
		}
		td->offset += got;
		td->remainder -= got;
		if (!in) {
			/* a short packet ends it */
			if (got < td->req || td->remainder == 0)
				return (0);
			continue;
		}
		if (td->remainder != 0)
			continue;
		/*
		 * all sent: a zero-length packet after a full last packet if
		 * the transfer has to end short (short_pkt clear)
		 */
		if (got == 0 || (got % td->max_packet_size) != 0 ||
		    td->short_pkt)
			return (0);
		td->short_pkt = 1;
	}
}

static uint8_t
dwc3dci_data_rx(struct dwc3dci_td *td)
{
	return (dwc3dci_data_xfer(td, 0));
}

static uint8_t
dwc3dci_data_tx(struct dwc3dci_td *td)
{
	return (dwc3dci_data_xfer(td, 1));
}

/* The status stage, once the core asked for it (XferNotReady) */
static uint8_t
dwc3dci_ctrl_status(struct dwc3dci_td *td)
{
	struct dwc3dci_softc *sc = DWC3DCI_PC2SC(td->pc);
	struct dwc3dci_ep *ep;
	u_int phys = td->status_in ? 1 : 0;

	ep = &sc->sc_ep[phys];
	if (!td->started) {
		if (!sc->sc_status_ready || ep->busy)
			return (1);
		if (dwc3dci_start(sc, phys, 0, sc->sc_three_stage ?
		    DWC3_TRBCTL_STATUS3 : DWC3_TRBCTL_STATUS2) != 0) {
			td->error = 1;
			return (0);
		}
		td->started = 1;
		return (1);
	}
	if ((ep->trb->ctrl & DWC3_TRB_CTRL_HWO) != 0)
		return (1);
	td->started = 0;
	ep->busy = 0;
	sc->sc_status_ready = 0;
	dwc3dci_ep0_arm(sc);		/* the next SETUP */
	return (0);
}

static uint8_t
dwc3dci_xfer_do_fifo(struct usb_xfer *xfer)
{
	struct dwc3dci_td *td;

	DPRINTFN(9, "\n");

	td = xfer->td_transfer_cache;
	while (1) {
		if ((td->func) (td)) {
			/* operation in progress */
			break;
		}
		if (((void *)td) == xfer->td_transfer_last) {
			goto done;
		}
		if (td->error) {
			goto done;
		} else if (td->remainder > 0) {
			/*
			 * We had a short transfer. If there is no alternate
			 * next, stop processing !
			 */
			if (!td->alt_next) {
				goto done;
			}
		}
		/*
		 * Fetch the next transfer descriptor and transfer
		 * some flags to the next transfer descriptor
		 */
		td = td->obj_next;
		xfer->td_transfer_cache = td;
	}
	return (1);			/* not complete */

done:
	/* compute all actual lengths */

	dwc3dci_standard_done(xfer);
	return (0);			/* complete */
}

static void
dwc3dci_interrupt_poll(struct dwc3dci_softc *sc)
{
	struct usb_xfer *xfer;

repeat:
	TAILQ_FOREACH(xfer, &sc->sc_bus.intr_q.head, wait_entry) {
		if (!dwc3dci_xfer_do_fifo(xfer)) {
			/* queue has been modified */
			goto repeat;
		}
	}
}

/* The core's events: the device's, then the endpoints' by polling */
void
dwc3dci_interrupt(void *arg)
{
	struct dwc3dci_softc *sc = arg;
	uint32_t count, ev, i;
	u_int phys;

	USB_BUS_LOCK(&sc->sc_bus);
	count = dwc3_rd(sc, DWC3_GEVNTCOUNT(0)) & DWC3_GEVNTCOUNT_MASK;
	for (i = 0; i < count; i += 4) {
		ev = sc->sc_ev[sc->sc_ev_pos / 4];
		sc->sc_ev_pos = (sc->sc_ev_pos + 4) % DWC3DCI_EVBUF_SIZE;
		sc->sc_n_events++;
		if (DWC3_EV_IS_DEV(ev)) {
			switch (DWC3_DEVT_TYPE(ev)) {
			case DWC3_DEVT_DISCONNECT:
				sc->sc_flags.status_bus_reset = 0;
				sc->sc_flags.status_suspend = 0;
				sc->sc_flags.change_connect = 1;
				break;
			case DWC3_DEVT_RESET:
				/* Linux's dwc3_gadget_reset_interrupt */
				sc->sc_n_resets++;
				for (phys = 2; phys < DWC3DCI_PHYS_MAX; phys++) {
					dwc3dci_end(sc, phys);
					sc->sc_ep[phys].configured = 0;
				}
				dwc3_wr(sc, DWC3_DALEPENA, 3);
				dwc3_wr(sc, DWC3_DCFG, dwc3_rd(sc, DWC3_DCFG) &
				    ~DWC3_DCFG_DEVADDR_MASK);
				sc->sc_cfg_started = 0;
				sc->sc_status_ready = 0;
				sc->sc_flags.status_bus_reset = 1;
				sc->sc_flags.status_suspend = 0;
				sc->sc_flags.change_suspend = 0;
				sc->sc_flags.change_connect = 1;
				break;
			case DWC3_DEVT_CONNECTDONE:
				sc->sc_n_connects++;
				sc->sc_flags.status_high_speed =
				    (dwc3_rd(sc, DWC3_DSTS) &
				    DWC3_DSTS_CONNECTSPD) == DWC3_DSTS_HIGHSPEED;
				/* the control endpoint at 64 bytes (HS, FS) */
				dwc3dci_ep_config(sc, 0, DWC3_EP_CONTROL, 64, 0, 1);
				dwc3dci_ep_config(sc, 1, DWC3_EP_CONTROL, 64, 0, 1);
				dwc3dci_ep0_arm(sc);
				break;
			default:
				break;
			}
		} else if (DWC3_DEPEVT_TYPE(ev) == DWC3_DEPEVT_XFERNOTREADY &&
		    DWC3_DEPEVT_EP(ev) < 2 &&
		    DWC3_DEPEVT_STATUS(ev) == DWC3_DEPEVT_STATUS_CTRL_STATUS) {
			sc->sc_status_ready = 1;
		}
	}
	if (count != 0)
		dwc3_wr(sc, DWC3_GEVNTCOUNT(0), count);
	dwc3dci_ep0_check(sc);
	if (sc->sc_flags.change_connect)
		dwc3dci_root_intr(sc);
	dwc3dci_interrupt_poll(sc);
	/* an idle control endpoint waits for its next SETUP */
	if (sc->sc_flags.status_bus_reset && !sc->sc_setup_ready)
		dwc3dci_ep0_arm(sc);
	USB_BUS_UNLOCK(&sc->sc_bus);
}

static void
dwc3dci_setup_standard_chain_sub(struct dwc3dci_std_temp *temp)
{
	struct dwc3dci_td *td;

	/* get current Transfer Descriptor */
	td = temp->td_next;
	temp->td = td;

	/* prepare for next TD */
	temp->td_next = td->obj_next;

	/* fill out the Transfer Descriptor */
	td->func = temp->func;
	td->pc = temp->pc;
	td->offset = temp->offset;
	td->remainder = temp->len;
	td->error = 0;
	td->did_stall = temp->did_stall;
	td->short_pkt = temp->short_pkt;
	td->alt_next = temp->setup_alt_next;
}

static void
dwc3dci_setup_standard_chain(struct usb_xfer *xfer)
{
	struct dwc3dci_std_temp temp;
	struct dwc3dci_td *td;
	uint32_t x;

	temp.max_frame_size = xfer->max_frame_size;
	td = xfer->td_start[0];
	xfer->td_transfer_first = td;
	xfer->td_transfer_cache = td;
	temp.pc = NULL;
	temp.td = NULL;
	temp.td_next = xfer->td_start[0];
	temp.offset = 0;
	temp.setup_alt_next = xfer->flags_int.short_frames_ok ||
	    xfer->flags_int.isochronous_xfr;
	temp.did_stall = !xfer->flags_int.control_stall;

	if (xfer->flags_int.control_xfr) {
		if (xfer->flags_int.control_hdr) {
			temp.func = &dwc3dci_setup_rx;
			temp.len = xfer->frlengths[0];
			temp.pc = xfer->frbuffers + 0;
			temp.short_pkt = temp.len ? 1 : 0;
			if (xfer->nframes == 1 && xfer->flags_int.control_act)
				temp.setup_alt_next = 0;
			dwc3dci_setup_standard_chain_sub(&temp);
		}
		x = 1;
	} else
		x = 0;

	if (x != xfer->nframes) {
		temp.func = (xfer->endpointno & UE_DIR_IN) ?
		    &dwc3dci_data_tx : &dwc3dci_data_rx;
		temp.pc = xfer->frbuffers + x;
	}
	while (x != xfer->nframes) {
		temp.len = xfer->frlengths[x];
		x++;
		if (x == xfer->nframes) {
			if (!xfer->flags_int.control_xfr ||
			    xfer->flags_int.control_act)
				temp.setup_alt_next = 0;
		}
		if (temp.len == 0)
			temp.short_pkt = 0;
		else
			temp.short_pkt = xfer->flags.force_short_xfer ? 0 : 1;
		dwc3dci_setup_standard_chain_sub(&temp);
		temp.pc = xfer->frbuffers + x;
	}

	/* the status stage, opposite to the data (IN without data) */
	if (xfer->flags_int.control_xfr && !xfer->flags_int.control_act) {
		temp.pc = xfer->frbuffers + 0;
		temp.len = 0;
		temp.short_pkt = 0;
		temp.setup_alt_next = 0;
		temp.func = &dwc3dci_ctrl_status;
		dwc3dci_setup_standard_chain_sub(&temp);
		temp.td->status_in = !(xfer->endpointno & UE_DIR_IN);
	}
	xfer->td_transfer_last = temp.td;
}

static void
dwc3dci_timeout(void *arg)
{
	struct usb_xfer *xfer = arg;

	DPRINTF("xfer=%p\n", xfer);

	USB_BUS_LOCK_ASSERT(xfer->xroot->bus, MA_OWNED);

	/* transfer is transferred */
	dwc3dci_device_done(xfer, USB_ERR_TIMEOUT);
}

static void
dwc3dci_start_standard_chain(struct usb_xfer *xfer)
{
	DPRINTFN(9, "\n");

	/* poll one time - will turn on interrupts */
	if (dwc3dci_xfer_do_fifo(xfer)) {
		/* put transfer on interrupt queue */
		usbd_transfer_enqueue(&xfer->xroot->bus->intr_q, xfer);

		/* start timeout, if any */
		if (xfer->timeout != 0) {
			usbd_transfer_timeout_ms(xfer,
			    &dwc3dci_timeout, xfer->timeout);
		}
	}
}

static void
dwc3dci_root_intr(struct dwc3dci_softc *sc)
{
	DPRINTFN(9, "\n");

	USB_BUS_LOCK_ASSERT(&sc->sc_bus, MA_OWNED);

	/* set port bit */
	sc->sc_hub_idata[0] = 0x02;	/* we only have one port */

	uhub_root_intr(&sc->sc_bus, sc->sc_hub_idata,
	    sizeof(sc->sc_hub_idata));
 }

static usb_error_t
dwc3dci_standard_done_sub(struct usb_xfer *xfer)
{
	struct dwc3dci_td *td;
	uint32_t len;
	uint8_t error;

	DPRINTFN(9, "\n");

	td = xfer->td_transfer_cache;

	do {
		len = td->remainder;

		if (xfer->aframes != xfer->nframes) {
			/*
		         * Verify the length and subtract
		         * the remainder from "frlengths[]":
		         */
			if (len > xfer->frlengths[xfer->aframes]) {
				td->error = 1;
			} else {
				xfer->frlengths[xfer->aframes] -= len;
			}
		}
		/* Check for transfer error */
		if (td->error) {
			/* the transfer is finished */
			error = 1;
			td = NULL;
			break;
		}
		/* Check for short transfer */
		if (len > 0) {
			if (xfer->flags_int.short_frames_ok ||
			    xfer->flags_int.isochronous_xfr) {
				/* follow alt next */
				if (td->alt_next) {
					td = td->obj_next;
				} else {
					td = NULL;
				}
			} else {
				/* the transfer is finished */
				td = NULL;
			}
			error = 0;
			break;
		}
		td = td->obj_next;

		/* this USB frame is complete */
		error = 0;
		break;

	} while (0);

	/* update transfer cache */

	xfer->td_transfer_cache = td;

	return (error ?
	    USB_ERR_STALLED : USB_ERR_NORMAL_COMPLETION);
}

static void
dwc3dci_standard_done(struct usb_xfer *xfer)
{
	usb_error_t err = 0;

	DPRINTFN(13, "xfer=%p endpoint=%p transfer done\n",
	    xfer, xfer->endpoint);

	/* reset scanner */

	xfer->td_transfer_cache = xfer->td_transfer_first;

	if (xfer->flags_int.control_xfr) {
		if (xfer->flags_int.control_hdr) {
			err = dwc3dci_standard_done_sub(xfer);
		}
		xfer->aframes = 1;

		if (xfer->td_transfer_cache == NULL) {
			goto done;
		}
	}
	while (xfer->aframes != xfer->nframes) {
		err = dwc3dci_standard_done_sub(xfer);
		xfer->aframes++;

		if (xfer->td_transfer_cache == NULL) {
			goto done;
		}
	}

	if (xfer->flags_int.control_xfr &&
	    !xfer->flags_int.control_act) {
		err = dwc3dci_standard_done_sub(xfer);
	}
done:
	dwc3dci_device_done(xfer, err);
}

static void
dwc3dci_device_done(struct usb_xfer *xfer, usb_error_t error)
{
	struct dwc3dci_softc *sc = DWC3DCI_BUS2SC(xfer->xroot->bus);
	struct dwc3dci_td *td;
	uint8_t ep_no;

	USB_BUS_LOCK_ASSERT(&sc->sc_bus, MA_OWNED);
	if (xfer->flags_int.usb_mode == USB_MODE_DEVICE) {
		/* a TRB still with the hardware: the transfer ended */
		ep_no = xfer->endpointno & UE_ADDR;
		for (td = xfer->td_transfer_first; td != NULL;
		    td = td->obj_next) {
			if (td->started) {
				dwc3dci_end(sc, td->func == &dwc3dci_ctrl_status ?
				    (td->status_in ? 1 : 0) : dwc3_phys(ep_no,
				    td->func == &dwc3dci_data_tx));
				td->started = 0;
			}
			if ((void *)td == xfer->td_transfer_last)
				break;
		}
	}
	usbd_transfer_done(xfer, error);
}

static void
dwc3dci_xfer_stall(struct usb_xfer *xfer)
{
	dwc3dci_device_done(xfer, USB_ERR_STALLED);
}

static void
dwc3dci_set_stall(struct usb_device *udev,
    struct usb_endpoint *ep, uint8_t *did_stall)
{
	struct dwc3dci_softc *sc;
	uint8_t ep_no;
	u_int phys;

	USB_BUS_LOCK_ASSERT(udev->bus, MA_OWNED);
	sc = DWC3DCI_BUS2SC(udev->bus);
	ep_no = ep->edesc->bEndpointAddress & UE_ADDR;
	if (ep_no == 0) {
		/* the control endpoint: stalled, then the next SETUP */
		dwc3dci_end(sc, 1);
		if (sc->sc_setup_armed)
			sc->sc_setup_armed = 0;
		dwc3dci_end(sc, 0);
		dwc3dci_depcmd(sc, 0, DWC3_DEPCMD_SETSTALL, 0, 0, 0);
		sc->sc_status_ready = 0;
		dwc3dci_ep0_arm(sc);
		return;
	}
	phys = dwc3_phys(ep_no, ep->edesc->bEndpointAddress & UE_DIR_IN);
	dwc3dci_depcmd(sc, phys, DWC3_DEPCMD_SETSTALL, 0, 0, 0);
	sc->sc_ep[phys].stalled = 1;
}

/* An endpoint as the stack sets it up (SET_CONFIG, clear halt) */
static void
dwc3dci_clear_stall_sub(struct dwc3dci_softc *sc, uint8_t ep_no,
    uint8_t ep_type, uint8_t ep_dir)
{
	struct dwc3dci_ep *ep;
	u_int phys;

	if (ep_type == UE_CONTROL)
		return;
	phys = dwc3_phys(ep_no, ep_dir & UE_DIR_IN);
	ep = &sc->sc_ep[phys];
	if (!ep->configured) {
		/* the first after a reset: STARTCFG with index 2 (Linux) */
		if (!sc->sc_cfg_started) {
			dwc3dci_depcmd(sc, 0, DWC3_DEPCMD_STARTCFG |
			    DWC3_DEPCMD_PARAM(2), 0, 0, 0);
			sc->sc_cfg_started = 1;
		}
		dwc3dci_ep_config(sc, phys, ep_type == UE_INTERRUPT ?
		    DWC3_EP_INTERRUPT : DWC3_EP_BULK,
		    sc->sc_flags.status_high_speed ? 512 : 64,
		    ep_type == UE_INTERRUPT ? 1 : 0, 0);
	}
	if (ep->stalled) {
		dwc3dci_end(sc, phys);
		dwc3dci_depcmd(sc, phys, DWC3_DEPCMD_CLEARSTALL, 0, 0, 0);
		ep->stalled = 0;
	}
}

static void
dwc3dci_clear_stall(struct usb_device *udev, struct usb_endpoint *ep)
{
	struct dwc3dci_softc *sc;
	struct usb_endpoint_descriptor *ed;

	DPRINTFN(5, "endpoint=%p\n", ep);

	USB_BUS_LOCK_ASSERT(udev->bus, MA_OWNED);

	/* check mode */
	if (udev->flags.usb_mode != USB_MODE_DEVICE) {
		/* not supported */
		return;
	}
	/* get softc */
	sc = DWC3DCI_BUS2SC(udev->bus);

	/* get endpoint descriptor */
	ed = ep->edesc;

	/* reset endpoint */
	dwc3dci_clear_stall_sub(sc,
	    (ed->bEndpointAddress & UE_ADDR),
	    (ed->bmAttributes & UE_XFERTYPE),
	    (ed->bEndpointAddress & (UE_DIR_IN | UE_DIR_OUT)));
}

/* The core in device mode (Linux's dwc3_core_init, dwc3_gadget_start) */
usb_error_t
dwc3dci_init(struct dwc3dci_softc *sc)
{
	uint32_t v;
	int i;

	sc->sc_bus.usbrev = USB_REV_2_0;
	sc->sc_bus.methods = &dwc3dci_bus_methods;

	sc->sc_snpsid = dwc3_rd(sc, DWC3_GSNPSID);
	if ((sc->sc_snpsid >> 16) != 0x5533 && (sc->sc_snpsid >> 16) != 0x3331) {
		device_printf(sc->sc_dev, "not a DWC_usb3 (GSNPSID %08x)\n",
		    sc->sc_snpsid);
		return (USB_ERR_INVAL);
	}
	/* device mode, then the core's soft reset */
	v = dwc3_rd(sc, DWC3_GCTL) & ~(DWC3_GCTL_PRTCAP_MASK |
	    DWC3_GCTL_SCALEDOWN_MASK | DWC3_GCTL_DISSCRAMBLE);
	dwc3_wr(sc, DWC3_GCTL, v | DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_DEVICE));
	dwc3_wr(sc, DWC3_DCTL, DWC3_DCTL_CSFTRST);
	for (i = 0; i < 5000 && (dwc3_rd(sc, DWC3_DCTL) & DWC3_DCTL_CSFTRST);
	    i++)
		DELAY(10);
	if (dwc3_rd(sc, DWC3_DCTL) & DWC3_DCTL_CSFTRST) {
		device_printf(sc->sc_dev, "core soft reset timed out\n");
		return (USB_ERR_TIMEOUT);
	}
	/* the device tree's quirks: dis_u2_susphy, dis_enblslpm */
	v = dwc3_rd(sc, DWC3_GUSB2PHYCFG(0));
	dwc3_wr(sc, DWC3_GUSB2PHYCFG(0),
	    v & ~(DWC3_GUSB2PHYCFG_SUSPHY | DWC3_GUSB2PHYCFG_ENBLSLPM));
	/* the event buffer */
	dwc3_wr(sc, DWC3_GEVNTADRLO(0), (uint32_t)sc->sc_ev_pa);
	dwc3_wr(sc, DWC3_GEVNTADRHI(0), (uint32_t)(sc->sc_ev_pa >> 32));
	dwc3_wr(sc, DWC3_GEVNTSIZ(0), DWC3DCI_EVBUF_SIZE);
	dwc3_wr(sc, DWC3_GEVNTCOUNT(0), 0);
	sc->sc_ev_pos = 0;
	/* high speed, address 0, all endpoints */
	v = dwc3_rd(sc, DWC3_DCFG) & ~(DWC3_DCFG_SPEED_MASK |
	    DWC3_DCFG_DEVADDR_MASK | DWC3_DCFG_NUMP_MASK | DWC3_DCFG_LPM_CAP);
	dwc3_wr(sc, DWC3_DCFG, v | DWC3_DCFG_HIGHSPEED | DWC3_DCFG_NUMP(16));
	dwc3_wr(sc, DWC3_DEVTEN, DWC3_DEVTEN_DISCONNEVTEN |
	    DWC3_DEVTEN_USBRSTEN | DWC3_DEVTEN_CONNECTDONEEN);
	/* the control endpoint, Linux's dwc3_gadget_start */
	dwc3dci_depcmd(sc, 0, DWC3_DEPCMD_STARTCFG | DWC3_DEPCMD_PARAM(0),
	    0, 0, 0);
	dwc3dci_ep_config(sc, 0, DWC3_EP_CONTROL, 64, 0, 0);
	dwc3dci_ep_config(sc, 1, DWC3_EP_CONTROL, 64, 0, 0);
	dwc3dci_ep0_arm(sc);

	USB_BUS_LOCK(&sc->sc_bus);
	/* the cable's VBUS is forced valid by the glue */
	sc->sc_flags.status_vbus = 1;
	sc->sc_flags.change_connect = 1;
	dwc3dci_root_intr(sc);
	USB_BUS_UNLOCK(&sc->sc_bus);
	device_printf(sc->sc_dev, "DWC_usb3 %08x in device mode, high speed\n",
	    sc->sc_snpsid);
	return (0);
}

void
dwc3dci_uninit(struct dwc3dci_softc *sc)
{
	u_int phys;

	USB_BUS_LOCK(&sc->sc_bus);
	for (phys = 0; phys < DWC3DCI_PHYS_MAX; phys++)
		dwc3dci_end(sc, phys);
	sc->sc_flags.port_powered = 0;
	sc->sc_flags.status_vbus = 0;
	sc->sc_flags.status_bus_reset = 0;
	sc->sc_flags.status_suspend = 0;
	sc->sc_flags.change_suspend = 0;
	sc->sc_flags.change_connect = 1;
	dwc3dci_pull_down(sc);
	dwc3_wr(sc, DWC3_DEVTEN, 0);
	USB_BUS_UNLOCK(&sc->sc_bus);
}

static void
dwc3dci_suspend(struct dwc3dci_softc *sc)
{
}

static void
dwc3dci_resume(struct dwc3dci_softc *sc)
{
}

static void
dwc3dci_do_poll(struct usb_bus *bus)
{
	struct dwc3dci_softc *sc = DWC3DCI_BUS2SC(bus);

	USB_BUS_LOCK(&sc->sc_bus);
	dwc3dci_interrupt_poll(sc);
	USB_BUS_UNLOCK(&sc->sc_bus);
}

/*------------------------------------------------------------------------*
 * dwc3dci bulk support
 * dwc3dci control support
 * dwc3dci interrupt support
 *------------------------------------------------------------------------*/
static void
dwc3dci_device_non_isoc_open(struct usb_xfer *xfer)
{
	return;
}

static void
dwc3dci_device_non_isoc_close(struct usb_xfer *xfer)
{
	dwc3dci_device_done(xfer, USB_ERR_CANCELLED);
}

static void
dwc3dci_device_non_isoc_enter(struct usb_xfer *xfer)
{
	return;
}

static void
dwc3dci_device_non_isoc_start(struct usb_xfer *xfer)
{
	/* setup TDs */
	dwc3dci_setup_standard_chain(xfer);
	dwc3dci_start_standard_chain(xfer);
}

static const struct usb_pipe_methods dwc3dci_device_non_isoc_methods =
{
	.open = dwc3dci_device_non_isoc_open,
	.close = dwc3dci_device_non_isoc_close,
	.enter = dwc3dci_device_non_isoc_enter,
	.start = dwc3dci_device_non_isoc_start,
};

static const struct usb_device_descriptor dwc3dci_devd = {
	.bLength = sizeof(struct usb_device_descriptor),
	.bDescriptorType = UDESC_DEVICE,
	.bcdUSB = {0x00, 0x02},
	.bDeviceClass = UDCLASS_HUB,
	.bDeviceSubClass = UDSUBCLASS_HUB,
	.bDeviceProtocol = UDPROTO_FSHUB,
	.bMaxPacketSize = 64,
	.bcdDevice = {0x00, 0x01},
	.iManufacturer = 1,
	.iProduct = 2,
	.bNumConfigurations = 1,
};

static const struct dwc3dci_config_desc dwc3dci_confd = {
	.confd = {
		.bLength = sizeof(struct usb_config_descriptor),
		.bDescriptorType = UDESC_CONFIG,
		.wTotalLength[0] = sizeof(dwc3dci_confd),
		.bNumInterface = 1,
		.bConfigurationValue = 1,
		.iConfiguration = 0,
		.bmAttributes = UC_SELF_POWERED,
		.bMaxPower = 0,
	},
	.ifcd = {
		.bLength = sizeof(struct usb_interface_descriptor),
		.bDescriptorType = UDESC_INTERFACE,
		.bNumEndpoints = 1,
		.bInterfaceClass = UICLASS_HUB,
		.bInterfaceSubClass = UISUBCLASS_HUB,
		.bInterfaceProtocol = 0,
	},
	.endpd = {
		.bLength = sizeof(struct usb_endpoint_descriptor),
		.bDescriptorType = UDESC_ENDPOINT,
		.bEndpointAddress = (UE_DIR_IN | DWC3DCI_INTR_ENDPT),
		.bmAttributes = UE_INTERRUPT,
		.wMaxPacketSize[0] = 8,
		.bInterval = 255,
	},
};
#define	HSETW(ptr, val) ptr = { (uint8_t)(val), (uint8_t)((val) >> 8) }

static const struct usb_hub_descriptor_min dwc3dci_hubd = {
	.bDescLength = sizeof(dwc3dci_hubd),
	.bDescriptorType = UDESC_HUB,
	.bNbrPorts = 1,
	HSETW(.wHubCharacteristics, (UHD_PWR_NO_SWITCH | UHD_OC_INDIVIDUAL)),
	.bPwrOn2PwrGood = 50,
	.bHubContrCurrent = 0,
	.DeviceRemovable = {0},		/* port is removable */
};

#define	STRING_VENDOR \
  "A\0T\0M\0E\0G\0A"

#define	STRING_PRODUCT \
  "D\0C\0I\0 \0R\0o\0o\0t\0 \0H\0U\0B"

USB_MAKE_STRING_DESC(STRING_VENDOR, dwc3dci_vendor);
USB_MAKE_STRING_DESC(STRING_PRODUCT, dwc3dci_product);

static usb_error_t
dwc3dci_roothub_exec(struct usb_device *udev,
    struct usb_device_request *req, const void **pptr, uint16_t *plength)
{
	struct dwc3dci_softc *sc = DWC3DCI_BUS2SC(udev->bus);
	const void *ptr;
	uint16_t len;
	uint16_t value;
	uint16_t index;
	uint8_t temp;
	usb_error_t err;

	USB_BUS_LOCK_ASSERT(&sc->sc_bus, MA_OWNED);

	/* buffer reset */
	ptr = (const void *)&sc->sc_hub_temp;
	len = 0;
	err = 0;

	value = UGETW(req->wValue);
	index = UGETW(req->wIndex);

	/* demultiplex the control request */

	switch (req->bmRequestType) {
	case UT_READ_DEVICE:
		switch (req->bRequest) {
		case UR_GET_DESCRIPTOR:
			goto tr_handle_get_descriptor;
		case UR_GET_CONFIG:
			goto tr_handle_get_config;
		case UR_GET_STATUS:
			goto tr_handle_get_status;
		default:
			goto tr_stalled;
		}
		break;

	case UT_WRITE_DEVICE:
		switch (req->bRequest) {
		case UR_SET_ADDRESS:
			goto tr_handle_set_address;
		case UR_SET_CONFIG:
			goto tr_handle_set_config;
		case UR_CLEAR_FEATURE:
			goto tr_valid;	/* nop */
		case UR_SET_DESCRIPTOR:
			goto tr_valid;	/* nop */
		case UR_SET_FEATURE:
		default:
			goto tr_stalled;
		}
		break;

	case UT_WRITE_ENDPOINT:
		switch (req->bRequest) {
		case UR_CLEAR_FEATURE:
			switch (UGETW(req->wValue)) {
			case UF_ENDPOINT_HALT:
				goto tr_handle_clear_halt;
			case UF_DEVICE_REMOTE_WAKEUP:
				goto tr_handle_clear_wakeup;
			default:
				goto tr_stalled;
			}
			break;
		case UR_SET_FEATURE:
			switch (UGETW(req->wValue)) {
			case UF_ENDPOINT_HALT:
				goto tr_handle_set_halt;
			case UF_DEVICE_REMOTE_WAKEUP:
				goto tr_handle_set_wakeup;
			default:
				goto tr_stalled;
			}
			break;
		case UR_SYNCH_FRAME:
			goto tr_valid;	/* nop */
		default:
			goto tr_stalled;
		}
		break;

	case UT_READ_ENDPOINT:
		switch (req->bRequest) {
		case UR_GET_STATUS:
			goto tr_handle_get_ep_status;
		default:
			goto tr_stalled;
		}
		break;

	case UT_WRITE_INTERFACE:
		switch (req->bRequest) {
		case UR_SET_INTERFACE:
			goto tr_handle_set_interface;
		case UR_CLEAR_FEATURE:
			goto tr_valid;	/* nop */
		case UR_SET_FEATURE:
		default:
			goto tr_stalled;
		}
		break;

	case UT_READ_INTERFACE:
		switch (req->bRequest) {
		case UR_GET_INTERFACE:
			goto tr_handle_get_interface;
		case UR_GET_STATUS:
			goto tr_handle_get_iface_status;
		default:
			goto tr_stalled;
		}
		break;

	case UT_WRITE_CLASS_INTERFACE:
	case UT_WRITE_VENDOR_INTERFACE:
		/* XXX forward */
		break;

	case UT_READ_CLASS_INTERFACE:
	case UT_READ_VENDOR_INTERFACE:
		/* XXX forward */
		break;

	case UT_WRITE_CLASS_DEVICE:
		switch (req->bRequest) {
		case UR_CLEAR_FEATURE:
			goto tr_valid;
		case UR_SET_DESCRIPTOR:
		case UR_SET_FEATURE:
			break;
		default:
			goto tr_stalled;
		}
		break;

	case UT_WRITE_CLASS_OTHER:
		switch (req->bRequest) {
		case UR_CLEAR_FEATURE:
			goto tr_handle_clear_port_feature;
		case UR_SET_FEATURE:
			goto tr_handle_set_port_feature;
		case UR_CLEAR_TT_BUFFER:
		case UR_RESET_TT:
		case UR_STOP_TT:
			goto tr_valid;

		default:
			goto tr_stalled;
		}
		break;

	case UT_READ_CLASS_OTHER:
		switch (req->bRequest) {
		case UR_GET_TT_STATE:
			goto tr_handle_get_tt_state;
		case UR_GET_STATUS:
			goto tr_handle_get_port_status;
		default:
			goto tr_stalled;
		}
		break;

	case UT_READ_CLASS_DEVICE:
		switch (req->bRequest) {
		case UR_GET_DESCRIPTOR:
			goto tr_handle_get_class_descriptor;
		case UR_GET_STATUS:
			goto tr_handle_get_class_status;

		default:
			goto tr_stalled;
		}
		break;
	default:
		goto tr_stalled;
	}
	goto tr_valid;

tr_handle_get_descriptor:
	switch (value >> 8) {
	case UDESC_DEVICE:
		if (value & 0xff) {
			goto tr_stalled;
		}
		len = sizeof(dwc3dci_devd);
		ptr = (const void *)&dwc3dci_devd;
		goto tr_valid;
	case UDESC_CONFIG:
		if (value & 0xff) {
			goto tr_stalled;
		}
		len = sizeof(dwc3dci_confd);
		ptr = (const void *)&dwc3dci_confd;
		goto tr_valid;
	case UDESC_STRING:
		switch (value & 0xff) {
		case 0:		/* Language table */
			len = sizeof(usb_string_lang_en);
			ptr = (const void *)&usb_string_lang_en;
			goto tr_valid;

		case 1:		/* Vendor */
			len = sizeof(dwc3dci_vendor);
			ptr = (const void *)&dwc3dci_vendor;
			goto tr_valid;

		case 2:		/* Product */
			len = sizeof(dwc3dci_product);
			ptr = (const void *)&dwc3dci_product;
			goto tr_valid;
		default:
			break;
		}
		break;
	default:
		goto tr_stalled;
	}
	goto tr_stalled;

tr_handle_get_config:
	len = 1;
	sc->sc_hub_temp.wValue[0] = sc->sc_conf;
	goto tr_valid;

tr_handle_get_status:
	len = 2;
	USETW(sc->sc_hub_temp.wValue, UDS_SELF_POWERED);
	goto tr_valid;

tr_handle_set_address:
	if (value & 0xFF00) {
		goto tr_stalled;
	}
	sc->sc_rt_addr = value;
	goto tr_valid;

tr_handle_set_config:
	if (value >= 2) {
		goto tr_stalled;
	}
	sc->sc_conf = value;
	goto tr_valid;

tr_handle_get_interface:
	len = 1;
	sc->sc_hub_temp.wValue[0] = 0;
	goto tr_valid;

tr_handle_get_tt_state:
tr_handle_get_class_status:
tr_handle_get_iface_status:
tr_handle_get_ep_status:
	len = 2;
	USETW(sc->sc_hub_temp.wValue, 0);
	goto tr_valid;

tr_handle_set_halt:
tr_handle_set_interface:
tr_handle_set_wakeup:
tr_handle_clear_wakeup:
tr_handle_clear_halt:
	goto tr_valid;

tr_handle_clear_port_feature:
	if (index != 1) {
		goto tr_stalled;
	}
	DPRINTFN(9, "UR_CLEAR_PORT_FEATURE on port %d\n", index);

	switch (value) {
	case UHF_PORT_SUSPEND:
		dwc3dci_wakeup_peer(sc);
		break;

	case UHF_PORT_ENABLE:
		sc->sc_flags.port_enabled = 0;
		break;

	case UHF_PORT_TEST:
	case UHF_PORT_INDICATOR:
	case UHF_C_PORT_ENABLE:
	case UHF_C_PORT_OVER_CURRENT:
	case UHF_C_PORT_RESET:
		/* nops */
		break;
	case UHF_PORT_POWER:
		sc->sc_flags.port_powered = 0;
		dwc3dci_pull_down(sc);
		dwc3dci_clocks_off(sc);
		break;
	case UHF_C_PORT_CONNECTION:
		/* clear connect change flag */
		sc->sc_flags.change_connect = 0;

		if (!sc->sc_flags.status_bus_reset) {
			/* we are not connected */
			break;
		}

		/* the control endpoint is set up at the reset (dwc3dci_interrupt) */
		break;
	case UHF_C_PORT_SUSPEND:
		sc->sc_flags.change_suspend = 0;
		break;
	default:
		err = USB_ERR_IOERROR;
		goto done;
	}
	goto tr_valid;

tr_handle_set_port_feature:
	if (index != 1) {
		goto tr_stalled;
	}
	DPRINTFN(9, "UR_SET_PORT_FEATURE\n");

	switch (value) {
	case UHF_PORT_ENABLE:
		sc->sc_flags.port_enabled = 1;
		break;
	case UHF_PORT_SUSPEND:
	case UHF_PORT_RESET:
	case UHF_PORT_TEST:
	case UHF_PORT_INDICATOR:
		/* nops */
		break;
	case UHF_PORT_POWER:
		sc->sc_flags.port_powered = 1;
		break;
	default:
		err = USB_ERR_IOERROR;
		goto done;
	}
	goto tr_valid;

tr_handle_get_port_status:

	DPRINTFN(9, "UR_GET_PORT_STATUS\n");

	if (index != 1) {
		goto tr_stalled;
	}
	if (sc->sc_flags.status_vbus) {
		dwc3dci_clocks_on(sc);
		dwc3dci_pull_up(sc);
	} else {
		dwc3dci_pull_down(sc);
		dwc3dci_clocks_off(sc);
	}

	/* Select FULL-speed and Device Side Mode */

	value = UPS_PORT_MODE_DEVICE;

	if (sc->sc_flags.port_powered) {
		value |= UPS_PORT_POWER;
	}
	if (sc->sc_flags.port_enabled) {
		value |= UPS_PORT_ENABLED;
	}
	if (sc->sc_flags.status_vbus &&
	    sc->sc_flags.status_bus_reset) {
		value |= UPS_CURRENT_CONNECT_STATUS;
		if (sc->sc_flags.status_high_speed)
			value |= UPS_HIGH_SPEED;
	}
	if (sc->sc_flags.status_suspend) {
		value |= UPS_SUSPEND;
	}
	USETW(sc->sc_hub_temp.ps.wPortStatus, value);

	value = 0;

	if (sc->sc_flags.change_connect) {
		value |= UPS_C_CONNECT_STATUS;
	}
	if (sc->sc_flags.change_suspend) {
		value |= UPS_C_SUSPEND;
	}
	USETW(sc->sc_hub_temp.ps.wPortChange, value);
	len = sizeof(sc->sc_hub_temp.ps);
	goto tr_valid;

tr_handle_get_class_descriptor:
	if (value & 0xFF) {
		goto tr_stalled;
	}
	ptr = (const void *)&dwc3dci_hubd;
	len = sizeof(dwc3dci_hubd);
	goto tr_valid;

tr_stalled:
	err = USB_ERR_STALLED;
tr_valid:
done:
	*plength = len;
	*pptr = ptr;
	return (err);
}

static void
dwc3dci_xfer_setup(struct usb_setup_params *parm)
{
	const struct usb_hw_ep_profile *pf;
	struct usb_xfer *xfer;
	void *last_obj;
	uint32_t ntd;
	uint32_t n;
	uint8_t ep_no;

	xfer = parm->curr_xfer;

	/*
	 * NOTE: This driver does not use any of the parameters that
	 * are computed from the following values. Just set some
	 * reasonable dummies:
	 */
	parm->hc_max_packet_size = 0x500;
	parm->hc_max_packet_count = 1;
	parm->hc_max_frame_size = 0x500;

	usbd_transfer_setup_sub(parm);

	/*
	 * compute maximum number of TDs
	 */
	if ((xfer->endpoint->edesc->bmAttributes & UE_XFERTYPE) == UE_CONTROL) {
		ntd = xfer->nframes + 1 /* STATUS */ + 1 /* SYNC 1 */
		    + 1 /* SYNC 2 */ ;
	} else {
		ntd = xfer->nframes + 1 /* SYNC */ ;
	}

	/*
	 * check if "usbd_transfer_setup_sub" set an error
	 */
	if (parm->err)
		return;

	/*
	 * allocate transfer descriptors
	 */
	last_obj = NULL;

	/*
	 * get profile stuff
	 */
	ep_no = xfer->endpointno & UE_ADDR;
	dwc3dci_get_hw_ep_profile(parm->udev, &pf, ep_no);

	if (pf == NULL) {
		/* should not happen */
		parm->err = USB_ERR_INVAL;
		return;
	}

	/* align data */
	parm->size[0] += ((-parm->size[0]) & (USB_HOST_ALIGN - 1));

	for (n = 0; n != ntd; n++) {
		struct dwc3dci_td *td;

		if (parm->buf) {
			td = USB_ADD_BYTES(parm->buf, parm->size[0]);

			/* init TD */
			td->max_packet_size = xfer->max_packet_size;
			td->ep_no = ep_no;
			if (pf->support_multi_buffer) {
				td->support_multi_buffer = 1;
			}
			td->obj_next = last_obj;

			last_obj = td;
		}
		parm->size[0] += sizeof(*td);
	}

	xfer->td_start[0] = last_obj;
}

static void
dwc3dci_xfer_unsetup(struct usb_xfer *xfer)
{
	return;
}

static void
dwc3dci_ep_init(struct usb_device *udev, struct usb_endpoint_descriptor *edesc,
    struct usb_endpoint *ep)
{
	struct dwc3dci_softc *sc = DWC3DCI_BUS2SC(udev->bus);

	if (udev->device_index != sc->sc_rt_addr) {
		if (udev->speed != USB_SPEED_HIGH &&
		    udev->speed != USB_SPEED_FULL)
			return;
		if ((edesc->bmAttributes & UE_XFERTYPE) == UE_ISOCHRONOUS)
			return;		/* not supported */
		ep->methods = &dwc3dci_device_non_isoc_methods;
	}
}

static void
dwc3dci_set_hw_power_sleep(struct usb_bus *bus, uint32_t state)
{
	struct dwc3dci_softc *sc = DWC3DCI_BUS2SC(bus);

	switch (state) {
	case USB_HW_POWER_SUSPEND:
		dwc3dci_suspend(sc);
		break;
	case USB_HW_POWER_SHUTDOWN:
		dwc3dci_uninit(sc);
		break;
	case USB_HW_POWER_RESUME:
		dwc3dci_resume(sc);
		break;
	default:
		break;
	}
}

static const struct usb_bus_methods dwc3dci_bus_methods =
{
	.endpoint_init = &dwc3dci_ep_init,
	.xfer_setup = &dwc3dci_xfer_setup,
	.xfer_unsetup = &dwc3dci_xfer_unsetup,
	.get_hw_ep_profile = &dwc3dci_get_hw_ep_profile,
	.xfer_stall = &dwc3dci_xfer_stall,
	.set_stall = &dwc3dci_set_stall,
	.clear_stall = &dwc3dci_clear_stall,
	.roothub_exec = &dwc3dci_roothub_exec,
	.xfer_poll = &dwc3dci_do_poll,
	.set_hw_power_sleep = &dwc3dci_set_hw_power_sleep,
};
