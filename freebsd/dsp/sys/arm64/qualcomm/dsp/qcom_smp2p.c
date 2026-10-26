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
	struct smp2p_entry *entries[SMP2P_MAX_ENTRY];
	int		nentries;
};

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

/* The remote's item, and our inbound entries' values in it */
static void
smp2p_match_in(struct smp2p_softc *sc)
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
				e->last = *e->value;
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
	smp2p_match_in(sc);
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

static int
smp2p_alloc_out(struct smp2p_softc *sc)
{
	struct smp2p_item *out;
	int error;

	error = qcom_smem_alloc(sc->remote_pid, sc->item[1], sizeof(*out));
	if (error != 0 && error != EEXIST)
		return (error);
	out = qcom_smem_get(sc->remote_pid, sc->item[1], NULL, &error);
	if (out == NULL)
		return (error);
	memset(out, 0, sizeof(*out));
	out->magic = SMP2P_MAGIC;
	out->local_pid = sc->local_pid;
	out->remote_pid = sc->remote_pid;
	out->total_entries = SMP2P_MAX_ENTRY;
	out->valid_entries = 0;
	/* the header before the version that validates it */
	wmb();
	out->version_features = SMP2P_VERSION | (SMP2P_FEATURE_SSR_ACK << 8);
	sc->out = out;
	smp2p_kick(sc);
	return (0);
}

static int
smp2p_attach(device_t dev)
{
	struct smp2p_softc *sc = device_get_softc(dev);
	struct smp2p_entry *e;
	phandle_t node, child;
	pcell_t items[2];
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
		if (!e->inbound) {
			memcpy(sc->out->entries[sc->out->valid_entries].name,
			    e->name, SMP2P_NAME_LEN);
			e->value = &sc->out->entries[sc->out->valid_entries].value;
			sc->out->valid_entries++;
		}
		sc->entries[sc->nentries++] = e;
		mtx_lock(&smp2p_list_lock);
		TAILQ_INSERT_TAIL(&smp2p_all, e, link);
		mtx_unlock(&smp2p_list_lock);
	}
	mtx_lock(&sc->lock);
	smp2p_match_in(sc);
	mtx_unlock(&sc->lock);
	smp2p_kick(sc);

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
	device_printf(dev, "pid %u <-> %u, items %u/%u, %d entries%s\n",
	    sc->local_pid, sc->remote_pid, sc->item[1], sc->item[0],
	    sc->nentries, sc->in != NULL ? ", remote up" : "");
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
	smp2p_match_in(e->sc);
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
