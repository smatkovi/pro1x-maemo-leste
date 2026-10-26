/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Qualcomm shared memory (SMEM), after Linux's qcom_smem.c: the boot loader
 * lays out a region all processors share (the "qcom,smem" reserved memory
 * node, 2 MB at 0x46000000 on the SM6115) with a table of partitions at its
 * end ("$TOC"): one global partition and one per pair of hosts (the
 * application processor is host 0, the ADSP 2, the modem 1, the CDSP 5).
 * A partition ("$PRT") holds items, found by number: uncached ones growing
 * from its start, cached ones from its end. Allocations take the TCSR
 * hardware mutex the node names (hwlocks = <&tcsr_mutex 3>), which the other
 * processors take too.
 *
 * Only the partitioned layout (SMEM version 12, the SM6115's) is handled.
 * The region is mapped once, uncached, when the first user asks for it.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/atomic.h>
#include <machine/cpufunc.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "qcom_dsp.h"

#define	SMEM_GLOBAL_PART_VERSION 12
#define	SMEM_SBL_VERSION_INDEX	7
#define	SMEM_ITEM_LAST_FIXED	8
#define	SMEM_ITEM_COUNT		512
#define	SMEM_HOST_APPS		0
#define	SMEM_GLOBAL_HOST	0xfffe
#define	SMEM_HOST_COUNT		25
#define	SMEM_PRIVATE_CANARY	0xa5a5
#define	SMEM_PTABLE_SIZE	4096

/* The TCSR mutex: one 4K page per lock; we are processor id 1 */
#define	HWLOCK_STRIDE		0x1000
#define	HWLOCK_APPS_PROC_ID	1
#define	HWLOCK_TIMEOUT_US	1000000

struct smem_global_entry {
	uint32_t	allocated;
	uint32_t	offset;
	uint32_t	size;
	uint32_t	aux_base;
};

struct smem_header {
	uint32_t	proc_comm[4][4];
	uint32_t	version[32];
	uint32_t	initialized;
	uint32_t	free_offset;
	uint32_t	available;
	uint32_t	reserved;
	struct smem_global_entry toc[SMEM_ITEM_COUNT];
};

struct smem_ptable_entry {
	uint32_t	offset;
	uint32_t	size;
	uint32_t	flags;
	uint16_t	host0;
	uint16_t	host1;
	uint32_t	cacheline;
	uint32_t	reserved[7];
};

struct smem_ptable {
	uint8_t		magic[4];	/* "$TOC" */
	uint32_t	version;
	uint32_t	num_entries;
	uint32_t	reserved[5];
	struct smem_ptable_entry entry[];
};

struct smem_info {
	uint8_t		magic[4];	/* "SIII" */
	uint32_t	size;
	uint32_t	base_addr;
	uint32_t	reserved;
	uint16_t	num_items;
};

struct smem_partition_header {
	uint8_t		magic[4];	/* "$PRT" */
	uint16_t	host0;
	uint16_t	host1;
	uint32_t	size;
	uint32_t	offset_free_uncached;
	uint32_t	offset_free_cached;
	uint32_t	reserved[3];
};

struct smem_private_entry {
	uint16_t	canary;
	uint16_t	item;
	uint32_t	size;		/* with the padding */
	uint16_t	padding_data;
	uint16_t	padding_hdr;
	uint32_t	reserved;
};

struct smem_part {
	struct smem_partition_header *hdr;
	size_t		size;
	size_t		cacheline;
};

static struct {
	struct mtx	lock;		/* our side of the hardware mutex */
	int		ready;
	vm_paddr_t	pa;
	vm_size_t	size;
	char		*va;
	volatile uint32_t *hwlock;	/* the mutex SMEM uses */
	u_int		item_count;
	struct smem_part global;
	struct smem_part part[SMEM_HOST_COUNT];
} smem;

MTX_SYSINIT(qcom_smem_lock, &smem.lock, "qcom_smem", MTX_SPIN);

/* --- the hardware mutex ---------------------------------------------------------- */

static int
hwlock_take(void)
{
	int us;

	for (us = 0; us < HWLOCK_TIMEOUT_US; us++) {
		*smem.hwlock = HWLOCK_APPS_PROC_ID;
		dsb(sy);
		if (*smem.hwlock == HWLOCK_APPS_PROC_ID)
			return (0);
		DELAY(1);
	}
	return (ETIMEDOUT);
}

static void
hwlock_release(void)
{
	dsb(sy);
	*smem.hwlock = 0;
}

/* The mutex of hwlocks = <&mutex N>: map its page */
static int
hwlock_map(phandle_t node)
{
	pcell_t cells[2];
	phandle_t mutex;
	pcell_t reg[4];
	vm_paddr_t pa;

	if (OF_getencprop(node, "hwlocks", cells, sizeof(cells)) != sizeof(cells))
		return (ENXIO);
	mutex = OF_node_from_xref(cells[0]);
	if (OF_getencprop(mutex, "reg", reg, sizeof(reg)) != sizeof(reg))
		return (ENXIO);
	pa = ((vm_paddr_t)reg[0] << 32 | reg[1]) + cells[1] * HWLOCK_STRIDE;
	smem.hwlock = pmap_mapdev(pa, PAGE_SIZE);
	return (smem.hwlock != NULL ? 0 : ENOMEM);
}

/* --- partitions ------------------------------------------------------------------ */

static struct smem_private_entry *
uncached_first(struct smem_partition_header *h)
{
	return ((struct smem_private_entry *)((char *)h + sizeof(*h)));
}

static void *
uncached_end(struct smem_partition_header *h)
{
	return ((char *)h + h->offset_free_uncached);
}

static struct smem_private_entry *
uncached_next(struct smem_private_entry *e)
{
	return ((struct smem_private_entry *)((char *)e + sizeof(*e) +
	    e->padding_hdr + e->size));
}

static void *
uncached_item(struct smem_private_entry *e)
{
	return ((char *)e + sizeof(*e) + e->padding_hdr);
}

static struct smem_private_entry *
cached_first(struct smem_partition_header *h, size_t cacheline)
{
	return ((struct smem_private_entry *)((char *)h + h->size -
	    roundup2(sizeof(struct smem_private_entry), cacheline)));
}

static void *
cached_end(struct smem_partition_header *h)
{
	return ((char *)h + h->offset_free_cached);
}

static struct smem_private_entry *
cached_next(struct smem_private_entry *e, size_t cacheline)
{
	return ((struct smem_private_entry *)((char *)e - e->size -
	    roundup2(sizeof(*e), cacheline)));
}

static void *
cached_item(struct smem_private_entry *e)
{
	return ((char *)e - e->size);
}

static struct smem_ptable *
ptable(void)
{
	struct smem_ptable *pt;

	pt = (struct smem_ptable *)(smem.va + smem.size - SMEM_PTABLE_SIZE);
	if (memcmp(pt->magic, "$TOC", 4) != 0 || pt->version != 1)
		return (NULL);
	return (pt);
}

static int
part_init(struct smem_part *p, struct smem_ptable_entry *e)
{
	struct smem_partition_header *h;

	if (e->offset + (uint64_t)e->size > smem.size)
		return (EINVAL);
	h = (struct smem_partition_header *)(smem.va + e->offset);
	if (memcmp(h->magic, "$PRT", 4) != 0 || h->host0 != e->host0 ||
	    h->host1 != e->host1 || h->size != e->size ||
	    h->offset_free_uncached > h->size)
		return (EINVAL);
	p->hdr = h;
	p->size = e->size;
	p->cacheline = MAX(e->cacheline, 8);
	return (0);
}

int
qcom_smem_init(void)
{
	struct smem_header *hdr;
	struct smem_ptable *pt;
	struct smem_ptable_entry *e;
	struct smem_info *info;
	phandle_t node, rmem;
	pcell_t reg[4];
	u_int i, version, remote;
	int error;

	if (atomic_load_acq_int(&smem.ready))
		return (0);
	rmem = OF_finddevice("/reserved-memory");
	node = rmem > 0 ? ofw_bus_find_compatible(rmem, "qcom,smem") : 0;
	if (node <= 0)
		node = ofw_bus_find_compatible(OF_finddevice("/"), "qcom,smem");
	if (node <= 0)
		return (ENXIO);
	if (OF_getencprop(node, "reg", reg, sizeof(reg)) != sizeof(reg))
		return (ENXIO);
	smem.pa = (vm_paddr_t)reg[0] << 32 | reg[1];
	smem.size = (vm_size_t)reg[2] << 32 | reg[3];
	if ((error = hwlock_map(node)) != 0)
		return (error);
	smem.va = pmap_mapdev_attr(smem.pa, smem.size, VM_MEMATTR_UNCACHEABLE);
	if (smem.va == NULL)
		return (ENOMEM);

	hdr = (struct smem_header *)smem.va;
	if (hdr->initialized != 1 || hdr->reserved != 0) {
		printf("qcom_smem: not initialised by the boot loader\n");
		return (ENXIO);
	}
	version = hdr->version[SMEM_SBL_VERSION_INDEX] >> 16;
	if (version != SMEM_GLOBAL_PART_VERSION) {
		printf("qcom_smem: version %u, only %u is supported\n", version,
		    SMEM_GLOBAL_PART_VERSION);
		return (ENXIO);
	}
	if ((pt = ptable()) == NULL) {
		printf("qcom_smem: no partition table\n");
		return (ENXIO);
	}
	smem.item_count = SMEM_ITEM_COUNT;
	info = (struct smem_info *)&pt->entry[pt->num_entries];
	if (memcmp(info->magic, "SIII", 4) == 0)
		smem.item_count = info->num_items;

	for (i = 0; i < pt->num_entries; i++) {
		e = &pt->entry[i];
		if (e->offset == 0 || e->size == 0)
			continue;
		if (e->host0 == SMEM_GLOBAL_HOST && e->host1 == SMEM_GLOBAL_HOST) {
			if ((error = part_init(&smem.global, e)) != 0)
				return (error);
			continue;
		}
		if (e->host0 == SMEM_HOST_APPS)
			remote = e->host1;
		else if (e->host1 == SMEM_HOST_APPS)
			remote = e->host0;
		else
			continue;
		if (remote >= SMEM_HOST_COUNT || smem.part[remote].hdr != NULL)
			return (EINVAL);
		if ((error = part_init(&smem.part[remote], e)) != 0)
			return (error);
	}
	if (smem.global.hdr == NULL) {
		printf("qcom_smem: no global partition\n");
		return (ENXIO);
	}
	atomic_store_rel_int(&smem.ready, 1);
	return (0);
}

static struct smem_part *
part_of(int host)
{
	if (host >= 0 && host < SMEM_HOST_COUNT && smem.part[host].hdr != NULL)
		return (&smem.part[host]);
	return (&smem.global);
}

static int
alloc_private(struct smem_part *p, u_int item, size_t size)
{
	struct smem_partition_header *h = p->hdr;
	struct smem_private_entry *e;
	char *end, *cached, *pend;
	size_t len;

	pend = (char *)h + p->size;
	end = uncached_end(h);
	cached = cached_end(h);
	if (end > pend || cached > pend)
		return (EINVAL);
	for (e = uncached_first(h); (char *)e < end; e = uncached_next(e)) {
		if (e->canary != SMEM_PRIVATE_CANARY)
			return (EINVAL);
		if (e->item == item)
			return (EEXIST);
	}
	len = sizeof(*e) + roundup2(size, 8);
	if ((char *)e + len > cached)
		return (ENOSPC);
	e->canary = SMEM_PRIVATE_CANARY;
	e->item = item;
	e->size = roundup2(size, 8);
	e->padding_data = e->size - size;
	e->padding_hdr = 0;
	/* the entry before the free offset that publishes it */
	wmb();
	h->offset_free_uncached += len;
	return (0);
}

int
qcom_smem_alloc(int host, u_int item, size_t size)
{
	int error;

	if ((error = qcom_smem_init()) != 0)
		return (error);
	if (item < SMEM_ITEM_LAST_FIXED || item >= smem.item_count)
		return (EINVAL);
	mtx_lock_spin(&smem.lock);
	if ((error = hwlock_take()) == 0) {
		error = alloc_private(part_of(host), item, size);
		hwlock_release();
	}
	mtx_unlock_spin(&smem.lock);
	return (error);
}

static void *
get_private(struct smem_part *p, u_int item, size_t *size, int *error)
{
	struct smem_partition_header *h = p->hdr;
	struct smem_private_entry *e;
	char *end, *pend;

	pend = (char *)h + p->size;
	end = uncached_end(h);
	for (e = uncached_first(h); (char *)e < end; e = uncached_next(e)) {
		if (e->canary != SMEM_PRIVATE_CANARY || (char *)e >= pend)
			goto bad;
		if (e->item == item) {
			if (e->size > p->size || e->padding_data > e->size)
				goto bad;
			if (size != NULL)
				*size = e->size - e->padding_data;
			return (uncached_item(e));
		}
	}
	end = cached_end(h);
	if ((char *)end > pend)
		goto bad;
	for (e = cached_first(h, p->cacheline); (char *)e > end;
	    e = cached_next(e, p->cacheline)) {
		if (e->canary != SMEM_PRIVATE_CANARY || (char *)e < (char *)h)
			goto bad;
		if (e->item == item) {
			if (e->size > p->size || e->padding_data > e->size)
				goto bad;
			if (size != NULL)
				*size = e->size - e->padding_data;
			return (cached_item(e));
		}
	}
	*error = ENOENT;
	return (NULL);
bad:
	printf("qcom_smem: bad entry in the partition of hosts %u:%u\n",
	    h->host0, h->host1);
	*error = EINVAL;
	return (NULL);
}

void *
qcom_smem_get(int host, u_int item, size_t *size, int *error)
{
	int dummy;

	if (error == NULL)
		error = &dummy;
	if ((*error = qcom_smem_init()) != 0)
		return (NULL);
	if (item >= smem.item_count) {
		*error = EINVAL;
		return (NULL);
	}
	return (get_private(part_of(host), item, size, error));
}

vm_paddr_t
qcom_smem_virt_to_phys(void *p)
{
	if (!smem.ready || (char *)p < smem.va || (char *)p >= smem.va + smem.size)
		return (0);
	return (smem.pa + ((char *)p - smem.va));
}

/* For the test runs: the partitions found */
void
qcom_smem_report(void)
{
	struct smem_partition_header *h;
	u_int i;

	if (qcom_smem_init() != 0) {
		printf("qcom_smem: not available\n");
		return;
	}
	printf("qcom_smem: %#jx, %ju KB, %u items\n", (uintmax_t)smem.pa,
	    (uintmax_t)smem.size / 1024, smem.item_count);
	h = smem.global.hdr;
	printf("qcom_smem: global partition %zu KB, %u bytes used\n",
	    smem.global.size / 1024, h->offset_free_uncached);
	for (i = 0; i < SMEM_HOST_COUNT; i++) {
		if ((h = smem.part[i].hdr) == NULL)
			continue;
		printf("qcom_smem: partition with host %u: %zu KB, %u bytes used\n",
		    i, smem.part[i].size / 1024, h->offset_free_uncached);
	}
}
