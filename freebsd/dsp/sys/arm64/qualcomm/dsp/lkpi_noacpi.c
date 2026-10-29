/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ath10k's mac.c reads a regulatory domain from ACPI (the WRDD method)
 * when its device has an ACPI handle. On a kernel without ACPI (the
 * Pro1X's, a device tree machine) LinuxKPI gives no handle, so ath10k
 * returns before it calls ACPICA; these stand in for ACPICA's functions so
 * that the module links (freebsd/dsp, modules/ath10k_snoc).
 */

#include "opt_acpi.h"

#ifndef DEV_ACPI
#include <sys/param.h>
#include <sys/systm.h>

#include <contrib/dev/acpica/include/acpi.h>

ACPI_STATUS
AcpiGetHandle(ACPI_HANDLE Parent, const char *Pathname, ACPI_HANDLE *RetHandle)
{
	return (AE_NOT_FOUND);
}

ACPI_STATUS
AcpiEvaluateObject(ACPI_HANDLE Object, ACPI_STRING Pathname,
    ACPI_OBJECT_LIST *ParameterObjects, ACPI_BUFFER *ReturnObjectBuffer)
{
	return (AE_NOT_FOUND);
}
#endif
