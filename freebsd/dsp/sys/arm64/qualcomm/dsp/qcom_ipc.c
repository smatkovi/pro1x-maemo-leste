/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The APCS IPC doorbells ("qcom,sm6115-apcs-hmss-global", Linux's
 * qcom-apcs-ipc-mailbox): writing bit N of the register at offset 8 raises
 * an interrupt at another processor. Users name a doorbell as Linux's
 * mailbox users do, mboxes = <&apcs_glb N>. Each block is mapped once.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/cpufunc.h>

#include <dev/ofw/openfirm.h>

#include "qcom_dsp.h"

#define	APCS_IPC_OFFSET		8
#define	APCS_MAX		4

static struct {
	struct mtx	lock;
	struct {
		phandle_t	xref;
		volatile uint32_t *va;
	} blk[APCS_MAX];
} ipc;

MTX_SYSINIT(qcom_ipc_lock, &ipc.lock, "qcom_ipc", MTX_DEF);

int
qcom_ipc_get(phandle_t node, int index, phandle_t *xref, u_int *bit)
{
	pcell_t cells[8];
	ssize_t len;

	len = OF_getencprop(node, "mboxes", cells, sizeof(cells));
	if (len < (ssize_t)((index + 1) * 2 * sizeof(pcell_t)))
		return (ENXIO);
	*xref = cells[index * 2];
	*bit = cells[index * 2 + 1];
	return (*bit < 32 ? 0 : EINVAL);
}

static volatile uint32_t *
ipc_map(phandle_t xref)
{
	volatile uint32_t *va;
	pcell_t reg[4];
	vm_paddr_t pa;
	int i;

	mtx_lock(&ipc.lock);
	for (i = 0; i < APCS_MAX && ipc.blk[i].xref != 0; i++)
		if (ipc.blk[i].xref == xref) {
			va = ipc.blk[i].va;
			mtx_unlock(&ipc.lock);
			return (va);
		}
	va = NULL;
	if (i < APCS_MAX && OF_getencprop(OF_node_from_xref(xref), "reg", reg,
	    sizeof(reg)) == sizeof(reg)) {
		pa = (vm_paddr_t)reg[0] << 32 | reg[1];
		va = pmap_mapdev(pa, PAGE_SIZE);
		ipc.blk[i].xref = xref;
		ipc.blk[i].va = va;
	}
	mtx_unlock(&ipc.lock);
	return (va);
}

int
qcom_ipc_kick(phandle_t xref, u_int bit)
{
	volatile uint32_t *va;

	if ((va = ipc_map(xref)) == NULL)
		return (ENXIO);
	/* what the other side is told about is in memory first */
	dsb(sy);
	va[APCS_IPC_OFFSET / 4] = 1U << bit;
	return (0);
}
