/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's ELF32 file and program headers by their struct names, for
 * mdt_loader.c (the zap shader's .mbn is a 32-bit ELF); LinuxKPI's
 * linux/elf.h is empty. (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_ELF_H
#define	_MSM_LKPI_LINUX_ELF_H

#include <linux/types.h>

#define	EI_NIDENT	16

struct elf32_hdr {
	unsigned char	e_ident[EI_NIDENT];
	u16		e_type;
	u16		e_machine;
	u32		e_version;
	u32		e_entry;
	u32		e_phoff;
	u32		e_shoff;
	u32		e_flags;
	u16		e_ehsize;
	u16		e_phentsize;
	u16		e_phnum;
	u16		e_shentsize;
	u16		e_shnum;
	u16		e_shstrndx;
};

struct elf32_phdr {
	u32	p_type;
	u32	p_offset;
	u32	p_vaddr;
	u32	p_paddr;
	u32	p_filesz;
	u32	p_memsz;
	u32	p_flags;
	u32	p_align;
};

#ifndef PT_LOAD
#define	PT_LOAD		1
#endif

#endif
