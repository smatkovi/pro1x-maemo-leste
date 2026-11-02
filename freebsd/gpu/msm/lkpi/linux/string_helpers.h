/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * LinuxKPI's string helpers and kstrdup_quotable_cmdline() for msm's hang
 * report: the process's name (no command line arguments here).
 * (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_LINUX_STRING_HELPERS_H
#define	_MSM_LKPI_LINUX_STRING_HELPERS_H
#include_next <linux/string_helpers.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
static inline char *
kstrdup_quotable_cmdline(struct task_struct *task, gfp_t gfp)
{
	return (kstrdup(task->comm, gfp));
}
#endif
