/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Qualcomm SMP2P, after Linux's smp2p.c: each pair of processors exchanges
 * up to 16 named 32-bit values through two SMEM items, one written by each
 * side; a change is signalled by an interrupt (from the remote: the node's
 * GIC interrupt; to it: an APCS doorbell, mboxes). The remote processors use
 * the bits of their "slave-kernel" entry for fatal error, ready, handover
 * and stop acknowledgement, and read the stop request from our
 * "master-kernel" entry.
 *
 * Linux makes inbound entries interrupt controllers; here the users of a
 * bit (qcom_pas.c) register a callback for it, or read the value, by the
 * xref of the entry's device tree node.
 *
 * As Linux's qcom_smp2p_start_in and qcom_smp2p_notify_in: an entry found
 * at attach starts from its value then (nothing is reported for what was
 * set before), one the remote adds later starts from 0, so that the bits
 * it already has when it first appears (a DSP that set "ready" before its
 * interrupt was seen) are reported as changes.
 *
 * After a kexec the items stay in SMEM. Linux zeroes its outbound item
 * when it probes; here, when the remote's item shows it running (a DSP
 * Linux started, taken over: pro1x-dsp adopt), our item is left as Linux
 * left it (entries, values, flags, version), our entries point at its
 * entries of their names, and nothing is kicked unless one had to be
 * added: the remote sees no change.
 *
 *   sysctl hw.qcom_smp2p     each edge: pids, items, entries and values
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/rman.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <machine/atomic.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "qcom_dsp.h"

#define	SMP2P_MAGIC		0x504d5324	/* "$SMP" */
#define	SMP2P_MAX_ENTRY		16
#define	SMP2P_NAME_LEN		16
#define	SMP2P_VERSION		2
#define	SMP2P_FEATURE_SSR_ACK	0x1
#define	SMP2P_FLAG_RESTART_DONE	(1U << 0)
#define	SMP2P_FLAG_RESTART_ACK	(1U << 1)

struct smp2p_item {
	uint32_t	magic;
	uint32_t	version_features;	/* version: low byte */
	uint16_t	local_pid;
	uint16_t	remote_pid;
	uint16_t	total_entries;
	uint16_t	valid_entries;
	uint32_t	flags;
	struct {
		char		name[SMP2P_NAME_LEN];
		uint32_t	value;
	} entries[SMP2P_MAX_ENTRY];
} __packed;

#define	ITEM_VERSION(it)	((it)->version_features & 0xff)
#define	ITEM_FEATURES(it)	((it)->version_features >> 8)

static MALLOC_DEFINE(M_SMP2P, "qcom_smp2p", "Qualcomm SMP2P");

struct smp2p_softc;

struct smp2p_entry {
	TAILQ_ENTRY(smp2p_entry) link;	/* in the global list */
	struct smp2p_softc *sc;
	phandle_t	xref;
	char		name[SMP2P_NAME_LEN];
	bool		inbound;
	volatile uint32_t *value;	/* in the SMEM item, once known */
	uint32_t	last;
	struct {
		qcom_smp2p_cb_t	*cb;
		void		*arg;
	} watch[32];
};

struct smp2p_softc {
	device_t	dev;
	struct mtx	lock;
	struct resource	*irq;
	void		*ih;
	u_int		item[2];	/* SMEM items: inbound, outbound */
	u_int		local_pid, remote_pid;
	phandle_t	ipc_xref;
	u_int		ipc_bit;
	struct smp2p_item *in, *out;
	u_int		valid_in;	/* inbound entries matched so far */
	bool		negotiated, ssr_ack_enabled, ssr_ack;
	bool		kept;		/* the remote was up: our values kept */
	u_int		nintr;		/* interrupts from the remote */
	u_int		version;	/* ours, as negotiated */
	struct smp2p_entry *entries[SMP2P_MAX_ENTRY];
	int		nentries;
	TAILQ_ENTRY(smp2p_softc) link;	/* for the report */
};

static TAILQ_HEAD(, smp2p_softc) smp2p_devs = TAILQ_HEAD_INITIALIZER(smp2p_devs);

static TAILQ_HEAD(, smp2p_entry) smp2p_all = TAILQ_HEAD_INITIALIZER(smp2p_all);
static struct mtx smp2p_list_lock;
MTX_SYSINIT(qcom_smp2p_list, &smp2p_list_lock, "qcom_smp2p list", MTX_DEF);

static void
smp2p_kick(struct smp2p_softc *sc)
{
	wmb();
	qcom_ipc_kick(sc->ipc_xref, sc->ipc_bit);
}

static struct smp2p_entry *
entry_find(phandle_t xref)
{
	struct smp2p_entry *e;

	mtx_lock(&smp2p_list_lock);
	TAILQ_FOREACH(e, &smp2p_all, link)
		if (e->xref == xref)
			break;
	mtx_unlock(&smp2p_list_lock);
	return (e);
}

/*
 * The remote's item, and our inbound entries' values in it. baseline: at
 * attach, an entry found starts from its current value; later ones start
 * from 0 (their set bits are news).
 */
static void
smp2p_match_in(struct smp2p_softc *sc, bool baseline)
{
	char name[SMP2P_NAME_LEN];
	struct smp2p_entry *e;
	u_int i;
	int k, error;

	if (sc->in == NULL) {
		sc->in = qcom_smem_get(sc->remote_pid, sc->item[0], NULL, &error);
		if (sc->in == NULL)
			return;
	}
	if (ITEM_VERSION(sc->in) == 0)
		return;
	for (i = sc->valid_in; i < MIN(sc->in->valid_entries, SMP2P_MAX_ENTRY);
	    i++) {
		memcpy(name, sc->in->entries[i].name, sizeof(name));
		name[SMP2P_NAME_LEN - 1] = '\0';
		for (k = 0; k < sc->nentries; k++) {
			e = sc->entries[k];
			if (e->inbound && e->value == NULL &&
			    strcmp(name, e->name) == 0) {
				e->value = &sc->in->entries[i].value;
				e->last = baseline ? *e->value : 0;
			}
		}
	}
	sc->valid_in = i;
}

static void
smp2p_negotiate(struct smp2p_softc *sc)
{
	struct smp2p_item *in = sc->in, *out = sc->out;

	if (ITEM_VERSION(in) == ITEM_VERSION(out)) {
		out->version_features = ITEM_VERSION(out) |
		    ((ITEM_FEATURES(out) & ITEM_FEATURES(in)) << 8);
		sc->ssr_ack_enabled = (ITEM_FEATURES(out) &
		    SMP2P_FEATURE_SSR_ACK) != 0;
		sc->negotiated = true;
	} else if (ITEM_VERSION(in) != 0 && ITEM_VERSION(in) < ITEM_VERSION(out)) {
		out->version_features = ITEM_VERSION(in) |
		    (ITEM_FEATURES(out) << 8);
		smp2p_kick(sc);
	}
}

static void
smp2p_intr(void *arg)
{
	struct smp2p_softc *sc = arg;
	struct smp2p_entry *e;
	struct {
		qcom_smp2p_cb_t	*cb;
		void		*arg;
		u_int		bit;
		bool		set;
	} calls[SMP2P_MAX_ENTRY * 4];
	uint32_t val, changed;
	bool restart, ack = false;
	int k, n, b;

	n = 0;
	mtx_lock(&sc->lock);
	sc->nintr++;
	smp2p_match_in(sc, false);
	if (sc->in == NULL || ITEM_VERSION(sc->in) == 0)
		goto out;
	if (!sc->negotiated)
		smp2p_negotiate(sc);
	if (!sc->negotiated)
		goto out;
	if (sc->ssr_ack_enabled) {
		restart = (sc->in->flags & SMP2P_FLAG_RESTART_DONE) != 0;
		if (restart != sc->ssr_ack) {
			/* the remote restarted: its values start from 0 */
			for (k = 0; k < sc->nentries; k++)
				if (sc->entries[k]->inbound)
					sc->entries[k]->last = 0;
			ack = true;
		}
	}
	for (k = 0; k < sc->nentries; k++) {
		e = sc->entries[k];
		if (!e->inbound || e->value == NULL)
			continue;
		val = *e->value;
		changed = val ^ e->last;
		e->last = val;
		while (changed != 0 && n < (int)nitems(calls)) {
			b = ffs(changed) - 1;
			changed &= ~(1U << b);
			if (e->watch[b].cb == NULL)
				continue;
			calls[n].cb = e->watch[b].cb;
			calls[n].arg = e->watch[b].arg;
			calls[n].bit = b;
			calls[n].set = (val & (1U << b)) != 0;
			n++;
		}
	}
	if (ack) {
		sc->ssr_ack = !sc->ssr_ack;
		val = sc->out->flags & ~SMP2P_FLAG_RESTART_ACK;
		if (sc->ssr_ack)
			val |= SMP2P_FLAG_RESTART_ACK;
		sc->out->flags = val;
		smp2p_kick(sc);
	}
out:
	mtx_unlock(&sc->lock);
	for (k = 0; k < n; k++)
		calls[k].cb(calls[k].arg, calls[k].bit, calls[k].set);
}

static int
smp2p_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev) || !ofw_bus_is_compatible(dev, "qcom,smp2p"))
		return (ENXIO);
	device_set_desc(dev, "Qualcomm SMP2P");
	return (BUS_PROBE_DEFAULT);
}

/* The remote's item's version, 0 without one (Linux's qcom_smp2p_in_version) */
static u_int
smp2p_in_version(struct smp2p_softc *sc)
{
	struct smp2p_item *in;
	int error;

	in = qcom_smem_get(sc->remote_pid, sc->item[0], NULL, &error);
	return (in != NULL ? ITEM_VERSION(in) : 0);
}

/*
 * Our item. Linux's qcom_smp2p_alloc_outbound_item; with the remote up
 * (its item there, with a version) and ours valid, ours stays untouched
 * (sc->kept).
 */
static int
smp2p_alloc_out(struct smp2p_softc *sc)
{
	struct smp2p_item *out;
	u_int in_version;
	int error;

	error = qcom_smem_alloc(sc->remote_pid, sc->item[1], sizeof(*out));
	if (error != 0 && error != EEXIST)
		return (error);
	out = qcom_smem_get(sc->remote_pid, sc->item[1], NULL, &error);
	if (out == NULL)
		return (error);
	in_version = smp2p_in_version(sc);
	if (in_version > SMP2P_VERSION) {
		device_printf(sc->dev, "remote's version %u, at most %u known\n",
		    in_version, SMP2P_VERSION);
		return (EINVAL);
	}
	if (in_version != 0 && out->magic == SMP2P_MAGIC &&
	    ITEM_VERSION(out) != 0 && out->valid_entries <= SMP2P_MAX_ENTRY) {
		/* the remote runs: what it reads from us stays as it is */
		sc->kept = true;
		sc->version = ITEM_VERSION(out);
		sc->ssr_ack = (out->flags & SMP2P_FLAG_RESTART_ACK) != 0;
		sc->out = out;
		return (0);
	}
	memset(out, 0, sizeof(*out));
	out->magic = SMP2P_MAGIC;
	out->local_pid = sc->local_pid;
	out->remote_pid = sc->remote_pid;
	out->total_entries = SMP2P_MAX_ENTRY;
	out->valid_entries = 0;
	/* the header before the version that validates it */
	wmb();
	sc->version = in_version != 0 ? in_version : SMP2P_VERSION;
	out->version_features = sc->version | (SMP2P_FEATURE_SSR_ACK << 8);
	sc->out = out;
	smp2p_kick(sc);
	return (0);
}

/* An entry of ours: the one of its name in a kept item, else a new one */
static volatile uint32_t *
smp2p_out_entry(struct smp2p_softc *sc, const char *name, bool *added)
{
	struct smp2p_item *out = sc->out;
	int k;

	if (sc->kept)
		for (k = 0; k < out->valid_entries; k++)
			if (strncmp(out->entries[k].name, name,
			    SMP2P_NAME_LEN) == 0)
				return (&out->entries[k].value);
	if (out->valid_entries == SMP2P_MAX_ENTRY)
		return (NULL);
	memcpy(out->entries[out->valid_entries].name, name, SMP2P_NAME_LEN);
	*added = true;
	return (&out->entries[out->valid_entries++].value);
}

static int
smp2p_attach(device_t dev)
{
	struct smp2p_softc *sc = device_get_softc(dev);
	struct smp2p_entry *e;
	phandle_t node, child;
	pcell_t items[2];
	bool added = false;
	int rid, error;

	sc->dev = dev;
	node = ofw_bus_get_node(dev);
	mtx_init(&sc->lock, device_get_nameunit(dev), NULL, MTX_DEF);
	if (OF_getencprop(node, "qcom,smem", items, sizeof(items)) !=
	    sizeof(items) ||
	    OF_getencprop(node, "qcom,local-pid", &sc->local_pid,
	    sizeof(sc->local_pid)) <= 0 ||
	    OF_getencprop(node, "qcom,remote-pid", &sc->remote_pid,
	    sizeof(sc->remote_pid)) <= 0) {
		device_printf(dev, "incomplete device tree node\n");
		return (ENXIO);
	}
	/* qcom,smem = <inbound outbound> */
	sc->item[0] = items[0];		/* the remote's */
	sc->item[1] = items[1];		/* ours */
	if ((error = qcom_ipc_get(node, 0, &sc->ipc_xref, &sc->ipc_bit)) != 0) {
		device_printf(dev, "no doorbell (mboxes)\n");
		return (error);
	}
	if ((error = qcom_smem_init()) != 0) {
		device_printf(dev, "no SMEM: %d\n", error);
		return (error);
	}
	if ((error = smp2p_alloc_out(sc)) != 0) {
		device_printf(dev, "SMEM item %u: %d\n", sc->item[1], error);
		return (error);
	}

	for (child = OF_child(node); child != 0; child = OF_peer(child)) {
		if (sc->nentries == SMP2P_MAX_ENTRY)
			break;
		e = malloc(sizeof(*e), M_SMP2P, M_WAITOK | M_ZERO);
		if (OF_getprop(child, "qcom,entry-name", e->name,
		    sizeof(e->name) - 1) <= 0) {
			free(e, M_SMP2P);
			continue;
		}
		e->sc = sc;
		e->xref = OF_xref_from_node(child);
		e->inbound = OF_hasprop(child, "interrupt-controller");
		if (!e->inbound &&
		    (e->value = smp2p_out_entry(sc, e->name, &added)) == NULL) {
			free(e, M_SMP2P);
			continue;
		}
		sc->entries[sc->nentries++] = e;
		mtx_lock(&smp2p_list_lock);
		TAILQ_INSERT_TAIL(&smp2p_all, e, link);
		mtx_unlock(&smp2p_list_lock);
	}
	mtx_lock(&sc->lock);
	smp2p_match_in(sc, true);
	mtx_unlock(&sc->lock);
	/* (a kept item unchanged: the remote has nothing to look at) */
	if (!sc->kept || added)
		smp2p_kick(sc);
	mtx_lock(&smp2p_list_lock);
	TAILQ_INSERT_TAIL(&smp2p_devs, sc, link);
	mtx_unlock(&smp2p_list_lock);

	rid = 0;
	sc->irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid, RF_ACTIVE);
	if (sc->irq == NULL) {
		device_printf(dev, "no interrupt\n");
		return (ENXIO);
	}
	error = bus_setup_intr(dev, sc->irq, INTR_TYPE_MISC | INTR_MPSAFE, NULL,
	    smp2p_intr, sc, &sc->ih);
	if (error != 0)
		return (error);
	device_printf(dev, "pid %u <-> %u, items %u/%u, %d entries%s%s\n",
	    sc->local_pid, sc->remote_pid, sc->item[1], sc->item[0],
	    sc->nentries, sc->in != NULL ? ", remote's item there" : "",
	    sc->kept ? ", remote running: our values kept" : "");
	return (0);
}

static int
smp2p_detach(device_t dev)
{
	return (EBUSY);
}

/* --- for the users ------------------------------------------------------------- */

int
qcom_smp2p_in_watch(phandle_t xref, u_int bit, qcom_smp2p_cb_t *cb, void *arg)
{
	struct smp2p_entry *e;

	if (bit >= 32 || (e = entry_find(xref)) == NULL || !e->inbound)
		return (ENXIO);
	mtx_lock(&e->sc->lock);
	e->watch[bit].arg = arg;
	e->watch[bit].cb = cb;
	mtx_unlock(&e->sc->lock);
	return (0);
}

void
qcom_smp2p_in_unwatch(phandle_t xref, u_int bit)
{
	struct smp2p_entry *e;

	if (bit >= 32 || (e = entry_find(xref)) == NULL)
		return;
	mtx_lock(&e->sc->lock);
	e->watch[bit].cb = NULL;
	mtx_unlock(&e->sc->lock);
}

int
qcom_smp2p_in_read(phandle_t xref, uint32_t *value)
{
	struct smp2p_entry *e;
	int error = 0;

	if ((e = entry_find(xref)) == NULL || !e->inbound)
		return (ENXIO);
	mtx_lock(&e->sc->lock);
	smp2p_match_in(e->sc, false);
	if (e->value != NULL)
		*value = *e->value;
	else
		error = ENOENT;
	mtx_unlock(&e->sc->lock);
	return (error);
}

int
qcom_smp2p_out_update(phandle_t xref, uint32_t mask, uint32_t value)
{
	struct smp2p_entry *e;
	uint32_t old, val;

	if ((e = entry_find(xref)) == NULL || e->inbound)
		return (ENXIO);
	mtx_lock(&e->sc->lock);
	old = *e->value;
	val = (old & ~mask) | (value & mask);
	*e->value = val;
	mtx_unlock(&e->sc->lock);
	if (val != old)
		smp2p_kick(e->sc);
	return (0);
}

/* For the test runs: each edge's state (hw.qcom_smp2p, qcom_smp2p_report) */
static void
smp2p_report_sbuf(struct sbuf *sb)
{
	struct smp2p_softc *sc;
	struct smp2p_entry *e;
	int k;

	mtx_lock(&smp2p_list_lock);
	TAILQ_FOREACH(sc, &smp2p_devs, link) {
		mtx_lock(&sc->lock);
		smp2p_match_in(sc, false);
		sbuf_printf(sb, "%s: pid %u<->%u items out %u in %u: remote's "
		    "item %s, version %u, %s, %u interrupts%s\n",
		    device_get_nameunit(sc->dev), sc->local_pid, sc->remote_pid,
		    sc->item[1], sc->item[0],
		    sc->in == NULL ? "absent" : ITEM_VERSION(sc->in) == 0 ?
		    "not valid yet" : "valid",
		    sc->in != NULL ? ITEM_VERSION(sc->in) : 0,
		    sc->negotiated ? "negotiated" : "not negotiated",
		    sc->nintr, sc->kept ? ", values kept from before" : "");
		for (k = 0; k < sc->nentries; k++) {
			e = sc->entries[k];
			if (e->value != NULL)
				sbuf_printf(sb, "  %s %-16s 0x%08x\n",
				    e->inbound ? "in " : "out", e->name, *e->value);
			else
				sbuf_printf(sb, "  %s %-16s (not there)\n",
				    e->inbound ? "in " : "out", e->name);
		}
		mtx_unlock(&sc->lock);
	}
	mtx_unlock(&smp2p_list_lock);
}

void
qcom_smp2p_report(void)
{
	struct sbuf *sb;

	sb = sbuf_new_auto();
	smp2p_report_sbuf(sb);
	sbuf_finish(sb);
	printf("%s", sbuf_data(sb));
	sbuf_delete(sb);
}

/* rendered with the locks held, copied out without them */
static int
smp2p_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct sbuf *sb;
	int error;

	sb = sbuf_new_auto();
	sbuf_printf(sb, "\n");
	smp2p_report_sbuf(sb);
	if ((error = sbuf_finish(sb)) == 0)
		error = SYSCTL_OUT(req, sbuf_data(sb), sbuf_len(sb) + 1);
	sbuf_delete(sb);
	return (error);
}

SYSCTL_PROC(_hw, OID_AUTO, qcom_smp2p, CTLTYPE_STRING | CTLFLAG_RD |
    CTLFLAG_MPSAFE, NULL, 0, smp2p_sysctl, "A",
    "SMP2P edges: items, entries and their values");

static device_method_t smp2p_methods[] = {
	DEVMETHOD(device_probe,		smp2p_probe),
	DEVMETHOD(device_attach,	smp2p_attach),
	DEVMETHOD(device_detach,	smp2p_detach),
	DEVMETHOD_END
};

static driver_t smp2p_driver = {
	"qcom_smp2p",
	smp2p_methods,
	sizeof(struct smp2p_softc),
};

DRIVER_MODULE(qcom_smp2p, ofwbus, smp2p_driver, 0, 0);
DRIVER_MODULE(qcom_smp2p, simplebus, smp2p_driver, 0, 0);
MODULE_VERSION(qcom_smp2p, 1);
