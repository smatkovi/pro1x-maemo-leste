/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-cam's arithmetic without hardware (also built by the host tests).
 */

#include <sys/types.h>
#include <stdint.h>

uint32_t pm8008_grid_uv(u_int ldo, uint32_t min_uv);

/*
 * The PM8008 LDO voltage the regulator core picks for a minimum:
 * qcom-pm8008-regulator.c's linear ranges, NLDO (LDO 1-2) 528 mV + 8 mV a
 * step, PLDO (LDO 3-7) 1504 mV + 8 mV, the lowest step at or above it.
 */
uint32_t
pm8008_grid_uv(u_int ldo, uint32_t min_uv)
{
	uint32_t base = ldo <= 2 ? 528000 : 1504000, sel;

	if (min_uv <= base)
		return (base);
	sel = (min_uv - base + 7999) / 8000;
	return (base + sel * 8000);
}
