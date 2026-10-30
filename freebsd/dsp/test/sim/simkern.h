/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The part of FreeBSD's kernel interface the DSP stack's drivers use,
 * on the PC (freebsd/dsp/test/sim): the drivers' own files compile against
 * this unchanged (with -D_KERNEL -DQCOM_DSP_SIM), and simkern.c,
 * fdt.c and hw.c play the kernel, the device tree and the hardware.
 * Locks are POSIX mutexes, condition variables and sleeps POSIX ones,
 * every task queue and interrupt a thread of its own, as FreeBSD's
 * taskqueue threads and ithreads.
 */

#ifndef _SIMKERN_H_
#define	_SIMKERN_H_

#include <sys/types.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>	/* first: the kernel's pause() is another */

/* --- types -------------------------------------------------------------------------- */

typedef uint64_t	vm_paddr_t;
typedef uint64_t	vm_size_t;
typedef uintptr_t	vm_offset_t;
typedef int64_t		vm_ooffset_t;
typedef int		vm_memattr_t;
typedef uint64_t	bus_addr_t;
typedef uint64_t	bus_size_t;
typedef uint32_t	phandle_t;
typedef uint32_t	pcell_t;
typedef int64_t		sbintime_t;
typedef char		*caddr_t;
typedef struct sim_device *device_t;
typedef struct sim_module *module_t;

#ifndef EWOULDBLOCK
#define	EWOULDBLOCK	EAGAIN
#endif
#define	ENOIOCTL	(-3)

/* --- odds and ends -------------------------------------------------------------- */

#ifndef MIN
#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define	MAX(a, b)	((a) > (b) ? (a) : (b))
#endif
#define	roundup2(x, y)	(((x) + ((y) - 1)) & ~((__typeof(x))(y) - 1))
#define	rounddown2(x, y) ((x) & ~((__typeof(x))(y) - 1))
#ifndef nitems
#define	nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#endif
#ifndef howmany
#define	howmany(x, y)	(((x) + ((y) - 1)) / (y))
#endif
#define	PAGE_SIZE	4096
#define	round_page(x)	roundup2((x), (__typeof(x))PAGE_SIZE)
#define	__packed	__attribute__((__packed__))
#define	__unused	__attribute__((__unused__))
#define	KASSERT(exp, msg) do {						\
	if (!(exp)) {							\
		printf msg;						\
		printf("\n");						\
		abort();						\
	}								\
} while (0)
#define	CTASSERT(x)	_Static_assert(x, "compile-time assertion failed")
#define	__DECONST(type, var)	((type)(uintptr_t)(const void *)(var))

/* strlcpy: the C library's (glibc 2.38) */

/* the console, kept for the checks (sim.h) */
int	sim_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define	printf(...)	sim_printf(__VA_ARGS__)

extern int hz;

/* --- memory --------------------------------------------------------------------------- */

struct malloc_type { const char *name; };
#define	MALLOC_DEFINE(type, shortdesc, longdesc) \
	struct malloc_type type[1] = { { shortdesc } }
#define	MALLOC_DECLARE(type)	extern struct malloc_type type[1]
#define	M_NOWAIT	0x0001
#define	M_WAITOK	0x0002
#define	M_ZERO		0x0100
void	*sim_kmalloc(size_t size, struct malloc_type *type, int flags);
void	sim_kfree(void *p);
#define	malloc(s, t, f)	sim_kmalloc((s), (t), (f))
#define	free(p, t)	sim_kfree(p)

#define	VM_MEMATTR_DEVICE		0
#define	VM_MEMATTR_UNCACHEABLE		1
#define	VM_MEMATTR_WRITE_COMBINING	2
#define	VM_MEMATTR_WRITE_BACK		3
void	*kmem_alloc_contig(vm_size_t size, int flags, vm_paddr_t low,
	    vm_paddr_t high, u_long align, vm_paddr_t boundary,
	    vm_memattr_t memattr);
void	kmem_free(void *addr, vm_size_t size);
/* physical addresses: the simulated regions (hw.c), else the address */
void	*pmap_mapdev(vm_paddr_t pa, vm_size_t size);
void	*pmap_mapdev_attr(vm_paddr_t pa, vm_size_t size, vm_memattr_t ma);
void	pmap_unmapdev(void *va, vm_size_t size);
vm_paddr_t pmap_kextract(vm_offset_t va);
int	copyin(const void *uaddr, void *kaddr, size_t len);
int	copyout(const void *kaddr, void *uaddr, size_t len);

/* barriers and atomics */
#define	wmb()		__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define	rmb()		__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define	mb()		__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define	dsb(x)		__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define	atomic_load_acq_int(p)	__atomic_load_n((p), __ATOMIC_ACQUIRE)
#define	atomic_store_rel_int(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

/* --- time and sleeping ------------------------------------------------------------- */

#define	SBT_1S		((sbintime_t)1 << 32)
#define	SBT_1MS		(SBT_1S / 1000)
#define	SBT_1US		(SBT_1S / 1000000)
sbintime_t getsbinuptime(void);
void	DELAY(int us);
int	sim_kpause(const char *wmesg, int ticks);
#define	pause(w, t)	sim_kpause((w), (t))
int	pause_sbt(const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags);

/* --- locks ------------------------------------------------------------------------------ */

struct mtx { pthread_mutex_t m; const char *name; int inited; };
#define	MTX_DEF		0
#define	MTX_SPIN	1
#define	MTX_DUPOK	0x10
#define	MA_OWNED	1
void	mtx_init(struct mtx *m, const char *name, const char *type, int opts);
void	mtx_destroy(struct mtx *m);
void	mtx_lock(struct mtx *m);
void	mtx_unlock(struct mtx *m);
#define	mtx_lock_spin(m)	mtx_lock(m)
#define	mtx_unlock_spin(m)	mtx_unlock(m)
#define	mtx_assert(m, w)	((void)0)
#define	MTX_SYSINIT(name, mtx, desc, opts)				\
	static void __attribute__((constructor)) name##_sysinit(void)	\
	{ mtx_init((mtx), (desc), NULL, (opts)); }

struct sx { struct mtx m; };
#define	sx_init(s, n)	mtx_init(&(s)->m, (n), NULL, 0)
#define	sx_destroy(s)	mtx_destroy(&(s)->m)
#define	sx_xlock(s)	mtx_lock(&(s)->m)
#define	sx_xunlock(s)	mtx_unlock(&(s)->m)
#define	sx_slock(s)	mtx_lock(&(s)->m)
#define	sx_sunlock(s)	mtx_unlock(&(s)->m)
#define	sx_assert(s, w)	((void)0)
#define	SA_XLOCKED	1
#define	SX_SYSINIT(name, sx, desc)					\
	static void __attribute__((constructor)) name##_sysinit(void)	\
	{ sx_init((sx), (desc)); }

struct cv { pthread_cond_t c; };
void	cv_init(struct cv *cv, const char *desc);
void	cv_destroy(struct cv *cv);
void	cv_broadcast(struct cv *cv);
void	cv_signal(struct cv *cv);
void	cv_wait(struct cv *cv, struct mtx *m);
int	cv_timedwait_sbt(struct cv *cv, struct mtx *m, sbintime_t sbt,
	    sbintime_t pr, int flags);

int	msleep_sbt(void *chan, struct mtx *m, int pri, const char *wmesg,
	    sbintime_t sbt, sbintime_t pr, int flags);
void	wakeup(void *chan);

/* --- task queues ----------------------------------------------------------------------- */

struct taskqueue;
typedef void task_fn_t(void *context, int pending);
struct task {
	struct task	*ta_next;
	task_fn_t	*ta_func;
	void		*ta_context;
	int		ta_pending;
	int		ta_running;
};
#define	TASK_INIT(task, prio, func, ctx) do {				\
	memset((task), 0, sizeof(*(task)));				\
	(task)->ta_func = (func);					\
	(task)->ta_context = (ctx);					\
} while (0)
typedef void taskqueue_enqueue_fn(void *context);
extern struct taskqueue *taskqueue_thread;
void	taskqueue_thread_enqueue(void *context);
struct taskqueue *taskqueue_create(const char *name, int flags,
	    taskqueue_enqueue_fn *enqueue, void *context);
int	taskqueue_start_threads(struct taskqueue **tqp, int count, int pri,
	    const char *fmt, ...);
int	taskqueue_enqueue(struct taskqueue *tq, struct task *task);
void	taskqueue_drain(struct taskqueue *tq, struct task *task);
void	taskqueue_drain_all(struct taskqueue *tq);
void	taskqueue_free(struct taskqueue *tq);
#define	PI_NET		0
#define	PI_DISK		0

/* --- the device tree (fdt.c) ------------------------------------------------------- */

phandle_t OF_finddevice(const char *path);
phandle_t OF_child(phandle_t node);
phandle_t OF_peer(phandle_t node);
phandle_t OF_parent(phandle_t node);
ssize_t	OF_getprop(phandle_t node, const char *prop, void *buf, size_t len);
ssize_t	OF_getencprop(phandle_t node, const char *prop, pcell_t *buf,
	    size_t len);
ssize_t	OF_getproplen(phandle_t node, const char *prop);
int	OF_hasprop(phandle_t node, const char *prop);
ssize_t	OF_getencprop_alloc_multi(phandle_t node, const char *prop,
	    int elsz, void **buf);
void	OF_prop_free(void *buf);
phandle_t OF_node_from_xref(phandle_t xref);
phandle_t OF_xref_from_node(phandle_t node);

struct ofw_compat_data {
	const char	*ocd_str;
	uintptr_t	ocd_data;
};
phandle_t ofw_bus_get_node(device_t dev);
int	ofw_bus_status_okay(device_t dev);
int	ofw_bus_is_compatible(device_t dev, const char *compat);
int	ofw_bus_node_is_compatible(phandle_t node, const char *compat);
const struct ofw_compat_data *ofw_bus_search_compatible(device_t dev,
	    const struct ofw_compat_data *compat);
phandle_t ofw_bus_find_compatible(phandle_t node, const char *compat);
int	ofw_bus_find_string_index(phandle_t node, const char *prop,
	    const char *str, int *idx);
phandle_t ofw_bus_find_iparent(phandle_t node);
int	ofw_bus_map_intr(device_t dev, phandle_t iparent, int icells,
	    pcell_t *intr);

/* --- newbus ------------------------------------------------------------------------- */

typedef struct { const char *name; void *fn; } device_method_t;
#define	DEVMETHOD(name, fn)	{ #name, (void *)(fn) }
#define	DEVMETHOD_END		{ NULL, NULL }
typedef struct {
	const char	*name;
	device_method_t	*methods;
	size_t		size;
} driver_t;
#define	BUS_PROBE_DEFAULT	(-20)
void	sim_driver_register(const char *bus, driver_t *drv);
#define	DRIVER_MODULE(name, busname, driver, evh, arg)			\
	static void __attribute__((constructor))			\
	name##_##busname##_driver_reg(void)				\
	{ sim_driver_register(#busname, &(driver)); }
#define	MODULE_VERSION(name, ver)
#define	MODULE_DEPEND(name, a, b, c, d)
void	*device_get_softc(device_t dev);
const char *device_get_nameunit(device_t dev);
int	device_get_unit(device_t dev);
void	device_set_desc(device_t dev, const char *desc);
int	device_printf(device_t dev, const char *fmt, ...)
	    __attribute__((format(printf, 2, 3)));

struct resource;
#define	SYS_RES_IRQ	1
#define	SYS_RES_MEMORY	3
#define	RF_ACTIVE	0x2
#define	RF_SHAREABLE	0x4
#define	INTR_TYPE_MISC	0x10
#define	INTR_TYPE_NET	0x20
#define	INTR_MPSAFE	0x200
typedef int driver_filter_t(void *);
typedef void driver_intr_t(void *);
struct resource *bus_alloc_resource_any(device_t dev, int type, int *rid,
	    u_int flags);
int	bus_release_resource(device_t dev, int type, int rid,
	    struct resource *r);
int	bus_set_resource(device_t dev, int type, int rid, u_long start,
	    u_long count);
int	bus_setup_intr(device_t dev, struct resource *r, int flags,
	    driver_filter_t *filter, driver_intr_t *ithread, void *arg,
	    void **cookiep);
int	bus_teardown_intr(device_t dev, struct resource *r, void *cookie);

/* --- modules ---------------------------------------------------------------------------- */

enum { MOD_LOAD, MOD_UNLOAD, MOD_SHUTDOWN, MOD_QUIESCE };
typedef int (*modeventhand_t)(module_t, int, void *);
typedef struct moduledata {
	const char	*name;
	modeventhand_t	evhand;
	void		*priv;
} moduledata_t;
void	sim_module_register(moduledata_t *md);
#define	DECLARE_MODULE(name, data, sub, order)				\
	static void __attribute__((constructor)) name##_modreg(void)	\
	{ sim_module_register(&(data)); }

/* --- character devices ------------------------------------------------------------ */

struct thread { int td_dummy; };
struct cdev;
struct uio;
typedef int d_ioctl_t(struct cdev *dev, u_long cmd, caddr_t data, int fflag,
    struct thread *td);
typedef int d_rdwr_t(struct cdev *dev, struct uio *uio, int ioflag);
typedef int d_mmap_t(struct cdev *dev, vm_ooffset_t offset, vm_paddr_t *paddr,
    int nprot, vm_memattr_t *memattr);
typedef int d_open_t(struct cdev *dev, int oflags, int devtype,
    struct thread *td);
typedef int d_close_t(struct cdev *dev, int fflag, int devtype,
    struct thread *td);
struct cdevsw {
	int		d_version;
	int		d_flags;
	const char	*d_name;
	d_open_t	*d_open;
	d_close_t	*d_close;
	d_rdwr_t	*d_read;
	d_rdwr_t	*d_write;
	d_ioctl_t	*d_ioctl;
	d_mmap_t	*d_mmap;
};
struct cdev {
	void		*si_drv1, *si_drv2;
	struct cdevsw	*si_devsw;
	char		si_name[64];
	struct cdev	*si_next;
};
#define	D_VERSION	1
#define	UID_ROOT	0
#define	GID_WHEEL	0
struct cdev *make_dev(struct cdevsw *devsw, int unit, int uid, int gid,
	    int perms, const char *fmt, ...) __attribute__((format(printf, 6, 7)));
void	destroy_dev(struct cdev *dev);
struct cdev *sim_cdev_find(const char *name);

enum uio_rw { UIO_READ, UIO_WRITE };
struct uio {
	int64_t		uio_offset;
	ssize_t		uio_resid;
	enum uio_rw	uio_rw;
	char		*uio_buf;
};
int	uiomove(void *cp, int n, struct uio *uio);

/* --- sbuf and sysctl ------------------------------------------------------------- */

struct sysctl_req;
struct sbuf {
	char		*s_buf;
	size_t		s_len, s_size;
	struct sysctl_req *s_req;
	int		s_dyn;
};
struct sbuf *sbuf_new_auto(void);
struct sbuf *sbuf_new_for_sysctl(struct sbuf *s, char *buf, int len,
	    struct sysctl_req *req);
int	sbuf_printf(struct sbuf *s, const char *fmt, ...)
	    __attribute__((format(printf, 2, 3)));
int	sbuf_cat(struct sbuf *s, const char *str);
int	sbuf_finish(struct sbuf *s);
char	*sbuf_data(struct sbuf *s);
ssize_t	sbuf_len(struct sbuf *s);
void	sbuf_delete(struct sbuf *s);

struct sysctl_oid {
	const char	*oid_name;
	struct sysctl_oid *oid_parent;
	int		oid_kind;	/* 0 node, 1 proc, 2 int, 3 u32, 4 u64 */
	void		*oid_arg1;
	intmax_t	oid_arg2;
	int		(*oid_handler)(struct sysctl_oid *, void *, intmax_t,
			    struct sysctl_req *);
	struct sysctl_oid *oid_next;
};
struct sysctl_req {
	char		*out;
	size_t		outlen, outcap;
	const void	*newptr;
	size_t		newlen;
};
#define	SYSCTL_HANDLER_ARGS struct sysctl_oid *oidp, void *arg1,	\
	intmax_t arg2, struct sysctl_req *req
int	sim_sysctl_out(struct sysctl_req *req, const void *p, size_t len);
int	sim_sysctl_in(struct sysctl_req *req, void *p, size_t len);
#define	SYSCTL_OUT(req, p, l)	sim_sysctl_out((req), (p), (l))
#define	SYSCTL_IN(req, p, l)	sim_sysctl_in((req), (p), (l))
void	sim_sysctl_register(struct sysctl_oid *oid);
extern struct sysctl_oid sysctl__hw, sysctl__net, sysctl__dev;
#define	OID_AUTO	(-1)
#define	CTLTYPE_STRING	0x3
#define	CTLTYPE_INT	0x2
#define	CTLTYPE_U64	0x9
#define	CTLFLAG_RD	0x80000000
#define	CTLFLAG_WR	0x40000000
#define	CTLFLAG_RW	(CTLFLAG_RD | CTLFLAG_WR)
#define	CTLFLAG_TUN	0x00080000
#define	CTLFLAG_RDTUN	(CTLFLAG_RD | CTLFLAG_TUN)
#define	CTLFLAG_RWTUN	(CTLFLAG_RW | CTLFLAG_TUN)
#define	CTLFLAG_MPSAFE	0x00040000
#define	SYSCTL_DECL(name)	extern struct sysctl_oid sysctl_##name
#define	SYSCTL_NODE(parent, nbr, name, access, handler, descr)		\
	struct sysctl_oid sysctl_##parent##_##name = { #name,		\
	    &sysctl_##parent, 0, NULL, 0, NULL, NULL };			\
	static void __attribute__((constructor))			\
	sysctl_##parent##_##name##_reg(void)				\
	{ sim_sysctl_register(&sysctl_##parent##_##name); }
#define	SIM_SYSCTL_LEAF(parent, name, kind, a1, a2, h)			\
	static struct sysctl_oid sysctl_##parent##_##name = { #name,	\
	    &sysctl_##parent, (kind), (void *)(a1), (a2), (h), NULL };	\
	static void __attribute__((constructor))			\
	sysctl_##parent##_##name##_reg(void)				\
	{ sim_sysctl_register(&sysctl_##parent##_##name); }
#define	SYSCTL_PROC(parent, nbr, name, access, a1, a2, h, fmt, descr)	\
	SIM_SYSCTL_LEAF(parent, name, 1, a1, a2, h)
#define	SYSCTL_INT(parent, nbr, name, access, ptr, val, descr)		\
	SIM_SYSCTL_LEAF(parent, name, 2, ptr, val, NULL)
#define	SYSCTL_UINT(parent, nbr, name, access, ptr, val, descr)	\
	SIM_SYSCTL_LEAF(parent, name, 3, ptr, val, NULL)
#define	SYSCTL_U32(parent, nbr, name, access, ptr, val, descr)		\
	SIM_SYSCTL_LEAF(parent, name, 3, ptr, val, NULL)
#define	SYSCTL_U64(parent, nbr, name, access, ptr, val, descr)		\
	SIM_SYSCTL_LEAF(parent, name, 4, ptr, val, NULL)
/* "hw.qcom_smp2p" etc.: the output in a buffer of the caller's */
int	sim_sysctl(const char *name, char *buf, size_t len);
int	sim_sysctl_set_int(const char *name, int value);

/* --- the secure monitor and the doorbells (hw.c) --------------------------------- */

void	qcom_dsp_sim_smc(uint64_t a[8], uint64_t *x6, uint64_t res[4]);
void	qcom_dsp_sim_doorbell(vm_paddr_t reg, uint32_t bits);

#endif /* _SIMKERN_H_ */
