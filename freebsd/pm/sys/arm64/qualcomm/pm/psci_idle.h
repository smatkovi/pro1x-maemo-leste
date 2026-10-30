/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * CPU idle states through PSCI CPU_SUSPEND (psci_idle.c), shared with the
 * assembler that keeps a core's context across a power-down state
 * (psci_idle_asm.S). The offsets are checked against the C structure at
 * compile time (psci_idle.c).
 */
#ifndef _ARM64_QUALCOMM_PM_PSCI_IDLE_H_
#define	_ARM64_QUALCOMM_PM_PSCI_IDLE_H_

/* struct psci_idle_ctx, one per CPU, 512 bytes, 512-byte aligned */
#define	PIC_X18		0
#define	PIC_X19		8	/* x19 ... x30: 8 ... 96 */
#define	PIC_SP		104
#define	PIC_TTBR0	112
#define	PIC_TTBR1	120
#define	PIC_TCR		128
#define	PIC_MAIR	136
#define	PIC_SCTLR	144
#define	PIC_VBAR	152
#define	PIC_CPACR	160
#define	PIC_CNTKCTL	168
#define	PIC_TPIDR1	176
#define	PIC_TPIDR0	184
#define	PIC_TPIDRRO	192
#define	PIC_SPEL0	200
#define	PIC_CONTEXTIDR	208
#define	PIC_MDSCR	216
#define	PIC_CNTV_CTL	224
#define	PIC_CNTV_CVAL	232
#define	PIC_ICC_SRE	240
#define	PIC_ICC_PMR	248
#define	PIC_ICC_BPR1	256
#define	PIC_ICC_CTLR	264
#define	PIC_ICC_GRPEN1	272
#define	PIC_IDMAP	280	/* TTBR0 for the way back: the identity map */
#define	PIC_SELF_VA	288	/* this context's virtual address */
#define	PIC_SELF_PA	296	/* and its physical one */
#define	PIC_SIM_PA	304	/* simulate: the physical address of the MMU off */
#define	PIC_RESUMED	312	/* the returns through psci_idle_resume */
#define	PIC_SIZE	512

#ifndef LOCORE
#include <sys/types.h>

struct psci_idle_ctx {
	uint64_t	x[13];		/* x18 ... x30 */
	uint64_t	sp;
	uint64_t	ttbr0, ttbr1, tcr, mair, sctlr, vbar, cpacr, cntkctl;
	uint64_t	tpidr1, tpidr0, tpidrro, spel0, contextidr, mdscr;
	uint64_t	cntv_ctl, cntv_cval;
	uint64_t	icc_sre, icc_pmr, icc_bpr1, icc_ctlr, icc_grpen1;
	uint64_t	idmap, self_va, self_pa, sim_pa;
	uint64_t	resumed;
	uint64_t	pad[(PIC_SIZE - PIC_RESUMED - 8) / 8];
};

/*
 * CPU_SUSPEND with the core's context kept: fid, the state's parameter, the
 * physical entry point (psci_idle_resume), the context and its physical
 * address (the context id), the conduit (hvc, else smc), simulate (QEMU:
 * after a successful return, the core's state destroyed as a power-down
 * would, and the way back taken). The PSCI result, or 0 after a return
 * through psci_idle_resume.
 */
int	psci_idle_suspend(register_t fid, register_t param, register_t entry_pa,
	    struct psci_idle_ctx *ctx, register_t ctx_pa, int hvc, int simulate);
/* the warm boot entry (MMU off, x0 the context's physical address) */
void	psci_idle_resume(void);
/* simulate: the MMU's switch off, at its physical address */
void	psci_idle_sim_off(void);

/* the broadcast timer (arm_tmr_mem.c): wake cpu at the latest after sbt */
bool	arm_tmr_mem_ok(void);
void	arm_tmr_mem_arm(u_int cpu, sbintime_t sbt);
void	arm_tmr_mem_disarm(u_int cpu);
uint64_t arm_tmr_mem_fires(void);
#endif
#endif
