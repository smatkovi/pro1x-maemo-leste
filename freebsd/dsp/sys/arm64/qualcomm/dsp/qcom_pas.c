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
 * is 51 MB.
 *
 * Linux votes a processor's proxy power domains and its xo clock from the
 * start until the handover bit (qcom_pas_start, qcom_pas_handover): for
 * the SM6115 only the modem has one, "cx" (sc8180x_mpss_resource; the
 * ADSP's and CDSP's data have no proxy_pd_names). The xo clock stays on
 * while this processor runs. The modem's CX vote is sent here as Linux's
 * rpmpd sends it (resource "rwcx" 0: "swen" 1, "vlvl" the SoC's highest
 * level, active and sleep set) when hw.qcom_pas.proxy_votes=1 (loader
 * tunable; off by default: it goes over the RPM's channel, device run
 * dsp7a). Linux lowers CX again at the handover to what its other
 * consumers ask; FreeBSD does not know those and leaves it.
 *
 * The SMP2P bits are watched (callbacks) and also read while waiting, so
 * that a bit set before its interrupt was seen is not missed. Each event
 * is logged with the time since the start, for the device runs.
 *
 * A processor Linux started (kexec from a running Leste) is taken over
 * with QCOM_PAS_ADOPT: running if its ready bit is set, its GLINK edge
 * taken as it is (qcom_glink_edge_adopt).
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

#include <sys/sysctl.h>

#define	META_MAX		(1024 * 1024)
#define	STOP_TIMEOUT_MS		5000	/* Linux's qcom_q6v5_request_stop */

/* Linux's rpmpd (drivers/pmdomain/qcom/rpmpd.c, sm6115_desc) */
#define	RPMPD_RWCX		0x78637772	/* "rwcx" */
#define	RPMPD_KEY_ENABLE	0x6e657773	/* "swen" */
#define	RPMPD_KEY_LEVEL		0x6c766c76	/* "vlvl" */
#define	RPM_SMD_LEVEL_TURBO_NO_CPR 416		/* the SM6115's max_state */

struct pas_data {
	const char	*name;
	uint32_t	pas_id;
	u_int		crash_reason_item;
	bool		proxy_cx;	/* Linux's proxy_pd_names "cx" */
};

/* Linux's adsp_resource_init, cdsp_resource_init, sc8180x_mpss_resource */
static const struct pas_data adsp_data = { "adsp", 1, 423, false };
static const struct pas_data cdsp_data = { "cdsp", 18, 601, false };
static const struct pas_data mpss_data = { "modem", 4, 421, true };

static int pas_proxy_votes;
SYSCTL_NODE(_hw, OID_AUTO, qcom_pas, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "Qualcomm remote processors");
SYSCTL_INT(_hw_qcom_pas, OID_AUTO, proxy_votes, CTLFLAG_RWTUN,
    &pas_proxy_votes, 0,
    "Vote the modem's CX at the RPM for its start, as Linux (1)");

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
	sbintime_t	t_start;	/* of the last start, for the log */
	bool		stopping;	/* a watchdog now is the stop's end */
	bool		cx_voted;
	/* set at the start (left from before): counts once seen clear */
	bool		stale[IN_COUNT];
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

static u_int
pas_ms(struct pas_softc *sc)
{
	return ((u_int)((getsbinuptime() - sc->t_start) / SBT_1MS));
}

/* An SMP2P bit of the processor came up (callback or read); with sc->mtx */
static void
pas_event(struct pas_softc *sc, int i, const char *how)
{
	if ((sc->flags & in_flags[i]) != 0)
		return;
	sc->flags |= in_flags[i];
	if (sc->state == QCOM_PAS_STATE_OFF && i != IN_FATAL)
		return;
	device_printf(sc->dev, "%s: %s after %u ms%s\n", sc->data->name,
	    in_names[i], pas_ms(sc), how);
	if (i == IN_FATAL && sc->state != QCOM_PAS_STATE_OFF) {
		sc->state = QCOM_PAS_STATE_CRASHED;
		pas_crash_reason(sc);
		device_printf(sc->dev, "%s: fatal error: %s\n", sc->data->name,
		    sc->crash_reason[0] != '\0' ? sc->crash_reason :
		    "(no message in SMEM)");
		taskqueue_enqueue(taskqueue_thread, &sc->crash_task);
	}
	if (i == IN_READY && sc->state == QCOM_PAS_STATE_LOADING)
		sc->state = QCOM_PAS_STATE_RUNNING;
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
		pas_event(sc, i, "");
	}
	wakeup(sc);
	mtx_unlock(&sc->mtx);
}

/*
 * The bits as they are now: a bit whose interrupt was missed (or came
 * before the watch) is seen here. With sc->mtx.
 */
static void
pas_poll_bits(struct pas_softc *sc)
{
	uint32_t v;
	int i;

	for (i = 0; i < IN_COUNT; i++) {
		if (sc->in[i].xref == 0 ||
		    qcom_smp2p_in_read(sc->in[i].xref, &v) != 0)
			continue;
		if ((v & (1U << sc->in[i].bit)) == 0)
			sc->stale[i] = false;
		else if (!sc->stale[i])
			pas_event(sc, i, " (read, no interrupt)");
	}
}

/* The bits as the start finds them: what is set is from before */
static void
pas_mark_stale(struct pas_softc *sc)
{
	uint32_t v;
	int i;

	for (i = 0; i < IN_COUNT; i++)
		sc->stale[i] = sc->in[i].xref != 0 &&
		    qcom_smp2p_in_read(sc->in[i].xref, &v) == 0 &&
		    (v & (1U << sc->in[i].bit)) != 0;
}

static void
pas_wdog_intr(void *arg)
{
	struct pas_softc *sc = arg;

	mtx_lock(&sc->mtx);
	sc->flags |= QCOM_PAS_F_WDOG;
	if (sc->stopping) {
		/* Linux: "Sometimes the stop triggers a watchdog rather than a
		 * stop-ack" */
		sc->flags |= QCOM_PAS_F_STOP_ACK;
		device_printf(sc->dev, "%s: watchdog while stopping: stopped\n",
		    sc->data->name);
	} else if (sc->state != QCOM_PAS_STATE_OFF) {
		sc->state = QCOM_PAS_STATE_CRASHED;
		pas_crash_reason(sc);
		device_printf(sc->dev, "%s: watchdog after %u ms: %s\n",
		    sc->data->name, pas_ms(sc), sc->crash_reason[0] != '\0' ?
		    sc->crash_reason : "(no message in SMEM)");
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
	for (;;) {
		pas_poll_bits(sc);
		if ((sc->flags & flags) != 0 ||
		    sc->state == QCOM_PAS_STATE_CRASHED)
			break;
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

/* Linux's proxy vote for the modem's start: rpmpd_power_on, set_performance */
static void
pas_proxy_vote(struct pas_softc *sc)
{
	const uint32_t on[2] = { RPMPD_KEY_ENABLE, 1 };
	const uint32_t lvl[2] = { RPMPD_KEY_LEVEL, RPM_SMD_LEVEL_TURBO_NO_CPR };
	int e1, e2, e3;

	if (!sc->data->proxy_cx || !pas_proxy_votes || sc->cx_voted)
		return;
	e1 = qcom_rpm_write(0, RPMPD_RWCX, 0, on, 1);
	e2 = qcom_rpm_write(0, RPMPD_RWCX, 0, lvl, 1);
	e3 = qcom_rpm_write(1, RPMPD_RWCX, 0, lvl, 1);
	sc->cx_voted = e1 == 0 && e2 == 0 && e3 == 0;
	device_printf(sc->dev, "%s: proxy vote CX (rwcx 0) on, level %u: %s "
	    "(%d %d %d)\n", sc->data->name, RPM_SMD_LEVEL_TURBO_NO_CPR,
	    sc->cx_voted ? "done" : "FAILED", e1, e2, e3);
}

static int
pas_start(struct pas_softc *sc, uint32_t timeout_ms)
{
	uint32_t v;
	int error;

	if (sc->state != QCOM_PAS_STATE_LOADING)
		return (EINVAL);
	if ((error = pas_watch(sc, true)) != 0)
		return (error);
	/* bits set before the start are not this start's: noted, not taken */
	if (sc->in[IN_READY].xref != 0 &&
	    qcom_smp2p_in_read(sc->in[IN_READY].xref, &v) == 0 && v != 0)
		device_printf(sc->dev, "%s: SMP2P bits %#x before the start\n",
		    sc->data->name, v);
	pas_proxy_vote(sc);
	mtx_lock(&sc->mtx);
	sc->flags = 0;
	sc->t_start = getsbinuptime();
	pas_mark_stale(sc);
	mtx_unlock(&sc->mtx);
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
	device_printf(sc->dev, "%s: authenticated, out of reset; waiting %u ms "
	    "for ready\n", sc->data->name, MAX(timeout_ms, 100));
	error = pas_wait(sc, QCOM_PAS_F_READY, MAX(timeout_ms, 100));
	pas_meta_free(sc);
	if (error != 0) {
		v = 0;
		if (sc->in[IN_READY].xref != 0)
			qcom_smp2p_in_read(sc->in[IN_READY].xref, &v);
		device_printf(sc->dev, "%s: no ready (%d) after %u ms, SMP2P "
		    "bits %#x; shutting it down\n", sc->data->name, error,
		    pas_ms(sc), v);
		qcom_scm_pas_shutdown(sc->data->pas_id);
		mtx_lock(&sc->mtx);
		if (sc->state != QCOM_PAS_STATE_CRASHED)
			sc->state = QCOM_PAS_STATE_OFF;
		mtx_unlock(&sc->mtx);
		return (error == EWOULDBLOCK ? ETIMEDOUT : error);
	}
	device_printf(sc->dev, "%s: running (ready after %u ms)\n",
	    sc->data->name, pas_ms(sc));
	/* its channels: GLINK over SMEM, when it has an edge */
	if (qcom_glink_edge_start(sc->dev, ofw_bus_get_node(sc->dev),
	    &sc->glink) != 0)
		sc->glink = NULL;
	return (0);
}

/*
 * A processor Linux started and left running (a kexec from Leste): taken
 * over if its ready bit is set and no fatal one, with its GLINK edge as it
 * is. Nothing goes to TrustZone.
 */
static int
pas_adopt(struct pas_softc *sc)
{
	uint32_t v = 0;
	int error;

	if (sc->state == QCOM_PAS_STATE_RUNNING)
		return (0);
	if (sc->state != QCOM_PAS_STATE_OFF)
		return (EBUSY);
	if ((error = pas_watch(sc, true)) != 0)
		return (error);
	if (sc->in[IN_READY].xref == 0 ||
	    qcom_smp2p_in_read(sc->in[IN_READY].xref, &v) != 0) {
		device_printf(sc->dev, "%s: adopt: no SMP2P entry of the "
		    "remote (not started?)\n", sc->data->name);
		return (ENXIO);
	}
	/* ready, and neither crashed nor stopped (bits left from before) */
	if ((v & (1U << sc->in[IN_READY].bit)) == 0 ||
	    (sc->in[IN_FATAL].xref != 0 &&
	    (v & (1U << sc->in[IN_FATAL].bit)) != 0) ||
	    (sc->in[IN_STOP_ACK].xref != 0 &&
	    (v & (1U << sc->in[IN_STOP_ACK].bit)) != 0)) {
		device_printf(sc->dev, "%s: adopt: SMP2P bits %#x: not running\n",
		    sc->data->name, v);
		return (ENXIO);
	}
	mtx_lock(&sc->mtx);
	sc->t_start = getsbinuptime();
	sc->state = QCOM_PAS_STATE_RUNNING;
	sc->flags = QCOM_PAS_F_READY | QCOM_PAS_F_ADOPTED;
	if (sc->in[IN_HANDOVER].xref != 0 &&
	    (v & (1U << sc->in[IN_HANDOVER].bit)) != 0)
		sc->flags |= QCOM_PAS_F_HANDOVER;
	sc->crash_reason[0] = '\0';
	mtx_unlock(&sc->mtx);
	device_printf(sc->dev, "%s: adopted, running (SMP2P bits %#x)\n",
	    sc->data->name, v);
	if (qcom_glink_edge_adopt(sc->dev, ofw_bus_get_node(sc->dev),
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
		mtx_lock(&sc->mtx);
		sc->stopping = true;
		sc->flags &= ~QCOM_PAS_F_STOP_ACK;
		sc->t_start = getsbinuptime();
		pas_mark_stale(sc);
		mtx_unlock(&sc->mtx);
		qcom_smp2p_out_update(sc->stop_xref, 1U << sc->stop_bit,
		    1U << sc->stop_bit);
		if (pas_wait(sc, QCOM_PAS_F_STOP_ACK, STOP_TIMEOUT_MS) != 0)
			device_printf(sc->dev, "%s: no stop acknowledgement in "
			    "%u ms\n", sc->data->name, STOP_TIMEOUT_MS);
		qcom_smp2p_out_update(sc->stop_xref, 1U << sc->stop_bit, 0);
	}
	error = qcom_scm_pas_shutdown(sc->data->pas_id);
	if (error != 0)
		device_printf(sc->dev, "%s: PAS shutdown: %d\n", sc->data->name,
		    error);
	mtx_lock(&sc->mtx);
	sc->state = QCOM_PAS_STATE_OFF;
	sc->stopping = false;
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
	case QCOM_PAS_ADOPT:
		error = pas_adopt(sc);
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
		pas_poll_bits(sc);
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
