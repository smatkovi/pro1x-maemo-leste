/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * qcom_swr.ko: the interrupt lines of the LPASS SoundWire masters
 * ("qcom,soundwire-v1.6.0", Linux's drivers/soundwire/qcom.c) for
 * pro1x-audiod, which drives the masters from user space (swr.c through
 * /dev/qcom_lpass). The interface is qcom_swr.h's.
 *
 * Linux's master driver takes two interrupts from the device tree: the
 * master's (index 0; "soundwire": command completion, slave status
 * changes, a slave's alert) and, where there is one, the wake-up
 * interrupt (index 1; "swr_wake_irq", swr0 only on the SM6115: SPI 79),
 * which the LPASS raises when a slave pulls the data line while the bus'
 * clock is stopped. Linux disables the wake-up interrupt in its handler
 * and enables it again at the next runtime suspend; the master's
 * interrupt it handles in a thread with the master clocked (hclk on).
 *
 * Here neither is ever handled in the kernel: the filter masks the
 * interrupt in the GIC (PIC_DISABLE_INTR) and returns, a task wakes the
 * reader. The line may stay asserted (the master's interrupt in level
 * mode, which swr.c sets in its place of Linux's pulse; the wake-up
 * interrupt is level high in the device tree) until the daemon, with the
 * master clocked, has cleared its status; then it arms the interrupt
 * again. Nothing is armed at attach (whatever Linux left asserted cannot
 * storm the boot) and nothing is armed while the device is closed.
 *
 * Both lines are MPM pins (the device tree's qcom,mpm-pin-map: <5 296>,
 * <24 79>): pro1x_pm's hand-over enables an MPM pin while its SPI is
 * enabled in the GIC (hw.pro1x_pm.mpm_spi), so an armed interrupt wakes
 * the SoC from vlow/vmin as well.
 *
 *   sysctl dev.qcom_swr.N.stats    the interrupts, how often each fired,
 *                                  armed or not
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/interrupt.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/rman.h>
#include <sys/sbuf.h>
#include <sys/selinfo.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/uio.h>

#include <machine/bus.h>
#include <machine/intr.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_subr.h>
#include <dev/ofw/openfirm.h>

#include "pic_if.h"

#include "qcom_swr.h"

static struct ofw_compat_data qswr_compat[] = {
	{ "qcom,soundwire-v1.3.0",	1 },
	{ "qcom,soundwire-v1.5.1",	1 },
	{ "qcom,soundwire-v1.6.0",	1 },
	{ "qcom,soundwire-v1.7.0",	1 },
	{ NULL,				0 }
};

/* the daemon's names: swr.c's swr0 (TX) and swr1 (RX) on the SM6115 */
static const struct {
	bus_addr_t	pa;
	int		unit;
} qswr_names[] = {
	{ 0x0a740000, 0 },
	{ 0x0a610000, 1 },
};

struct qswr_softc;

struct qswr_irq {
	struct qswr_softc *sc;
	struct resource	*res;
	void		*cookie;
	uint32_t	bit;		/* QCOM_SWR_MASTER, QCOM_SWR_WAKE */
	uint32_t	intid;
	uint64_t	count;
};

struct qswr_softc {
	device_t	dev;
	struct mtx	mtx;		/* spin: the filter's */
	struct qswr_irq	irq[2];
	uint32_t	have, armed, fired;
	bus_addr_t	pa;
	struct cdev	*cdev;
	struct selinfo	rsel;
	struct task	task;
	bool		open, gone;
};

static d_open_t		qswr_open;
static d_close_t	qswr_close;
static d_read_t		qswr_read;
static d_ioctl_t	qswr_ioctl;
static d_poll_t		qswr_poll;

static struct cdevsw qswr_cdevsw = {
	.d_version =	D_VERSION,
	.d_flags =	D_TRACKCLOSE,
	.d_open =	qswr_open,
	.d_close =	qswr_close,
	.d_read =	qswr_read,
	.d_ioctl =	qswr_ioctl,
	.d_poll =	qswr_poll,
	.d_name =	"qcom_swr",
};

static void
qswr_mask(struct qswr_irq *q)
{
	struct intr_irqsrc *isrc;

	if (q->cookie == NULL || (isrc = (struct intr_irqsrc *)
	    intr_handler_source(q->cookie)) == NULL)
		return;
	PIC_DISABLE_INTR(isrc->isrc_dev, isrc);
}

static void
qswr_unmask(struct qswr_irq *q)
{
	struct intr_irqsrc *isrc;

	if (q->cookie == NULL || (isrc = (struct intr_irqsrc *)
	    intr_handler_source(q->cookie)) == NULL)
		return;
	PIC_ENABLE_INTR(isrc->isrc_dev, isrc);
}

/*
 * The filter: masked at once (the GIC's EOI follows for a level
 * interrupt; masked, it does not come again), noted, the reader woken
 * from a task (selwakeup cannot run here).
 */
static int
qswr_filter(void *arg)
{
	struct qswr_irq *q = arg;
	struct qswr_softc *sc = q->sc;

	qswr_mask(q);
	mtx_lock_spin(&sc->mtx);
	sc->armed &= ~q->bit;
	sc->fired |= q->bit;
	q->count++;
	mtx_unlock_spin(&sc->mtx);
	taskqueue_enqueue(taskqueue_fast, &sc->task);
	return (FILTER_HANDLED);
}

static void
qswr_task(void *arg, int pending __unused)
{
	struct qswr_softc *sc = arg;

	selwakeup(&sc->rsel);
	wakeup(sc);
}

static void
qswr_set(struct qswr_softc *sc, uint32_t bits, bool arm)
{
	int i;

	for (i = 0; i < 2; i++) {
		struct qswr_irq *q = &sc->irq[i];

		if (q->res == NULL || (bits & q->bit) == 0)
			continue;
		mtx_lock_spin(&sc->mtx);
		if (arm) {
			sc->armed |= q->bit;
			mtx_unlock_spin(&sc->mtx);
			qswr_unmask(q);
		} else {
			sc->armed &= ~q->bit;
			mtx_unlock_spin(&sc->mtx);
			qswr_mask(q);
		}
	}
}

static int
qswr_open(struct cdev *cdev, int oflags __unused, int devtype __unused,
    struct thread *td __unused)
{
	struct qswr_softc *sc = cdev->si_drv1;

	mtx_lock_spin(&sc->mtx);
	if (sc->open || sc->gone) {
		mtx_unlock_spin(&sc->mtx);
		return (EBUSY);
	}
	sc->open = true;
	sc->fired = 0;
	mtx_unlock_spin(&sc->mtx);
	return (0);
}

static int
qswr_close(struct cdev *cdev, int fflag __unused, int devtype __unused,
    struct thread *td __unused)
{
	struct qswr_softc *sc = cdev->si_drv1;

	qswr_set(sc, QCOM_SWR_MASTER | QCOM_SWR_WAKE, false);
	mtx_lock_spin(&sc->mtx);
	sc->open = false;
	sc->fired = 0;
	mtx_unlock_spin(&sc->mtx);
	return (0);
}

static int
qswr_read(struct cdev *cdev, struct uio *uio, int ioflag)
{
	struct qswr_softc *sc = cdev->si_drv1;
	struct qcom_swr_event ev;
	int error;

	if (uio->uio_resid < sizeof(ev))
		return (EINVAL);
	for (;;) {
		mtx_lock_spin(&sc->mtx);
		if (sc->fired != 0 || sc->gone)
			break;
		mtx_unlock_spin(&sc->mtx);
		if (ioflag & O_NONBLOCK)
			return (EWOULDBLOCK);
		/* a wake-up between the look and the sleep: 100 ms late */
		error = tsleep(sc, PCATCH, "swrirq", hz / 10 + 1);
		if (error != 0 && error != EWOULDBLOCK)
			return (error);
	}
	memset(&ev, 0, sizeof(ev));
	ev.fired = sc->fired;
	ev.armed = sc->armed;
	ev.count[0] = sc->irq[0].count;
	ev.count[1] = sc->irq[1].count;
	sc->fired = 0;
	mtx_unlock_spin(&sc->mtx);
	if (sc->gone)
		return (ENXIO);
	return (uiomove(&ev, sizeof(ev), uio));
}

static int
qswr_poll(struct cdev *cdev, int events, struct thread *td)
{
	struct qswr_softc *sc = cdev->si_drv1;
	int revents = 0;

	if ((events & (POLLIN | POLLRDNORM)) == 0)
		return (0);
	mtx_lock_spin(&sc->mtx);
	if (sc->fired != 0 || sc->gone)
		revents = events & (POLLIN | POLLRDNORM);
	mtx_unlock_spin(&sc->mtx);
	if (revents == 0) {
		selrecord(td, &sc->rsel);
		/* fired between the look and selrecord: look again */
		mtx_lock_spin(&sc->mtx);
		if (sc->fired != 0 || sc->gone)
			revents = events & (POLLIN | POLLRDNORM);
		mtx_unlock_spin(&sc->mtx);
	}
	return (revents);
}

static int
qswr_ioctl(struct cdev *cdev, u_long cmd, caddr_t data, int fflag __unused,
    struct thread *td __unused)
{
	struct qswr_softc *sc = cdev->si_drv1;
	struct qcom_swr_info *info;
	uint32_t bits;

	switch (cmd) {
	case QCOM_SWR_ARM:
	case QCOM_SWR_DISARM:
		bits = *(uint32_t *)data;
		if ((bits & ~sc->have) != 0)
			return (ENXIO);
		qswr_set(sc, bits, cmd == QCOM_SWR_ARM);
		return (0);
	case QCOM_SWR_INFO:
		info = (struct qcom_swr_info *)data;
		memset(info, 0, sizeof(*info));
		info->have = sc->have;
		info->intid[0] = sc->irq[0].intid;
		info->intid[1] = sc->irq[1].intid;
		info->pa = sc->pa;
		return (0);
	default:
		return (ENOTTY);
	}
}

static int
qswr_sysctl_stats(SYSCTL_HANDLER_ARGS)
{
	struct qswr_softc *sc = arg1;
	struct sbuf sb;
	int i, error;
	static const char *names[2] = { "master", "wake-up" };

	sbuf_new_for_sysctl(&sb, NULL, 160, req);
	for (i = 0; i < 2; i++) {
		if (sc->irq[i].res == NULL)
			continue;
		sbuf_printf(&sb, "%s%s INTID %u: %ju, %s", i ? "; " : "",
		    names[i], sc->irq[i].intid, (uintmax_t)sc->irq[i].count,
		    (sc->armed & sc->irq[i].bit) ? "armed" : "masked");
	}
	sbuf_printf(&sb, "; %s", sc->open ? "open" : "closed");
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

static int
qswr_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (ofw_bus_search_compatible(dev, qswr_compat)->ocd_data == 0)
		return (ENXIO);
	device_set_desc(dev, "Qualcomm SoundWire master's interrupts");
	return (BUS_PROBE_DEFAULT);
}

/* the INTIDs from the node's interrupts (GIC cells: SPI, number, flags) */
static void
qswr_intids(struct qswr_softc *sc, phandle_t node)
{
	pcell_t cells[6];
	ssize_t n;
	int i;

	n = OF_getencprop(node, "interrupts", cells, sizeof(cells));
	for (i = 0; i < 2 && n >= (ssize_t)(3 * (i + 1) * sizeof(pcell_t));
	    i++)
		sc->irq[i].intid = cells[3 * i + 1] +
		    (cells[3 * i] == 0 ? 32 : 16);
}

static int
qswr_attach(device_t dev)
{
	struct qswr_softc *sc = device_get_softc(dev);
	struct make_dev_args args;
	phandle_t node = ofw_bus_get_node(dev);
	bus_size_t size;
	int i, rid, unit = -1, error;
	u_int j;

	sc->dev = dev;
	mtx_init(&sc->mtx, "qcom_swr", NULL, MTX_SPIN);
	TASK_INIT(&sc->task, 0, qswr_task, sc);
	knlist_init_mtx(&sc->rsel.si_note, NULL);
	if (ofw_reg_to_paddr(node, 0, &sc->pa, &size, NULL) != 0)
		sc->pa = 0;
	for (j = 0; j < nitems(qswr_names); j++)
		if (qswr_names[j].pa == sc->pa)
			unit = qswr_names[j].unit;
	if (unit < 0)
		unit = 2 + device_get_unit(dev);
	qswr_intids(sc, node);
	for (i = 0; i < 2; i++) {
		struct qswr_irq *q = &sc->irq[i];

		q->sc = sc;
		q->bit = i == 0 ? QCOM_SWR_MASTER : QCOM_SWR_WAKE;
		rid = i;
		q->res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
		    RF_ACTIVE);
		if (q->res == NULL) {
			if (i == 0) {
				device_printf(dev, "no interrupt\n");
				error = ENXIO;
				goto fail;
			}
			continue;
		}
		/* masked right after: the filter masks it should it come */
		error = bus_setup_intr(dev, q->res, INTR_TYPE_AV | INTR_MPSAFE,
		    qswr_filter, NULL, q, &q->cookie);
		if (error != 0) {
			device_printf(dev, "interrupt %d: %d\n", i, error);
			bus_release_resource(dev, SYS_RES_IRQ, i, q->res);
			q->res = NULL;
			if (i == 0)
				goto fail;
			continue;
		}
		qswr_mask(q);
		bus_describe_intr(dev, q->res, q->cookie, i == 0 ? "master" :
		    "wake");
		sc->have |= q->bit;
	}
	make_dev_args_init(&args);
	args.mda_devsw = &qswr_cdevsw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_WHEEL;
	args.mda_mode = 0600;
	args.mda_si_drv1 = sc;
	error = make_dev_s(&args, &sc->cdev, "qcom_swr%d", unit);
	if (error != 0) {
		device_printf(dev, "qcom_swr%d: %d\n", unit, error);
		goto fail;
	}
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "stats",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0,
	    qswr_sysctl_stats, "A", "The interrupts, how often they fired");
	if (sc->have & QCOM_SWR_WAKE)
		device_printf(dev, "/dev/qcom_swr%d at %#jx: master INTID %u, "
		    "wake-up INTID %u, masked until armed\n", unit,
		    (uintmax_t)sc->pa, sc->irq[0].intid, sc->irq[1].intid);
	else
		device_printf(dev, "/dev/qcom_swr%d at %#jx: master INTID %u, "
		    "masked until armed\n", unit, (uintmax_t)sc->pa,
		    sc->irq[0].intid);
	return (0);
fail:
	for (i = 0; i < 2; i++) {
		if (sc->irq[i].cookie != NULL)
			bus_teardown_intr(dev, sc->irq[i].res,
			    sc->irq[i].cookie);
		if (sc->irq[i].res != NULL)
			bus_release_resource(dev, SYS_RES_IRQ, i,
			    sc->irq[i].res);
	}
	knlist_destroy(&sc->rsel.si_note);
	mtx_destroy(&sc->mtx);
	return (error);
}

static int
qswr_detach(device_t dev)
{
	struct qswr_softc *sc = device_get_softc(dev);
	int i;

	mtx_lock_spin(&sc->mtx);
	if (sc->open) {
		mtx_unlock_spin(&sc->mtx);
		return (EBUSY);
	}
	sc->gone = true;
	mtx_unlock_spin(&sc->mtx);
	destroy_dev(sc->cdev);
	for (i = 0; i < 2; i++) {
		if (sc->irq[i].cookie != NULL)
			bus_teardown_intr(dev, sc->irq[i].res,
			    sc->irq[i].cookie);
		if (sc->irq[i].res != NULL)
			bus_release_resource(dev, SYS_RES_IRQ, i,
			    sc->irq[i].res);
	}
	taskqueue_drain(taskqueue_fast, &sc->task);
	seldrain(&sc->rsel);
	knlist_destroy(&sc->rsel.si_note);
	mtx_destroy(&sc->mtx);
	return (0);
}

static device_method_t qswr_methods[] = {
	DEVMETHOD(device_probe,		qswr_probe),
	DEVMETHOD(device_attach,	qswr_attach),
	DEVMETHOD(device_detach,	qswr_detach),
	DEVMETHOD_END
};

static driver_t qswr_driver = {
	"qcom_swr",
	qswr_methods,
	sizeof(struct qswr_softc),
};

DRIVER_MODULE(qcom_swr, simplebus, qswr_driver, 0, 0);
MODULE_VERSION(qcom_swr, 1);
