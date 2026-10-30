/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Qualcomm secure monitor calls (SCM) in the ARM SMC calling convention, as
 * Linux's qcom_scm-smc.c and qcom_scm.c do them: function id with the SIP
 * owner and (service << 8 | command), x1 the argument description, x2..x5
 * the arguments. Only what the remote processors need: the PAS ("peripheral
 * authentication service") calls that hand signed firmware to TrustZone,
 * which checks it and starts the processor.
 *
 * Qualcomm's monitor may interrupt a call (x0 = 1); it is then resumed with
 * x0 = 1 and the session id it returned in x6. It may be busy (x0 = -12):
 * retried after 30 ms. Linux also votes the crypto engine's clock and
 * interconnect around the PAS calls; FreeBSD has no driver for those and
 * relies on what the boot left on (freebsd/dsp/README.md, risks).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/malloc.h>
#include <sys/sx.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>

#include "qcom_dsp.h"

#define	SMCCC_YIELDING		0
#define	SMCCC_SMC64		(1U << 30)
#define	SMCCC_OWNER_SIP		(2U << 24)
#define	SCM_FNID(svc, cmd)	((((svc) & 0xff) << 8) | ((cmd) & 0xff))
#define	SCM_CALL(svc, cmd)	(SMCCC_YIELDING | SMCCC_SMC64 | SMCCC_OWNER_SIP | \
				    SCM_FNID(svc, cmd))
/* x1: number of arguments, then two bits of type each (0 value, 1 ro, 2 rw) */
#define	SCM_ARGS(n)		((n) & 0xf)
#define	SCM_ARG_RO(i)		(1U << (4 + 2 * (i)))
#define	SCM_ARG_RW(i)		(2U << (4 + 2 * (i)))
/* x2..x5 hold four arguments; with more, x5 points to the 4th on */
#define	SCM_REG_ARGS		4
#define	SCM_MAX_ARGS		10

#define	SCM_SVC_PIL		0x02
#define	SCM_PAS_INIT_IMAGE	0x01
#define	SCM_PAS_MEM_SETUP	0x02
#define	SCM_PAS_AUTH_AND_RESET	0x05
#define	SCM_PAS_SHUTDOWN	0x06
#define	SCM_PAS_IS_SUPPORTED	0x07
#define	SCM_SVC_INFO		0x06
#define	SCM_INFO_IS_CALL_AVAIL	0x01
#define	SCM_SVC_MP		0x0c
#define	SCM_MP_ASSIGN		0x16
#define	SCM_VMID_HLOS		0x3

#define	SCM_INTERRUPTED		1
#define	SCM_WAITQ_SLEEP		2
#define	SCM_V2_EBUSY		(-12)
#define	SCM_EBUSY_WAIT_MS	30
#define	SCM_EBUSY_RETRIES	20

struct scm_res {
	uint64_t	x0, x1, x2, x3;
};

static struct sx scm_lock;
SX_SYSINIT(qcom_scm_lock, &scm_lock, "qcom_scm");

/* One SMC with all eight argument registers; x6 comes back too */
#ifdef QCOM_DSP_SIM
/* the simulation's TrustZone (freebsd/dsp/test/sim/hw.c) */
static void
scm_smc(uint64_t a[8], uint64_t *x6, struct scm_res *res)
{
	uint64_t r[4];

	qcom_dsp_sim_smc(a, x6, r);
	res->x0 = r[0];
	res->x1 = r[1];
	res->x2 = r[2];
	res->x3 = r[3];
}
#else
static void
scm_smc(uint64_t a[8], uint64_t *x6, struct scm_res *res)
{
	register uint64_t x0 __asm("x0") = a[0];
	register uint64_t x1 __asm("x1") = a[1];
	register uint64_t x2 __asm("x2") = a[2];
	register uint64_t x3 __asm("x3") = a[3];
	register uint64_t x4 __asm("x4") = a[4];
	register uint64_t x5 __asm("x5") = a[5];
	register uint64_t r6 __asm("x6") = a[6];
	register uint64_t x7 __asm("x7") = a[7];

	__asm __volatile("smc #0"
	    : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5),
	      "+r" (r6), "+r" (x7)
	    :
	    : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16",
	      "x17", "memory");
	res->x0 = x0;
	res->x1 = x1;
	res->x2 = x2;
	res->x3 = x3;
	*x6 = r6;
}
#endif

/*
 * A call; *result is x1 of the answer. Beyond four arguments the 4th and
 * the rest go in a buffer TrustZone reads, its address in x5.
 */
static int
scm_call(u_int svc, u_int cmd, uint32_t arginfo, const uint64_t *args,
    int nargs, uint64_t *result)
{
	struct scm_res res;
	uint64_t a[8], x6, *ext = NULL;
	int64_t status;
	int i, retries;

	KASSERT(nargs <= SCM_MAX_ARGS, ("scm_call: %d arguments", nargs));
	if (nargs > SCM_REG_ARGS) {
		ext = kmem_alloc_contig(PAGE_SIZE, M_WAITOK | M_ZERO, 0,
		    ~(vm_paddr_t)0, PAGE_SIZE, 0, VM_MEMATTR_UNCACHEABLE);
		if (ext == NULL)
			return (ENOMEM);
		for (i = SCM_REG_ARGS - 1; i < nargs; i++)
			ext[i - (SCM_REG_ARGS - 1)] = args[i];
	}
	retries = 0;
	sx_xlock(&scm_lock);
	for (;;) {
		memset(a, 0, sizeof(a));
		a[0] = SCM_CALL(svc, cmd);
		a[1] = arginfo;
		for (i = 0; i < MIN(nargs, SCM_REG_ARGS); i++)
			a[2 + i] = args[i];
		if (ext != NULL)
			a[2 + SCM_REG_ARGS - 1] = pmap_kextract((vm_offset_t)ext);
		x6 = 0;
		for (;;) {
			scm_smc(a, &x6, &res);
			if (res.x0 != SCM_INTERRUPTED)
				break;
			/* resume the interrupted call with its session */
			a[0] = SCM_INTERRUPTED;
			a[6] = x6;
		}
		status = (int64_t)res.x0;
		if (status != SCM_V2_EBUSY || ++retries > SCM_EBUSY_RETRIES)
			break;
		sx_xunlock(&scm_lock);
		pause("qcomscm", MAX(1, SCM_EBUSY_WAIT_MS * hz / 1000));
		sx_xlock(&scm_lock);
	}
	sx_xunlock(&scm_lock);
	if (ext != NULL)
		kmem_free(ext, PAGE_SIZE);
	if (result != NULL)
		*result = res.x1;
	switch (status) {
	case 0:
		return (0);
	case SCM_WAITQ_SLEEP:
		/* wait queues (newer SoCs) are not implemented */
		return (EOPNOTSUPP);
	case -2:	/* EINVAL_ARG */
	case -3:	/* EINVAL_ADDR */
		return (EINVAL);
	case -4:	/* EOPNOTSUPP */
		return (EOPNOTSUPP);
	case -5:	/* ENOMEM */
		return (ENOMEM);
	case SCM_V2_EBUSY:
		return (EBUSY);
	default:
		return (EIO);
	}
}

static int
scm_is_call_available(u_int svc, u_int cmd, bool *avail)
{
	uint64_t arg, result;
	int error;

	arg = SCM_FNID(svc, cmd) | SMCCC_OWNER_SIP;
	error = scm_call(SCM_SVC_INFO, SCM_INFO_IS_CALL_AVAIL, SCM_ARGS(1),
	    &arg, 1, &result);
	*avail = error == 0 && result != 0;
	return (error);
}

/* Whether the monitor answers in the SMC64 convention at all */
int
qcom_scm_available(void)
{
	bool avail;

	return (scm_is_call_available(SCM_SVC_PIL, SCM_PAS_IS_SUPPORTED,
	    &avail));
}

int
qcom_scm_pas_supported(uint32_t pas_id, bool *supported)
{
	uint64_t arg, result;
	bool avail;
	int error;

	*supported = false;
	error = scm_is_call_available(SCM_SVC_PIL, SCM_PAS_IS_SUPPORTED, &avail);
	if (error != 0 || !avail)
		return (error != 0 ? error : EOPNOTSUPP);
	arg = pas_id;
	error = scm_call(SCM_SVC_PIL, SCM_PAS_IS_SUPPORTED, SCM_ARGS(1), &arg, 1,
	    &result);
	*supported = error == 0 && result != 0;
	return (error);
}

/* metadata: physical address of the ELF header and hash segment */
int
qcom_scm_pas_init_image(uint32_t pas_id, vm_paddr_t metadata)
{
	uint64_t args[2], result;
	int error;

	args[0] = pas_id;
	args[1] = metadata;
	error = scm_call(SCM_SVC_PIL, SCM_PAS_INIT_IMAGE,
	    SCM_ARGS(2) | SCM_ARG_RW(1), args, 2, &result);
	return (error != 0 ? error : (result != 0 ? EIO : 0));
}

int
qcom_scm_pas_mem_setup(uint32_t pas_id, vm_paddr_t addr, vm_size_t size)
{
	uint64_t args[3], result;
	int error;

	args[0] = pas_id;
	args[1] = addr;
	args[2] = size;
	error = scm_call(SCM_SVC_PIL, SCM_PAS_MEM_SETUP, SCM_ARGS(3), args, 3,
	    &result);
	return (error != 0 ? error : (result != 0 ? EIO : 0));
}

int
qcom_scm_pas_auth_and_reset(uint32_t pas_id)
{
	uint64_t arg, result;
	int error;

	arg = pas_id;
	error = scm_call(SCM_SVC_PIL, SCM_PAS_AUTH_AND_RESET, SCM_ARGS(1), &arg,
	    1, &result);
	return (error != 0 ? error : (result != 0 ? EIO : 0));
}

int
qcom_scm_pas_shutdown(uint32_t pas_id)
{
	uint64_t arg, result;
	int error;

	arg = pas_id;
	error = scm_call(SCM_SVC_PIL, SCM_PAS_SHUTDOWN, SCM_ARGS(1), &arg, 1,
	    &result);
	return (error != 0 ? error : (result != 0 ? EIO : 0));
}

/*
 * Memory shared between VMs (Linux's qcom_scm_assign_mem): TrustZone reads
 * the current owners (source VMIDs), the region and the new owners with
 * their permissions from one buffer, each part at a 64-byte boundary.
 */
struct scm_vmperm_info {
	uint32_t	vmid;
	uint32_t	perm;
	uint64_t	ctx;
	uint32_t	ctx_size;
	uint32_t	unused;
};

struct scm_mem_map_info {
	uint64_t	addr;
	uint64_t	size;
};

int
qcom_scm_assign_mem_from(vm_paddr_t addr, vm_size_t size,
    const uint32_t *srcvm, int nsrc, const uint32_t *vmids,
    const uint32_t *perms, int n)
{
	struct scm_vmperm_info *dst;
	struct scm_mem_map_info *map;
	uint32_t *src;
	uint64_t args[7], result;
	vm_paddr_t pa;
	char *buf;
	int i, error;

	if (n < 1 || n > 8 || nsrc < 1 || nsrc > 8)
		return (EINVAL);
	/* src at 0, the region at 64, the destinations at 128 */
	buf = kmem_alloc_contig(PAGE_SIZE, M_WAITOK | M_ZERO, 0, ~(vm_paddr_t)0,
	    PAGE_SIZE, 0, VM_MEMATTR_UNCACHEABLE);
	if (buf == NULL)
		return (ENOMEM);
	pa = pmap_kextract((vm_offset_t)buf);
	src = (uint32_t *)buf;
	for (i = 0; i < nsrc; i++)
		src[i] = srcvm[i];
	map = (struct scm_mem_map_info *)(buf + 64);
	map->addr = addr;
	map->size = size;
	dst = (struct scm_vmperm_info *)(buf + 128);
	for (i = 0; i < n; i++) {
		dst[i].vmid = vmids[i];
		dst[i].perm = perms[i];
	}
	args[0] = pa + 64;
	args[1] = sizeof(*map);
	args[2] = pa;
	args[3] = nsrc * sizeof(uint32_t);
	args[4] = pa + 128;
	args[5] = n * sizeof(*dst);
	args[6] = 0;
	error = scm_call(SCM_SVC_MP, SCM_MP_ASSIGN, SCM_ARGS(7) | SCM_ARG_RO(0) |
	    SCM_ARG_RO(2) | SCM_ARG_RO(4), args, 7, &result);
	kmem_free(buf, PAGE_SIZE);
	return (error != 0 ? error : (result != 0 ? EIO : 0));
}

/* From this processor (HLOS) only: what Linux's callers here do */
int
qcom_scm_assign_mem(vm_paddr_t addr, vm_size_t size, const uint32_t *vmids,
    const uint32_t *perms, int n)
{
	const uint32_t hlos = SCM_VMID_HLOS;

	return (qcom_scm_assign_mem_from(addr, size, &hlos, 1, vmids, perms,
	    n));
}
