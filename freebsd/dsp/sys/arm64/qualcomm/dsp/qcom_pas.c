/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Qualcomm remote processors through TrustZone's PAS, after Linux's
 * qcom_q6v5_pas.c and qcom_q6v5.c: the firmware's metadata (ELF header and
 * hash segment) goes to TrustZone, its segments into the processor's
 * reserved memory, then TrustZone checks the image and releases the
 * processor from reset. The processor reports over SMP2P: ready, handover
 * (the boot votes may go), fatal error, stop acknowledged; its watchdog
 * interrupt comes from the GIC.
 *
 * The firmware itself is read by a loader in userspace (pro1x-dsp), which
 * parses it as Linux's mdt_loader does and hands this driver the pieces
 * through /dev/qcom_pas/<name> (qcom_pas_ioctl.h): the modem's image alone
 * is 51 MB. Linux also votes the processor's power domains and clocks
 * (rpmpd, xo) until the handover; FreeBSD has no RPM driver and relies on
 * what the boot left voted (freebsd/dsp/README.md, risks).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/sx.h>
#include <sys/taskqueue.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "qcom_dsp.h"
#include "qcom_pas_ioctl.h"

#define	META_MAX		(1024 * 1024)
#define	STOP_TIMEOUT_MS		5000

struct pas_data {
	const char	*name;
	uint32_t	pas_id;
	u_int		crash_reason_item;
};

static const struct pas_data adsp_data = { "adsp", 1, 423 };
static const struct pas_data cdsp_data = { "cdsp", 18, 601 };
static const struct pas_data mpss_data = { "modem", 4, 421 };

static struct ofw_compat_data compat_data[] = {
	{ "qcom,sm6115-adsp-pas",	(uintptr_t)&adsp_data },
	{ "qcom,sm6115-cdsp-pas",	(uintptr_t)&cdsp_data },
	{ "qcom,sm6115-mpss-pas",	(uintptr_t)&mpss_data },
	{ NULL,				0 }
};

/* The SMP2P bits of interrupts-extended, by name */
enum { IN_FATAL, IN_READY, IN_HANDOVER, IN_STOP_ACK, IN_COUNT };
static const char *in_names[IN_COUNT] = { "fatal", "ready", "handover",
    "stop-ack" };
static const uint32_t in_flags[IN_COUNT] = { QCOM_PAS_F_FATAL,
    QCOM_PAS_F_READY, QCOM_PAS_F_HANDOVER, QCOM_PAS_F_STOP_ACK };

struct pas_softc {
	device_t	dev;
	const struct pas_data *data;
	struct sx	sx;		/* one user operation at a time */
	struct mtx	mtx;		/* state, flags */
	struct cdev	*cdev;
	vm_paddr_t	mem_phys;
	vm_size_t	mem_size;
	char		*mem;		/* mapped write-combining */
	struct resource	*wdog;
	void		*wdog_ih;
	struct {
		phandle_t	xref;
		u_int		bit;
	} in[IN_COUNT];
	phandle_t	stop_xref;
	u_int		stop_bit;
	void		*meta;		/* until the start, for TrustZone */
	vm_size_t	meta_size;
	uint32_t	state;
	uint32_t	flags;
	char		crash_reason[128];
	void		*glink;		/* the GLINK edge while running */
	struct task	crash_task;
};

static d_ioctl_t pas_ioctl;
static int pas_watch(struct pas_softc *, bool);

static struct cdevsw pas_cdevsw = {
	.d_version =	D_VERSION,
	.d_ioctl =	pas_ioctl,
	.d_name =	"qcom_pas",
};

/* --- events from the processor ------------------------------------------------ */

static void
pas_crash_reason(struct pas_softc *sc)
{
	const char *msg;
	size_t len;
	int error;

	msg = qcom_smem_get(QCOM_SMEM_HOST_ANY, sc->data->crash_reason_item,
	    &len, &error);
	if (msg == NULL || len == 0)
		return;
	len = MIN(len, sizeof(sc->crash_reason) - 1);
	memcpy(sc->crash_reason, msg, len);
	sc->crash_reason[len] = '\0';
}

static void
pas_in_cb(void *arg, u_int bit, bool set)
{
	struct pas_softc *sc = arg;
	int i;

	if (!set)
		return;
	mtx_lock(&sc->mtx);
	for (i = 0; i < IN_COUNT; i++) {
		if (sc->in[i].xref == 0 || sc->in[i].bit != bit)
			continue;
		sc->flags |= in_flags[i];
		if (i == IN_FATAL) {
			sc->state = QCOM_PAS_STATE_CRASHED;
			pas_crash_reason(sc);
			device_printf(sc->dev, "fatal error: %s\n",
			    sc->crash_reason);
			taskqueue_enqueue(taskqueue_thread, &sc->crash_task);
		}
		if (i == IN_READY && sc->state == QCOM_PAS_STATE_LOADING)
			sc->state = QCOM_PAS_STATE_RUNNING;
	}
	wakeup(sc);
	mtx_unlock(&sc->mtx);
}

static void
pas_wdog_intr(void *arg)
{
	struct pas_softc *sc = arg;

	mtx_lock(&sc->mtx);
	sc->flags |= QCOM_PAS_F_WDOG;
	if (sc->state != QCOM_PAS_STATE_OFF) {
		sc->state = QCOM_PAS_STATE_CRASHED;
		pas_crash_reason(sc);
		device_printf(sc->dev, "watchdog: %s\n", sc->crash_reason);
		taskqueue_enqueue(taskqueue_thread, &sc->crash_task);
	}
	wakeup(sc);
	mtx_unlock(&sc->mtx);
}

/* After a crash: its GLINK edge goes (the users hear it), in a task */
static void
pas_crash_task(void *arg, int pending)
{
	struct pas_softc *sc = arg;

	sx_xlock(&sc->sx);
	if (sc->glink != NULL) {
		qcom_glink_edge_stop(sc->glink);
		sc->glink = NULL;
	}
	sx_xunlock(&sc->sx);
}

/* Wait for one of the flags, or a crash; EWOULDBLOCK on timeout */
static int
pas_wait(struct pas_softc *sc, uint32_t flags, u_int ms)
{
	sbintime_t end;
	int error = 0;

	end = getsbinuptime() + ms * SBT_1MS;
	mtx_lock(&sc->mtx);
	while ((sc->flags & flags) == 0 && sc->state != QCOM_PAS_STATE_CRASHED) {
		if (getsbinuptime() >= end) {
			error = EWOULDBLOCK;
			break;
		}
		msleep_sbt(sc, &sc->mtx, 0, "qcompas", 10 * SBT_1MS, 0, 0);
	}
	if (error == 0 && sc->state == QCOM_PAS_STATE_CRASHED &&
	    (sc->flags & flags) == 0)
		error = EIO;
	mtx_unlock(&sc->mtx);
	return (error);
}

/* --- operations -------------------------------------------------------------------- */

static void
pas_meta_free(struct pas_softc *sc)
{
	if (sc->meta != NULL) {
		kmem_free(sc->meta, sc->meta_size);
		sc->meta = NULL;
	}
}

static int
pas_init(struct pas_softc *sc, struct qcom_pas_init *in)
{
	int error;

	if (in->meta_len == 0 || in->meta_len > META_MAX)
		return (EINVAL);
	if (sc->state == QCOM_PAS_STATE_RUNNING)
		return (EBUSY);
	pas_meta_free(sc);
	sc->meta_size = round_page(in->meta_len);
	/* contiguous and uncached: TrustZone reads it by physical address */
	sc->meta = kmem_alloc_contig(sc->meta_size, M_WAITOK | M_ZERO, 0,
	    ~(vm_paddr_t)0, PAGE_SIZE, 0, VM_MEMATTR_UNCACHEABLE);
	if (sc->meta == NULL)
		return (ENOMEM);
	error = copyin((void *)(uintptr_t)in->meta, sc->meta, in->meta_len);
	if (error == 0)
		error = qcom_scm_pas_init_image(sc->data->pas_id,
		    pmap_kextract((vm_offset_t)sc->meta));
	if (error != 0) {
		device_printf(sc->dev, "PAS init image: %d\n", error);
		pas_meta_free(sc);
		return (error);
	}
	mtx_lock(&sc->mtx);
	sc->state = QCOM_PAS_STATE_LOADING;
	sc->flags = 0;
	sc->crash_reason[0] = '\0';
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
pas_load(struct pas_softc *sc, struct qcom_pas_segment *seg)
{
	int error;

	if (sc->state != QCOM_PAS_STATE_LOADING)
		return (EINVAL);
	if (seg->filesz > seg->memsz || seg->offset > sc->mem_size ||
	    seg->memsz > sc->mem_size - seg->offset)
		return (EINVAL);
	error = copyin((void *)(uintptr_t)seg->data, sc->mem + seg->offset,
	    seg->filesz);
	if (error != 0)
		return (error);
	if (seg->memsz > seg->filesz)
		memset(sc->mem + seg->offset + seg->filesz, 0,
		    seg->memsz - seg->filesz);
	return (0);
}

static int
pas_start(struct pas_softc *sc, uint32_t timeout_ms)
{
	int error;

	if (sc->state != QCOM_PAS_STATE_LOADING)
		return (EINVAL);
	if ((error = pas_watch(sc, true)) != 0)
		return (error);
	/* the image in memory before TrustZone looks at it */
	wmb();
	error = qcom_scm_pas_auth_and_reset(sc->data->pas_id);
	if (error != 0) {
		device_printf(sc->dev, "PAS auth and reset: %d\n", error);
		pas_meta_free(sc);
		mtx_lock(&sc->mtx);
		sc->state = QCOM_PAS_STATE_OFF;
		mtx_unlock(&sc->mtx);
		return (error);
	}
	error = pas_wait(sc, QCOM_PAS_F_READY, MAX(timeout_ms, 100));
	pas_meta_free(sc);
	if (error != 0) {
		device_printf(sc->dev, "no ready (%d), shutting it down\n",
		    error);
		qcom_scm_pas_shutdown(sc->data->pas_id);
		mtx_lock(&sc->mtx);
		if (sc->state != QCOM_PAS_STATE_CRASHED)
			sc->state = QCOM_PAS_STATE_OFF;
		mtx_unlock(&sc->mtx);
		return (error == EWOULDBLOCK ? ETIMEDOUT : error);
	}
	device_printf(sc->dev, "running\n");
	/* its channels: GLINK over SMEM, when it has an edge */
	if (qcom_glink_edge_start(sc->dev, ofw_bus_get_node(sc->dev),
	    &sc->glink) != 0)
		sc->glink = NULL;
	return (0);
}

static int
pas_stop(struct pas_softc *sc)
{
	int error;

	if (sc->state == QCOM_PAS_STATE_OFF)
		return (0);
	if (sc->glink != NULL) {
		qcom_glink_edge_stop(sc->glink);
		sc->glink = NULL;
	}
	if (sc->stop_xref != 0 && sc->state == QCOM_PAS_STATE_RUNNING) {
		qcom_smp2p_out_update(sc->stop_xref, 1U << sc->stop_bit,
		    1U << sc->stop_bit);
		if (pas_wait(sc, QCOM_PAS_F_STOP_ACK, STOP_TIMEOUT_MS) != 0)
			device_printf(sc->dev, "no stop acknowledgement\n");
		qcom_smp2p_out_update(sc->stop_xref, 1U << sc->stop_bit, 0);
	}
	error = qcom_scm_pas_shutdown(sc->data->pas_id);
	if (error != 0)
		device_printf(sc->dev, "PAS shutdown: %d\n", error);
	mtx_lock(&sc->mtx);
	sc->state = QCOM_PAS_STATE_OFF;
	mtx_unlock(&sc->mtx);
	return (error);
}

static int
pas_ioctl(struct cdev *cdev, u_long cmd, caddr_t data, int fflag,
    struct thread *td)
{
	struct pas_softc *sc = cdev->si_drv1;
	struct qcom_pas_status *st;
	uint32_t v;
	int error;

	sx_xlock(&sc->sx);
	switch (cmd) {
	case QCOM_PAS_INIT:
		error = pas_init(sc, (struct qcom_pas_init *)data);
		break;
	case QCOM_PAS_MEM_SETUP:
		if (sc->state != QCOM_PAS_STATE_LOADING ||
		    *(uint64_t *)data > sc->mem_size) {
			error = EINVAL;
			break;
		}
		error = qcom_scm_pas_mem_setup(sc->data->pas_id, sc->mem_phys,
		    *(uint64_t *)data);
		if (error != 0)
			device_printf(sc->dev, "PAS mem setup: %d\n", error);
		break;
	case QCOM_PAS_LOAD:
		error = pas_load(sc, (struct qcom_pas_segment *)data);
		break;
	case QCOM_PAS_START:
		error = pas_start(sc, *(uint32_t *)data);
		break;
	case QCOM_PAS_STOP:
		error = pas_stop(sc);
		break;
	case QCOM_PAS_SHUTDOWN:
		error = qcom_scm_pas_shutdown(sc->data->pas_id);
		break;
	case QCOM_PAS_STATUS:
		st = (struct qcom_pas_status *)data;
		memset(st, 0, sizeof(*st));
		st->pas_id = sc->data->pas_id;
		st->mem_phys = sc->mem_phys;
		st->mem_size = sc->mem_size;
		if (sc->in[IN_READY].xref != 0 &&
		    qcom_smp2p_in_read(sc->in[IN_READY].xref, &v) == 0)
			st->smp2p_in = v;
		mtx_lock(&sc->mtx);
		st->state = sc->state;
		st->flags = sc->flags;
		strlcpy(st->crash_reason, sc->crash_reason,
		    sizeof(st->crash_reason));
		mtx_unlock(&sc->mtx);
		error = 0;
		break;
	default:
		error = ENOTTY;
	}
	sx_xunlock(&sc->sx);
	return (error);
}

/* --- attachment ------------------------------------------------------------------- */

/* interrupts-extended entries with their names: the SMP2P ones */
static void
pas_parse_interrupts(struct pas_softc *sc, phandle_t node)
{
	int index[IN_COUNT];
	pcell_t *cells;
	phandle_t parent;
	pcell_t icells;
	ssize_t len, n;
	int entry, i;

	for (i = 0; i < IN_COUNT; i++)
		if (ofw_bus_find_string_index(node, "interrupt-names",
		    in_names[i], &index[i]) != 0)
			index[i] = -1;
	len = OF_getencprop_alloc_multi(node, "interrupts-extended",
	    sizeof(pcell_t), (void **)&cells);
	if (len <= 0)
		return;
	for (n = 0, entry = 0; n < len; entry++) {
		parent = OF_node_from_xref(cells[n]);
		if (OF_getencprop(parent, "#interrupt-cells", &icells,
		    sizeof(icells)) <= 0 || n + 1 + icells > len)
			break;
		for (i = 0; i < IN_COUNT; i++)
			if (index[i] == entry) {
				sc->in[i].xref = cells[n];
				sc->in[i].bit = cells[n + 1];
			}
		n += 1 + icells;
	}
	OF_prop_free(cells);
}

/* Callbacks for the SMP2P bits; verbose: report the ones missing */
static int
pas_watch(struct pas_softc *sc, bool verbose)
{
	int i, error, first = 0;

	for (i = 0; i < IN_COUNT; i++) {
		if (sc->in[i].xref == 0)
			continue;
		error = qcom_smp2p_in_watch(sc->in[i].xref, sc->in[i].bit,
		    pas_in_cb, sc);
		if (error != 0 && verbose)
			device_printf(sc->dev, "SMP2P %s: %d\n", in_names[i],
			    error);
		if (error != 0 && first == 0)
			first = error;
	}
	return (first);
}

static int
pas_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev) ||
	    ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);
	device_set_desc(dev, "Qualcomm remote processor (PAS)");
	return (BUS_PROBE_DEFAULT);
}

static int
pas_attach(device_t dev)
{
	struct pas_softc *sc = device_get_softc(dev);
	phandle_t node, mem;
	pcell_t cells[2], reg[4];
	bool supported;
	int rid, error;

	sc->dev = dev;
	sc->data = (const struct pas_data *)
	    ofw_bus_search_compatible(dev, compat_data)->ocd_data;
	node = ofw_bus_get_node(dev);
	sx_init(&sc->sx, "qcom_pas");
	mtx_init(&sc->mtx, device_get_nameunit(dev), NULL, MTX_DEF);
	TASK_INIT(&sc->crash_task, 0, pas_crash_task, sc);

	if (OF_getencprop(node, "memory-region", cells, sizeof(pcell_t)) <= 0) {
		device_printf(dev, "no memory-region\n");
		return (ENXIO);
	}
	mem = OF_node_from_xref(cells[0]);
	if (OF_getencprop(mem, "reg", reg, sizeof(reg)) != sizeof(reg)) {
		device_printf(dev, "memory-region without reg\n");
		return (ENXIO);
	}
	sc->mem_phys = (vm_paddr_t)reg[0] << 32 | reg[1];
	sc->mem_size = (vm_size_t)reg[2] << 32 | reg[3];
	sc->mem = pmap_mapdev_attr(sc->mem_phys, sc->mem_size,
	    VM_MEMATTR_WRITE_COMBINING);

	pas_parse_interrupts(sc, node);
	/* SMP2P may attach after us (module load order): again at the start */
	pas_watch(sc, false);
	if (OF_getencprop(node, "qcom,smem-states", cells, sizeof(cells)) ==
	    sizeof(cells)) {
		sc->stop_xref = cells[0];
		sc->stop_bit = cells[1];
	}

	/* the watchdog: the first interrupt, at the GIC */
	rid = 0;
	sc->wdog = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid, RF_ACTIVE);
	if (sc->wdog != NULL &&
	    bus_setup_intr(dev, sc->wdog, INTR_TYPE_MISC | INTR_MPSAFE, NULL,
	    pas_wdog_intr, sc, &sc->wdog_ih) != 0)
		device_printf(dev, "no watchdog interrupt\n");

	error = qcom_scm_pas_supported(sc->data->pas_id, &supported);
	device_printf(dev, "%s: PAS id %u %s (%d), memory %#jx+%#jx\n",
	    sc->data->name, sc->data->pas_id,
	    supported ? "supported" : "not supported", error,
	    (uintmax_t)sc->mem_phys, (uintmax_t)sc->mem_size);

	sc->cdev = make_dev(&pas_cdevsw, device_get_unit(dev), UID_ROOT,
	    GID_WHEEL, 0600, "qcom_pas/%s", sc->data->name);
	sc->cdev->si_drv1 = sc;
	return (0);
}

static int
pas_detach(device_t dev)
{
	struct pas_softc *sc = device_get_softc(dev);
	int i;

	if (sc->state != QCOM_PAS_STATE_OFF)
		return (EBUSY);
	destroy_dev(sc->cdev);
	for (i = 0; i < IN_COUNT; i++)
		if (sc->in[i].xref != 0)
			qcom_smp2p_in_unwatch(sc->in[i].xref, sc->in[i].bit);
	if (sc->wdog_ih != NULL)
		bus_teardown_intr(dev, sc->wdog, sc->wdog_ih);
	if (sc->wdog != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->wdog);
	if (sc->mem != NULL)
		pmap_unmapdev(sc->mem, sc->mem_size);
	pas_meta_free(sc);
	mtx_destroy(&sc->mtx);
	sx_destroy(&sc->sx);
	return (0);
}

static device_method_t pas_methods[] = {
	DEVMETHOD(device_probe,		pas_probe),
	DEVMETHOD(device_attach,	pas_attach),
	DEVMETHOD(device_detach,	pas_detach),
	DEVMETHOD_END
};

static driver_t pas_driver = {
	"qcom_pas",
	pas_methods,
	sizeof(struct pas_softc),
};

DRIVER_MODULE(qcom_pas, simplebus, pas_driver, 0, 0);
MODULE_VERSION(qcom_pas, 1);
