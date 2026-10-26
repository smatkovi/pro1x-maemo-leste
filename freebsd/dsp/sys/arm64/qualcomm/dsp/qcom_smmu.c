/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Streams through the SoC's SMMU (ARM MMU-500, "apps_smmu") without
 * translation, for devices whose memory FreeBSD hands out by physical
 * address: the DSPs' FastRPC context banks. FreeBSD has no SMMU driver.
 * Qualcomm's hypervisor does not allow bypass streams (S2CR type bypass);
 * Linux's arm-smmu-qcom uses a context bank with translation off instead,
 * and so does this: a stream (SID/mask of the node's "iommus") that already
 * has a stream match entry gets translation turned off in its context bank
 * (as qcom_dpu does for the display); a new one gets a free match entry and
 * a free context bank (from the top, Linux allocates from 0) with SCTLR 0.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/bus.h>
#include <machine/cpufunc.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_subr.h>

#include "qcom_dsp.h"

#define	SMMU_IDR0		0x020
#define	SMMU_IDR1		0x024
#define	SMMU_SMR(n)		(0x800 + 4 * (n))
#define	SMMU_S2CR(n)		(0xc00 + 4 * (n))
#define	SMMU_CBAR(n)		(4 * (n))		/* in GR1 */
#define	SMR_VALID		(1U << 31)
#define	S2CR_TYPE(x)		(((x) >> 16) & 0x3)
#define	S2CR_TYPE_TRANS		0
#define	CBAR_TYPE_S1_BYPASS_S2	(1U << 16)
#define	CB_SCTLR		0x000

static struct mtx smmu_lock;
MTX_SYSINIT(qcom_smmu_lock, &smmu_lock, "qcom_smmu", MTX_DEF);

static volatile uint32_t *
reg(vm_offset_t va, vm_size_t off)
{
	return ((volatile uint32_t *)(va + off));
}

/*
 * The stream (sid, mask) of the SMMU node xref without translation. 0, or
 * an error when no match entry or context bank is left.
 */
int
qcom_smmu_bypass(phandle_t xref, uint32_t sid, uint32_t mask, const char *who)
{
	bus_addr_t pa;
	bus_size_t size;
	vm_offset_t va, gr1, cbbase;
	uint32_t idr0, idr1, smr, s2cr, used[8];
	u_int i, nsmr, ncb, pgshift, numpage, cb, freesmr;
	int error = 0;

	if (ofw_reg_to_paddr(OF_node_from_xref(xref), 0, &pa, &size, NULL) != 0)
		return (ENXIO);
	va = (vm_offset_t)pmap_mapdev(pa, size);
	idr0 = *reg(va, SMMU_IDR0);
	idr1 = *reg(va, SMMU_IDR1);
	nsmr = idr0 & 0xff;
	ncb = idr1 & 0xff;
	pgshift = (idr1 & (1U << 31)) ? 16 : 12;
	numpage = 1U << (((idr1 >> 28) & 0x7) + 1);
	gr1 = va + ((vm_size_t)1 << pgshift);
	cbbase = va + ((vm_size_t)numpage << pgshift);

	mtx_lock(&smmu_lock);
	memset(used, 0, sizeof(used));
	freesmr = nsmr;
	for (i = 0; i < nsmr; i++) {
		smr = *reg(va, SMMU_SMR(i));
		if ((smr & SMR_VALID) == 0) {
			if (freesmr == nsmr)
				freesmr = i;
			continue;
		}
		s2cr = *reg(va, SMMU_S2CR(i));
		if (S2CR_TYPE(s2cr) == S2CR_TYPE_TRANS && (s2cr & 0xff) < 256)
			used[(s2cr & 0xff) / 32] |= 1U << ((s2cr & 0xff) % 32);
		if (((smr ^ sid) & 0x7fff & ~(mask | (smr >> 16))) != 0)
			continue;
		/* already matched: translation off in its context bank */
		if (S2CR_TYPE(s2cr) != S2CR_TYPE_TRANS) {
			printf("qcom_smmu: %s: stream %#x has S2CR %#x\n", who,
			    sid, s2cr);
			goto out;
		}
		cb = s2cr & 0xff;
		*reg(cbbase, ((vm_size_t)cb << pgshift) + CB_SCTLR) = 0;
		printf("qcom_smmu: %s: stream %#x, context bank %u, translation "
		    "off\n", who, sid, cb);
		goto out;
	}
	/* a new match entry and a free context bank */
	for (cb = ncb; cb-- > 0; )
		if ((used[cb / 32] & (1U << (cb % 32))) == 0)
			break;
	if (freesmr == nsmr || cb >= ncb) {
		printf("qcom_smmu: %s: stream %#x: no free match entry or "
		    "context bank\n", who, sid);
		error = ENOSPC;
		goto out;
	}
	*reg(cbbase, ((vm_size_t)cb << pgshift) + CB_SCTLR) = 0;
	*reg(gr1, SMMU_CBAR(cb)) = CBAR_TYPE_S1_BYPASS_S2;
	*reg(va, SMMU_S2CR(freesmr)) = S2CR_TYPE_TRANS << 16 | cb;
	dsb(sy);
	*reg(va, SMMU_SMR(freesmr)) = SMR_VALID | (mask & 0x7fff) << 16 |
	    (sid & 0x7fff);
	dsb(sy);
	printf("qcom_smmu: %s: stream %#x: match entry %u, context bank %u "
	    "(SMR %#x), translation off\n", who, sid, freesmr, cb,
	    *reg(va, SMMU_SMR(freesmr)));
out:
	mtx_unlock(&smmu_lock);
	pmap_unmapdev((void *)va, size);
	return (error);
}
