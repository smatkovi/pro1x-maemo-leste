/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The module qcom_dsp: SCM, SMEM, the IPC doorbells, SMP2P and the remote
 * processors (freebsd/dsp/README.md). Loading it reports what was found,
 * for the test runs on the phone: whether the secure monitor answers and
 * SMEM's partitions. The drivers (SMP2P, PAS) attach to their device tree
 * nodes; nothing is started until the loader asks.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "qcom_dsp.h"

static int
qcom_dsp_modevent(module_t mod, int what, void *arg)
{
	int error;

	switch (what) {
	case MOD_LOAD:
		/* a secure monitor call elsewhere may be an undefined instruction */
		if (!ofw_bus_node_is_compatible(OF_finddevice("/"),
		    "qcom,sm6115")) {
			printf("qcom_dsp: not a Qualcomm SM6115, nothing to do\n");
			return (0);
		}
		error = qcom_scm_available();
		printf("qcom_dsp: secure monitor %s (%d)\n",
		    error == 0 ? "answers" : "does not answer", error);
		qcom_smem_report();
		return (0);
	case MOD_UNLOAD:
		/* the drivers keep SMEM mapped and their state shared */
		return (EBUSY);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t qcom_dsp_mod = {
	"qcom_dsp",
	qcom_dsp_modevent,
	NULL
};

/* before the drivers attach (SI_SUB_DRIVERS) */
DECLARE_MODULE(qcom_dsp, qcom_dsp_mod, SI_SUB_CONFIGURE, SI_ORDER_FIRST);
MODULE_VERSION(qcom_dsp, 1);
