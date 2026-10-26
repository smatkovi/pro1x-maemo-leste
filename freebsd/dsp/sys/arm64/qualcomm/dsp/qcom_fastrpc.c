/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * FastRPC, remote procedure calls to the DSPs (Linux's drivers/misc/
 * fastrpc.c), as far as hexagonrpcd needs it for the sensors: attaching to
 * the sensors' or the root protection domain, invocations with plain
 * buffers (no dma-buf handles), and memory for the DSP (mmap/munmap).
 *
 * A call's arguments are packed into one buffer the DSP reads and writes:
 * a header per argument (pointer and length), the page list, then the
 * buffers, each at a 128-byte boundary (overlapping ones share, as Linux
 * does it). The DSP gets the buffer's address over the GLINK channel
 * "fastrpcglink-apps-dsp" with the call's context id and answers there.
 * Addresses carry the session's stream id from bit 32 up; FreeBSD has no
 * SMMU driver, so the session's streams run without translation
 * (qcom_smmu.c) and the buffers are physically contiguous below 4 GB.
 *
 * Each DSP with a "qcom,fastrpc" node becomes /dev/fastrpc-<label> once
 * it opens the channel.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/sx.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "glink.h"
#include "qcom_dsp.h"
#include "fastrpc_ioctl.h"

#define	FASTRPC_ALIGN		128
#define	FASTRPC_MAX_FDLIST	16
#define	FASTRPC_MAX_CRCLIST	64
#define	FASTRPC_CTX_MAX		256
#define	FASTRPC_MAX_SESSIONS	14
#define	FASTRPC_INIT_HANDLE	1
#define	FASTRPC_CTXID_MASK	0xff0
#define	FASTRPC_KERNEL_TIMEOUT_MS 10000

#define	RMID_INIT_ATTACH	0
#define	RMID_INIT_RELEASE	1
#define	RMID_INIT_MMAP		4
#define	RMID_INIT_MUNMAP	5

#define	ROOT_PD			0
#define	SENSORS_PD		2

#define	ADSP_MMAP_REMOTE_HEAP_ADDR 8
#define	ADSP_MMAP_ADD_PAGES	0x1000

#define	SC_INBUFS(sc)		(((sc) >> 16) & 0xff)
#define	SC_OUTBUFS(sc)		(((sc) >> 8) & 0xff)
#define	SC_INHANDLES(sc)	(((sc) >> 4) & 0x0f)
#define	SC_OUTHANDLES(sc)	((sc) & 0x0f)
#define	SC_LENGTH(sc)		(SC_INBUFS(sc) + SC_OUTBUFS(sc) + \
				    SC_INHANDLES(sc) + SC_OUTHANDLES(sc))
#define	SCALARS(method, in, out) \
	((((method) & 0x1f) << 24) | (((in) & 0xff) << 16) | (((out) & 0xff) << 8))

static MALLOC_DEFINE(M_FASTRPC, "fastrpc", "Qualcomm FastRPC");

/* On the wire (and in the argument buffer), as the DSP has them */
struct frpc_msg {
	int32_t		client_id;
	int32_t		tid;
	uint64_t	ctx;
	uint32_t	handle;
	uint32_t	sc;
	uint64_t	addr;
	uint64_t	size;
};

struct frpc_rsp {
	uint64_t	ctx;
	int32_t		retval;
};

struct remote_buf {
	uint64_t	pv;
	uint64_t	len;
};

struct remote_dmahandle {
	int32_t		fd;
	uint32_t	offset;
	uint32_t	len;
};

union remote_arg {
	struct remote_buf	buf;
	struct remote_dmahandle	dma;
};

struct invoke_buf {
	uint32_t	num;
	uint32_t	pgidx;
};

struct phy_page {
	uint64_t	addr;
	uint64_t	size;
};

struct mmap_req_msg {
	int32_t		client_id;
	uint32_t	flags;
	uint64_t	vaddr;
	int32_t		num;
};

struct mmap_rsp_msg {
	uint64_t	vaddr;
};

struct munmap_req_msg {
	int32_t		client_id;
	uint64_t	vaddr;
	uint64_t	size;
};

/* Physically contiguous, uncached memory the DSP reaches */
struct frpc_buf {
	LIST_ENTRY(frpc_buf) link;
	char		*va;
	vm_size_t	size;
	uint64_t	dma;		/* physical, with the stream id */
	uint64_t	raddr;		/* the DSP's address of an mmap */
};

struct frpc_chan;

struct frpc_ctx {
	uint32_t	ctxid;
	int32_t		retval;
	bool		done;
	bool		abandoned;	/* the caller left: freed on the answer */
	struct frpc_buf	*buf;
};

struct frpc_user {
	struct frpc_chan *chan;
	int		session;	/* -1: none */
	int		client_id;	/* session + 1 */
	uint32_t	sid;
	int		pd;
	bool		attached;
	struct sx	lock;		/* the mmaps */
	LIST_HEAD(, frpc_buf) mmaps;
};

struct frpc_chan {
	LIST_ENTRY(frpc_chan) link;
	char		label[16];	/* "adsp" */
	phandle_t	node;		/* qcom,fastrpc */
	struct mtx	lock;		/* contexts, sessions, channel */
	struct glink_channel *ch;
	struct frpc_ctx	*ctxs[FASTRPC_CTX_MAX];
	u_int		next_ctx;
	struct {
		uint32_t	sid;
		bool		used;
	} sess[FASTRPC_MAX_SESSIONS];
	int		nsess;
	struct cdev	*cdev;
};

static LIST_HEAD(, frpc_chan) chans = LIST_HEAD_INITIALIZER(chans);
static struct mtx chans_lock;
MTX_SYSINIT(qcom_fastrpc_chans, &chans_lock, "fastrpc chans", MTX_DEF);

/* --- memory ------------------------------------------------------------------------ */

static struct frpc_buf *
buf_alloc(vm_size_t size, uint32_t sid)
{
	struct frpc_buf *b;

	b = malloc(sizeof(*b), M_FASTRPC, M_WAITOK | M_ZERO);
	b->size = round_page(MAX(size, 1));
	b->va = kmem_alloc_contig(b->size, M_WAITOK | M_ZERO, 0, 0xffffffffUL,
	    PAGE_SIZE, 0, VM_MEMATTR_UNCACHEABLE);
	if (b->va == NULL) {
		free(b, M_FASTRPC);
		return (NULL);
	}
	b->dma = pmap_kextract((vm_offset_t)b->va) | (uint64_t)sid << 32;
	return (b);
}

static void
buf_free(struct frpc_buf *b)
{
	if (b == NULL)
		return;
	kmem_free(b->va, b->size);
	free(b, M_FASTRPC);
}

/* --- invocations ------------------------------------------------------------------- */

struct olap {
	uint64_t	start, end, mstart, mend, offset;
	int		raix;
};

/* Linux's fastrpc_get_buff_overlaps: buffers sorted, overlaps shared */
static void
get_overlaps(const struct fastrpc_invoke_args *args, int nbufs,
    struct olap *o)
{
	struct olap t;
	uint64_t max_end = 0;
	int i, j;

	for (i = 0; i < nbufs; i++) {
		o[i].start = args[i].ptr;
		o[i].end = o[i].start + args[i].length;
		o[i].raix = i;
	}
	/* lowest start first, then the highest end */
	for (i = 1; i < nbufs; i++)
		for (j = i; j > 0 && (o[j].start < o[j - 1].start ||
		    (o[j].start == o[j - 1].start && o[j].end > o[j - 1].end));
		    j--) {
			t = o[j];
			o[j] = o[j - 1];
			o[j - 1] = t;
		}
	for (i = 0; i < nbufs; i++) {
		if (o[i].start < max_end) {
			o[i].mstart = max_end;
			o[i].mend = o[i].end;
			o[i].offset = max_end - o[i].start;
			if (o[i].end > max_end)
				max_end = o[i].end;
			else
				o[i].mstart = o[i].mend = 0;
		} else {
			o[i].mstart = o[i].start;
			o[i].mend = o[i].end;
			o[i].offset = 0;
			max_end = o[i].end;
		}
	}
}

static void
ctx_free(struct frpc_chan *c, struct frpc_ctx *ctx)
{
	mtx_lock(&c->lock);
	c->ctxs[ctx->ctxid >> 4] = NULL;
	mtx_unlock(&c->lock);
	buf_free(ctx->buf);
	free(ctx, M_FASTRPC);
}

/*
 * One call. kernel: the arguments are kernel pointers and the call is the
 * driver's own (the init handle, client 0).
 */
static int
invoke(struct frpc_user *u, bool kernel, uint32_t handle, uint32_t sc,
    struct fastrpc_invoke_args *args)
{
	struct frpc_chan *c = u->chan;
	struct frpc_ctx *ctx;
	struct frpc_msg msg;
	union remote_arg *rpra;
	struct invoke_buf *list;
	struct phy_page *pages;
	struct olap *o = NULL;
	uint64_t len, rlen, pkt_size, pg_start, pg_end, mlen;
	uintptr_t ap;
	int nscalars, nbufs, inbufs, metalen, i, oix, id, error = 0;

	if (u->session < 0)
		return (EINVAL);
	if (handle == FASTRPC_INIT_HANDLE && !kernel)
		return (EPERM);
	nscalars = SC_LENGTH(sc);
	inbufs = SC_INBUFS(sc);
	nbufs = inbufs + SC_OUTBUFS(sc);
	for (i = 0; i < nbufs; i++)
		if (args[i].fd > 0)
			return (EOPNOTSUPP);	/* no dma-buf handles */

	ctx = malloc(sizeof(*ctx), M_FASTRPC, M_WAITOK | M_ZERO);
	ctx->retval = -1;
	mtx_lock(&c->lock);
	for (i = 0; i < FASTRPC_CTX_MAX - 1; i++) {
		id = 1 + (c->next_ctx++ % (FASTRPC_CTX_MAX - 1));
		if (c->ctxs[id] == NULL)
			break;
	}
	if (i == FASTRPC_CTX_MAX - 1) {
		mtx_unlock(&c->lock);
		free(ctx, M_FASTRPC);
		return (EBUSY);
	}
	ctx->ctxid = id << 4;
	c->ctxs[id] = ctx;
	mtx_unlock(&c->lock);

	/* the size: headers, then the buffers at 128-byte boundaries */
	metalen = (sizeof(struct remote_buf) + sizeof(struct invoke_buf) +
	    sizeof(struct phy_page)) * nscalars +
	    sizeof(uint64_t) * FASTRPC_MAX_FDLIST +
	    sizeof(uint32_t) * FASTRPC_MAX_CRCLIST;
	if (nbufs > 0) {
		o = malloc(sizeof(*o) * nbufs, M_FASTRPC, M_WAITOK | M_ZERO);
		get_overlaps(args, nbufs, o);
	}
	pkt_size = roundup2(metalen, FASTRPC_ALIGN);
	for (oix = 0; oix < nbufs; oix++) {
		if (o[oix].offset == 0)
			pkt_size = roundup2(pkt_size, FASTRPC_ALIGN);
		pkt_size += o[oix].mend - o[oix].mstart;
	}
	ctx->buf = buf_alloc(pkt_size, u->sid);
	if (ctx->buf == NULL) {
		error = ENOMEM;
		goto out;
	}

	rpra = (union remote_arg *)ctx->buf->va;
	list = (struct invoke_buf *)&rpra[nscalars];
	pages = (struct phy_page *)&list[nscalars];
	ap = (uintptr_t)ctx->buf->va + metalen;
	rlen = pkt_size - metalen;
	for (oix = 0; oix < nbufs; oix++) {
		i = o[oix].raix;
		len = args[i].length;
		rpra[i].buf.pv = 0;
		rpra[i].buf.len = len;
		list[i].num = len != 0;
		list[i].pgidx = i;
		if (len == 0)
			continue;
		if (o[oix].offset == 0) {
			rlen -= roundup2(ap, FASTRPC_ALIGN) - ap;
			ap = roundup2(ap, FASTRPC_ALIGN);
		}
		mlen = o[oix].mend - o[oix].mstart;
		if (rlen < mlen) {
			error = EINVAL;
			goto out;
		}
		rpra[i].buf.pv = ap - o[oix].offset;
		pages[i].addr = (ctx->buf->dma - o[oix].offset +
		    (pkt_size - rlen)) & ~(uint64_t)PAGE_MASK;
		pg_start = (rpra[i].buf.pv & ~(uint64_t)PAGE_MASK) >> PAGE_SHIFT;
		pg_end = ((rpra[i].buf.pv + len - 1) & ~(uint64_t)PAGE_MASK) >>
		    PAGE_SHIFT;
		pages[i].size = (pg_end - pg_start + 1) * PAGE_SIZE;
		ap += mlen;
		rlen -= mlen;
		if (i < inbufs) {
			if (kernel)
				memcpy((void *)(uintptr_t)rpra[i].buf.pv,
				    (void *)(uintptr_t)args[i].ptr, len);
			else if ((error = copyin((void *)(uintptr_t)args[i].ptr,
			    (void *)(uintptr_t)rpra[i].buf.pv, len)) != 0)
				goto out;
		}
	}
	for (i = nbufs; i < nscalars; i++) {
		list[i].num = args[i].length != 0;
		list[i].pgidx = i;
		rpra[i].dma.fd = args[i].fd;
		rpra[i].dma.len = args[i].length;
		rpra[i].dma.offset = args[i].ptr;
	}
	/* everything in memory before the DSP looks */
	wmb();

	memset(&msg, 0, sizeof(msg));
	msg.client_id = kernel ? 0 : u->client_id;
	msg.tid = curthread->td_tid;
	msg.ctx = ctx->ctxid | u->pd;
	msg.handle = handle;
	msg.sc = sc;
	msg.addr = ctx->buf->dma;
	msg.size = round_page(pkt_size);
	mtx_lock(&c->lock);
	if (c->ch == NULL) {
		mtx_unlock(&c->lock);
		error = EPIPE;
		goto out;
	}
	mtx_unlock(&c->lock);
	if ((error = glink_send(c->ch, &msg, sizeof(msg))) != 0)
		goto out;

	mtx_lock(&c->lock);
	while (!ctx->done && error == 0) {
		error = msleep(ctx, &c->lock, kernel ? 0 : PCATCH, "fastrpc",
		    kernel ? FASTRPC_KERNEL_TIMEOUT_MS * hz / 1000 : 0);
		if (c->ch == NULL && !ctx->done)
			error = EPIPE;
	}
	if (ctx->done)
		error = 0;	/* answered as the signal or timeout came */
	if (error != 0) {
		/* the DSP may still write into the buffer: freed on its answer */
		ctx->abandoned = true;
		mtx_unlock(&c->lock);
		free(o, M_FASTRPC);
		return (error == EWOULDBLOCK ? ETIMEDOUT : error);
	}
	mtx_unlock(&c->lock);
	rmb();

	for (i = inbufs; i < nbufs; i++) {
		len = rpra[i].buf.len;
		if (len > args[i].length)
			len = args[i].length;
		if (kernel)
			memcpy((void *)(uintptr_t)args[i].ptr,
			    (void *)(uintptr_t)rpra[i].buf.pv, len);
		else if ((error = copyout((void *)(uintptr_t)rpra[i].buf.pv,
		    (void *)(uintptr_t)args[i].ptr, len)) != 0)
			goto out;
	}
	if (ctx->retval != 0)
		error = ctx->retval > 0 ? ctx->retval : EIO;
out:
	free(o, M_FASTRPC);
	ctx_free(c, ctx);
	return (error);
}

/* The DSP's answer (GLINK's receive context) */
static void
chan_rx(void *arg, const void *data, size_t len)
{
	struct frpc_chan *c = arg;
	struct frpc_rsp rsp;
	struct frpc_ctx *ctx;
	bool abandoned = false;

	if (len < sizeof(rsp))
		return;
	memcpy(&rsp, data, sizeof(rsp));
	mtx_lock(&c->lock);
	ctx = c->ctxs[(rsp.ctx & FASTRPC_CTXID_MASK) >> 4];
	if (ctx != NULL) {
		ctx->retval = rsp.retval;
		ctx->done = true;
		abandoned = ctx->abandoned;
		if (abandoned)
			c->ctxs[ctx->ctxid >> 4] = NULL;
		else
			wakeup(ctx);
	}
	mtx_unlock(&c->lock);
	if (abandoned) {
		buf_free(ctx->buf);
		free(ctx, M_FASTRPC);
	}
}

static void
chan_state(void *arg, bool up)
{
	struct frpc_chan *c = arg;
	int i;

	if (up)
		return;
	/* the DSP is gone: every waiting call fails (no GLINK call here) */
	mtx_lock(&c->lock);
	c->ch = NULL;
	for (i = 0; i < FASTRPC_CTX_MAX; i++)
		if (c->ctxs[i] != NULL)
			wakeup(c->ctxs[i]);
	mtx_unlock(&c->lock);
}

/* --- the device ---------------------------------------------------------------------- */

static void
user_release(void *data)
{
	struct frpc_user *u = data;
	struct fastrpc_invoke_args args[1];
	struct frpc_buf *b;
	int client_id = u->client_id;

	if (u->attached && u->chan->ch != NULL) {
		args[0].ptr = (uintptr_t)&client_id;
		args[0].length = sizeof(client_id);
		args[0].fd = -1;
		args[0].attr = 0;
		invoke(u, true, FASTRPC_INIT_HANDLE,
		    SCALARS(RMID_INIT_RELEASE, 1, 0), args);
	}
	/* the DSP process is gone, and with it its mappings */
	while ((b = LIST_FIRST(&u->mmaps)) != NULL) {
		LIST_REMOVE(b, link);
		buf_free(b);
	}
	mtx_lock(&u->chan->lock);
	if (u->session >= 0)
		u->chan->sess[u->session].used = false;
	mtx_unlock(&u->chan->lock);
	sx_destroy(&u->lock);
	free(u, M_FASTRPC);
}

static int
frpc_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	struct frpc_chan *c = dev->si_drv1;
	struct frpc_user *u;
	int i, error;

	u = malloc(sizeof(*u), M_FASTRPC, M_WAITOK | M_ZERO);
	u->chan = c;
	u->session = -1;
	sx_init(&u->lock, "fastrpc user");
	LIST_INIT(&u->mmaps);
	mtx_lock(&c->lock);
	for (i = 0; i < c->nsess; i++)
		if (!c->sess[i].used) {
			c->sess[i].used = true;
			u->session = i;
			u->client_id = i + 1;
			u->sid = c->sess[i].sid;
			break;
		}
	mtx_unlock(&c->lock);
	if (u->session < 0) {
		sx_destroy(&u->lock);
		free(u, M_FASTRPC);
		return (EBUSY);
	}
	if ((error = devfs_set_cdevpriv(u, user_release)) != 0)
		user_release(u);
	return (error);
}

static int
attach(struct frpc_user *u, int pd)
{
	struct fastrpc_invoke_args args[1];
	int client_id = u->client_id, error;

	args[0].ptr = (uintptr_t)&client_id;
	args[0].length = sizeof(client_id);
	args[0].fd = -1;
	args[0].attr = 0;
	u->pd = pd;
	error = invoke(u, true, FASTRPC_INIT_HANDLE,
	    SCALARS(RMID_INIT_ATTACH, 1, 0), args);
	if (error == 0)
		u->attached = true;
	return (error);
}

static int
req_mmap(struct frpc_user *u, struct fastrpc_req_mmap *req)
{
	struct fastrpc_invoke_args args[3];
	struct mmap_req_msg rq;
	struct mmap_rsp_msg rsp;
	struct phy_page page;
	struct frpc_buf *b;
	int error;

	if ((req->flags != ADSP_MMAP_ADD_PAGES &&
	    req->flags != ADSP_MMAP_REMOTE_HEAP_ADDR) || req->vaddrin != 0 ||
	    req->size == 0 || req->size > 256 * 1024 * 1024)
		return (EINVAL);
	/* the remote heap: by physical address, without the stream id */
	b = buf_alloc(req->size, req->flags == ADSP_MMAP_REMOTE_HEAP_ADDR ? 0 :
	    u->sid);
	if (b == NULL)
		return (ENOMEM);
	memset(&rq, 0, sizeof(rq));
	rq.client_id = u->client_id;
	rq.flags = req->flags;
	rq.vaddr = req->vaddrin;
	rq.num = sizeof(page);
	page.addr = b->dma;
	page.size = b->size;
	memset(&rsp, 0, sizeof(rsp));
	memset(args, 0, sizeof(args));
	args[0].ptr = (uintptr_t)&rq;
	args[0].length = sizeof(rq);
	args[1].ptr = (uintptr_t)&page;
	args[1].length = sizeof(page);
	args[2].ptr = (uintptr_t)&rsp;
	args[2].length = sizeof(rsp);
	args[0].fd = args[1].fd = args[2].fd = -1;
	error = invoke(u, true, FASTRPC_INIT_HANDLE,
	    SCALARS(RMID_INIT_MMAP, 2, 1), args);
	if (error != 0) {
		buf_free(b);
		return (error);
	}
	b->raddr = rsp.vaddr;
	req->vaddrout = rsp.vaddr;
	sx_xlock(&u->lock);
	LIST_INSERT_HEAD(&u->mmaps, b, link);
	sx_xunlock(&u->lock);
	return (0);
}

static int
req_munmap(struct frpc_user *u, struct fastrpc_req_munmap *req)
{
	struct fastrpc_invoke_args args[1];
	struct munmap_req_msg rq;
	struct frpc_buf *b;
	int error;

	sx_xlock(&u->lock);
	LIST_FOREACH(b, &u->mmaps, link)
		if (b->raddr == req->vaddrout && b->size == round_page(req->size))
			break;
	if (b == NULL) {
		sx_xunlock(&u->lock);
		return (EINVAL);
	}
	memset(&rq, 0, sizeof(rq));
	rq.client_id = u->client_id;
	rq.vaddr = b->raddr;
	rq.size = b->size;
	memset(args, 0, sizeof(args));
	args[0].ptr = (uintptr_t)&rq;
	args[0].length = sizeof(rq);
	args[0].fd = -1;
	error = invoke(u, true, FASTRPC_INIT_HANDLE,
	    SCALARS(RMID_INIT_MUNMAP, 1, 0), args);
	if (error == 0) {
		LIST_REMOVE(b, link);
		buf_free(b);
	}
	sx_xunlock(&u->lock);
	return (error);
}

static int
frpc_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag,
    struct thread *td)
{
	struct fastrpc_invoke *inv;
	struct fastrpc_invoke_args *args;
	struct frpc_user *u;
	u_int n;
	int error;

	if ((error = devfs_get_cdevpriv((void **)&u)) != 0)
		return (error);
	switch (cmd) {
	case FASTRPC_IOCTL_INVOKE:
		inv = (struct fastrpc_invoke *)data;
		n = SC_LENGTH(inv->sc);
		args = NULL;
		if (n != 0) {
			args = malloc(n * sizeof(*args), M_FASTRPC, M_WAITOK);
			error = copyin((void *)(uintptr_t)inv->args, args,
			    n * sizeof(*args));
		}
		if (error == 0)
			error = invoke(u, false, inv->handle, inv->sc, args);
		free(args, M_FASTRPC);
		return (error);
	case FASTRPC_IOCTL_INIT_ATTACH:
		return (attach(u, ROOT_PD));
	case FASTRPC_IOCTL_INIT_ATTACH_SNS:
		return (attach(u, SENSORS_PD));
	case FASTRPC_IOCTL_MMAP:
		return (req_mmap(u, (struct fastrpc_req_mmap *)data));
	case FASTRPC_IOCTL_MUNMAP:
		return (req_munmap(u, (struct fastrpc_req_munmap *)data));
	default:
		/* dynamic processes (INIT_CREATE) and dma-bufs: not here */
		return (ENOTTY);
	}
}

static struct cdevsw frpc_cdevsw = {
	.d_version =	D_VERSION,
	.d_open =	frpc_open,
	.d_ioctl =	frpc_ioctl,
	.d_name =	"fastrpc",
};

/* --- channels ---------------------------------------------------------------------- */

/* The qcom,fastrpc node in the glink-edge with this label */
static phandle_t
find_node(phandle_t start, const char *edge_label)
{
	phandle_t child, found;
	char label[32], compat[64];
	ssize_t n;

	for (child = OF_child(start); child != 0; child = OF_peer(child)) {
		n = OF_getprop(child, "compatible", compat, sizeof(compat) - 1);
		if (n > 0) {
			compat[n] = '\0';
			if (strcmp(compat, "qcom,fastrpc") == 0 &&
			    OF_getprop(OF_parent(child), "label", label,
			    sizeof(label)) > 0 && strcmp(label, edge_label) == 0)
				return (child);
		}
		if ((found = find_node(child, edge_label)) != 0)
			return (found);
	}
	return (0);
}

/* The sessions: the compute-cb children, their streams untranslated */
static void
chan_sessions(struct frpc_chan *c)
{
	phandle_t child;
	pcell_t reg, iommus[3];

	for (child = OF_child(c->node); child != 0 &&
	    c->nsess < FASTRPC_MAX_SESSIONS; child = OF_peer(child)) {
		if (!ofw_bus_node_is_compatible(child, "qcom,fastrpc-compute-cb"))
			continue;
		if (OF_getencprop(child, "reg", &reg, sizeof(reg)) <= 0)
			reg = 0;
		if (OF_getencprop(child, "iommus", iommus, sizeof(iommus)) ==
		    sizeof(iommus) &&
		    qcom_smmu_bypass(iommus[0], iommus[1], iommus[2], c->label)
		    != 0)
			continue;
		c->sess[c->nsess].sid = reg;
		c->nsess++;
	}
}

/* A DSP opened fastrpcglink-apps-dsp (the glink users' task queue) */
static void
fastrpc_announced(void *arg, struct glink_edge *edge, const char *label,
    const char *channel)
{
	struct frpc_chan *c;
	struct glink_channel *ch;
	phandle_t node;
	char flabel[16];
	int error;

	if ((node = find_node(OF_finddevice("/"), label)) == 0)
		return;
	if (OF_getprop(node, "label", flabel, sizeof(flabel)) <= 0)
		strlcpy(flabel, label, sizeof(flabel));
	mtx_lock(&chans_lock);
	LIST_FOREACH(c, &chans, link)
		if (c->node == node)
			break;
	mtx_unlock(&chans_lock);
	if (c == NULL) {
		c = malloc(sizeof(*c), M_FASTRPC, M_WAITOK | M_ZERO);
		c->node = node;
		strlcpy(c->label, flabel, sizeof(c->label));
		mtx_init(&c->lock, "fastrpc chan", NULL, MTX_DEF);
		chan_sessions(c);
		c->cdev = make_dev(&frpc_cdevsw, 0, UID_ROOT, GID_WHEEL, 0660,
		    "fastrpc-%s", c->label);
		c->cdev->si_drv1 = c;
		mtx_lock(&chans_lock);
		LIST_INSERT_HEAD(&chans, c, link);
		mtx_unlock(&chans_lock);
	}
	error = glink_open(edge, channel, 5, 512, chan_rx, chan_state, c, 5000,
	    &ch);
	if (error != 0) {
		printf("fastrpc: %s: open %s: %d\n", c->label, channel, error);
		return;
	}
	mtx_lock(&c->lock);
	c->ch = ch;
	mtx_unlock(&c->lock);
	printf("fastrpc: /dev/fastrpc-%s, %d sessions\n", c->label, c->nsess);
}

void
qcom_fastrpc_init(void)
{
	qcom_glink_register("fastrpcglink-apps-dsp", fastrpc_announced, NULL);
}
