/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * regs.def read out of Linux 7.1's own tables (reg/ipa_reg-v4.2.c,
 * reg/gsi_reg-v4.0.c, compiled unchanged from the Linux tree).
 */
#include <linux/types.h>
#include "ipa_version.h"
#include "reg.h"
#include "ipa_reg.h"
#include "gsi_reg.h"
#include "test_regs.h"

static const struct reg *
lreg(const struct regs *regs, unsigned int id)
{
	return (id < regs->reg_count ? regs->reg[id] : NULL);
}

static void
ent_reg(struct reg_entry *e, const char *name, const struct reg *r)
{
	e->name = name;
	e->missing = r == NULL;
	e->off = r ? r->offset : 0;
	e->stride = r ? r->stride : 0;
}

static void
ent_field(struct reg_entry *e, const char *name, const struct reg *r,
    unsigned int f)
{
	e->name = name;
	e->missing = r == NULL || f >= r->fcount;
	e->mask = e->missing ? 0 : r->fmask[f];
}

int
linux_regs(struct reg_entry *e)
{
	int n = 0;

#define	IR(r, m)	ent_reg(&e[n++], #r, lreg(&ipa_regs_v4_2, r));
#define	IS(r, m, s)	ent_reg(&e[n++], #r, lreg(&ipa_regs_v4_2, r));
#define	IX(r, m)	ent_reg(&e[n++], #r, lreg(&ipa_regs_v4_2, r));
#define	IE(r, m)	ent_reg(&e[n++], #r, lreg(&ipa_regs_v4_2, r));
#define	IF(r, f, m)	ent_field(&e[n++], #r "." #f, lreg(&ipa_regs_v4_2, r), f);
#define	GR(r, m)	ent_reg(&e[n++], #r, lreg(&gsi_regs_v4_0, r));
#define	GS(r, m)	ent_reg(&e[n++], #r, lreg(&gsi_regs_v4_0, r));
#define	GF(r, f, m)	ent_field(&e[n++], #r "." #f, lreg(&gsi_regs_v4_0, r), f);
#include "regs.def"
	return (n);
}
