/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Every register offset, stride and field mask of ipa_regs.h (regs.def)
 * against Linux 7.1's tables for IPA v4.2 and its GSI.
 */
#include <stdio.h>
#include <string.h>
#include "test_regs.h"

static struct reg_entry lx[512], my[512];

int
main(void)
{
	int n = linux_regs(lx), m = my_regs(my), i, bad = 0;

	if (n != m) {
		printf("regs: %d entries from Linux, %d here\n", n, m);
		return (1);
	}
	for (i = 0; i < n; i++) {
		if (strcmp(lx[i].name, my[i].name) != 0 || lx[i].missing) {
			printf("regs: %s: %s in Linux's table\n", my[i].name,
			    lx[i].missing ? "not" : "named differently");
			bad++;
			continue;
		}
		if (lx[i].off != my[i].off || lx[i].stride != my[i].stride ||
		    lx[i].mask != my[i].mask) {
			printf("regs: %s: Linux %#x/%#x/%#x, here %#x/%#x/%#x\n",
			    my[i].name, lx[i].off, lx[i].stride, lx[i].mask,
			    my[i].off, my[i].stride, my[i].mask);
			bad++;
		}
	}
	printf("regs: %d registers and fields compared with Linux's tables: %s\n",
	    n, bad ? "MISMATCH" : "ok");
	return (bad != 0);
}
