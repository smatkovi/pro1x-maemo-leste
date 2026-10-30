/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The simulation's kernel (simkern.h): memory, locks, sleeps, task queues
 * (a thread each), interrupts (a thread per line), newbus over the device
 * tree's buses, modules, character devices, sbuf, sysctl, and the console
 * the checks read.
 */

#include "simkern.h"
#include "sim.h"

#include <time.h>
#include <unistd.h>
#include <execinfo.h>

#undef malloc
#undef free
#undef printf

int hz = 1000;

/* --- the console ------------------------------------------------------------------- */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static char *logbuf;
static size_t loglen, logcap;
static bool log_quiet;

static void
log_add(const char *s, size_t n)
{
	pthread_mutex_lock(&log_lock);
	if (loglen + n + 1 > logcap) {
		logcap = (loglen + n + 1) * 2;
		logbuf = realloc(logbuf, logcap);
	}
	memcpy(logbuf + loglen, s, n);
	loglen += n;
	logbuf[loglen] = '\0';
	if (!log_quiet)
		fwrite(s, 1, n, stdout);
	pthread_mutex_unlock(&log_lock);
}

int
sim_printf(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log_add(buf, MIN((size_t)n, sizeof(buf) - 1));
	return (n);
}

void
sim_log_quiet(bool quiet)
{
	log_quiet = quiet;
}

size_t
sim_log_mark(void)
{
	size_t m;

	pthread_mutex_lock(&log_lock);
	m = loglen;
	pthread_mutex_unlock(&log_lock);
	return (m);
}

int
sim_log_count(size_t since, const char *needle)
{
	const char *p;
	int n = 0;

	pthread_mutex_lock(&log_lock);
	for (p = logbuf != NULL && since <= loglen ? logbuf + since : "";
	    (p = strstr(p, needle)) != NULL; p++)
		n++;
	pthread_mutex_unlock(&log_lock);
	return (n);
}

bool
sim_log_has(size_t since, const char *needle)
{
	return (sim_log_count(since, needle) > 0);
}

/* --- memory --------------------------------------------------------------------------- */

void *
sim_kmalloc(size_t size, struct malloc_type *type, int flags)
{
	void *p = (flags & M_ZERO) ? calloc(1, MAX(size, 1)) :
	    malloc(MAX(size, 1));

	if (p == NULL && (flags & M_WAITOK))
		abort();
	return (p);
}

void
sim_kfree(void *p)
{
	free(p);
}

void *
kmem_alloc_contig(vm_size_t size, int flags, vm_paddr_t low, vm_paddr_t high,
    u_long align, vm_paddr_t boundary, vm_memattr_t memattr)
{
	void *p;

	if (posix_memalign(&p, MAX(align, (u_long)PAGE_SIZE), size) != 0)
		return (NULL);
	if (flags & M_ZERO)
		memset(p, 0, size);
	return (p);
}

void
kmem_free(void *addr, vm_size_t size)
{
	free(addr);
}

int
copyin(const void *uaddr, void *kaddr, size_t len)
{
	if (len != 0)
		memcpy(kaddr, uaddr, len);
	return (0);
}

int
copyout(const void *kaddr, void *uaddr, size_t len)
{
	memcpy(uaddr, kaddr, len);
	return (0);
}

/* --- time ------------------------------------------------------------------------------- */

sbintime_t
getsbinuptime(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((sbintime_t)ts.tv_sec * SBT_1S +
	    (sbintime_t)ts.tv_nsec * SBT_1S / 1000000000);
}

static void
sleep_sbt(sbintime_t sbt)
{
	struct timespec ts;

	ts.tv_sec = sbt / SBT_1S;
	ts.tv_nsec = (sbt % SBT_1S) * 1000000000 / SBT_1S;
	nanosleep(&ts, NULL);
}

void
DELAY(int us)
{
	sleep_sbt((sbintime_t)us * SBT_1US);
}

int
sim_kpause(const char *wmesg, int ticks)
{
	sleep_sbt((sbintime_t)ticks * SBT_1S / hz);
	return (EWOULDBLOCK);
}

int
pause_sbt(const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags)
{
	sleep_sbt(sbt);
	return (EWOULDBLOCK);
}

static void
abstime(sbintime_t sbt, struct timespec *ts)
{
	clock_gettime(CLOCK_MONOTONIC, ts);
	ts->tv_sec += sbt / SBT_1S;
	ts->tv_nsec += (sbt % SBT_1S) * 1000000000 / SBT_1S;
	if (ts->tv_nsec >= 1000000000) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000;
	}
}

/* --- locks ------------------------------------------------------------------------------ */

void
mtx_init(struct mtx *m, const char *name, const char *type, int opts)
{
	pthread_mutexattr_t a;

	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
	pthread_mutex_init(&m->m, &a);
	pthread_mutexattr_destroy(&a);
	m->name = name;
	m->inited = 1;
}

void
mtx_destroy(struct mtx *m)
{
	pthread_mutex_destroy(&m->m);
	m->inited = 0;
}

void
mtx_lock(struct mtx *m)
{
	int e;

	if (!m->inited) {
		fprintf(stderr, "sim: lock %p not initialised\n", (void *)m);
		abort();
	}
	if ((e = pthread_mutex_lock(&m->m)) != 0) {
		fprintf(stderr, "sim: lock %s %p: %s (recursion?)\n", m->name,
		    (void *)m, strerror(e));
		{
			void *bt[32];

			backtrace_symbols_fd(bt, backtrace(bt, 32), 2);
		}
		abort();
	}
}

void
mtx_unlock(struct mtx *m)
{
	if (pthread_mutex_unlock(&m->m) != 0) {
		fprintf(stderr, "sim: unlock %s not held\n", m->name);
		abort();
	}
}

void
cv_init(struct cv *cv, const char *desc)
{
	pthread_condattr_t a;

	pthread_condattr_init(&a);
	pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
	pthread_cond_init(&cv->c, &a);
	pthread_condattr_destroy(&a);
}

void
cv_destroy(struct cv *cv)
{
	pthread_cond_destroy(&cv->c);
}

void
cv_broadcast(struct cv *cv)
{
	pthread_cond_broadcast(&cv->c);
}

void
cv_signal(struct cv *cv)
{
	pthread_cond_signal(&cv->c);
}

void
cv_wait(struct cv *cv, struct mtx *m)
{
	pthread_cond_wait(&cv->c, &m->m);
}

int
cv_timedwait_sbt(struct cv *cv, struct mtx *m, sbintime_t sbt, sbintime_t pr,
    int flags)
{
	struct timespec ts;

	abstime(sbt, &ts);
	return (pthread_cond_timedwait(&cv->c, &m->m, &ts) == ETIMEDOUT ?
	    EWOULDBLOCK : 0);
}

/* sleep queues: one for all channels, sleepers look again */
static pthread_mutex_t sq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sq_cond;
static uint64_t sq_gen;

static void __attribute__((constructor))
sq_init(void)
{
	pthread_condattr_t a;

	pthread_condattr_init(&a);
	pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
	pthread_cond_init(&sq_cond, &a);
}

int
msleep_sbt(void *chan, struct mtx *m, int pri, const char *wmesg,
    sbintime_t sbt, sbintime_t pr, int flags)
{
	struct timespec ts;
	uint64_t gen;
	int r = 0;

	pthread_mutex_lock(&sq_lock);
	gen = sq_gen;
	mtx_unlock(m);
	abstime(sbt > 0 ? sbt : SBT_1S * 3600, &ts);
	while (gen == sq_gen && r == 0)
		r = pthread_cond_timedwait(&sq_cond, &sq_lock, &ts);
	pthread_mutex_unlock(&sq_lock);
	mtx_lock(m);
	return (r == ETIMEDOUT ? EWOULDBLOCK : 0);
}

void
wakeup(void *chan)
{
	pthread_mutex_lock(&sq_lock);
	sq_gen++;
	pthread_cond_broadcast(&sq_cond);
	pthread_mutex_unlock(&sq_lock);
}

/* --- task queues ------------------------------------------------------------------------ */

struct taskqueue {
	char		name[32];
	pthread_mutex_t	lock;
	pthread_cond_t	cond, idle;
	struct task	*head, *tail;
	struct task	*running;
	bool		started, stop;
	pthread_t	thread;
};

struct taskqueue *taskqueue_thread;

static void *
tq_loop(void *arg)
{
	struct taskqueue *tq = arg;
	struct task *t;
	int pending;

	pthread_mutex_lock(&tq->lock);
	for (;;) {
		while (tq->head == NULL && !tq->stop)
			pthread_cond_wait(&tq->cond, &tq->lock);
		if (tq->head == NULL && tq->stop)
			break;
		t = tq->head;
		tq->head = t->ta_next;
		if (tq->head == NULL)
			tq->tail = NULL;
		pending = t->ta_pending;
		t->ta_pending = 0;
		tq->running = t;
		pthread_mutex_unlock(&tq->lock);
		t->ta_func(t->ta_context, pending);
		pthread_mutex_lock(&tq->lock);
		/* the task may be gone (freed by its function), as in FreeBSD */
		tq->running = NULL;
		pthread_cond_broadcast(&tq->idle);
	}
	pthread_mutex_unlock(&tq->lock);
	return (NULL);
}

void
taskqueue_thread_enqueue(void *context)
{
}

struct taskqueue *
taskqueue_create(const char *name, int flags, taskqueue_enqueue_fn *enqueue,
    void *context)
{
	struct taskqueue *tq = calloc(1, sizeof(*tq));

	strlcpy(tq->name, name, sizeof(tq->name));
	pthread_mutex_init(&tq->lock, NULL);
	pthread_cond_init(&tq->cond, NULL);
	pthread_cond_init(&tq->idle, NULL);
	return (tq);
}

int
taskqueue_start_threads(struct taskqueue **tqp, int count, int pri,
    const char *fmt, ...)
{
	struct taskqueue *tq = *tqp;

	if (!tq->started) {
		tq->started = true;
		pthread_create(&tq->thread, NULL, tq_loop, tq);
	}
	return (0);
}

static void __attribute__((constructor))
tq_system(void)
{
	taskqueue_thread = taskqueue_create("thread", 0, NULL, NULL);
	taskqueue_start_threads(&taskqueue_thread, 1, 0, "thread");
}

int
taskqueue_enqueue(struct taskqueue *tq, struct task *t)
{
	pthread_mutex_lock(&tq->lock);
	if (t->ta_pending > 0) {
		t->ta_pending++;
		pthread_mutex_unlock(&tq->lock);
		return (0);
	}
	t->ta_pending = 1;
	t->ta_next = NULL;
	if (tq->tail != NULL)
		tq->tail->ta_next = t;
	else
		tq->head = t;
	tq->tail = t;
	pthread_cond_signal(&tq->cond);
	pthread_mutex_unlock(&tq->lock);
	return (0);
}

void
taskqueue_drain(struct taskqueue *tq, struct task *t)
{
	pthread_mutex_lock(&tq->lock);
	while (t->ta_pending > 0 || tq->running == t)
		pthread_cond_wait(&tq->idle, &tq->lock);
	pthread_mutex_unlock(&tq->lock);
}

void
taskqueue_drain_all(struct taskqueue *tq)
{
	pthread_mutex_lock(&tq->lock);
	while (tq->head != NULL || tq->running != NULL)
		pthread_cond_wait(&tq->idle, &tq->lock);
	pthread_mutex_unlock(&tq->lock);
}

void
taskqueue_free(struct taskqueue *tq)
{
	pthread_mutex_lock(&tq->lock);
	tq->stop = true;
	pthread_cond_broadcast(&tq->cond);
	pthread_mutex_unlock(&tq->lock);
	if (tq->started && !pthread_equal(pthread_self(), tq->thread))
		pthread_join(tq->thread, NULL);
	else if (tq->started)
		pthread_detach(tq->thread);
}

/* --- interrupts ----------------------------------------------------------------------- */

#define	NIRQ	1024

static struct irq {
	pthread_mutex_t	lock;
	pthread_cond_t	cond;
	driver_intr_t	*handler;
	void		*arg;
	int		pending;
	bool		running, thread;
	pthread_t	tid;
} irqs[NIRQ];

static void *
ithread(void *arg)
{
	struct irq *q = arg;

	pthread_mutex_lock(&q->lock);
	for (;;) {
		while (q->pending == 0)
			pthread_cond_wait(&q->cond, &q->lock);
		q->pending = 0;
		if (q->handler != NULL) {
			q->running = true;
			pthread_mutex_unlock(&q->lock);
			q->handler(q->arg);
			pthread_mutex_lock(&q->lock);
			q->running = false;
			pthread_cond_broadcast(&q->cond);
		}
	}
	return (NULL);
}

static void __attribute__((constructor))
irq_init(void)
{
	int i;

	for (i = 0; i < NIRQ; i++) {
		pthread_mutex_init(&irqs[i].lock, NULL);
		pthread_cond_init(&irqs[i].cond, NULL);
	}
}

void
sim_irq_raise(int irq)
{
	struct irq *q;

	if (irq < 0 || irq >= NIRQ)
		return;
	q = &irqs[irq];
	pthread_mutex_lock(&q->lock);
	if (q->handler != NULL) {
		q->pending = 1;
		pthread_cond_broadcast(&q->cond);
	}
	pthread_mutex_unlock(&q->lock);
}

/* --- newbus ---------------------------------------------------------------------------- */

struct resource {
	int	type, rid;
	u_long	start;
};

struct sim_device {
	driver_t	*drv;
	void		*softc;
	phandle_t	node;
	int		unit;
	char		nameunit[40];
	struct {
		int	type, rid;
		u_long	start;
	} rl[8];
	int		nrl;
};

static struct {
	driver_t	*drv;
	char		bus[16];
} drivers[32];
static int ndrivers;
static struct sim_device *devs[64];
static int ndevs;

void
sim_driver_register(const char *bus, driver_t *drv)
{
	int i;

	for (i = 0; i < ndrivers; i++)
		if (drivers[i].drv == drv)
			return;
	drivers[ndrivers].drv = drv;
	strlcpy(drivers[ndrivers].bus, bus, sizeof(drivers[0].bus));
	ndrivers++;
}

static void *
method(driver_t *drv, const char *name)
{
	device_method_t *m;

	for (m = drv->methods; m->name != NULL; m++)
		if (strcmp(m->name, name) == 0)
			return (m->fn);
	return (NULL);
}

static bool
dev_attached(phandle_t node, driver_t *drv)
{
	int i;

	for (i = 0; i < ndevs; i++)
		if (devs[i]->node == node && devs[i]->drv == drv)
			return (true);
	return (false);
}

static int
unit_of(driver_t *drv)
{
	int i, n = 0;

	for (i = 0; i < ndevs; i++)
		if (strcmp(devs[i]->drv->name, drv->name) == 0)
			n++;
	return (n);
}

/* The children of "/" (ofwbus) and of simple-bus nodes (simplebus) */
static void
attach_node(phandle_t node)
{
	int (*probe)(device_t), (*attach)(device_t);
	struct sim_device *d;
	int i, r;

	for (i = 0; i < ndrivers; i++) {
		if (dev_attached(node, drivers[i].drv))
			continue;
		d = calloc(1, sizeof(*d));
		d->drv = drivers[i].drv;
		d->node = node;
		d->unit = unit_of(d->drv);
		snprintf(d->nameunit, sizeof(d->nameunit), "%s%d", d->drv->name,
		    d->unit);
		probe = method(d->drv, "device_probe");
		attach = method(d->drv, "device_attach");
		if (probe == NULL || (r = probe(d)) > 0) {
			free(d);
			continue;
		}
		d->softc = calloc(1, MAX(d->drv->size, 1));
		devs[ndevs++] = d;
		if (attach != NULL && (r = attach(d)) != 0)
			sim_printf("%s: attach failed: %d\n", d->nameunit, r);
		break;
	}
}

void
sim_bus_attach(void)
{
	phandle_t root, c, cc;

	root = OF_finddevice("/");
	for (c = OF_child(root); c != 0; c = OF_peer(c)) {
		attach_node(c);
		if (ofw_bus_node_is_compatible(c, "simple-bus"))
			for (cc = OF_child(c); cc != 0; cc = OF_peer(cc))
				attach_node(cc);
	}
}

phandle_t
sim_device_node(struct sim_device *dev)
{
	return (dev->node);
}

void *
device_get_softc(device_t dev)
{
	return (dev->softc);
}

const char *
device_get_nameunit(device_t dev)
{
	return (dev->nameunit);
}

int
device_get_unit(device_t dev)
{
	return (dev->unit);
}

void
device_set_desc(device_t dev, const char *desc)
{
}

int
device_printf(device_t dev, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	n = snprintf(buf, sizeof(buf), "%s: ", dev->nameunit);
	va_start(ap, fmt);
	vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	return (sim_printf("%s", buf));
}

int
bus_set_resource(device_t dev, int type, int rid, u_long start, u_long count)
{
	if (dev->nrl == (int)nitems(dev->rl))
		return (ENOSPC);
	dev->rl[dev->nrl].type = type;
	dev->rl[dev->nrl].rid = rid;
	dev->rl[dev->nrl].start = start;
	dev->nrl++;
	return (0);
}

/* rid 0 from "interrupts" or the first of "interrupts-extended" (GIC) */
static int
dt_irq0(phandle_t node)
{
	pcell_t c[4];

	if (OF_getencprop(node, "interrupts", c, 3 * 4) == 12 && c[0] == 0)
		return (32 + c[1]);
	if (OF_getencprop(node, "interrupts-extended", c, 4 * 4) >= 16 &&
	    c[1] == 0)
		return (32 + c[2]);
	return (-1);
}

struct resource *
bus_alloc_resource_any(device_t dev, int type, int *rid, u_int flags)
{
	struct resource *r;
	int i, irq = -1;

	if (type != SYS_RES_IRQ)
		return (NULL);
	for (i = 0; i < dev->nrl; i++)
		if (dev->rl[i].type == type && dev->rl[i].rid == *rid)
			irq = dev->rl[i].start;
	if (irq < 0 && *rid == 0)
		irq = dt_irq0(dev->node);
	if (irq < 0)
		return (NULL);
	r = calloc(1, sizeof(*r));
	r->type = type;
	r->rid = *rid;
	r->start = irq;
	return (r);
}

int
bus_release_resource(device_t dev, int type, int rid, struct resource *r)
{
	free(r);
	return (0);
}

int
bus_setup_intr(device_t dev, struct resource *r, int flags,
    driver_filter_t *filter, driver_intr_t *ithr, void *arg, void **cookiep)
{
	struct irq *q;

	if (r == NULL || r->start >= NIRQ)
		return (EINVAL);
	q = &irqs[r->start];
	pthread_mutex_lock(&q->lock);
	if (q->handler != NULL) {
		pthread_mutex_unlock(&q->lock);
		return (EBUSY);
	}
	q->handler = ithr;
	q->arg = arg;
	if (!q->thread) {
		q->thread = true;
		pthread_create(&q->tid, NULL, ithread, q);
	}
	pthread_mutex_unlock(&q->lock);
	*cookiep = q;
	return (0);
}

int
bus_teardown_intr(device_t dev, struct resource *r, void *cookie)
{
	struct irq *q = cookie;

	pthread_mutex_lock(&q->lock);
	while (q->running)
		pthread_cond_wait(&q->cond, &q->lock);
	q->handler = NULL;
	q->pending = 0;
	pthread_mutex_unlock(&q->lock);
	return (0);
}

/* --- modules ------------------------------------------------------------------------------ */

static moduledata_t *modules[16];
static int nmodules;

void
sim_module_register(moduledata_t *md)
{
	modules[nmodules++] = md;
}

/* kldload: the drivers attach (SI_SUB_DRIVERS), then the module's event */
int
sim_module_load(const char *name)
{
	int i;

	sim_bus_attach();
	for (i = 0; i < nmodules; i++)
		if (strcmp(modules[i]->name, name) == 0)
			return (modules[i]->evhand(NULL, MOD_LOAD,
			    modules[i]->priv));
	return (ENOENT);
}

/* --- character devices ------------------------------------------------------------- */

static struct cdev *cdevs;
static pthread_mutex_t cdev_lock = PTHREAD_MUTEX_INITIALIZER;

struct cdev *
make_dev(struct cdevsw *devsw, int unit, int uid, int gid, int perms,
    const char *fmt, ...)
{
	struct cdev *c = calloc(1, sizeof(*c));
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(c->si_name, sizeof(c->si_name), fmt, ap);
	va_end(ap);
	c->si_devsw = devsw;
	pthread_mutex_lock(&cdev_lock);
	c->si_next = cdevs;
	cdevs = c;
	pthread_mutex_unlock(&cdev_lock);
	return (c);
}

void
destroy_dev(struct cdev *dev)
{
	struct cdev **p;

	pthread_mutex_lock(&cdev_lock);
	for (p = &cdevs; *p != NULL; p = &(*p)->si_next)
		if (*p == dev) {
			*p = dev->si_next;
			break;
		}
	pthread_mutex_unlock(&cdev_lock);
	free(dev);
}

struct cdev *
sim_cdev_find(const char *name)
{
	struct cdev *c;

	pthread_mutex_lock(&cdev_lock);
	for (c = cdevs; c != NULL; c = c->si_next)
		if (strcmp(c->si_name, name) == 0)
			break;
	pthread_mutex_unlock(&cdev_lock);
	return (c);
}

int
uiomove(void *cp, int n, struct uio *uio)
{
	n = MIN(n, (int)uio->uio_resid);
	if (uio->uio_rw == UIO_READ)
		memcpy(uio->uio_buf, cp, n);
	else
		memcpy(cp, uio->uio_buf, n);
	uio->uio_buf += n;
	uio->uio_resid -= n;
	uio->uio_offset += n;
	return (0);
}

/* --- sbuf ------------------------------------------------------------------------------- */

static struct sbuf *
sb_init(struct sbuf *s)
{
	s->s_size = 256;
	s->s_buf = malloc(s->s_size);
	s->s_buf[0] = '\0';
	s->s_len = 0;
	return (s);
}

struct sbuf *
sbuf_new_auto(void)
{
	struct sbuf *s = calloc(1, sizeof(*s));

	s->s_dyn = 1;
	return (sb_init(s));
}

struct sbuf *
sbuf_new_for_sysctl(struct sbuf *s, char *buf, int len, struct sysctl_req *req)
{
	memset(s, 0, sizeof(*s));
	sb_init(s);
	s->s_req = req;
	return (s);
}

int
sbuf_printf(struct sbuf *s, const char *fmt, ...)
{
	va_list ap;
	int n;

	for (;;) {
		va_start(ap, fmt);
		n = vsnprintf(s->s_buf + s->s_len, s->s_size - s->s_len, fmt, ap);
		va_end(ap);
		if ((size_t)n < s->s_size - s->s_len)
			break;
		s->s_size = (s->s_len + n + 1) * 2;
		s->s_buf = realloc(s->s_buf, s->s_size);
	}
	s->s_len += n;
	return (0);
}

int
sbuf_cat(struct sbuf *s, const char *str)
{
	return (sbuf_printf(s, "%s", str));
}

int
sbuf_finish(struct sbuf *s)
{
	if (s->s_req != NULL)
		return (sim_sysctl_out(s->s_req, s->s_buf, s->s_len + 1));
	return (0);
}

char *
sbuf_data(struct sbuf *s)
{
	return (s->s_buf);
}

ssize_t
sbuf_len(struct sbuf *s)
{
	return (s->s_len);
}

void
sbuf_delete(struct sbuf *s)
{
	free(s->s_buf);
	if (s->s_dyn)
		free(s);
}

/* --- sysctl ----------------------------------------------------------------------------- */

struct sysctl_oid sysctl__hw = { "hw", NULL, 0, NULL, 0, NULL, NULL };
struct sysctl_oid sysctl__net = { "net", NULL, 0, NULL, 0, NULL, NULL };
struct sysctl_oid sysctl__dev = { "dev", NULL, 0, NULL, 0, NULL, NULL };
static struct sysctl_oid *oids;

void
sim_sysctl_register(struct sysctl_oid *oid)
{
	oid->oid_next = oids;
	oids = oid;
}

static void
oid_name(struct sysctl_oid *o, char *buf, size_t len)
{
	char tmp[128];

	if (o->oid_parent == NULL) {
		strlcpy(buf, o->oid_name, len);
		return;
	}
	oid_name(o->oid_parent, tmp, sizeof(tmp));
	snprintf(buf, len, "%s.%s", tmp, o->oid_name);
}

static struct sysctl_oid *
oid_find(const char *name)
{
	struct sysctl_oid *o;
	char n[128];

	for (o = oids; o != NULL; o = o->oid_next) {
		oid_name(o, n, sizeof(n));
		if (strcmp(n, name) == 0)
			return (o);
	}
	return (NULL);
}

int
sim_sysctl_out(struct sysctl_req *req, const void *p, size_t len)
{
	if (req->outlen + len > req->outcap) {
		req->outcap = (req->outlen + len) * 2;
		req->out = realloc(req->out, req->outcap);
	}
	memcpy(req->out + req->outlen, p, len);
	req->outlen += len;
	return (0);
}

int
sim_sysctl_in(struct sysctl_req *req, void *p, size_t len)
{
	if (req->newptr == NULL || len > req->newlen)
		return (EINVAL);
	memcpy(p, req->newptr, len);
	return (0);
}

int
sim_sysctl(const char *name, char *buf, size_t len)
{
	struct sysctl_req req;
	struct sysctl_oid *o;
	int error = 0;

	if ((o = oid_find(name)) == NULL)
		return (ENOENT);
	memset(&req, 0, sizeof(req));
	switch (o->oid_kind) {
	case 1:
		error = o->oid_handler(o, o->oid_arg1, o->oid_arg2, &req);
		snprintf(buf, len, "%.*s", (int)req.outlen,
		    req.out != NULL ? req.out : "");
		break;
	case 2:
		snprintf(buf, len, "%d", *(int *)o->oid_arg1);
		break;
	case 3:
		snprintf(buf, len, "%u", *(uint32_t *)o->oid_arg1);
		break;
	case 4:
		snprintf(buf, len, "%ju", (uintmax_t)*(uint64_t *)o->oid_arg1);
		break;
	default:
		error = EISDIR;
	}
	free(req.out);
	return (error);
}

int
sim_sysctl_set_int(const char *name, int value)
{
	struct sysctl_oid *o;

	if ((o = oid_find(name)) == NULL || o->oid_kind != 2)
		return (ENOENT);
	*(int *)o->oid_arg1 = value;
	return (0);
}
