/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * regs.def from freebsd/data's ipa_regs.h
 */
#include <stdint.h>
#include "ipa_regs.h"
#include "test_regs.h"

int
my_regs(struct reg_entry *e)
{
	int n = 0;

#define	ENT(nm, o, s, m) e[n].name = (nm); e[n].missing = 0; e[n].off = (o); \
	e[n].stride = (s); e[n].mask = (m); n++;
#define	IR(r, m)	ENT(#r, m, 0, 0)
#define	IS(r, m, s)	ENT(#r, m, s, 0)
#define	IX(r, m)	ENT(#r, m(0), m(1) - m(0), 0)
#define	IE(r, m)	ENT(#r, IPA_ENDP(m, 0), IPA_ENDP(m, 1) - IPA_ENDP(m, 0), 0)
#define	IF(r, f, m)	ENT(#r "." #f, 0, 0, m)
#define	GR(r, m)	ENT(#r, m, 0, 0)
#define	GS(r, m)	ENT(#r, m(0), m(1) - m(0), 0)
#define	GF(r, f, m)	ENT(#r "." #f, 0, 0, m)
#include "regs.def"
	return (n);
}
