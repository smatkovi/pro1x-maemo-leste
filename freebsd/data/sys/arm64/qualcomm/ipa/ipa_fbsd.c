/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The IPA v4.2 on FreeBSD (module qcom_ipa, freebsd/data/README.md): the
 * device tree node "qcom,sc7180-ipa" (the Pro1X's "qcom,qcm2290-ipa"), its
 * registers, interrupts and SMP2P bits, the votes Linux's ipa_power.c gets
 * from its clock and interconnect drivers (here requests at the RPM through
 * qcom_dsp), the SMMU streams without translation (so that DMA addresses
 * are physical, and IMEM and SMEM need no mapping), and:
 *
 *   /dev/qcom_pas/ipa  the GSI firmware (ipa_fws.mbn, PAS id 15) as
 *                      /dev/qcom_pas/<dsp> takes a DSP's (qcom_pas_ioctl.h),
 *                      so that `pro1x-dsp start ipa FILE` loads it; the
 *                      first ioctl powers and configures the IPA (ipa_config),
 *                      the start has TrustZone authenticate the image, then
 *                      ipa_setup() and the QMI handshake follow
 *   qmap0..            the modem's data path: raw IP, one interface per QMAP
 *                      mux id (qmap0: mux id 1), the MAPv4 checksum offload
 *                      both ways (qmap.c), created when the handshake is done
 *   dev.qcom_ipa.0.*   state, registers, counters; "modem.rx_endpoint_id",
 *                      "modem.tx_endpoint_id", "feature.rx_offload",
 *                      "feature.tx_offload" as Linux's sysfs files
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/sbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/epoch.h>
#include <sys/stdarg.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/bpf.h>
#include <net/netisr.h>
#include <net/route.h>
#include <net/vnet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/ip_var.h>
#include <netinet6/ip6_var.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "ipa_var.h"
#include "qmap.h"
#include "qcom_dsp.h"
#include "qcom_pas_ioctl.h"

MALLOC_DEFINE(M_QCOM_IPA, "qcom_ipa", "Qualcomm IPA");

/* RPM requests (Linux: clk-smd-rpm.c, icc-rpm.c, smd-rpm.h) */
#define	RPM_ACTIVE_STATE	0
#define	RPM_SLEEP_STATE		1
#define	QCOM_SMD_RPM_IPA_CLK	0x617069	/* "ipa" */
#define	QCOM_RPM_SMD_KEY_RATE	0x007a484b	/* "KHz" */
#define	RPM_BUS_MASTER_REQ	0x73616d62	/* "bmas" */
#define	RPM_KEY_BW		0x00007762	/* "bw" */
#define	IPA_CORE_CLOCK_KHZ	100000		/* ipa_data-v4.2.c: 100 MHz */
#define	SM6115_MAS_IPA_RPM_ID	59		/* sm6115.c: qxm_ipa */
#define	IPA_MEMORY_AVG_BW_BPS	80000000	/* "memory": 80000 kB/s */
#define	IPA_SMEM_SIZE		0x2000		/* ipa_data-v4.2.c smem_size */
#define	QCOM_SMEM_HOST_MODEM	1
#define	SMEM_IPA_FILTER_TABLE	497

#define	QMAP_IF_MAX		8
#define	IPA_TX_BUFS		512
#define	IPA_TX_QLEN		256

static struct ofw_compat_data compat_data[] = {
	{ "qcom,sc7180-ipa",	1 },	/* Linux's match for the QCM2290's */
	{ "qcom,qcm2290-ipa",	1 },
	{ NULL,			0 }
};

SYSCTL_NODE(_hw, OID_AUTO, qcom_ipa, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "Qualcomm IPA");
static int tun_rpm_clock = 1;
SYSCTL_INT(_hw_qcom_ipa, OID_AUTO, rpm_clock, CTLFLAG_RDTUN, &tun_rpm_clock, 0,
    "vote the IPA's core clock at the RPM before the first register access");
static int tun_rpm_bw = 1;
SYSCTL_INT(_hw_qcom_ipa, OID_AUTO, rpm_bw, CTLFLAG_RDTUN, &tun_rpm_bw, 0,
    "vote the IPA master's bandwidth at the RPM");
static int tun_smmu = 1;
SYSCTL_INT(_hw_qcom_ipa, OID_AUTO, smmu, CTLFLAG_RDTUN, &tun_smmu, 0,
    "the IPA's SMMU streams without translation");
static int tun_muxes = 1;
SYSCTL_INT(_hw_qcom_ipa, OID_AUTO, muxes, CTLFLAG_RDTUN, &tun_muxes, 0,
    "QMAP interfaces (qmap0: mux id 1, ...)");
static int tun_rx_buffers = 128;
SYSCTL_INT(_hw_qcom_ipa, OID_AUTO, rx_buffers, CTLFLAG_RDTUN, &tun_rx_buffers,
    0, "receive buffers per RX endpoint (8 KB each)");
static int tun_csum = 1;
SYSCTL_INT(_hw_qcom_ipa, OID_AUTO, csum, CTLFLAG_RDTUN, &tun_csum, 0,
    "MAPv4 checksum offload on the QMAP interfaces");
static int tun_dump_rx;
SYSCTL_INT(_hw_qcom_ipa, OID_AUTO, dump_rx, CTLFLAG_RWTUN, &tun_dump_rx, 0,
    "log the start of this many receive buffers of the modem");

struct ipa_softc;

struct ipa_dma_os {
	bus_dma_tag_t	tag;
	bus_dmamap_t	map;
};

struct ipa_txbuf {
	struct ipa_txbuf *next;
	struct mbuf	*m;
	bus_dmamap_t	map;
};

struct qmap_if {
	struct ipa_softc *sc;
	if_t		ifp;
	uint8_t		mux_id;
	struct mbufq	q;		/* waiting for room in the ring */
};

struct ipa_work_arg {
	struct ipa_softc *sc;
	enum ipa_work	w;
};

struct ipa_softc {
	struct ipa	ipa;		/* first: the core's state */
	device_t	dev;
	struct resource	*mem[3];	/* "ipa-reg", "ipa-shared", "gsi" */
	struct resource	*irq_ipa, *irq_gsi;
	void		*ih_ipa, *ih_gsi;
	int		rid_ipa, rid_gsi;
	struct sx	sx;		/* ioctls, power, setup */
	struct cdev	*cdev;
	/* the GSI firmware's region (memory-region) and the PAS state */
	vm_paddr_t	fw_phys;
	vm_size_t	fw_size;
	char		*fw;
	void		*meta;
	vm_size_t	meta_size;
	uint32_t	pas_state;
	int		last_error;
	/* power, in the order it is taken */
	bool		rpm_clock, rpm_bw, smmu, smem, powered;
	/* SMP2P (ipa_smp2p.c) */
	phandle_t	query_xref, ready_xref, valid_xref, enabled_xref;
	u_int		query_bit, ready_bit, valid_bit, enabled_bit;
	bool		watching, notified, answered_on;
	uint32_t	n_queries;
	/* tasks */
	struct taskqueue *tq_poll, *tq_work;
	struct task	poll_task[GSI_CHANNEL_COUNT_MAX];
	struct task	work_task[IPA_WORK_COUNT];
	struct ipa_work_arg work_arg[IPA_WORK_COUNT];
	struct task	start_task;
	/* transmit */
	bus_dma_tag_t	tx_tag;
	struct ipa_txbuf txbufs[IPA_TX_BUFS], *txfree;
	struct mtx	txlock;
	/* the QMAP interfaces, by mux id - 1 */
	struct mtx	netlock;
	struct qmap_if	*mux[QMAP_IF_MAX];
	int		nmux;
	struct qmap_stats qstats;
	uint64_t	rx_nomux, rx_nombuf, rx_notip, tx_drops, tx_nobuf;
	int		dumped;
};

static struct ipa_softc *
sc_of(struct ipa *ipa)
{
	return ((struct ipa_softc *)ipa);
}

/* --- the core's system functions ------------------------------------------------- */

uint32_t
ipa_os_read(struct ipa *ipa, int region, uint32_t off)
{
	return (bus_read_4(sc_of(ipa)->mem[region], off));
}

void
ipa_os_write(struct ipa *ipa, int region, uint32_t off, uint32_t val)
{
	bus_write_4(sc_of(ipa)->mem[region], off, val);
}

void
ipa_os_log(struct ipa *ipa, const char *fmt, ...)
{
	va_list ap;

	device_print_prettyname(sc_of(ipa)->dev);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

static void
ipa_dma_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	if (error == 0 && nseg == 1)
		*(bus_addr_t *)arg = segs[0].ds_addr;
}

/* 32-bit addresses: the GSI's doorbells and events carry the low half */
int
ipa_os_dma_alloc(struct ipa *ipa, struct ipa_dma *d, size_t size, size_t align,
    int flags)
{
	struct ipa_softc *sc = sc_of(ipa);
	struct ipa_dma_os *os;
	bus_addr_t pa = 0;
	int error;

	memset(d, 0, sizeof(*d));
	os = malloc(sizeof(*os), M_QCOM_IPA, M_WAITOK | M_ZERO);
	error = bus_dma_tag_create(bus_get_dma_tag(sc->dev), align, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL, size, 1, size,
	    0, NULL, NULL, &os->tag);
	if (error != 0)
		goto fail;
	error = bus_dmamem_alloc(os->tag, &d->va, BUS_DMA_WAITOK | BUS_DMA_ZERO |
	    ((flags & IPA_DMA_UNCACHED) ? BUS_DMA_COHERENT : 0), &os->map);
	if (error != 0) {
		bus_dma_tag_destroy(os->tag);
		goto fail;
	}
	error = bus_dmamap_load(os->tag, os->map, d->va, size, ipa_dma_cb, &pa,
	    BUS_DMA_NOWAIT);
	if (error != 0 || pa == 0) {
		bus_dmamem_free(os->tag, d->va, os->map);
		bus_dma_tag_destroy(os->tag);
		error = error ? error : ENOMEM;
		goto fail;
	}
	d->pa = pa;
	d->size = size;
	d->os = os;
	return (0);
fail:
	free(os, M_QCOM_IPA);
	d->va = NULL;
	return (error);
}

void
ipa_os_dma_free(struct ipa *ipa, struct ipa_dma *d)
{
	struct ipa_dma_os *os = d->os;

	(void)ipa;
	if (os == NULL)
		return;
	bus_dmamap_unload(os->tag, os->map);
	bus_dmamem_free(os->tag, d->va, os->map);
	bus_dma_tag_destroy(os->tag);
	free(os, M_QCOM_IPA);
	memset(d, 0, sizeof(*d));
}

void
ipa_os_dma_sync(struct ipa *ipa, struct ipa_dma *d, int how)
{
	struct ipa_dma_os *os = d->os;

	(void)ipa;
	if (os == NULL)
		return;
	bus_dmamap_sync(os->tag, os->map, how == IPA_SYNC_TODEV ?
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE :
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
}

void
ipa_os_poll_schedule(struct ipa *ipa, struct gsi_channel *ch)
{
	struct ipa_softc *sc = sc_of(ipa);

	taskqueue_enqueue(sc->tq_poll, &sc->poll_task[ch->id]);
}

void
ipa_os_schedule(struct ipa *ipa, enum ipa_work w)
{
	struct ipa_softc *sc = sc_of(ipa);

	if (w < IPA_WORK_COUNT)
		taskqueue_enqueue(sc->tq_work, &sc->work_task[w]);
}

bool
ipa_os_powered(struct ipa *ipa)
{
	return (sc_of(ipa)->powered);
}

static void
ipa_poll_task(void *arg, int pending)
{
	gsi_channel_poll(arg);
}

static void
ipa_work_task(void *arg, int pending)
{
	struct ipa_work_arg *a = arg;

	ipa_work_run(&a->sc->ipa, a->w);
}

/* --- receive: QMAP frames to the interfaces --------------------------------------- */

static void
qmap_rx_pkt(void *arg, const struct qmap_pkt *p)
{
	struct ipa_softc *sc = arg;
	struct epoch_tracker et;
	struct qmap_if *qif;
	struct mbuf *m;
	if_t ifp;
	int isr;

	if (p->version == 0) {
		sc->rx_notip++;
		return;
	}
	mtx_lock(&sc->netlock);
	qif = p->mux_id >= 1 && p->mux_id <= QMAP_IF_MAX ?
	    sc->mux[p->mux_id - 1] : NULL;
	mtx_unlock(&sc->netlock);
	/* (the interfaces go only after this task queue is drained) */
	if (qif == NULL) {
		sc->rx_nomux++;
		return;
	}
	ifp = qif->ifp;
	if ((if_getflags(ifp) & IFF_UP) == 0) {
		if_inc_counter(ifp, IFCOUNTER_IQDROPS, 1);
		return;
	}
	m = m_get2(p->len, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL) {
		sc->rx_nombuf++;
		if_inc_counter(ifp, IFCOUNTER_IQDROPS, 1);
		return;
	}
	memcpy(mtod(m, void *), p->ip, p->len);
	m->m_len = m->m_pkthdr.len = p->len;
	m->m_pkthdr.rcvif = ifp;
	if ((if_getcapenable(ifp) & (p->version == 4 ? IFCAP_RXCSUM :
	    IFCAP_RXCSUM_IPV6)) != 0 &&
	    (p->csum == QMAP_CSUM_OK || p->csum == QMAP_CSUM_SKIPPED)) {
		/* rmnet's CHECKSUM_UNNECESSARY; the IPv4 header was checked */
		m->m_pkthdr.csum_flags = CSUM_DATA_VALID | CSUM_PSEUDO_HDR;
		m->m_pkthdr.csum_data = 0xffff;
		if (p->version == 4)
			m->m_pkthdr.csum_flags |= CSUM_IP_CHECKED | CSUM_IP_VALID;
	}
	if_inc_counter(ifp, IFCOUNTER_IPACKETS, 1);
	if_inc_counter(ifp, IFCOUNTER_IBYTES, p->len);
	BPF_MTAP(ifp, m);
	isr = p->version == 4 ? NETISR_IP : NETISR_IPV6;
	CURVNET_SET(if_getvnet(ifp));
	M_SETFIB(m, if_getfib(ifp));
	NET_EPOCH_ENTER(et);
	netisr_dispatch(isr, m);
	NET_EPOCH_EXIT(et);
	CURVNET_RESTORE();
}

/* A receive buffer of AP_MODEM_RX: aggregated QMAP frames, MAPv4 trailers */
void
ipa_os_rx(struct ipa *ipa, struct ipa_endpoint *ep, const uint8_t *buf,
    uint32_t len)
{
	struct ipa_softc *sc = sc_of(ipa);
	int i;

	if (sc->dumped < tun_dump_rx) {
		sc->dumped++;
		device_printf(sc->dev, "rx buffer of endpoint %u, %u bytes:",
		    ep->endpoint_id, len);
		for (i = 0; i < 48 && (uint32_t)i < len; i++)
			printf(" %02x", buf[i]);
		printf("\n");
	}
	qmap_deaggregate(buf, len, true, qmap_rx_pkt, sc, &sc->qstats);
}

/* --- transmit ------------------------------------------------------------------------ */

static void
ipa_txbuf_put(struct ipa_softc *sc, struct ipa_txbuf *tb)
{
	tb->m = NULL;
	tb->next = sc->txfree;
	sc->txfree = tb;
}

/*
 * The MAPv4 uplink headers before the packet; the transport checksum left
 * to the IPA when the stack asks for it (rmnet_map_checksum_uplink_packet).
 * NULL when the packet was dropped.
 */
static struct mbuf *
qmap_tx_prepare(struct qmap_if *qif, struct mbuf *m)
{
	uint16_t start = 0, field = 0, iplen;
	bool csum = false, udp = false;
	int flags, need, version;
	uint8_t *p;

	if (m->m_len < 1 && (m = m_pullup(m, 1)) == NULL)
		return (NULL);
	version = mtod(m, uint8_t *)[0] >> 4;
	flags = m->m_pkthdr.csum_flags;
	if (version == 4 && (flags & (CSUM_IP_TCP | CSUM_IP_UDP)) != 0) {
		if (m->m_len < (int)sizeof(struct ip) &&
		    (m = m_pullup(m, sizeof(struct ip))) == NULL)
			return (NULL);
		start = mtod(m, struct ip *)->ip_hl << 2;
		field = m->m_pkthdr.csum_data;
		udp = (flags & CSUM_IP_UDP) != 0;
		csum = true;
	} else if (version == 6 && (flags & (CSUM_IP6_TCP | CSUM_IP6_UDP)) != 0) {
		if (m->m_len < (int)sizeof(struct ip6_hdr) &&
		    (m = m_pullup(m, sizeof(struct ip6_hdr))) == NULL)
			return (NULL);
		start = sizeof(struct ip6_hdr);
		field = m->m_pkthdr.csum_data;
		udp = (flags & CSUM_IP6_UDP) != 0;
		csum = true;
		/* the IPA: TCP or UDP right after the header only */
		if (mtod(m, struct ip6_hdr *)->ip6_nxt !=
		    (udp ? IPPROTO_UDP : IPPROTO_TCP)) {
			int off, nxt = 0;

			off = ip6_lasthdr(m, 0, IPPROTO_IPV6, &nxt);
			if (off > 0)
				in6_delayed_cksum(m, m->m_pkthdr.len - off, off);
			csum = false;
		}
	} else if (version == 4 && (flags & CSUM_DELAY_DATA) != 0) {
		in_delayed_cksum(m);
	}
	if (csum) {
		need = start + field + 2;
		if (need > m->m_pkthdr.len) {
			m_freem(m);
			return (NULL);
		}
		if (m->m_len < need && (m = m_pullup(m, need)) == NULL)
			return (NULL);
		/* the IPA wants the pseudo-header sum complemented (rmnet) */
		qmap_ul_complement(mtod(m, uint8_t *) + start + field);
	}
	m->m_pkthdr.csum_flags = 0;
	iplen = m->m_pkthdr.len;
	M_PREPEND(m, QMAP_TX_HDR_LEN, M_NOWAIT);
	if (m == NULL)
		return (NULL);
	p = mtod(m, uint8_t *);
	qmap_ul_header(p, qif->mux_id, iplen, csum, udp, start, field);
	return (m);
}

/* The queued packets while there is room; txlock held */
static void
qmap_tx_drain(struct ipa_softc *sc, struct qmap_if *qif)
{
	struct gsi_channel *ch = &sc->ipa.channel[
	    sc->ipa.name_map[IPA_EP_AP_MODEM_TX]->channel_id];
	bus_dma_segment_t segs[IPA_COMMAND_TRANS_TRE_MAX];
	struct gsi_seg gs[IPA_COMMAND_TRANS_TRE_MAX];
	struct ipa_txbuf *tb;
	struct mbuf *m, *n;
	int nsegs, i, error, len;

	mtx_assert(&sc->txlock, MA_OWNED);
	while (mbufq_len(&qif->q) > 0) {
		if (sc->txfree == NULL || gsi_channel_tre_free(ch) < ch->tlv_count) {
			/*
			 * Again when transfers complete; their events have BEI
			 * (Linux's), so look for them rather than wait for the
			 * moderation timer's interrupt alone
			 */
			taskqueue_enqueue(sc->tq_poll, &sc->poll_task[ch->id]);
			return;
		}
		m = mbufq_dequeue(&qif->q);
		len = m->m_pkthdr.len;
		tb = sc->txfree;
		error = bus_dmamap_load_mbuf_sg(sc->tx_tag, tb->map, m, segs,
		    &nsegs, BUS_DMA_NOWAIT);
		if (error == EFBIG) {
			n = m_collapse(m, M_NOWAIT, ch->tlv_count);
			if (n == NULL) {
				m_freem(m);
				sc->tx_drops++;
				if_inc_counter(qif->ifp, IFCOUNTER_OERRORS, 1);
				continue;
			}
			m = n;
			error = bus_dmamap_load_mbuf_sg(sc->tx_tag, tb->map, m,
			    segs, &nsegs, BUS_DMA_NOWAIT);
		}
		if (error != 0) {
			m_freem(m);
			sc->tx_drops++;
			if_inc_counter(qif->ifp, IFCOUNTER_OERRORS, 1);
			continue;
		}
		bus_dmamap_sync(sc->tx_tag, tb->map, BUS_DMASYNC_PREWRITE);
		for (i = 0; i < nsegs; i++) {
			gs[i].pa = segs[i].ds_addr;
			gs[i].len = segs[i].ds_len;
			gs[i].opcode = IPA_CMD_NONE;
		}
		sc->txfree = tb->next;
		tb->m = m;
		error = ipa_modem_tx(&sc->ipa, gs, nsegs, tb,
		    mbufq_len(&qif->q) > 0);
		if (error != 0) {
			bus_dmamap_unload(sc->tx_tag, tb->map);
			ipa_txbuf_put(sc, tb);
			m_freem(m);
			sc->tx_drops++;
			if_inc_counter(qif->ifp, IFCOUNTER_OERRORS, 1);
			continue;
		}
		if_inc_counter(qif->ifp, IFCOUNTER_OPACKETS, 1);
		if_inc_counter(qif->ifp, IFCOUNTER_OBYTES, len - QMAP_TX_HDR_LEN);
	}
}

void
ipa_os_tx_done(struct ipa *ipa, struct ipa_endpoint *ep, void *cookie)
{
	struct ipa_softc *sc = sc_of(ipa);
	struct ipa_txbuf *tb = cookie;
	struct mbuf *m;

	(void)ep;
	mtx_lock(&sc->txlock);
	m = tb->m;
	bus_dmamap_sync(sc->tx_tag, tb->map, BUS_DMASYNC_POSTWRITE);
	bus_dmamap_unload(sc->tx_tag, tb->map);
	ipa_txbuf_put(sc, tb);
	mtx_unlock(&sc->txlock);
	m_freem(m);
	taskqueue_enqueue(sc->tq_poll, &sc->start_task);
}

static void
qmap_start_task(void *arg, int pending)
{
	struct ipa_softc *sc = arg;
	int i;

	mtx_lock(&sc->netlock);
	mtx_lock(&sc->txlock);
	for (i = 0; i < QMAP_IF_MAX; i++)
		if (sc->mux[i] != NULL)
			qmap_tx_drain(sc, sc->mux[i]);
	mtx_unlock(&sc->txlock);
	mtx_unlock(&sc->netlock);
}

static int
qmap_output(if_t ifp, struct mbuf *m, const struct sockaddr *dst,
    struct route *ro)
{
	struct qmap_if *qif = if_getsoftc(ifp);
	struct ipa_softc *sc = qif->sc;
	uint32_t af;

	if ((if_getflags(ifp) & IFF_UP) == 0 ||
	    (if_getdrvflags(ifp) & IFF_DRV_RUNNING) == 0) {
		m_freem(m);
		return (EHOSTDOWN);
	}
	if (dst->sa_family == AF_UNSPEC)
		bcopy(dst->sa_data, &af, sizeof(af));
	else
		af = RO_GET_FAMILY(ro, dst);
	if (af != AF_INET && af != AF_INET6) {
		m_freem(m);
		return (EAFNOSUPPORT);
	}
	BPF_MTAP(ifp, m);
	if ((m = qmap_tx_prepare(qif, m)) == NULL) {
		if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
		return (ENOBUFS);
	}
	mtx_lock(&sc->txlock);
	if (mbufq_enqueue(&qif->q, m) != 0) {
		mtx_unlock(&sc->txlock);
		m_freem(m);
		if_inc_counter(ifp, IFCOUNTER_OQDROPS, 1);
		return (ENOBUFS);
	}
	qmap_tx_drain(sc, qif);
	mtx_unlock(&sc->txlock);
	return (0);
}

static void
qmap_hwassist(if_t ifp)
{
	int assist = 0, cap = if_getcapenable(ifp);

	if (cap & IFCAP_TXCSUM)
		assist |= CSUM_IP_TCP | CSUM_IP_UDP;
	if (cap & IFCAP_TXCSUM_IPV6)
		assist |= CSUM_IP6_TCP | CSUM_IP6_UDP;
	if_sethwassist(ifp, assist);
}

static int
qmap_ioctl(if_t ifp, u_long cmd, caddr_t data)
{
	struct ifreq *ifr = (struct ifreq *)data;
	int mask;

	switch (cmd) {
	case SIOCSIFADDR:
		if_setflagbits(ifp, IFF_UP, 0);
		if_setdrvflagbits(ifp, IFF_DRV_RUNNING, 0);
		return (0);
	case SIOCSIFDSTADDR:
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		return (0);
	case SIOCSIFFLAGS:
		if (if_getflags(ifp) & IFF_UP)
			if_setdrvflagbits(ifp, IFF_DRV_RUNNING, 0);
		else
			if_setdrvflagbits(ifp, 0, IFF_DRV_RUNNING);
		return (0);
	case SIOCSIFMTU:
		/* the IPA's own limit (ipa_modem.c: max_mtu IPA_MTU) */
		if (ifr->ifr_mtu < 576 || ifr->ifr_mtu > IPA_MTU)
			return (EINVAL);
		if_setmtu(ifp, ifr->ifr_mtu);
		return (0);
	case SIOCSIFCAP:
		mask = ifr->ifr_reqcap ^ if_getcapenable(ifp);
		if_togglecapenable(ifp, mask & (IFCAP_HWCSUM | IFCAP_HWCSUM_IPV6));
		qmap_hwassist(ifp);
		return (0);
	default:
		return (EINVAL);
	}
}

/* --- the modem's side up and down: the interfaces ------------------------------------ */

void
ipa_os_modem_up(struct ipa *ipa)
{
	struct ipa_softc *sc = sc_of(ipa);
	struct qmap_if *qif;
	if_t ifp;
	int i, n = MAX(1, MIN(tun_muxes, QMAP_IF_MAX));

	/* from a task: the interfaces belong to the base system's vnet */
	CURVNET_SET(vnet0);
	for (i = 0; i < n; i++) {
		if (sc->mux[i] != NULL)
			continue;
		qif = malloc(sizeof(*qif), M_QCOM_IPA, M_WAITOK | M_ZERO);
		qif->sc = sc;
		qif->mux_id = i + 1;
		mbufq_init(&qif->q, IPA_TX_QLEN);
		ifp = if_alloc(IFT_PPP);
		if_initname(ifp, "qmap", i);
		if_setsoftc(ifp, qif);
		if_setflags(ifp, IFF_POINTOPOINT | IFF_MULTICAST | IFF_NOARP);
		if_setmtu(ifp, IPA_MTU);
		if_setioctlfn(ifp, qmap_ioctl);
		if_setoutputfn(ifp, qmap_output);
		if (tun_csum) {
			if_setcapabilities(ifp, IFCAP_HWCSUM | IFCAP_HWCSUM_IPV6);
			if_setcapenable(ifp, IFCAP_HWCSUM | IFCAP_HWCSUM_IPV6);
		}
		qmap_hwassist(ifp);
		if_attach(ifp);
		bpfattach(ifp, DLT_RAW, 0);
		qif->ifp = ifp;
		mtx_lock(&sc->netlock);
		sc->mux[i] = qif;
		sc->nmux = MAX(sc->nmux, i + 1);
		mtx_unlock(&sc->netlock);
		device_printf(sc->dev, "%s: QMAP mux id %u%s\n", if_name(ifp),
		    qif->mux_id, tun_csum ? ", MAPv4 checksum offload" : "");
	}
	CURVNET_RESTORE();
}

void
ipa_os_modem_down(struct ipa *ipa)
{
	struct ipa_softc *sc = sc_of(ipa);
	struct qmap_if *gone[QMAP_IF_MAX];
	int i;

	mtx_lock(&sc->netlock);
	for (i = 0; i < QMAP_IF_MAX; i++) {
		gone[i] = sc->mux[i];
		sc->mux[i] = NULL;
	}
	sc->nmux = 0;
	mtx_unlock(&sc->netlock);
	/* nobody in the receive path holds an interface after this */
	taskqueue_drain_all(sc->tq_poll);
	for (i = 0; i < QMAP_IF_MAX; i++) {
		if (gone[i] == NULL)
			continue;
		device_printf(sc->dev, "%s: gone\n", if_name(gone[i]->ifp));
		if_setdrvflagbits(gone[i]->ifp, 0, IFF_DRV_RUNNING);
		CURVNET_SET(if_getvnet(gone[i]->ifp));
		bpfdetach(gone[i]->ifp);
		if_detach(gone[i]->ifp);
		CURVNET_RESTORE();
	}
	/* no qmap_output() running after this: it runs in the net epoch */
	NET_EPOCH_WAIT();
	for (i = 0; i < QMAP_IF_MAX; i++) {
		if (gone[i] == NULL)
			continue;
		mtx_lock(&sc->txlock);
		mbufq_drain(&gone[i]->q);
		mtx_unlock(&sc->txlock);
		if_free(gone[i]->ifp);
		free(gone[i], M_QCOM_IPA);
	}
}

/* --- SMP2P: the modem asks whether the IPA is powered (ipa_smp2p.c) --------------- */

static void
ipa_smp2p_notify(struct ipa_softc *sc)
{
	bool on;

	if (sc->notified || sc->valid_xref == 0 || sc->enabled_xref == 0)
		return;
	on = sc->powered;
	qcom_smp2p_out_update(sc->enabled_xref, 1U << sc->enabled_bit,
	    on ? 1U << sc->enabled_bit : 0);
	qcom_smp2p_out_update(sc->valid_xref, 1U << sc->valid_bit,
	    1U << sc->valid_bit);
	sc->notified = true;
	sc->answered_on = on;
	device_printf(sc->dev, "the modem asked for the IPA's clock: %s\n",
	    on ? "enabled" : "not enabled");
}

/* ipa_smp2p_notify_reset(): the valid flag and the answer cleared */
void
ipa_os_modem_gone(struct ipa *ipa)
{
	struct ipa_softc *sc = sc_of(ipa);

	if (!sc->notified)
		return;
	qcom_smp2p_out_update(sc->valid_xref, 1U << sc->valid_bit, 0);
	qcom_smp2p_out_update(sc->enabled_xref, 1U << sc->enabled_bit, 0);
	sc->notified = false;
	device_printf(sc->dev, "SMP2P clock answer reset for the modem's next "
	    "boot\n");
}

static void
ipa_smp2p_cb(void *arg, u_int bit, bool set)
{
	struct ipa_softc *sc = arg;

	if (!set)
		return;
	if (bit == sc->query_bit) {
		sc->n_queries++;
		ipa_smp2p_notify(sc);
	} else if (bit == sc->ready_bit)
		/* "ipa-setup-ready": the modem loads the GSI firmware (not here) */
		device_printf(sc->dev, "the modem says setup ready (unused: the "
		    "AP loads the GSI firmware)\n");
}

static void
ipa_smp2p_watch(struct ipa_softc *sc)
{
	int error;

	if (sc->watching || sc->query_xref == 0)
		return;
	error = qcom_smp2p_in_watch(sc->query_xref, sc->query_bit,
	    ipa_smp2p_cb, sc);
	device_printf(sc->dev, "SMP2P: watching the modem's clock query: %s "
	    "(%d)\n", error == 0 ? "yes" : "not yet", error);
	if (error == 0) {
		sc->watching = true;
		if (sc->ready_xref != 0)
			(void)qcom_smp2p_in_watch(sc->ready_xref, sc->ready_bit,
			    ipa_smp2p_cb, sc);
	}
}

/* --- power and configuration ------------------------------------------------------ */

static int
ipa_rpm_votes(struct ipa_softc *sc)
{
	uint32_t kv[2];
	int error;

	if (tun_rpm_clock && !sc->rpm_clock) {
		/* clk-smd-rpm: the rate in kHz, active and sleep set */
		kv[0] = QCOM_RPM_SMD_KEY_RATE;
		kv[1] = IPA_CORE_CLOCK_KHZ;
		error = qcom_rpm_write(RPM_ACTIVE_STATE, QCOM_SMD_RPM_IPA_CLK, 0,
		    kv, 1);
		if (error == 0)
			error = qcom_rpm_write(RPM_SLEEP_STATE,
			    QCOM_SMD_RPM_IPA_CLK, 0, kv, 1);
		device_printf(sc->dev, "RPM: IPA core clock %u kHz: %s (%d)\n",
		    IPA_CORE_CLOCK_KHZ, error == 0 ? "voted" : "failed", error);
		if (error != 0)
			return (error);
		sc->rpm_clock = true;
	}
	if (tun_rpm_bw && !sc->rpm_bw) {
		/* icc-rpm: qxm_ipa's summed average, in bytes per second */
		kv[0] = RPM_KEY_BW;
		kv[1] = IPA_MEMORY_AVG_BW_BPS;
		error = qcom_rpm_write(RPM_ACTIVE_STATE, RPM_BUS_MASTER_REQ,
		    SM6115_MAS_IPA_RPM_ID, kv, 1);
		if (error == 0)
			error = qcom_rpm_write(RPM_SLEEP_STATE, RPM_BUS_MASTER_REQ,
			    SM6115_MAS_IPA_RPM_ID, kv, 1);
		device_printf(sc->dev, "RPM: IPA master bandwidth %u B/s: %s (%d)\n",
		    IPA_MEMORY_AVG_BW_BPS, error == 0 ? "voted" : "failed (going "
		    "on)", error);
		sc->rpm_bw = error == 0;
	}
	return (0);
}

/* The node's "iommus": each stream without translation */
static int
ipa_smmu_setup(struct ipa_softc *sc)
{
	phandle_t node = ofw_bus_get_node(sc->dev), smmu;
	pcell_t *cells, icells;
	ssize_t len, n;
	uint32_t sid, mask;
	int error = 0;

	if (!tun_smmu || sc->smmu)
		return (0);
	len = OF_getencprop_alloc_multi(node, "iommus", sizeof(pcell_t),
	    (void **)&cells);
	if (len <= 0) {
		device_printf(sc->dev, "no iommus: DMA as is\n");
		sc->smmu = true;
		return (0);
	}
	for (n = 0; n < len; n += 1 + icells) {
		smmu = OF_node_from_xref(cells[n]);
		if (OF_getencprop(smmu, "#iommu-cells", &icells,
		    sizeof(icells)) <= 0 || n + 1 + icells > len) {
			error = ENXIO;
			break;
		}
		sid = icells > 0 ? cells[n + 1] : 0;
		mask = icells > 1 ? cells[n + 2] : 0;
		error = qcom_smmu_bypass(cells[n], sid, mask, "qcom_ipa");
		if (error != 0)
			break;
	}
	OF_prop_free(cells);
	if (error != 0) {
		device_printf(sc->dev, "SMMU: %d\n", error);
		return (error);
	}
	sc->smmu = true;
	return (0);
}

/* ipa_smem_init(): SMEM item 497 for the modem (no mapping: no translation) */
static void
ipa_smem_setup(struct ipa_softc *sc)
{
	size_t size = 0;
	void *p;
	int error;

	if (sc->smem)
		return;
	error = qcom_smem_alloc(QCOM_SMEM_HOST_MODEM, SMEM_IPA_FILTER_TABLE,
	    IPA_SMEM_SIZE);
	p = qcom_smem_get(QCOM_SMEM_HOST_MODEM, SMEM_IPA_FILTER_TABLE, &size,
	    &error);
	if (p == NULL)
		device_printf(sc->dev, "SMEM item %u: %d\n", SMEM_IPA_FILTER_TABLE,
		    error);
	else {
		device_printf(sc->dev, "SMEM item %u: %#jx, %zu bytes\n",
		    SMEM_IPA_FILTER_TABLE, (uintmax_t)qcom_smem_virt_to_phys(p),
		    size);
		sc->smem = true;
	}
}

static void
ipa_intr(void *arg)
{
	struct ipa_softc *sc = arg;

	if (sc->powered)
		ipa_interrupt(&sc->ipa);
}

static void
gsi_intr(void *arg)
{
	struct ipa_softc *sc = arg;

	if (sc->powered)
		gsi_isr(&sc->ipa);
}

/*
 * Before the first register access: the clock (and bandwidth) votes, the
 * SMMU streams, SMEM; then ipa_config. Console lines before each step, so
 * that the RAM console shows where a reset happened.
 */
static int
ipa_power_on(struct ipa_softc *sc)
{
	int error;

	sx_assert(&sc->sx, SA_XLOCKED);
	if (sc->ipa.configured)
		return (0);
	device_printf(sc->dev, "power: RPM votes\n");
	if ((error = ipa_rpm_votes(sc)) != 0)
		return (error);
	device_printf(sc->dev, "power: SMMU streams\n");
	if ((error = ipa_smmu_setup(sc)) != 0)
		return (error);
	ipa_smem_setup(sc);
	sc->powered = true;
	device_printf(sc->dev, "power: the first register reads\n");
	DELAY(1000);
	if ((error = ipa_config(&sc->ipa)) != 0) {
		device_printf(sc->dev, "configuration: %d\n", error);
		return (error);
	}
	device_printf(sc->dev, "IPA v4.2 configured\n");
	if (sc->irq_ipa != NULL && sc->ih_ipa == NULL &&
	    bus_setup_intr(sc->dev, sc->irq_ipa, INTR_TYPE_NET | INTR_MPSAFE,
	    NULL, ipa_intr, sc, &sc->ih_ipa) != 0)
		device_printf(sc->dev, "no \"ipa\" interrupt\n");
	ipa_smp2p_watch(sc);
	return (0);
}

/* After TrustZone ran the GSI firmware: the interrupt, then ipa_setup */
static int
ipa_start(struct ipa_softc *sc)
{
	uint32_t st = 0;
	int i, error;

	for (i = 0; i < 1000; i++) {
		st = gsi_rd(&sc->ipa, GSI_STATUS);
		if (st & GSI_STATUS_ENABLED)
			break;
		DELAY(1000);
	}
	device_printf(sc->dev, "GSI status %#x after %d ms\n", st, i);
	if (sc->irq_gsi != NULL && sc->ih_gsi == NULL &&
	    bus_setup_intr(sc->dev, sc->irq_gsi, INTR_TYPE_NET | INTR_MPSAFE,
	    NULL, gsi_intr, sc, &sc->ih_gsi) != 0)
		device_printf(sc->dev, "no \"gsi\" interrupt\n");
	error = ipa_setup(&sc->ipa);
	if (error != 0)
		device_printf(sc->dev, "setup: %d\n", error);
	return (error);
}

/* --- /dev/qcom_pas/ipa: the GSI firmware (qcom_pas_ioctl.h) -------------------------- */

static void
ipa_meta_free(struct ipa_softc *sc)
{
	if (sc->meta != NULL) {
		kmem_free(sc->meta, sc->meta_size);
		sc->meta = NULL;
	}
}

static int
ipa_pas_init(struct ipa_softc *sc, struct qcom_pas_init *in)
{
	int error;

	if (in->meta_len == 0 || in->meta_len > 1024 * 1024)
		return (EINVAL);
	if (sc->pas_state == QCOM_PAS_STATE_RUNNING)
		return (EBUSY);
	if ((error = ipa_power_on(sc)) != 0)
		return (error);
	ipa_meta_free(sc);
	sc->meta_size = round_page(in->meta_len);
	sc->meta = kmem_alloc_contig(sc->meta_size, M_WAITOK | M_ZERO, 0,
	    ~(vm_paddr_t)0, PAGE_SIZE, 0, VM_MEMATTR_UNCACHEABLE);
	if (sc->meta == NULL)
		return (ENOMEM);
	error = copyin((void *)(uintptr_t)in->meta, sc->meta, in->meta_len);
	if (error == 0)
		error = qcom_scm_pas_init_image(IPA_PAS_ID,
		    pmap_kextract((vm_offset_t)sc->meta));
	if (error != 0) {
		device_printf(sc->dev, "PAS init image: %d\n", error);
		ipa_meta_free(sc);
		return (error);
	}
	sc->pas_state = QCOM_PAS_STATE_LOADING;
	return (0);
}

static int
ipa_pas_load(struct ipa_softc *sc, struct qcom_pas_segment *seg)
{
	int error;

	if (sc->pas_state != QCOM_PAS_STATE_LOADING)
		return (EINVAL);
	if (seg->filesz > seg->memsz || seg->offset > sc->fw_size ||
	    seg->memsz > sc->fw_size - seg->offset)
		return (EINVAL);
	error = copyin((void *)(uintptr_t)seg->data, sc->fw + seg->offset,
	    seg->filesz);
	if (error != 0)
		return (error);
	if (seg->memsz > seg->filesz)
		memset(sc->fw + seg->offset + seg->filesz, 0,
		    seg->memsz - seg->filesz);
	return (0);
}

static int
ipa_pas_start(struct ipa_softc *sc)
{
	int error;

	if (sc->pas_state != QCOM_PAS_STATE_LOADING)
		return (EINVAL);
	wmb();
	device_printf(sc->dev, "PAS %u: authenticating the GSI firmware\n",
	    IPA_PAS_ID);
	error = qcom_scm_pas_auth_and_reset(IPA_PAS_ID);
	ipa_meta_free(sc);
	if (error != 0) {
		device_printf(sc->dev, "PAS auth and reset: %d\n", error);
		sc->pas_state = QCOM_PAS_STATE_OFF;
		sc->last_error = error;
		return (error);
	}
	error = ipa_start(sc);
	sc->last_error = error;
	sc->pas_state = error == 0 ? QCOM_PAS_STATE_RUNNING :
	    QCOM_PAS_STATE_CRASHED;
	return (error);
}

static int
ipa_pas_ioctl(struct cdev *cdev, u_long cmd, caddr_t data, int fflag,
    struct thread *td)
{
	struct ipa_softc *sc = cdev->si_drv1;
	struct qcom_pas_status *st;
	int error;

	sx_xlock(&sc->sx);
	switch (cmd) {
	case QCOM_PAS_INIT:
		error = ipa_pas_init(sc, (struct qcom_pas_init *)data);
		break;
	case QCOM_PAS_MEM_SETUP:
		if (sc->pas_state != QCOM_PAS_STATE_LOADING ||
		    *(uint64_t *)data > sc->fw_size) {
			error = EINVAL;
			break;
		}
		error = qcom_scm_pas_mem_setup(IPA_PAS_ID, sc->fw_phys,
		    *(uint64_t *)data);
		if (error != 0)
			device_printf(sc->dev, "PAS mem setup: %d\n", error);
		break;
	case QCOM_PAS_LOAD:
		error = ipa_pas_load(sc, (struct qcom_pas_segment *)data);
		break;
	case QCOM_PAS_START:
		error = ipa_pas_start(sc);
		break;
	case QCOM_PAS_STOP:
		ipa_teardown(&sc->ipa);
		sc->pas_state = QCOM_PAS_STATE_OFF;
		error = 0;
		break;
	case QCOM_PAS_SHUTDOWN:
		error = qcom_scm_pas_shutdown(IPA_PAS_ID);
		break;
	case QCOM_PAS_STATUS:
		st = (struct qcom_pas_status *)data;
		memset(st, 0, sizeof(*st));
		st->pas_id = IPA_PAS_ID;
		st->state = sc->pas_state;
		st->mem_phys = sc->fw_phys;
		st->mem_size = sc->fw_size;
		st->flags = sc->ipa.setup_complete ? QCOM_PAS_F_READY : 0;
		if (sc->last_error != 0)
			snprintf(st->crash_reason, sizeof(st->crash_reason),
			    "IPA setup error %d", sc->last_error);
		error = 0;
		break;
	default:
		error = ENOTTY;
	}
	sx_xunlock(&sc->sx);
	return (error);
}

static struct cdevsw ipa_cdevsw = {
	.d_version =	D_VERSION,
	.d_ioctl =	ipa_pas_ioctl,
	.d_name =	"qcom_ipa",
};

/* --- sysctls ------------------------------------------------------------------------- */

static const char *
ipa_state_name(struct ipa_softc *sc)
{
	if (sc->ipa.modem_state == IPA_MODEM_STATE_RUNNING)
		return ("running");
	if (sc->ipa.setup_complete)
		return ("setup");
	if (sc->ipa.configured)
		return ("configured");
	if (sc->powered)
		return ("powered");
	return ("off");
}

static int
ipa_sysctl_state(SYSCTL_HANDLER_ARGS)
{
	struct ipa_softc *sc = arg1;
	const char *s = ipa_state_name(sc);

	return (sysctl_handle_string(oidp, __DECONST(char *, s), 0, req));
}

static int
ipa_sysctl_power(SYSCTL_HANDLER_ARGS)
{
	struct ipa_softc *sc = arg1;
	int val = sc->ipa.configured, error;

	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL || val == 0)
		return (error);
	sx_xlock(&sc->sx);
	error = ipa_power_on(sc);
	sx_xunlock(&sc->sx);
	return (error);
}

static const char *const gsi_ch_states[16] = { "not allocated", "allocated",
    "started", "stopped", "stop in progress", "flow controlled",
    [15] = "error" };

static int
ipa_sysctl_regs(SYSCTL_HANDLER_ARGS)
{
	struct ipa_softc *sc = arg1;
	struct ipa *ipa = &sc->ipa;
	struct sbuf *sb;
	uint32_t v;
	int i, error;

	sb = sbuf_new_for_sysctl(NULL, NULL, 512, req);
	if (!sc->powered) {
		sbuf_printf(sb, "not powered");
	} else {
		sbuf_printf(sb, "FLAVOR_0 %#x SHARED_MEM_SIZE %#x ROUTE %#x "
		    "IRQ_STTS %#x IRQ_EN %#x GSI_STATUS %#x HW_PARAM_2 %#x "
		    "TYPE_IRQ %#x TYPE_IRQ_MSK %#x ERROR_LOG %#x",
		    ipa_rd(ipa, IPA_FLAVOR_0), ipa_rd(ipa, IPA_SHARED_MEM_SIZE),
		    ipa_rd(ipa, IPA_ROUTE), ipa_rd(ipa, IPA_IRQ_STTS),
		    ipa_rd(ipa, IPA_IRQ_EN), gsi_rd(ipa, GSI_STATUS),
		    gsi_rd(ipa, GSI_HW_PARAM_2), gsi_rd(ipa, GSI_CNTXT_TYPE_IRQ),
		    gsi_rd(ipa, GSI_CNTXT_TYPE_IRQ_MSK), gsi_rd(ipa, GSI_ERROR_LOG));
		for (i = 0; i < GSI_CHANNEL_COUNT_MAX; i++) {
			if (!ipa->channel[i].used)
				continue;
			v = IPA_GET(CHSTATE, gsi_rd(ipa, GSI_CH_C_CNTXT_0(i)));
			sbuf_printf(sb, "; channel %d %s", i, gsi_ch_states[v] ?
			    gsi_ch_states[v] : "?");
		}
	}
	error = sbuf_finish(sb);
	sbuf_delete(sb);
	return (error);
}

static int
ipa_sysctl_stats(SYSCTL_HANDLER_ARGS)
{
	struct ipa_softc *sc = arg1;
	struct ipa *ipa = &sc->ipa;
	struct sbuf *sb;
	int i, error;

	sb = sbuf_new_for_sysctl(NULL, NULL, 1024, req);
	sbuf_printf(sb, "gsi errors %ju general %ju command timeouts %ju "
	    "polled %ju; smp2p queries %u (answered %s)",
	    (uintmax_t)ipa->n_gsi_errors, (uintmax_t)ipa->n_gsi_general,
	    (uintmax_t)ipa->n_cmd_timeouts, (uintmax_t)ipa->n_polled_done,
	    sc->n_queries, !sc->notified ? "-" : sc->answered_on ? "on" : "off");
	for (i = 0; i < GSI_CHANNEL_COUNT_MAX; i++) {
		struct gsi_channel *ch = &ipa->channel[i];

		if (!ch->used)
			continue;
		sbuf_printf(sb, "; channel %d: %ju transactions %ju events %ju "
		    "polls %ju cancelled, %u TREs free", i, (uintmax_t)ch->n_trans,
		    (uintmax_t)ch->n_events, (uintmax_t)ch->n_polls,
		    (uintmax_t)ch->n_cancelled, gsi_channel_tre_free(ch));
	}
	for (i = 0; i < IPA_EP_NAMES; i++) {
		struct ipa_endpoint *ep = ipa->name_map[i];

		if (ep == NULL || ep->ee != GSI_EE_AP)
			continue;
		sbuf_printf(sb, "; endpoint %u%s: rx %ju buffers %ju bytes %ju "
		    "dropped, tx %ju", ep->endpoint_id, ep->enabled ? " (on)" :
		    "", (uintmax_t)ep->rx_buffers, (uintmax_t)ep->rx_bytes,
		    (uintmax_t)ep->rx_dropped, (uintmax_t)ep->tx_packets);
	}
	sbuf_printf(sb, "; qmap: %ju packets %ju commands %ju empty %ju "
	    "truncated %ju bad mux, csum ok %ju skipped %ju none %ju bad %ju; "
	    "rx no mux %ju no mbuf %ju not IP %ju; tx drops %ju",
	    (uintmax_t)sc->qstats.packets, (uintmax_t)sc->qstats.commands,
	    (uintmax_t)sc->qstats.empty, (uintmax_t)sc->qstats.truncated,
	    (uintmax_t)sc->qstats.bad_mux, (uintmax_t)sc->qstats.csum_ok,
	    (uintmax_t)sc->qstats.csum_skipped, (uintmax_t)sc->qstats.csum_none,
	    (uintmax_t)sc->qstats.csum_bad, (uintmax_t)sc->rx_nomux,
	    (uintmax_t)sc->rx_nombuf, (uintmax_t)sc->rx_notip,
	    (uintmax_t)sc->tx_drops);
	error = sbuf_finish(sb);
	sbuf_delete(sb);
	return (error);
}

static int
ipa_sysctl_qmi(SYSCTL_HANDLER_ARGS)
{
	const struct ipa_qmi_state *q = ipa_qmi_state(&((struct ipa_softc *)
	    arg1)->ipa);
	struct sbuf *sb;
	int error;

	sb = sbuf_new_for_sysctl(NULL, NULL, 256, req);
	sbuf_printf(sb, "modem %u:%u, init driver %d, modem ready %d, uc ready "
	    "%d, indication requested %d sent %d, initial boot %d, byes %u, "
	    "modem control endpoint %u", q->modem_node, q->modem_port,
	    q->init_driver_result, q->modem_ready, q->uc_ready,
	    q->indication_requested, q->indication_sent, q->initial_boot,
	    q->n_bye, q->rsp_ctrl_endpoint);
	error = sbuf_finish(sb);
	sbuf_delete(sb);
	return (error);
}

static void
ipa_sysctls(struct ipa_softc *sc)
{
	struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(sc->dev);
	struct sysctl_oid_list *top = SYSCTL_CHILDREN(device_get_sysctl_tree(
	    sc->dev));
	struct sysctl_oid *o;
	struct ipa *ipa = &sc->ipa;

	SYSCTL_ADD_PROC(ctx, top, OID_AUTO, "state", CTLTYPE_STRING | CTLFLAG_RD |
	    CTLFLAG_MPSAFE, sc, 0, ipa_sysctl_state, "A", "the IPA's state");
	SYSCTL_ADD_PROC(ctx, top, OID_AUTO, "power", CTLTYPE_INT | CTLFLAG_RW |
	    CTLFLAG_MPSAFE, sc, 0, ipa_sysctl_power, "I",
	    "1: vote, configure (ipa_config), without the GSI firmware");
	SYSCTL_ADD_PROC(ctx, top, OID_AUTO, "regs", CTLTYPE_STRING | CTLFLAG_RD |
	    CTLFLAG_MPSAFE, sc, 0, ipa_sysctl_regs, "A", "some registers");
	SYSCTL_ADD_PROC(ctx, top, OID_AUTO, "stats", CTLTYPE_STRING | CTLFLAG_RD |
	    CTLFLAG_MPSAFE, sc, 0, ipa_sysctl_stats, "A", "counters");
	SYSCTL_ADD_PROC(ctx, top, OID_AUTO, "qmi", CTLTYPE_STRING | CTLFLAG_RD |
	    CTLFLAG_MPSAFE, sc, 0, ipa_sysctl_qmi, "A", "the QMI handshake");
	SYSCTL_ADD_U32(ctx, top, OID_AUTO, "mem_offset", CTLFLAG_RD,
	    &ipa->mem_offset, 0, "SHARED_MEM_SIZE's base");
	SYSCTL_ADD_STRING(ctx, top, OID_AUTO, "version", CTLFLAG_RD, "4.2", 0,
	    "IPA version (sysfs version)");
	o = SYSCTL_ADD_NODE(ctx, top, OID_AUTO, "feature", CTLFLAG_RD |
	    CTLFLAG_MPSAFE, NULL, "as Linux's sysfs feature/");
	SYSCTL_ADD_STRING(ctx, SYSCTL_CHILDREN(o), OID_AUTO, "rx_offload",
	    CTLFLAG_RD, "MAPv4", 0, "MAP checksum offload, receive");
	SYSCTL_ADD_STRING(ctx, SYSCTL_CHILDREN(o), OID_AUTO, "tx_offload",
	    CTLFLAG_RD, "MAPv4", 0, "MAP checksum offload, transmit");
	o = SYSCTL_ADD_NODE(ctx, top, OID_AUTO, "modem", CTLFLAG_RD |
	    CTLFLAG_MPSAFE, NULL, "as Linux's sysfs modem/");
	SYSCTL_ADD_U8(ctx, SYSCTL_CHILDREN(o), OID_AUTO, "rx_endpoint_id",
	    CTLFLAG_RD, &ipa->name_map[IPA_EP_AP_MODEM_RX]->endpoint_id, 0,
	    "the AP's endpoint receiving from the modem");
	SYSCTL_ADD_U8(ctx, SYSCTL_CHILDREN(o), OID_AUTO, "tx_endpoint_id",
	    CTLFLAG_RD, &ipa->name_map[IPA_EP_AP_MODEM_TX]->endpoint_id, 0,
	    "the AP's endpoint sending to the modem");
	SYSCTL_ADD_INT(ctx, SYSCTL_CHILDREN(o), OID_AUTO, "state", CTLFLAG_RD,
	    &ipa->modem_state, 0, "0 stopped, 1 starting, 2 running, 3 stopping");
}

/* --- attachment ------------------------------------------------------------------- */

/* "interrupts-extended" by name: the SMP2P entries' xref and bit */
static void
ipa_parse_interrupts(struct ipa_softc *sc, phandle_t node)
{
	int iq, ir, entry;
	pcell_t *cells, icells;
	ssize_t len, n;
	phandle_t parent;

	if (ofw_bus_find_string_index(node, "interrupt-names", "ipa-clock-query",
	    &iq) != 0)
		iq = -1;
	if (ofw_bus_find_string_index(node, "interrupt-names", "ipa-setup-ready",
	    &ir) != 0)
		ir = -1;
	len = OF_getencprop_alloc_multi(node, "interrupts-extended",
	    sizeof(pcell_t), (void **)&cells);
	if (len <= 0)
		return;
	for (n = 0, entry = 0; n < len; entry++) {
		parent = OF_node_from_xref(cells[n]);
		if (OF_getencprop(parent, "#interrupt-cells", &icells,
		    sizeof(icells)) <= 0 || n + 1 + icells > len)
			break;
		if (entry == iq) {
			sc->query_xref = cells[n];
			sc->query_bit = cells[n + 1];
		} else if (entry == ir) {
			sc->ready_xref = cells[n];
			sc->ready_bit = cells[n + 1];
		}
		n += 1 + icells;
	}
	OF_prop_free(cells);
}

/* "qcom,smem-states" by "qcom,smem-state-names" */
static void
ipa_parse_states(struct ipa_softc *sc, phandle_t node)
{
	pcell_t cells[4];
	int iv, ie;

	if (OF_getencprop(node, "qcom,smem-states", cells, sizeof(cells)) !=
	    sizeof(cells))
		return;
	if (ofw_bus_find_string_index(node, "qcom,smem-state-names",
	    "ipa-clock-enabled-valid", &iv) != 0 || iv > 1)
		iv = 0;
	if (ofw_bus_find_string_index(node, "qcom,smem-state-names",
	    "ipa-clock-enabled", &ie) != 0 || ie > 1)
		ie = 1;
	sc->valid_xref = cells[2 * iv];
	sc->valid_bit = cells[2 * iv + 1];
	sc->enabled_xref = cells[2 * ie];
	sc->enabled_bit = cells[2 * ie + 1];
}

static int
ipa_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev) ||
	    ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);
	device_set_desc(dev, "Qualcomm IPA v4.2");
	return (BUS_PROBE_DEFAULT);
}

static int
ipa_attach(device_t dev)
{
	static const char *const names[3] = { "ipa-reg", "ipa-shared", "gsi" };
	struct ipa_softc *sc = device_get_softc(dev);
	phandle_t node = ofw_bus_get_node(dev), fwnode;
	pcell_t cell, reg[4];
	bool supported = false;
	int i, rid, error;

	sc->dev = dev;
	sc->ipa.os = sc;
	node = ofw_bus_get_node(dev);
	sx_init(&sc->sx, "qcom_ipa");
	mtx_init(&sc->txlock, "qcom_ipa tx", NULL, MTX_DEF);
	mtx_init(&sc->netlock, "qcom_ipa net", NULL, MTX_DEF);

	/* "loader" self only: the AP has the GSI firmware authenticated */
	if (OF_hasprop(node, "modem-init")) {
		device_printf(dev, "modem-init: the modem loads the GSI firmware "
		    "(not supported)\n");
		return (ENXIO);
	}
	for (i = 0; i < 3; i++) {
		if (ofw_bus_find_string_index(node, "reg-names", names[i],
		    &rid) != 0) {
			device_printf(dev, "no \"%s\" region\n", names[i]);
			error = ENXIO;
			goto fail;
		}
		sc->mem[i] = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
		    RF_ACTIVE);
		if (sc->mem[i] == NULL) {
			device_printf(dev, "\"%s\" not mapped\n", names[i]);
			error = ENXIO;
			goto fail;
		}
	}
	sc->ipa.mem_size = rman_get_size(sc->mem[IPA_REGION_SHARED]);
	if (ofw_bus_find_string_index(node, "interrupt-names", "ipa",
	    &sc->rid_ipa) == 0)
		sc->irq_ipa = bus_alloc_resource_any(dev, SYS_RES_IRQ,
		    &sc->rid_ipa, RF_ACTIVE);
	if (ofw_bus_find_string_index(node, "interrupt-names", "gsi",
	    &sc->rid_gsi) == 0)
		sc->irq_gsi = bus_alloc_resource_any(dev, SYS_RES_IRQ,
		    &sc->rid_gsi, RF_ACTIVE);
	if (sc->irq_ipa == NULL || sc->irq_gsi == NULL)
		device_printf(dev, "interrupts: ipa %s, gsi %s\n",
		    sc->irq_ipa ? "ok" : "missing", sc->irq_gsi ? "ok" : "missing");
	ipa_parse_interrupts(sc, node);
	ipa_parse_states(sc, node);

	/* the GSI firmware's region */
	if (OF_getencprop(node, "memory-region", &cell, sizeof(cell)) <= 0 ||
	    (fwnode = OF_node_from_xref(cell)) == -1 ||
	    OF_getencprop(fwnode, "reg", reg, sizeof(reg)) != sizeof(reg)) {
		device_printf(dev, "no memory-region\n");
		error = ENXIO;
		goto fail;
	}
	sc->fw_phys = (vm_paddr_t)reg[0] << 32 | reg[1];
	sc->fw_size = (vm_size_t)reg[2] << 32 | reg[3];
	sc->fw = pmap_mapdev_attr(sc->fw_phys, sc->fw_size,
	    VM_MEMATTR_WRITE_COMBINING);

	/* the core (no hardware access) */
	sc->ipa.rx_buffers_max = MAX(8, tun_rx_buffers);
	if ((error = ipa_init(&sc->ipa)) != 0) {
		device_printf(dev, "init: %d\n", error);
		goto fail;
	}
	error = bus_dma_tag_create(bus_get_dma_tag(dev), 1, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL, 65536,
	    IPA_COMMAND_TRANS_TRE_MAX, 65536, 0, NULL, NULL, &sc->tx_tag);
	if (error != 0)
		goto fail_core;
	for (i = 0; i < IPA_TX_BUFS; i++) {
		if (bus_dmamap_create(sc->tx_tag, 0, &sc->txbufs[i].map) != 0)
			break;
		ipa_txbuf_put(sc, &sc->txbufs[i]);
	}

	sc->tq_poll = taskqueue_create("qcom_ipa", M_WAITOK,
	    taskqueue_thread_enqueue, &sc->tq_poll);
	taskqueue_start_threads(&sc->tq_poll, 1, PI_NET, "%s poll",
	    device_get_nameunit(dev));
	sc->tq_work = taskqueue_create("qcom_ipa work", M_WAITOK,
	    taskqueue_thread_enqueue, &sc->tq_work);
	taskqueue_start_threads(&sc->tq_work, 1, PI_NET, "%s work",
	    device_get_nameunit(dev));
	for (i = 0; i < GSI_CHANNEL_COUNT_MAX; i++)
		TASK_INIT(&sc->poll_task[i], 0, ipa_poll_task, &sc->ipa.channel[i]);
	for (i = 0; i < IPA_WORK_COUNT; i++) {
		sc->work_arg[i].sc = sc;
		sc->work_arg[i].w = i;
		TASK_INIT(&sc->work_task[i], 0, ipa_work_task, &sc->work_arg[i]);
	}
	TASK_INIT(&sc->start_task, 0, qmap_start_task, sc);

	error = qcom_scm_pas_supported(IPA_PAS_ID, &supported);
	device_printf(dev, "GSI firmware: PAS id %u %s (%d), memory %#jx+%#jx; "
	    "SMP2P query %s, states %s\n", IPA_PAS_ID, supported ? "supported" :
	    "not supported", error, (uintmax_t)sc->fw_phys,
	    (uintmax_t)sc->fw_size, sc->query_xref ? "found" : "missing",
	    sc->valid_xref ? "found" : "missing");
	/* SMP2P may attach after us: again at the power-on */
	ipa_smp2p_watch(sc);
	ipa_sysctls(sc);
	sc->cdev = make_dev(&ipa_cdevsw, device_get_unit(dev), UID_ROOT,
	    GID_WHEEL, 0600, "qcom_pas/ipa");
	sc->cdev->si_drv1 = sc;
	return (0);
fail_core:
	ipa_exit(&sc->ipa);
fail:
	for (i = 0; i < 3; i++)
		if (sc->mem[i] != NULL)
			bus_release_resource(dev, SYS_RES_MEMORY,
			    rman_get_rid(sc->mem[i]), sc->mem[i]);
	if (sc->irq_ipa != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, sc->rid_ipa, sc->irq_ipa);
	if (sc->irq_gsi != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, sc->rid_gsi, sc->irq_gsi);
	if (sc->fw != NULL)
		pmap_unmapdev(sc->fw, sc->fw_size);
	mtx_destroy(&sc->netlock);
	mtx_destroy(&sc->txlock);
	sx_destroy(&sc->sx);
	return (error);
}

/* Once powered the IPA stays (Linux's runtime PM is not ported) */
static int
ipa_detach(device_t dev)
{
	struct ipa_softc *sc = device_get_softc(dev);
	int i;

	if (sc->powered)
		return (EBUSY);
	if (sc->watching)
		qcom_smp2p_in_unwatch(sc->query_xref, sc->query_bit);
	if (sc->watching && sc->ready_xref != 0)
		qcom_smp2p_in_unwatch(sc->ready_xref, sc->ready_bit);
	destroy_dev(sc->cdev);
	taskqueue_free(sc->tq_poll);
	taskqueue_free(sc->tq_work);
	for (i = 0; i < IPA_TX_BUFS; i++)
		if (sc->txbufs[i].map != NULL)
			bus_dmamap_destroy(sc->tx_tag, sc->txbufs[i].map);
	bus_dma_tag_destroy(sc->tx_tag);
	ipa_exit(&sc->ipa);
	for (i = 0; i < 3; i++)
		bus_release_resource(dev, SYS_RES_MEMORY, rman_get_rid(sc->mem[i]),
		    sc->mem[i]);
	if (sc->irq_ipa != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, sc->rid_ipa, sc->irq_ipa);
	if (sc->irq_gsi != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, sc->rid_gsi, sc->irq_gsi);
	pmap_unmapdev(sc->fw, sc->fw_size);
	ipa_meta_free(sc);
	mtx_destroy(&sc->netlock);
	mtx_destroy(&sc->txlock);
	sx_destroy(&sc->sx);
	return (0);
}

static device_method_t ipa_methods[] = {
	DEVMETHOD(device_probe,		ipa_probe),
	DEVMETHOD(device_attach,	ipa_attach),
	DEVMETHOD(device_detach,	ipa_detach),
	DEVMETHOD_END
};

static driver_t ipa_driver = {
	"qcom_ipa",
	ipa_methods,
	sizeof(struct ipa_softc),
};

DRIVER_MODULE(qcom_ipa, simplebus, ipa_driver, 0, 0);
MODULE_VERSION(qcom_ipa, 1);
MODULE_DEPEND(qcom_ipa, qcom_dsp, 1, 1, 1);
MODULE_DEPEND(qcom_ipa, qrtr, 1, 1, 1);
