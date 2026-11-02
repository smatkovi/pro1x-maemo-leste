/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * LinuxKPI's linux/mm.h and what msm's GEM fault and VRAM pages use beyond
 * it: vmf_insert_pfn() (LinuxKPI calls a driver's fault with the VM object
 * unlocked: the locked insert under the object's lock), vmf_error(),
 * arm64's __phys_to_pfn(). (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_MM_H
#define	_MSM_LKPI_LINUX_MM_H

#include_next <linux/mm.h>

#ifndef __phys_to_pfn
#define	__phys_to_pfn(paddr)	((unsigned long)((paddr) >> PAGE_SHIFT))
#endif
#ifndef __pfn_to_phys
#define	__pfn_to_phys(pfn)	((phys_addr_t)(pfn) << PAGE_SHIFT)
#endif

static inline vm_fault_t
vmf_error(int err)
{
	if (err == -ENOMEM)
		return (VM_FAULT_OOM);
	return (VM_FAULT_SIGBUS);
}

static inline vm_fault_t
vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr,
    unsigned long pfn)
{
	vm_fault_t ret;

	VM_OBJECT_WLOCK(vma->vm_obj);
	ret = lkpi_vmf_insert_pfn_prot_locked(vma, addr, pfn,
	    vma->vm_page_prot);
	VM_OBJECT_WUNLOCK(vma->vm_obj);
	return (ret);
}

#endif
