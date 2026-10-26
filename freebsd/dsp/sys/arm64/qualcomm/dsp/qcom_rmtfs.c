/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The modem's remote file system memory (Linux's rmtfs_mem.c): a reserved
 * region ("qcom,rmtfs-mem") the modem and rmtfs share, through which the
 * modem reads and writes its EFS partitions. The region is shared with the
 * modem's VMIDs (qcom,vmid) through TrustZone, and rmtfs reaches it as
 * /dev/qcom_rmtfs_mem<client-id> (read, write, mmap). Linux's sysfs
 * attributes are sysctls here: hw.qcom_rmtfs.phys_addr, size, client_id.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "qcom_dsp.h"

static struct {
	vm_paddr_t	pa;
	vm_size_t	size;
	uint32_t	client_id;
	char		*va;
	struct cdev	*cdev;
} rmtfs;

static SYSCTL_NODE(_hw, OID_AUTO, qcom_rmtfs, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "The modem's remote file system memory");
SYSCTL_U64(_hw_qcom_rmtfs, OID_AUTO, phys_addr, CTLFLAG_RD, &rmtfs.pa, 0,
    "Physical address");
SYSCTL_U64(_hw_qcom_rmtfs, OID_AUTO, size, CTLFLAG_RD, &rmtfs.size, 0,
    "Size in bytes");
SYSCTL_U32(_hw_qcom_rmtfs, OID_AUTO, client_id, CTLFLAG_RD, &rmtfs.client_id,
    0, "rmtfs client id");

static int
rmtfs_rw(struct cdev *dev, struct uio *uio, int flags)
{
	size_t len;

	if (uio->uio_offset < 0)
		return (EINVAL);
	if ((uint64_t)uio->uio_offset >= rmtfs.size)
		return (0);
	len = MIN(uio->uio_resid, rmtfs.size - uio->uio_offset);
	return (uiomove(rmtfs.va + uio->uio_offset, len, uio));
}

static int
rmtfs_mmap(struct cdev *dev, vm_ooffset_t offset, vm_paddr_t *paddr,
    int nprot, vm_memattr_t *memattr)
{
	if (offset < 0 || (uint64_t)offset >= rmtfs.size)
		return (EINVAL);
	*paddr = rmtfs.pa + offset;
	*memattr = VM_MEMATTR_WRITE_COMBINING;
	return (0);
}

static struct cdevsw rmtfs_cdevsw = {
	.d_version =	D_VERSION,
	.d_read =	rmtfs_rw,
	.d_write =	rmtfs_rw,
	.d_mmap =	rmtfs_mmap,
	.d_name =	"qcom_rmtfs_mem",
};

void
qcom_rmtfs_init(void)
{
	uint32_t vmids[3], perms[3], vm[2];
	phandle_t node, rmem;
	pcell_t reg[4];
	ssize_t len;
	int i, nvm, error;

	rmem = OF_finddevice("/reserved-memory");
	if (rmem <= 0 ||
	    (node = ofw_bus_find_compatible(rmem, "qcom,rmtfs-mem")) <= 0)
		return;
	if (OF_getencprop(node, "reg", reg, sizeof(reg)) != sizeof(reg) ||
	    OF_getencprop(node, "qcom,client-id", &rmtfs.client_id,
	    sizeof(rmtfs.client_id)) <= 0) {
		printf("qcom_rmtfs: incomplete node\n");
		return;
	}
	rmtfs.pa = (vm_paddr_t)reg[0] << 32 | reg[1];
	rmtfs.size = (vm_size_t)reg[2] << 32 | reg[3];
	rmtfs.va = pmap_mapdev_attr(rmtfs.pa, rmtfs.size,
	    VM_MEMATTR_WRITE_COMBINING);

	/* this processor and the modem's VMIDs, all read-write */
	vmids[0] = QCOM_SCM_VMID_HLOS;
	perms[0] = QCOM_SCM_PERM_RW;
	len = OF_getencprop(node, "qcom,vmid", vm, sizeof(vm));
	/* Linux takes at most two */
	nvm = len > 0 ? MIN(len / (ssize_t)sizeof(pcell_t), nitems(vm)) : 0;
	for (i = 0; i < nvm; i++) {
		vmids[i + 1] = vm[i];
		perms[i + 1] = QCOM_SCM_PERM_RW;
	}
	if (nvm > 0) {
		error = qcom_scm_assign_mem(rmtfs.pa, rmtfs.size, vmids, perms,
		    nvm + 1);
		printf("qcom_rmtfs: %#jx+%#jx shared with VMIDs %u,%u: %d\n",
		    (uintmax_t)rmtfs.pa, (uintmax_t)rmtfs.size, vm[0],
		    nvm > 1 ? vm[1] : 0, error);
	}
	rmtfs.cdev = make_dev(&rmtfs_cdevsw, 0, UID_ROOT, GID_WHEEL, 0600,
	    "qcom_rmtfs_mem%u", rmtfs.client_id);
}
