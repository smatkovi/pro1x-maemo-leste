/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The simulation's hardware: physical memory regions (SMEM, the TCSR
 * mutex, the APCS IPC register, the DSPs' carveouts, rmtfs), SMEM as the
 * boot loader lays it out (Linux's smem.c structures: version 12, a table
 * of partitions at the end, a global partition and one per pair with the
 * apps processor, items uncached from the front), the doorbells, and
 * TrustZone's secure monitor calls as Linux's qcom_scm makes them: the PAS
 * calls check the metadata, the memory setup and every loaded segment
 * against the image's hash segment before the remote starts; MP_ASSIGN
 * checks the current owners of the rmtfs region and moves it. TrustZone
 * can interrupt a call once (x0 = 1, resumed with the session in x6) or
 * be busy once (-12), as Qualcomm's monitor does.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "sim.h"
#include "simfw.h"

#undef printf
#define	printf	sim_printf

/* --- regions ------------------------------------------------------------------------ */

static struct region {
	uint64_t	pa, size;
	char		*va;
	char		name[24];
} regions[32];
static int nregions;
static pthread_mutex_t region_lock = PTHREAD_MUTEX_INITIALIZER;

void *
sim_region_add(uint64_t pa, uint64_t size, const char *name)
{
	struct region *r;

	pthread_mutex_lock(&region_lock);
	r = &regions[nregions++];
	r->pa = pa;
	r->size = size;
	r->va = mmap(NULL, size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	snprintf(r->name, sizeof(r->name), "%s", name);
	pthread_mutex_unlock(&region_lock);
	return (r->va);
}

void *
sim_phys(uint64_t pa, uint64_t len)
{
	int i;

	for (i = 0; i < nregions; i++)
		if (pa >= regions[i].pa && pa + len <= regions[i].pa + regions[i].size)
			return (regions[i].va + (pa - regions[i].pa));
	/* a kernel buffer (kmem_alloc_contig): its address is its own */
	return ((void *)(uintptr_t)pa);
}

void *
pmap_mapdev(uint64_t pa, uint64_t size)
{
	int i;

	for (i = 0; i < nregions; i++)
		if (pa >= regions[i].pa && pa + size <= regions[i].pa + regions[i].size)
			return (regions[i].va + (pa - regions[i].pa));
	printf("sim: unknown physical range %#jx+%#jx mapped (scratch)\n",
	    (uintmax_t)pa, (uintmax_t)size);
	return (sim_region_add(pa, size, "scratch"));
}

void *
pmap_mapdev_attr(uint64_t pa, uint64_t size, int ma)
{
	return (pmap_mapdev(pa, size));
}

void
pmap_unmapdev(void *va, uint64_t size)
{
}

uint64_t
pmap_kextract(uintptr_t va)
{
	int i;

	for (i = 0; i < nregions; i++)
		if ((char *)va >= regions[i].va &&
		    (char *)va < regions[i].va + regions[i].size)
			return (regions[i].pa + ((char *)va - regions[i].va));
	return (va);
}

/* --- SMEM as XBL leaves it ------------------------------------------------------------ */

#define	SMEM_PA		0x46000000ULL
#define	SMEM_SIZE	0x200000ULL
#define	TCSR_PA		0x00340000ULL
#define	APCS_PA		0x0f111000ULL
#define	RMTFS_PA	0x89b01000ULL
#define	RMTFS_SIZE	0x200000ULL

struct smem_ptable_entry {
	uint32_t	offset, size, flags;
	uint16_t	host0, host1;
	uint32_t	cacheline, reserved[7];
};
struct smem_part_hdr {
	char		magic[4];
	uint16_t	host0, host1;
	uint32_t	size, offset_free_uncached, offset_free_cached;
	uint32_t	reserved[3];
};
struct smem_private_entry {
	uint16_t	canary, item;
	uint32_t	size;
	uint16_t	padding_data, padding_hdr;
	uint32_t	reserved;
};

static char *smem;
static volatile uint32_t *tcsr_lock3;
static const struct {
	uint16_t	host0, host1;
	uint32_t	offset, size;
} parts[] = {
	{ 0xfffe, 0xfffe, 0x010000, 0x080000 },	/* global */
	{ 0, 1, 0x090000, 0x040000 },		/* the modem */
	{ 0, 2, 0x0d0000, 0x030000 },		/* the ADSP */
	{ 0, 5, 0x100000, 0x030000 },		/* the CDSP */
	{ 1, 2, 0x130000, 0x010000 },		/* not ours: passed over */
};

void
sim_smem_boot(void)
{
	struct smem_ptable_entry *e;
	struct smem_part_hdr *h;
	uint32_t *u, *pt;
	size_t i, n = sizeof(parts) / sizeof(parts[0]);

	memset(smem, 0, SMEM_SIZE);
	u = (uint32_t *)smem;
	u[16 + 7] = 12 << 16 | 3;		/* version[SBL] */
	u[16 + 32] = 1;				/* initialized */
	/* the table of contents at the end, then "SIII" */
	pt = (uint32_t *)(smem + SMEM_SIZE - 4096);
	memcpy(pt, "$TOC", 4);
	pt[1] = 1;
	pt[2] = n;
	e = (struct smem_ptable_entry *)(pt + 8);
	for (i = 0; i < n; i++) {
		e[i].offset = parts[i].offset;
		e[i].size = parts[i].size;
		e[i].host0 = parts[i].host0;
		e[i].host1 = parts[i].host1;
		e[i].cacheline = 64;
		h = (struct smem_part_hdr *)(smem + parts[i].offset);
		memcpy(h->magic, "$PRT", 4);
		h->host0 = parts[i].host0;
		h->host1 = parts[i].host1;
		h->size = parts[i].size;
		h->offset_free_uncached = sizeof(*h);
		h->offset_free_cached = parts[i].size;
	}
	memcpy(&e[n], "SIII", 4);
	((uint16_t *)&e[n])[8] = 512;		/* num_items */
	/* the SoC's identity (item 137 in the global partition) */
	{
		uint32_t *si = sim_smem_alloc(-1, 137, 44);

		si[0] = 0x0f;
		si[1] = 417;			/* SM6115 */
		si[2] = 0x10000;
	}
}

static struct smem_part_hdr *
part_hdr(int host)
{
	size_t i;

	for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
		if (host >= 0 ? (parts[i].host0 == 0 && parts[i].host1 == host) :
		    parts[i].host0 == 0xfffe)
			return ((struct smem_part_hdr *)(smem + parts[i].offset));
	return (NULL);
}

/* The remote's allocation (Linux's qcom_smem_alloc_private), under the
 * TCSR mutex 3 as the remote takes it (id 2..6: never ours, 1) */
void *
sim_smem_alloc(int host, unsigned item, size_t size)
{
	struct smem_part_hdr *h = part_hdr(host);
	struct smem_private_entry *e;
	char *p, *end;
	void *r = NULL;

	if (h == NULL)
		return (NULL);
	while (__atomic_load_n(tcsr_lock3, __ATOMIC_ACQUIRE) != 0)
		;
	*tcsr_lock3 = 7;
	end = (char *)h + h->offset_free_uncached;
	for (p = (char *)(h + 1); p < end;
	    p += sizeof(*e) + e->padding_hdr + e->size) {
		e = (struct smem_private_entry *)p;
		if (e->item == item) {
			r = p + sizeof(*e) + e->padding_hdr;
			goto out;
		}
	}
	e = (struct smem_private_entry *)p;
	e->canary = 0xa5a5;
	e->item = item;
	e->size = (size + 7) & ~7;
	e->padding_data = e->size - size;
	e->padding_hdr = 0;
	memset(e + 1, 0, e->size);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	h->offset_free_uncached += sizeof(*e) + e->size;
	r = e + 1;
out:
	__atomic_store_n(tcsr_lock3, 0, __ATOMIC_RELEASE);
	return (r);
}

void *
sim_smem_get(int host, unsigned item, size_t *size)
{
	struct smem_part_hdr *h = part_hdr(host);
	struct smem_private_entry *e;
	char *p, *end;

	if (h == NULL)
		return (NULL);
	end = (char *)h + h->offset_free_uncached;
	for (p = (char *)(h + 1); p < end;
	    p += sizeof(*e) + e->padding_hdr + e->size) {
		e = (struct smem_private_entry *)p;
		if (e->canary != 0xa5a5)
			return (NULL);
		if (e->item == item) {
			if (size != NULL)
				*size = e->size - e->padding_data;
			return (p + sizeof(*e) + e->padding_hdr);
		}
	}
	return (NULL);
}

/* --- doorbells ------------------------------------------------------------------------- */

static struct {
	sim_doorbell_cb_t *cb;
	void		*arg;
} bells[32];

void
sim_doorbell_listen(int bit, sim_doorbell_cb_t *cb, void *arg)
{
	bells[bit].cb = cb;
	bells[bit].arg = arg;
}

/* qcom_ipc_kick's write to APCS IPC (0xf111008) */
void
qcom_dsp_sim_doorbell(uint64_t reg, uint32_t bits)
{
	int b;

	if (reg != APCS_PA + 8) {
		printf("sim: a doorbell write to %#jx, not APCS IPC\n",
		    (uintmax_t)reg);
		return;
	}
	for (b = 0; b < 32; b++)
		if ((bits & (1U << b)) && bells[b].cb != NULL)
			bells[b].cb(bells[b].arg, b);
}

/* --- TrustZone -------------------------------------------------------------------------- */

struct sim_tz sim_tz;
static pthread_mutex_t tz_lock = PTHREAD_MUTEX_INITIALIZER;

static struct pas {
	uint32_t	id;
	uint64_t	mem, size;
	int		state;		/* 0 off, 1 image, 2 memory, 3 running */
	struct simfw_meta meta;		/* from the metadata */
	uint64_t	reloc_size;
} pas[] = {
	{ 1, 0x53800000ULL, 0x2800000ULL },	/* ADSP: memory@53800000 */
	{ 4, 0x4ab00000ULL, 0x6900000ULL },	/* modem: memory@4ab00000 */
	{ 18, 0x51a00000ULL, 0x1e00000ULL },	/* CDSP: memory@51a00000 */
};

static struct pas *
pas_of(uint32_t id)
{
	size_t i;

	for (i = 0; i < sizeof(pas) / sizeof(pas[0]); i++)
		if (pas[i].id == id)
			return (&pas[i]);
	return (NULL);
}

#define	SVC_PIL		0x02
#define	SVC_INFO	0x06
#define	SVC_MP		0x0c
#define	R_EINVAL	((uint64_t)-2)
#define	R_EOPNOTSUPP	((uint64_t)-4)
#define	R_EBUSY		((uint64_t)-12)

static bool
known(uint32_t svc, uint32_t cmd)
{
	return ((svc == SVC_PIL && (cmd == 1 || cmd == 2 || cmd == 5 ||
	    cmd == 6 || cmd == 7)) || (svc == SVC_MP && cmd == 0x16) ||
	    (svc == SVC_INFO && cmd == 1));
}

static void
pil(uint32_t cmd, const uint64_t *arg, uint64_t res[4])
{
	struct pas *p = pas_of((uint32_t)arg[0]);
	int i;

	res[1] = 0;
	if (p == NULL) {
		res[0] = R_EINVAL;
		return;
	}
	switch (cmd) {
	case 7:		/* IS_SUPPORTED */
		res[1] = 1;
		break;
	case 1:		/* INIT_IMAGE: ELF and program headers, the hash */
		sim_tz.init_image++;
		if (simfw_parse_meta(sim_phys(arg[1], 4096), &p->meta) != 0) {
			printf("tz: PAS %u: metadata refused\n", p->id);
			res[1] = 1;
			break;
		}
		p->state = 1;
		p->reloc_size = 0;
		break;
	case 2:		/* MEM_SETUP: the relocated image's place */
		sim_tz.mem_setup++;
		if (p->state != 1 || arg[1] != p->mem || arg[2] > p->size ||
		    arg[2] < p->meta.span) {
			printf("tz: PAS %u: memory %#jx+%#jx refused\n", p->id,
			    (uintmax_t)arg[1], (uintmax_t)arg[2]);
			res[1] = 1;
			break;
		}
		p->reloc_size = arg[2];
		p->state = 2;
		break;
	case 5:		/* AUTH_AND_RESET: every segment as the hash says */
		sim_tz.auth_reset++;
		if (p->state != (p->meta.relocatable ? 2 : 1)) {
			res[1] = 1;
			break;
		}
		i = simfw_check(&p->meta, sim_phys(p->mem, p->size), p->size,
		    p->meta.relocatable ? p->meta.min : p->mem);
		sim_tz.auth_fail = i;
		if (i != 0) {
			printf("tz: PAS %u: segment %d does not match its hash\n",
			    p->id, i);
			res[1] = 1;
			break;
		}
		p->state = 3;
		sim_remote_boot(p->id);
		break;
	case 6:		/* SHUTDOWN */
		sim_tz.shutdown++;
		if (p->state == 3)
			sim_remote_shutdown(p->id);
		p->state = 0;
		break;
	default:
		res[0] = R_EOPNOTSUPP;
	}
}

struct vmperm {
	uint32_t	vmid, perm;
	uint64_t	ctx;
	uint32_t	ctx_size, unused;
};

static void
assign(const uint64_t *arg, uint32_t arginfo, uint64_t res[4])
{
	const uint64_t *map = sim_phys(arg[0], 16);
	const uint32_t *src = sim_phys(arg[2], arg[3]);
	const struct vmperm *dst = sim_phys(arg[4], arg[5]);
	int nsrc = arg[3] / 4, ndst = arg[5] / sizeof(*dst), i, j, ok;

	sim_tz.assign++;
	res[1] = 1;
	/* Linux's QCOM_SCM_ARGS(7, RO, VAL, RO, VAL, RO, VAL, VAL) */
	if (arginfo != (7 | 1 << 4 | 1 << 8 | 1 << 12) || arg[1] != 16 ||
	    map[0] != RMTFS_PA || map[1] != RMTFS_SIZE || nsrc < 1 ||
	    ndst < 1 || ndst > 4) {
		printf("tz: assign: request malformed\n");
		return;
	}
	/* the source must be who owns it now, no more, no less */
	ok = nsrc == sim_tz.nowners;
	for (i = 0; ok && i < nsrc; i++) {
		for (j = 0; j < sim_tz.nowners; j++)
			if (src[i] == sim_tz.rmtfs_owners[j])
				break;
		ok = j < sim_tz.nowners;
	}
	if (!ok) {
		printf("tz: assign: the source VMIDs are not the owners\n");
		return;
	}
	for (i = 0; i < ndst; i++)
		sim_tz.rmtfs_owners[i] = dst[i].vmid;
	sim_tz.nowners = ndst;
	res[1] = 0;
}

static struct {
	bool		pending;
	uint64_t	a[8];
} interrupted;

/* The SMC (qcom_scm.c's scm_smc): x0..x7 in, x0..x3 and x6 out */
void
qcom_dsp_sim_smc(uint64_t a[8], uint64_t *x6, uint64_t res[4])
{
	uint64_t arg[10], *ext, fn;
	uint32_t svc, cmd, nargs;
	int i;

	pthread_mutex_lock(&tz_lock);
	memset(res, 0, 4 * sizeof(*res));
	sim_tz.calls++;
	if (a[0] == 1) {
		/* the resumption of an interrupted call */
		if (!interrupted.pending || a[6] != 0x5e55) {
			res[0] = R_EINVAL;
			goto out;
		}
		interrupted.pending = false;
		memcpy(a, interrupted.a, sizeof(interrupted.a));
	}
	fn = a[0];
	if ((fn >> 30 & 1) == 0 || (fn >> 24 & 0x3f) != 2) {
		res[0] = R_EOPNOTSUPP;	/* only SMC64, SiP */
		goto out;
	}
	svc = fn >> 8 & 0xff;
	cmd = fn & 0xff;
	nargs = a[1] & 0xf;
	for (i = 0; i < 4 && i < (int)nargs; i++)
		arg[i] = a[2 + i];
	if (nargs > 4) {
		/* Linux's layout: x5 points to the 4th argument on */
		ext = sim_phys(a[5], 7 * 8);
		for (i = 3; i < (int)nargs; i++)
			arg[i] = ext[i - 3];
	}
	if (svc == SVC_PIL && sim_tz.busy_next) {
		sim_tz.busy_next = 0;
		sim_tz.busy++;
		res[0] = R_EBUSY;
		goto out;
	}
	if (svc == SVC_PIL && sim_tz.interrupt_next) {
		sim_tz.interrupt_next = 0;
		sim_tz.interrupted++;
		interrupted.pending = true;
		memcpy(interrupted.a, a, sizeof(interrupted.a));
		res[0] = 1;
		*x6 = 0x5e55;
		goto out;
	}
	if (svc == SVC_INFO && cmd == 1) {
		res[1] = known(arg[0] >> 8 & 0xff, arg[0] & 0xff) &&
		    (arg[0] >> 24 & 0x3f) == 2;
	} else if (svc == SVC_PIL)
		pil(cmd, arg, res);
	else if (svc == SVC_MP && cmd == 0x16 && nargs == 7)
		assign(arg, a[1], res);
	else
		res[0] = R_EOPNOTSUPP;
out:
	pthread_mutex_unlock(&tz_lock);
}

void
sim_hw_init(void)
{
	smem = sim_region_add(SMEM_PA, SMEM_SIZE, "smem");
	tcsr_lock3 = (uint32_t *)((char *)sim_region_add(TCSR_PA, 0x20000,
	    "tcsr-mutex") + 3 * 0x1000);
	sim_region_add(APCS_PA, 0x1000, "apcs");
	sim_region_add(0x53800000ULL, 0x2800000ULL, "adsp");
	sim_region_add(0x4ab00000ULL, 0x6900000ULL, "modem");
	sim_region_add(0x51a00000ULL, 0x1e00000ULL, "cdsp");
	sim_region_add(RMTFS_PA, RMTFS_SIZE, "rmtfs");
	sim_tz.rmtfs_owners[0] = 3;		/* HLOS, as after a boot */
	sim_tz.nowners = 1;
	sim_smem_boot();
}
