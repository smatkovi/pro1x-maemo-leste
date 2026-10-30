/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The SM6115's audio register windows (LPASS) for the FM radio's sound
 * programmer in user space (freebsd/fm/README.md, tools/pro1x-fmaudio):
 * /dev/qcom_lpass maps them as device memory (Device-nGnRE), which
 * /dev/mem does not (arm64's memmmap leaves the attribute at write-back,
 * the wrong one for registers). mmap(2) takes the physical address as its
 * offset, as /dev/mem does, and only pages inside one of the windows
 * below; there is no read(2)/write(2) and no interrupt: the programmer
 * polls.
 *
 * The windows are the device tree's (Linux's sm6115.dtsi with Leste's
 * patch 0021 for the SLIMbus; the downstream bengal.dtsi has the same
 * addresses). No register is touched here: whether a block's clock runs
 * is the programmer's business (a read of a gated block hangs the bus,
 * which the watchdog ends).
 *
 *   sysctl hw.qcom_lpass.windows    the windows, and how often each was
 *                                   mapped
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>

#include <vm/vm.h>

static const struct lpass_window {
	vm_paddr_t	pa;
	vm_size_t	size;
	const char	*name;
} lpass_windows[] = {
	/* sm6115.dtsi (0021): slimbam, "qcom,bam-v1.7.0", the NGD's BAM */
	{ 0x0a584000, 0x20000, "slimbam" },
	/* slim-ngd@a5c0000, "qcom,slim-ngd-v1.5.0"; NGD 1 at +0x1000 */
	{ 0x0a5c0000, 0x2c000, "slim-ngd" },
	/* codec@a600000, "qcom,sm6115-lpass-rx-macro" */
	{ 0x0a600000, 0x1000, "rxmacro" },
	/* soundwire@a610000, swr1 (RX), "qcom,soundwire-v1.6.0" */
	{ 0x0a610000, 0x10000, "swr1" },
	/* codec@a620000, "qcom,sm6115-lpass-tx-macro" */
	{ 0x0a620000, 0x1000, "txmacro" },
	/* clock-controller@a6a9000, "qcom,sm6115-lpassaudiocc" (resets) */
	{ 0x0a6a9000, 0x1000, "lpass_audiocc" },
	/* codec@a730000, "qcom,sm6115-lpass-va-macro" */
	{ 0x0a730000, 0x1000, "vamacro" },
	/* soundwire@a740000, swr0 (VA/TX), "qcom,soundwire-v1.6.0" */
	{ 0x0a740000, 0x10000, "swr0" },
	/* pinctrl@a7c0000, "qcom,sm6115-lpass-lpi-pinctrl": TLMM, slew */
	{ 0x0a7c0000, 0x20000, "lpass_tlmm" },
	{ 0x0a950000, 0x10000, "lpass_tlmm_slew" },
	/* clock-controller@a7e0000, "qcom,sm6115-lpasscc" (resets) */
	{ 0x0a7e0000, 0x12000, "lpasscc" },
};

#define	LPASS_NWINDOWS	nitems(lpass_windows)

static u_int lpass_mapped[LPASS_NWINDOWS];
static struct cdev *lpass_cdev;

/* the window holding the page at pa, or -1 */
static int
lpass_window_of(vm_paddr_t pa)
{
	u_int i;

	for (i = 0; i < LPASS_NWINDOWS; i++)
		if (pa >= lpass_windows[i].pa &&
		    pa + PAGE_SIZE <= lpass_windows[i].pa +
		    lpass_windows[i].size)
			return (i);
	return (-1);
}

static int
lpass_mmap(struct cdev *dev, vm_ooffset_t offset, vm_paddr_t *paddr,
    int nprot, vm_memattr_t *memattr)
{
	int w;

	if (offset < 0 || (offset & PAGE_MASK) != 0 ||
	    (w = lpass_window_of((vm_paddr_t)offset)) < 0)
		return (EINVAL);
	if (offset == (vm_ooffset_t)lpass_windows[w].pa)
		atomic_add_int(&lpass_mapped[w], 1);
	*paddr = (vm_paddr_t)offset;
	*memattr = VM_MEMATTR_DEVICE;
	return (0);
}

static struct cdevsw lpass_cdevsw = {
	.d_version =	D_VERSION,
	.d_mmap =	lpass_mmap,
	.d_name =	"qcom_lpass",
};

static int
lpass_windows_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	u_int i;
	int error;

	sbuf_new_for_sysctl(&sb, NULL, 512, req);
	for (i = 0; i < LPASS_NWINDOWS; i++)
		sbuf_printf(&sb, "%s%s %#jx+%#jx (%u)", i ? ", " : "",
		    lpass_windows[i].name, (uintmax_t)lpass_windows[i].pa,
		    (uintmax_t)lpass_windows[i].size, lpass_mapped[i]);
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

static SYSCTL_NODE(_hw, OID_AUTO, qcom_lpass, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "The LPASS register windows for user space (freebsd/fm)");
SYSCTL_PROC(_hw_qcom_lpass, OID_AUTO, windows, CTLTYPE_STRING | CTLFLAG_RD |
    CTLFLAG_MPSAFE, NULL, 0, lpass_windows_sysctl, "A",
    "The windows, their physical addresses and sizes (times mapped)");

static int
qcom_lpass_modevent(module_t mod, int what, void *arg)
{
	switch (what) {
	case MOD_LOAD:
		lpass_cdev = make_dev(&lpass_cdevsw, 0, UID_ROOT, GID_WHEEL,
		    0600, "qcom_lpass");
		return (0);
	case MOD_UNLOAD:
		/* mappings already made stay valid: the pages are not RAM */
		if (lpass_cdev != NULL)
			destroy_dev(lpass_cdev);
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t qcom_lpass_mod = {
	"qcom_lpass",
	qcom_lpass_modevent,
	NULL
};

DECLARE_MODULE(qcom_lpass, qcom_lpass_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(qcom_lpass, 1);
