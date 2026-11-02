/*-
 * SPDX-License-Identifier: GPL-2.0-only
 * Linux's RPMh command DB: only a real GMU (a6xx_gmu.c's RPMh votes) reads
 * it; the SM6115 has an RPM, not RPMh (freebsd/gpu/msm-port.md)
 */
#ifndef	_MSM_LKPI_SOC_QCOM_CMD_DB_H
#define	_MSM_LKPI_SOC_QCOM_CMD_DB_H
#include <linux/err.h>
#include <linux/types.h>
static inline const void *
cmd_db_read_aux_data(const char *resource_id, size_t *len)
{
	return (ERR_PTR(-ENODEV));
}
static inline u32
cmd_db_read_addr(const char *resource_id)
{
	return (0);
}
#endif
