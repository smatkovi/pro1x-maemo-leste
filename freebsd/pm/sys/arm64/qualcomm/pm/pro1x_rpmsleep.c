/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pro1x_rpmsleep.ko: the apps processor's RPM sleep set at 0 (freebsd/pm,
 * stage 3), as KolibriOS's kernel/modem/rpmsleep.c sends it and Linux
 * does when nothing uses a resource. The RPM keeps two sets of votes per
 * master: the active set while it runs, the sleep set while it sleeps.
 * After a kexec the sleep set is what Linux left (its consumers' votes and
 * the boot's maxima: rpmpd and clk-smd-rpm clamp to the highest until
 * sync_state), which keeps the SoC out of XO shutdown and vmin. Linux 7.1:
 *
 *   the XO           "clk0" 0 "Enab"       clk-smd-rpm.c bi_tcxo (MISC_CLK)
 *   its buffers      "clka" 1-3 LN_BB, 4-5 RF "swen"   (CLK_BUF_A, XO_BUFFER)
 *   CX, MX           "rwcx" 0, "rwmx" 0 "vlvl"         rpmpd.c sm6115
 *   the bus clocks   "clk2" 0 bimc, "clk1" 1 cnoc, "clk1" 2 snoc,
 *                    "mmxi" 0 mmnrt, 1 mmrt, "qup" 0 "KHz"
 *                                          clk-smd-rpm.c sm_qnoc_icc_clks
 *
 * Not voted: the interconnect's per-node bandwidth, the regulators (Linux
 * votes them in the active set only). The active set stays as it is. The
 * votes last until the next boot (Linux's values are not known here).
 *
 *   sysctl hw.pro1x_rpmsleep.set=1    the votes (=2: the RF buffers too,
 *                                     which the Wi-Fi uses)
 *   sysctl hw.pro1x_rpmsleep.result   what the RPM said to each
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>

/* freebsd/dsp/sys/arm64/qualcomm/dsp/qcom_rpm.c (qcom_dsp.ko) */
int	qcom_rpm_write(int state, uint32_t type, uint32_t id,
	    const uint32_t *kv, int npairs);
#define	RPM_SLEEP_STATE	1

#define	T_MISC		0x306b6c63u	/* clk0 */
#define	T_CLKBUF	0x616b6c63u	/* clka */
#define	T_BUS		0x316b6c63u	/* clk1 */
#define	T_MEM		0x326b6c63u	/* clk2 */
#define	T_MMAXI		0x69786d6du	/* mmxi */
#define	T_QUP		0x00707571u	/* qup */
#define	T_RWCX		0x78637772u	/* rwcx */
#define	T_RWMX		0x786d7772u	/* rwmx */
#define	K_ENABLE	0x62616e45u	/* Enab */
#define	K_SWEN		0x6e657773u	/* swen */
#define	K_RATE		0x007a484bu	/* KHz */
#define	K_LEVEL		0x6c766c76u	/* vlvl */

static const struct {
	uint32_t	type, id, key;
	const char	*name;
	bool		rf;
} votes[] = {
	{ T_MISC, 0, K_ENABLE, "XO", false },
	{ T_CLKBUF, 1, K_SWEN, "LN_BB1", false },
	{ T_CLKBUF, 2, K_SWEN, "LN_BB2", false },
	{ T_CLKBUF, 3, K_SWEN, "LN_BB3", false },
	{ T_CLKBUF, 4, K_SWEN, "RF1", true },
	{ T_CLKBUF, 5, K_SWEN, "RF2", true },
	{ T_RWCX, 0, K_LEVEL, "CX", false },
	{ T_RWMX, 0, K_LEVEL, "MX", false },
	{ T_MEM, 0, K_RATE, "bimc", false },
	{ T_BUS, 1, K_RATE, "cnoc", false },
	{ T_BUS, 2, K_RATE, "snoc", false },
	{ T_MMAXI, 0, K_RATE, "mmnrt", false },
	{ T_MMAXI, 1, K_RATE, "mmrt", false },
	{ T_QUP, 0, K_RATE, "qup", false },
};

static char result[256] = "not voted";

static int
rpmsleep_set(SYSCTL_HANDLER_ARGS)
{
	uint32_t kv[2];
	int v = 0, error, ok, n, len;
	u_int i;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL || v == 0)
		return (error);
	ok = n = 0;
	len = snprintf(result, sizeof(result), "refused:");
	for (i = 0; i < nitems(votes); i++) {
		if (votes[i].rf && v < 2)
			continue;
		kv[0] = votes[i].key;
		kv[1] = 0;
		n++;
		error = qcom_rpm_write(RPM_SLEEP_STATE, votes[i].type,
		    votes[i].id, kv, 1);
		if (error == 0)
			ok++;
		else if (len < (int)sizeof(result))
			len += snprintf(result + len, sizeof(result) - len,
			    " %s (%d)", votes[i].name, error);
	}
	printf("pro1x_rpmsleep: the sleep set: %d of %d resources at 0; %s\n",
	    ok, n, ok == n ? "none refused" : result);
	snprintf(result, sizeof(result), "%d of %d resources at 0%s", ok, n,
	    ok == n ? "" : " (some refused: see the console)");
	return (0);
}

static SYSCTL_NODE(_hw, OID_AUTO, pro1x_rpmsleep, CTLFLAG_RW | CTLFLAG_MPSAFE,
    0, "Pro1X: the RPM sleep set");
SYSCTL_PROC(_hw_pro1x_rpmsleep, OID_AUTO, set,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0, rpmsleep_set, "I",
    "1: the sleep set at 0 (2: with the RF buffers)");
SYSCTL_STRING(_hw_pro1x_rpmsleep, OID_AUTO, result, CTLFLAG_RD, result, 0,
    "The last votes");

static int
rpmsleep_modevent(module_t mod, int what, void *arg)
{
	return (what == MOD_LOAD || what == MOD_UNLOAD ? 0 : EOPNOTSUPP);
}

static moduledata_t rpmsleep_mod = {
	"pro1x_rpmsleep",
	rpmsleep_modevent,
	NULL
};
DECLARE_MODULE(pro1x_rpmsleep, rpmsleep_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(pro1x_rpmsleep, 1);
MODULE_DEPEND(pro1x_rpmsleep, qcom_dsp, 1, 1, 1);
