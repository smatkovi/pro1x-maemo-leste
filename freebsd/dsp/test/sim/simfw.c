/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The simulation's firmware images and TrustZone's check of them
 * (simfw.h).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simfw.h"

struct ehdr {
	uint8_t		e_ident[16];
	uint16_t	e_type, e_machine;
	uint32_t	e_version, e_entry, e_phoff, e_shoff, e_flags;
	uint16_t	e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum,
			e_shstrndx;
};

struct phdr {
	uint32_t	p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
			p_flags, p_align;
};

#define	PT_NULL		0
#define	PT_LOAD		1
#define	MDT_HASH	(2U << 24)
#define	MDT_RELOC	(1U << 27)

struct hashseg {
	char		magic[8];	/* "SIMHASH1" */
	uint32_t	n, pad;
	struct {
		uint32_t	index, pad;
		uint64_t	sum;
	} e[SIMFW_MAXSEG];
};

static uint64_t
fnv(const uint8_t *p, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ULL;

	while (n-- > 0)
		h = (h ^ *p++) * 0x100000001b3ULL;
	return (h);
}

/* The segments of each image: address, bytes in the file, in memory */
static const struct {
	const char	*name;
	uint32_t	base;
	struct { uint32_t off, filesz, memsz; } s[4];
	int		n;
} layouts[] = {
	{ "adsp", 0x5f000000, { { 0, 0x3000, 0x3000 }, { 0x4000, 0x1800, 0x5000 },
	    { 0xa000, 0, 0x2000 } }, 3 },
	{ "modem", 0x40000000, { { 0, 0x8000, 0x8000 }, { 0x10000, 0x2345, 0x9000 },
	    { 0x20000, 0x1000, 0x1000 }, { 0x30000, 0, 0x4000 } }, 4 },
	{ "cdsp", 0x5a000000, { { 0, 0x2000, 0x2000 } }, 1 },
};

int
simfw_write(const char *path, const char *name, bool split)
{
	struct ehdr eh;
	struct phdr ph[8];
	struct hashseg hs;
	uint8_t *seg[4];
	size_t hdrsz, off;
	char p2[1024];
	FILE *f;
	int L, i, n, nph;

	for (L = 0; L < 3 && strcmp(layouts[L].name, name) != 0; L++)
		;
	if (L == 3)
		return (EINVAL);
	n = layouts[L].n;
	nph = 2 + n + 1;	/* headers, hash, loadable, one empty PT_LOAD */
	hdrsz = sizeof(eh) + nph * sizeof(struct phdr);
	memset(&eh, 0, sizeof(eh));
	memcpy(eh.e_ident, "\177ELF\1\1\1", 7);
	eh.e_type = 2;
	eh.e_machine = 164;	/* Hexagon */
	eh.e_version = 1;
	eh.e_phoff = sizeof(eh);
	eh.e_ehsize = sizeof(eh);
	eh.e_phentsize = sizeof(struct phdr);
	eh.e_phnum = nph;
	memset(ph, 0, sizeof(ph));
	ph[0].p_type = PT_NULL;
	ph[0].p_filesz = hdrsz;
	ph[1].p_type = PT_NULL;
	ph[1].p_flags = MDT_HASH;
	ph[1].p_offset = 0x1000;
	ph[1].p_filesz = sizeof(hs);
	memset(&hs, 0, sizeof(hs));
	memcpy(hs.magic, "SIMHASH1", 8);
	off = 0x2000;
	for (i = 0; i < n; i++) {
		struct phdr *p = &ph[2 + i];
		size_t k;

		p->p_type = PT_LOAD;
		p->p_flags = MDT_RELOC | 5;
		p->p_paddr = layouts[L].base + layouts[L].s[i].off;
		p->p_vaddr = p->p_paddr;
		p->p_filesz = layouts[L].s[i].filesz;
		p->p_memsz = layouts[L].s[i].memsz;
		p->p_offset = p->p_filesz != 0 ? off : 0;
		seg[i] = malloc(p->p_filesz + 1);
		for (k = 0; k < p->p_filesz; k++)
			seg[i][k] = (uint8_t)(k * 31 + i * 7 + L * 101 + (k >> 8));
		off += (p->p_filesz + 0xfff) & ~0xfffu;
		hs.e[hs.n].index = 2 + i;
		hs.e[hs.n].sum = fnv(seg[i], p->p_filesz);
		hs.n++;
	}
	/* a loadable segment with nothing in memory: passed over */
	ph[2 + n].p_type = PT_LOAD;
	ph[2 + n].p_paddr = layouts[L].base;
	if ((f = fopen(path, "wb")) == NULL)
		return (errno);
	fwrite(&eh, sizeof(eh), 1, f);
	fwrite(ph, sizeof(struct phdr), nph, f);
	if (split) {
		/* .mdt: the headers and the hash right after them */
		fwrite(&hs, sizeof(hs), 1, f);
		fclose(f);
		for (i = 0; i < n; i++) {
			if (ph[2 + i].p_filesz == 0)
				continue;
			snprintf(p2, sizeof(p2), "%.*sb%02d", (int)strlen(path) - 3,
			    path, 2 + i);
			if ((f = fopen(p2, "wb")) == NULL)
				return (errno);
			fwrite(seg[i], 1, ph[2 + i].p_filesz, f);
			fclose(f);
		}
	} else {
		fseek(f, ph[1].p_offset, SEEK_SET);
		fwrite(&hs, sizeof(hs), 1, f);
		for (i = 0; i < n; i++)
			if (ph[2 + i].p_filesz != 0) {
				fseek(f, ph[2 + i].p_offset, SEEK_SET);
				fwrite(seg[i], 1, ph[2 + i].p_filesz, f);
			}
		fclose(f);
	}
	for (i = 0; i < n; i++)
		free(seg[i]);
	return (0);
}

/* TrustZone's INIT_IMAGE: the metadata Linux's qcom_mdt_read_metadata makes */
int
simfw_parse_meta(const void *meta, struct simfw_meta *m)
{
	const struct ehdr *eh = meta;
	const struct phdr *ph;
	struct hashseg hsbuf, *hs = &hsbuf;
	uint64_t max = 0;
	int i, j, hash = -1;

	memset(m, 0, sizeof(*m));
	if (memcmp(eh->e_ident, "\177ELF\1", 5) != 0 || eh->e_phnum < 2 ||
	    eh->e_phnum > 8 || eh->e_phentsize != sizeof(*ph))
		return (EINVAL);
	ph = (const struct phdr *)((const char *)meta + eh->e_phoff);
	for (i = 1; i < eh->e_phnum; i++)
		if ((ph[i].p_flags & (7U << 24)) == MDT_HASH)
			hash = i;
	if (hash < 0 || ph[0].p_type == PT_LOAD)
		return (EINVAL);
	/* (right after the headers: not aligned for its 64-bit sums) */
	memcpy(hs, (const char *)meta + ph[0].p_filesz, sizeof(*hs));
	if (memcmp(hs->magic, "SIMHASH1", 8) != 0 || hs->n > SIMFW_MAXSEG)
		return (EINVAL);
	m->min = UINT64_MAX;
	for (j = 0; j < (int)hs->n; j++) {
		i = hs->e[j].index;
		if (i >= eh->e_phnum || ph[i].p_type != PT_LOAD)
			return (EINVAL);
		m->seg[j].index = i;
		m->seg[j].paddr = ph[i].p_paddr;
		m->seg[j].filesz = ph[i].p_filesz;
		m->seg[j].memsz = ph[i].p_memsz;
		m->seg[j].hash = hs->e[j].sum;
		if (ph[i].p_flags & MDT_RELOC)
			m->relocatable = true;
		if (ph[i].p_paddr < m->min)
			m->min = ph[i].p_paddr;
		if (ph[i].p_paddr + (uint64_t)ph[i].p_memsz > max)
			max = (ph[i].p_paddr + (uint64_t)ph[i].p_memsz + 0xfff) &
			    ~0xfffULL;
	}
	m->nseg = hs->n;
	m->span = max - m->min;
	return (0);
}

int
simfw_check(const struct simfw_meta *m, const char *mem, uint64_t size,
    uint64_t base)
{
	const char *p;
	uint64_t off;
	uint32_t k;
	int i;

	for (i = 0; i < m->nseg; i++) {
		off = m->seg[i].paddr - base;
		if (m->seg[i].paddr < base || off + m->seg[i].memsz > size)
			return (m->seg[i].index);
		p = mem + off;
		if (fnv((const uint8_t *)p, m->seg[i].filesz) != m->seg[i].hash)
			return (m->seg[i].index);
		for (k = m->seg[i].filesz; k < m->seg[i].memsz; k++)
			if (p[k] != 0)
				return (m->seg[i].index);
	}
	return (0);
}
