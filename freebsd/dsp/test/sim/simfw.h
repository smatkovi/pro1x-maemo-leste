/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The simulation's firmware images (simfw.c): signed ELF32 images laid out
 * as Qualcomm's (Linux's mdt_loader reads them): program header 0 covers
 * the ELF and program headers (the metadata's first part), one segment of
 * type hash (flags 2 << 24) holds the "signature", the rest are loadable,
 * relocatable (flag 1 << 27), with zero-filled tails. The hash here: per
 * loadable segment its index and an FNV-1a sum of its bytes, which
 * TrustZone (hw.c) checks in the processor's memory before the start.
 */

#ifndef _SIMFW_H_
#define	_SIMFW_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define	SIMFW_MAXSEG	16

struct simfw_meta {
	int		nseg;
	struct {
		int		index;
		uint32_t	paddr, filesz, memsz;
		uint64_t	hash;
	} seg[SIMFW_MAXSEG];
	uint64_t	min, span;	/* lowest address, 4K-aligned extent */
	bool		relocatable;
};

/* name: adsp, modem, cdsp; split: .mdt with .bNN files. 0 or errno */
int	simfw_write(const char *path, const char *name, bool split);
int	simfw_parse_meta(const void *meta, struct simfw_meta *m);
/* 0, or the program header index of the first segment that differs */
int	simfw_check(const struct simfw_meta *m, const char *mem, uint64_t size,
	    uint64_t base);

#endif /* _SIMFW_H_ */
