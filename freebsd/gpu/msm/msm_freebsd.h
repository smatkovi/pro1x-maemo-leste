/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * msm.ko on FreeBSD (freebsd/gpu/msm-port.md): what the LinuxKPI half
 * (msm_freebsd.c: the Linux interfaces msm uses) and the FreeBSD half
 * (msm_freebsd_bsd.c: the device tree, the GPU's clocks, power domains,
 * SMMU, RPM vote, VRAM pages, secure monitor calls) call of each other, in
 * plain C types, as freebsd/dsp's lkpi_platform.h does. Device tree nodes
 * are phandles; errors are Linux's negative errnos.
 */

#ifndef _MSM_FREEBSD_H_
#define	_MSM_FREEBSD_H_

#include <sys/types.h>

/* --- the device tree --------------------------------------------------------------- */
uintptr_t msm_bsd_of_parse_phandle(uintptr_t node, const char *prop, int index);
int	msm_bsd_of_is_compatible(uintptr_t node, const char *compat);
int	msm_bsd_of_is_available(uintptr_t node);
/* the first node below the root (depth first) with this compatible */
uintptr_t msm_bsd_of_find_compatible(const char *compat);
int	msm_bsd_of_machine_is_compatible(const char *compat);
/* reg[index], translated to a physical address */
int	msm_bsd_of_reg(uintptr_t node, int index, uint64_t *pa, uint64_t *size);
/* the index of name in a string list property, or -ENOENT */
int	msm_bsd_of_string_index(uintptr_t node, const char *prop,
	    const char *name);
/* the cells of a property (big endian decoded), their count or -errno */
int	msm_bsd_of_cells(uintptr_t node, const char *prop, uint32_t *buf,
	    int max);
/* the platform device msm.ko attached to the node (its newbus softc) */
void	*msm_bsd_find_pdev(uintptr_t node);

/* --- clocks ------------------------------------------------------------------------ */
/* the SM6115's GPU clocks, by the clock controller and its binding's index */
enum msm_clk_id {
	MSM_CLK_NONE = 0,
	MSM_CLK_GX_GFX3D,	/* gpucc GPU_CC_GX_GFX3D_CLK ("core") */
	MSM_CLK_GPU_AHB,	/* gpucc GPU_CC_AHB_CLK ("iface") */
	MSM_CLK_CX_GMU,		/* gpucc GPU_CC_CX_GMU_CLK ("gmu") */
	MSM_CLK_CXO,		/* gpucc GPU_CC_CXO_CLK ("xo") */
	MSM_CLK_SMMU_VOTE,	/* gpucc GPU_CC_HLOS1_VOTE_GPU_SMMU_CLK */
	MSM_CLK_BIMC_GPU_AXI,	/* gcc GCC_BIMC_GPU_AXI_CLK ("mem_iface") */
	MSM_CLK_GPU_MEMNOC_GFX,	/* gcc GCC_GPU_MEMNOC_GFX_CLK ("alt_mem_iface") */
	MSM_CLK_GPU_SNOC_DVM,	/* gcc GCC_GPU_SNOC_DVM_GFX_CLK */
	MSM_CLK_GPU_GPLL0,	/* gcc GCC_GPU_GPLL0_CLK_SRC */
	MSM_CLK_GPU_GPLL0_DIV,	/* gcc GCC_GPU_GPLL0_DIV_CLK_SRC */
	MSM_CLK_COUNT
};
/* the clock of a node's clocks[index]: its id, or MSM_CLK_NONE (unknown) */
int	msm_bsd_clk_of(uintptr_t node, int index);
const char *msm_bsd_clk_name(int id);
int	msm_bsd_clk_enable(int id);
void	msm_bsd_clk_disable(int id);
int	msm_bsd_clk_set_rate(int id, unsigned long rate);
unsigned long msm_bsd_clk_get_rate(int id);
long	msm_bsd_clk_round_rate(int id, unsigned long rate);

/* --- power domains ------------------------------------------------------------------- */
#define	MSM_PD_GPU_CX		0	/* gpucc GPU_CX_GDSC */
#define	MSM_PD_GPU_GX		1	/* gpucc GPU_GX_GDSC */
/* the node's power domain by name: MSM_PD_*, or -errno */
int	msm_bsd_pd_of(uintptr_t node, const char *name);
/* a GDSC on or off (synced: wait for the off status, a6xx_recover) */
int	msm_bsd_gdsc(int pd, bool on, bool synced);

/* --- the RPM's VDDCX level for the GPU's OPPs --------------------------------------- */
/* the level (RPM_SMD_LEVEL_*), 0 to drop the GPU's need */
int	msm_bsd_cx_level(unsigned int level);
/* an OPP node's opp-level (required-opps' target) */
int	msm_bsd_opp_level(uintptr_t opp_node, uint32_t *level);
/* the OPPs of a node's operating-points-v2 table, in their order */
struct msm_bsd_opp {
	uint64_t	hz;		/* opp-hz */
	uint32_t	level;		/* required-opps[0]'s opp-level, or 0 */
	uint32_t	supported_hw;	/* opp-supported-hw, or 0: any */
};
int	msm_bsd_opp_table(uintptr_t node, struct msm_bsd_opp *opps, int max);
/* hw.msm.max_freq: OPPs above it are left out (0: all) */
unsigned long msm_bsd_max_freq(void);
/* hw.msm.autosuspend: the GPU's runtime suspend (0: kept on) */
bool	msm_bsd_autosuspend(void);
/* hw.msm.unprivileged_submit: submits without PRIV_IO (no IOMMU) */
bool	msm_bsd_unprivileged_submit(void);

/* --- the speed bin (nvmem cell in the QFPROM) --------------------------------------- */
int	msm_bsd_nvmem_cell_u32(uintptr_t node, const char *cell, uint32_t *val);

/* --- memory --------------------------------------------------------------------------- */
/* physically contiguous pages without a VM object, write combining (VRAM) */
void	*msm_bsd_vram_alloc(size_t size, uint64_t *pa);
void	msm_bsd_vram_free(uint64_t pa, size_t size);
/* physically contiguous memory written back to the point of coherency, for
 * TrustZone (the PAS metadata) */
void	*msm_bsd_contig_alloc(size_t size, uint64_t *pa);
void	msm_bsd_contig_free(void *va, size_t size);
void	msm_bsd_wb(void *va, size_t size);

/* --- secure monitor calls (qcom_dsp) ------------------------------------------------- */
int	msm_bsd_scm_available(void);
int	msm_bsd_pas_init_image(uint32_t pas_id, uint64_t metadata_pa);
int	msm_bsd_pas_mem_setup(uint32_t pas_id, uint64_t pa, uint64_t size);
int	msm_bsd_pas_auth_and_reset(uint32_t pas_id);
int	msm_bsd_pas_shutdown(uint32_t pas_id);
int	msm_bsd_pas_supported(uint32_t pas_id);

/* --- device runs ---------------------------------------------------------------------- */
/* the power-up self test (hw.msm.selftest=1 at load, sysctl hw.msm.selftest) */
int	msm_bsd_selftest(void);

#endif /* _MSM_FREEBSD_H_ */
