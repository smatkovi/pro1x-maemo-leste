/* SPDX-License-Identifier: GPL-2.0-only */
/* An entry of regs.def: a register's offset and stride, or a field's mask */
#ifndef _TEST_REGS_H_
#define	_TEST_REGS_H_
#include <stdint.h>
struct reg_entry {
	const char	*name;
	int		missing;
	uint32_t	off, stride, mask;
};
int	linux_regs(struct reg_entry *);
int	my_regs(struct reg_entry *);
#endif
