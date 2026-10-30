/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * A core's context across a PSCI CPU_SUSPEND into a power-down state
 * (psci_idle.c), the way Linux's arch/arm64/kernel/sleep.S and
 * mm/proc.S's cpu_do_suspend/cpu_do_resume keep it, after KolibriOS's
 * kernel/arch/arm64/suspend.S (whose kernel runs at its physical
 * addresses; FreeBSD's does not):
 *
 * psci_idle_suspend saves the callee-saved registers (x18, the per-CPU
 * pointer, among them), the stack pointer and the EL1 system registers
 * the kernel sets (translation, vectors, the FP access, the counters'
 * access, the thread pointers, the debug control, the virtual timer
 * FreeBSD's generic timer uses at EL1, the GIC's CPU interface) in a
 * context, cleans it to the point of coherency and calls CPU_SUSPEND
 * with psci_idle_resume's physical address and the context's as the
 * context id. A return from the call (refused, an interrupt already
 * pending, a state without power-down) returns its result.
 *
 * After a power-down the firmware enters psci_idle_resume at EL1 with the
 * MMU and the caches off and the context's physical address in x0. It
 * loads MAIR, TCR and TTBR1 (the kernel's map) from the context, an
 * identity map of the kernel image (a 1 GiB block, psci_idle.c) into
 * TTBR0, turns the MMU on under the running code, jumps to the kernel's
 * virtual address of the code that follows, puts the saved TTBR0 back and
 * the rest of the context, and returns 0 from psci_idle_suspend.
 *
 * simulate (QEMU, whose CPU_SUSPEND is a wfi): after a successful return
 * the core goes to its physical address through the identity map, turns
 * the MMU and the caches off, clears what a power-down loses and takes
 * the way back through psci_idle_resume.
 */
#include <sys/elf_common.h>
#include <machine/asm.h>
#include "psci_idle.h"

	.arch	armv8-a

#define	SCTLR_M	(1 << 0)
#define	SCTLR_C	(1 << 2)
#define	SCTLR_I	(1 << 12)

/*
 * int psci_idle_suspend(x0 fid, x1 param, x2 entry_pa, x3 ctx, x4 ctx_pa,
 *     w5 hvc, w6 simulate)
 */
ENTRY(psci_idle_suspend)
	stp	x18, x19, [x3, #PIC_X18]
	stp	x20, x21, [x3, #PIC_X19 + 8]
	stp	x22, x23, [x3, #PIC_X19 + 24]
	stp	x24, x25, [x3, #PIC_X19 + 40]
	stp	x26, x27, [x3, #PIC_X19 + 56]
	stp	x28, x29, [x3, #PIC_X19 + 72]
	str	x30, [x3, #PIC_X19 + 88]
	mov	x9, sp
	str	x9, [x3, #PIC_SP]
	mrs	x9, ttbr0_el1
	str	x9, [x3, #PIC_TTBR0]
	mrs	x9, ttbr1_el1
	str	x9, [x3, #PIC_TTBR1]
	mrs	x9, tcr_el1
	str	x9, [x3, #PIC_TCR]
	mrs	x9, mair_el1
	str	x9, [x3, #PIC_MAIR]
	mrs	x9, sctlr_el1
	str	x9, [x3, #PIC_SCTLR]
	mrs	x9, vbar_el1
	str	x9, [x3, #PIC_VBAR]
	mrs	x9, cpacr_el1
	str	x9, [x3, #PIC_CPACR]
	mrs	x9, cntkctl_el1
	str	x9, [x3, #PIC_CNTKCTL]
	mrs	x9, tpidr_el1
	str	x9, [x3, #PIC_TPIDR1]
	mrs	x9, tpidr_el0
	str	x9, [x3, #PIC_TPIDR0]
	mrs	x9, tpidrro_el0
	str	x9, [x3, #PIC_TPIDRRO]
	mrs	x9, sp_el0
	str	x9, [x3, #PIC_SPEL0]
	mrs	x9, contextidr_el1
	str	x9, [x3, #PIC_CONTEXTIDR]
	mrs	x9, mdscr_el1
	str	x9, [x3, #PIC_MDSCR]
	mrs	x9, cntv_ctl_el0
	str	x9, [x3, #PIC_CNTV_CTL]
	mrs	x9, cntv_cval_el0
	str	x9, [x3, #PIC_CNTV_CVAL]
	mrs	x9, icc_sre_el1
	str	x9, [x3, #PIC_ICC_SRE]
	mrs	x9, icc_pmr_el1
	str	x9, [x3, #PIC_ICC_PMR]
	mrs	x9, icc_bpr1_el1
	str	x9, [x3, #PIC_ICC_BPR1]
	mrs	x9, icc_ctlr_el1
	str	x9, [x3, #PIC_ICC_CTLR]
	mrs	x9, icc_igrpen1_el1
	str	x9, [x3, #PIC_ICC_GRPEN1]
	str	x3, [x3, #PIC_SELF_VA]
	str	x4, [x3, #PIC_SELF_PA]

	/* to the point of coherency: psci_idle_resume reads it uncached */
	mov	x9, x3
	add	x10, x3, #PIC_SIZE
1:	dc	civac, x9
	add	x9, x9, #64
	cmp	x9, x10
	b.lo	1b
	dsb	sy

	/*
	 * CPU_SUSPEND: x0 the function, x1 the state, x2 the entry point,
	 * x3 the context id. x18-x30 survive the call (SMCCC), so the
	 * context, the conduit and simulate go there (saved above).
	 */
	mov	x19, x3
	mov	w20, w6
	mov	w21, w5
	mov	x3, x4
	cbnz	w21, 2f
	smc	#0
	b	3f
2:	hvc	#0
3:	/* back without a power-down, or QEMU's wfi */
	cbz	w20, 4f
	cbz	x0, sim_loss
4:	mov	x9, x19
	ldp	x19, x20, [x9, #PIC_X19]
	ldr	x21, [x9, #PIC_X19 + 16]
	ret

/*
 * simulate: what a power-down leaves of the core, then the way back. From
 * the kernel's virtual address to the physical one through the identity
 * map, the MMU off there.
 */
sim_loss:
	ldr	x9, [x19, #PIC_IDMAP]
	msr	ttbr0_el1, x9
	isb
	tlbi	vmalle1
	dsb	nsh
	isb
	ldr	x10, [x19, #PIC_SIM_PA]
	ldr	x0, [x19, #PIC_SELF_PA]
	br	x10
END(psci_idle_suspend)

ENTRY(psci_idle_sim_off)
	BTI_J
	mrs	x9, sctlr_el1
	mov	x10, #(SCTLR_M | SCTLR_C | SCTLR_I)
	bic	x9, x9, x10
	msr	sctlr_el1, x9
	isb
	/*
	 * the FP/SIMD registers, which a power-down loses too: FP access on
	 * for that (the idle thread runs with it trapping; cpacr is cleared
	 * below)
	 */
	mov	x10, #(3 << 20)
	msr	cpacr_el1, x10
	isb
	movi	v0.16b, #0
	movi	v1.16b, #0
	movi	v2.16b, #0
	movi	v3.16b, #0
	movi	v4.16b, #0
	movi	v5.16b, #0
	movi	v6.16b, #0
	movi	v7.16b, #0
	movi	v8.16b, #0
	movi	v9.16b, #0
	movi	v10.16b, #0
	movi	v11.16b, #0
	movi	v12.16b, #0
	movi	v13.16b, #0
	movi	v14.16b, #0
	movi	v15.16b, #0
	movi	v16.16b, #0
	movi	v17.16b, #0
	movi	v18.16b, #0
	movi	v19.16b, #0
	movi	v20.16b, #0
	movi	v21.16b, #0
	movi	v22.16b, #0
	movi	v23.16b, #0
	movi	v24.16b, #0
	movi	v25.16b, #0
	movi	v26.16b, #0
	movi	v27.16b, #0
	movi	v28.16b, #0
	movi	v29.16b, #0
	movi	v30.16b, #0
	movi	v31.16b, #0
	msr	fpcr, xzr
	msr	fpsr, xzr
	msr	ttbr0_el1, xzr
	msr	ttbr1_el1, xzr
	msr	tcr_el1, xzr
	msr	mair_el1, xzr
	msr	vbar_el1, xzr
	msr	cntkctl_el1, xzr
	msr	tpidr_el0, xzr
	msr	tpidrro_el0, xzr
	msr	tpidr_el1, xzr
	msr	sp_el0, xzr
	msr	contextidr_el1, xzr
	msr	cntv_ctl_el0, xzr
	msr	cntv_cval_el0, xzr
	msr	icc_pmr_el1, xzr		/* everything masked */
	msr	icc_igrpen1_el1, xzr		/* group 1 off */
	msr	cpacr_el1, xzr
	isb
	mov	x9, #0
	mov	sp, x9
	mov	x18, #0
	mov	x19, #0
	mov	x29, #0
	mov	x30, #0
	b	psci_idle_resume
END(psci_idle_sim_off)

/*
 * The warm boot: EL1, the MMU and the caches off, x0 the context's
 * physical address. Only what the context holds is read before the MMU
 * is on (it was cleaned to the point of coherency).
 */
ENTRY(psci_idle_resume)
	msr	daifset, #0xf
	mov	x9, x0
	ldr	x10, [x9, #PIC_MAIR]
	msr	mair_el1, x10
	ldr	x10, [x9, #PIC_TCR]
	msr	tcr_el1, x10
	ldr	x10, [x9, #PIC_TTBR1]
	msr	ttbr1_el1, x10
	ldr	x10, [x9, #PIC_IDMAP]
	msr	ttbr0_el1, x10
	ldr	x10, [x9, #PIC_VBAR]
	msr	vbar_el1, x10
	ldr	x11, [x9, #PIC_SELF_VA]
	ldr	x12, [x9, #PIC_SCTLR]
	isb
	tlbi	vmalle1
	dsb	nsh
	isb
	msr	sctlr_el1, x12
	isb
	ic	iallu
	dsb	nsh
	isb
	/* the MMU on, here through the identity map: on to the kernel's address */
	ldr	x10, =resume_va
	br	x10
resume_va:
	BTI_J
	mov	x9, x11
	ldr	x10, [x9, #PIC_TTBR0]
	msr	ttbr0_el1, x10
	isb
	tlbi	vmalle1
	dsb	nsh
	isb
	ldr	x10, [x9, #PIC_CPACR]
	msr	cpacr_el1, x10
	ldr	x10, [x9, #PIC_CNTKCTL]
	msr	cntkctl_el1, x10
	ldr	x10, [x9, #PIC_TPIDR1]
	msr	tpidr_el1, x10
	ldr	x10, [x9, #PIC_TPIDR0]
	msr	tpidr_el0, x10
	ldr	x10, [x9, #PIC_TPIDRRO]
	msr	tpidrro_el0, x10
	ldr	x10, [x9, #PIC_SPEL0]
	msr	sp_el0, x10
	ldr	x10, [x9, #PIC_CONTEXTIDR]
	msr	contextidr_el1, x10
	/* the OS lock open, as FreeBSD's dbg_init leaves it */
	msr	oslar_el1, xzr
	ldr	x10, [x9, #PIC_MDSCR]
	msr	mdscr_el1, x10
	ldr	x10, [x9, #PIC_CNTV_CVAL]
	msr	cntv_cval_el0, x10
	ldr	x10, [x9, #PIC_CNTV_CTL]
	msr	cntv_ctl_el0, x10
	/* the GIC's CPU interface, as Linux's CPU PM notifier sets it up */
	ldr	x10, [x9, #PIC_ICC_SRE]
	msr	icc_sre_el1, x10
	isb
	ldr	x10, [x9, #PIC_ICC_PMR]
	msr	icc_pmr_el1, x10
	ldr	x10, [x9, #PIC_ICC_BPR1]
	msr	icc_bpr1_el1, x10
	ldr	x10, [x9, #PIC_ICC_CTLR]
	msr	icc_ctlr_el1, x10
	ldr	x10, [x9, #PIC_ICC_GRPEN1]
	msr	icc_igrpen1_el1, x10
	isb
	ldr	x10, [x9, #PIC_SP]
	mov	sp, x10
	ldr	x10, [x9, #PIC_RESUMED]
	add	x10, x10, #1
	str	x10, [x9, #PIC_RESUMED]
	ldp	x18, x19, [x9, #PIC_X18]
	ldp	x20, x21, [x9, #PIC_X19 + 8]
	ldp	x22, x23, [x9, #PIC_X19 + 24]
	ldp	x24, x25, [x9, #PIC_X19 + 40]
	ldp	x26, x27, [x9, #PIC_X19 + 56]
	ldp	x28, x29, [x9, #PIC_X19 + 72]
	ldr	x30, [x9, #PIC_X19 + 88]
	mov	x0, #0
	ret
END(psci_idle_resume)

GNU_PROPERTY_AARCH64_FEATURE_1_NOTE(GNU_PROPERTY_AARCH64_FEATURE_1_VAL)
