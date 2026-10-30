/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2009 Hans Petter Selasky. All rights reserved.
 * (the device controller framework of atmegadci.h, which this follows)
 *
 * DWC_usb3 in device mode for FreeBSD's USB device side (dwc3dci.c).
 */
#ifndef _DWC3DCI_H_
#define	_DWC3DCI_H_

#include <dev/usb/controller/dwc3dci_hw.h>

#define	DWC3DCI_MAX_DEVICES	(USB_MIN_DEVICES + 1)
#define	DWC3DCI_EP_MAX		16	/* logical endpoints */
#define	DWC3DCI_PHYS_MAX	(2 * DWC3DCI_EP_MAX)
#define	DWC3DCI_EVBUF_SIZE	4096
#define	DWC3DCI_EP0_BUF		4096
#define	DWC3DCI_EP_BUF		16384

#define	DWC3DCI_BUS2SC(bus) \
	__containerof(bus, struct dwc3dci_softc, sc_bus)

struct dwc3dci_td;
struct dwc3dci_softc;

typedef uint8_t (dwc3dci_cmd_t)(struct dwc3dci_td *td);

struct dwc3dci_td {
	struct dwc3dci_td *obj_next;
	dwc3dci_cmd_t *func;
	struct usb_page_cache *pc;
	struct dwc3dci_softc *sc;
	uint32_t offset;
	uint32_t remainder;
	uint32_t req;			/* the started TRB's length */
	uint32_t chunk;			/* of it, the data's */
	uint16_t max_packet_size;
	uint8_t	error:1;
	uint8_t	alt_next:1;
	uint8_t	short_pkt:1;
	uint8_t	support_multi_buffer:1;
	uint8_t	did_stall:1;
	uint8_t	started:1;
	uint8_t	status_in:1;		/* the status stage's direction */
	uint8_t	ep_no;
};

struct dwc3dci_std_temp {
	dwc3dci_cmd_t *func;
	struct usb_page_cache *pc;
	struct dwc3dci_td *td;
	struct dwc3dci_td *td_next;
	uint32_t len;
	uint32_t offset;
	uint16_t max_frame_size;
	uint8_t	short_pkt;	/* 0: end short (a zero-length packet), 1: not */
	uint8_t	setup_alt_next;
	uint8_t did_stall;
};

/* a physical endpoint: one TRB, a bounce buffer, both uncached */
struct dwc3dci_ep {
	volatile struct dwc3_trb *trb;
	uint64_t trb_pa;
	uint8_t	*buf;
	uint64_t buf_pa;
	uint32_t buf_size;
	uint16_t mps;
	uint8_t	busy:1;			/* a TRB is with the hardware */
	uint8_t	configured:1;
	uint8_t	stalled:1;
	uint8_t	rsc;			/* its transfer resource */
};

struct dwc3dci_flags {
	uint8_t	change_connect:1;
	uint8_t	change_suspend:1;
	uint8_t	status_suspend:1;
	uint8_t	status_vbus:1;
	uint8_t	status_bus_reset:1;
	uint8_t	status_high_speed:1;
	uint8_t	remote_wakeup:1;
	uint8_t	self_powered:1;
	uint8_t	clocks_off:1;
	uint8_t	port_powered:1;
	uint8_t	port_enabled:1;
	uint8_t	d_pulled_up:1;
};

struct dwc3dci_config_desc {
	struct usb_config_descriptor confd;
	struct usb_interface_descriptor ifcd;
	struct usb_endpoint_descriptor endpd;
} __packed;

union dwc3dci_hub_temp {
	uWord	wValue;
	struct usb_port_status ps;
};

struct dwc3dci_softc {
	struct usb_bus sc_bus;
	union dwc3dci_hub_temp sc_hub_temp;
	struct usb_device *sc_devices[DWC3DCI_MAX_DEVICES];

	device_t sc_dev;
	struct resource *sc_irq_res;
	void	*sc_intr_hdl;
	struct resource *sc_io_res;	/* the DWC3 core */
	uint8_t	*sc_qscratch;		/* the Qualcomm wrapper */
	uint8_t	*sc_gcc;
	uint8_t	*sc_phy;		/* QUSB2 */

	volatile uint32_t *sc_ev;	/* the event buffer */
	uint64_t sc_ev_pa;
	uint32_t sc_ev_pos;

	struct dwc3dci_ep sc_ep[DWC3DCI_PHYS_MAX];
	uint8_t	sc_setup[8];		/* the last SETUP packet */
	uint8_t	sc_setup_armed:1;	/* the SETUP TRB is out */
	uint8_t	sc_setup_ready:1;	/* a SETUP waits for setup_rx */
	uint8_t	sc_status_ready:1;	/* XferNotReady of the status stage */
	uint8_t	sc_three_stage:1;
	uint8_t	sc_cfg_started:1;	/* STARTCFG 2 after the reset */

	uint8_t	sc_rt_addr;		/* root hub address */
	uint8_t	sc_dv_addr;		/* device address */
	uint8_t	sc_conf;		/* root hub config */
	uint8_t	sc_hub_idata[1];

	struct dwc3dci_flags sc_flags;

	/* statistics (dev.dwc3dci.0.stats) */
	uint64_t sc_n_events, sc_n_resets, sc_n_connects, sc_n_setups,
		 sc_n_trbs, sc_n_cmderr;
	uint32_t sc_snpsid;
	char	sc_bringup[256];
};

usb_error_t dwc3dci_init(struct dwc3dci_softc *sc);
void	dwc3dci_uninit(struct dwc3dci_softc *sc);
void	dwc3dci_interrupt(void *arg);
#endif
