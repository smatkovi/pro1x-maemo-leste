# msm on FreeBSD: the Adreno 610 (step GPU 2)

`msm.ko` is Linux v6.12's `drivers/gpu/drm/msm`, GPU part only (adreno,
a6xx for the SM6115's Adreno 610, the GEM objects, submits, fences, the
scheduler's rings), on drm-kmod 6.12-lts and LinuxKPI, for the F(x)tec
Pro1X's PRO1X kernel. No KMS: the display stays FreeBSD's (`scfb` on the
framebuffer Linux left, `patches/0017`).

State (2026-11-02): **built, not loaded on the phone**. `build.sh msm`
compiles it from the Linux sources with `msm-patches/` and the glue in
`msm/`, and every undefined symbol of `msm.ko` resolves against the PRO1X
kernel, `linuxkpi.ko`, `qcom_dsp.ko`, `firmware.ko` (`out/modules`) and
`drm.ko`, `dmabuf.ko`, `lindebugfs.ko` (`out/gpu/modules`). Nothing has run
on hardware; QEMU has no Adreno.

Licence: msm and the Linux files built with it are GPL-2.0 (their headers
are kept); `msm/` (the FreeBSD glue, its shim headers), `msm-patches/` and
the freebsd/dsp glue it reuses are GPL-2.0 too and stay in their own
directories. drm-kmod's files keep their MIT/BSD licences
(`drm-kmod-patches/` change two of them).

## Decision A: the GPU's address space

**msm's VRAM carveout, with the GPU's SMMU in bypass; two small a6xx
patches.** From 6.12's sources (line numbers of the unpatched files):

| Where | What it shows |
| --- | --- |
| `msm_iommu.c:410` | `msm_iommu_new()` returns NULL for a device that is not iommu-mapped; `msm_iommu.c:446`: `msm_iommu_gpu_new()` passes that NULL on |
| `adreno/adreno_gpu.c:211` | `adreno_iommu_create_address_space()` returns `ERR_CAST(NULL)`, i.e. NULL |
| `msm_gpu.c:939-947` | `msm_gpu_init()` takes a NULL `gpu->aspace`: "no IOMMU, fallback to VRAM carveout!" |
| `msm_drv.c:111-123, 125-197` | `msm_use_mmu()` is false; `msm_init_vram()` allocates `vram=` (default `"16m"`, line 50) with `dma_alloc_attrs(DMA_ATTR_NO_KERNEL_MAPPING \| DMA_ATTR_WRITE_COMBINE)` (line 183) and a `drm_mm` over it |
| `msm_gem.c:1205-1260` | `msm_gem_new()`: every object from the carveout (`use_vram`, 1216); a VMA of no address space (`add_vma(obj, NULL)`, 1243) whose iova is the physical address (`physaddr()`, 25, 1260); its pages from the carveout (`get_pages_vram()`, 139) |
| `msm_gem.c:417, 510`; `msm_gem_vma.c:57-73` | pinning finds that VMA by its NULL address space (`lookup_vma()`), `msm_gem_vma_map()` maps nothing without an address space (line 72): a pinned object's iova is its physical address |
| `adreno/a6xx_gpu.c:2061` | **breaks**: `a6xx_create_private_address_space()` dereferences `gpu->aspace->mmu` at every DRM open |
| `msm_gpu.c:835-837` | and its caller writes `aspace->pid` into anything that is not an error, so the fix returns `ERR_PTR(-ENODEV)`, not NULL; then `ctx->aspace = gpu->aspace` (NULL) |
| `adreno/a6xx_gpu.c:116` | **breaks**: `a6xx_set_pagetable()` dereferences `ctx->aspace->mmu` at every submit; the fix returns early (no page table to switch) |
| `adreno/a6xx_gpu.c:2336` | the SMMU fault handler is only installed with an address space: none here (`adreno_fault_handler()`'s `resume_translation`, `adreno_gpu.c:265`, is not reached) |

Consequences for user space (to confirm in run gpu4 and with the Mesa
port, `README.md` "Which kernel interface freedreno needs"):

- `MSM_PARAM_VA_START`/`VA_SIZE` fail with `EINVAL` (`adreno_gpu.c:366`:
  `ctx->aspace == gpu->aspace`), `MSM_INFO_SET_IOVA` with `EOPNOTSUPP`
  (`msm_drv.c:539`); `MSM_INFO_GET_IOVA` returns the buffer's physical
  address. freedreno then has no address range of its own and takes the
  kernel's iovas (its softpin submit path, from msm 1.4, needs no
  relocations; relocations are refused on a6xx anyway,
  `adreno_gpu.c:1089`, `msm_gem_submit.c:802`). Whether turnip runs with
  kernel iovas is open.
- `msm_gem_submit.c:671`: without per-process page tables msm asks
  `CAP_SYS_RAWIO` of a submitting process. Here that is `PRIV_IO` (root
  outside a jail), or anyone with `hw.msm.unprivileged_submit=1`. That is
  the security of this design: the GPU's SMMU is bypassed, so whoever may
  submit can make the GPU read and write all of memory. A desktop session
  as a user needs the tunable (or a later IOMMU, below).

What the FreeBSD side does to the SMMU (`adreno_smmu@59a0000`, an MMU-500
the application processor owns; not the hypervisor-mediated apps SMMU
that `qcom_smmu.c` handles): the SMMU sits in the GPU's CX power domain
(its node: `power-domains = <&gpucc GPU_CX_GDSC>`, clocks
`GCC_GPU_MEMNOC_GFX_CLK`, `GPU_CC_HLOS1_VOTE_GPU_SMMU_CLK`,
`GCC_GPU_SNOC_DVM_GFX_CLK`). Its registers are only there with CX up and
those clocks on, and they come back from reset after each CX collapse
(Linux's `arm_smmu_runtime_resume()` programs them again for that reason,
7.1's `drivers/iommu/arm/arm-smmu/arm-smmu.c:2363`). At the kexec Linux's
`arm_smmu_device_shutdown()` sets `sCR0.CLIENTPD` (line 2342) and suspends
it; Linux's page tables are gone either way. So `msm_freebsd_bsd.c`, every
time it has brought CX up, turns on the three clocks and sets
`sCR0.CLIENTPD` (all client transactions bypass; TLB invalidated first),
or with `hw.msm.smmu=2` invalidates all stream match entries, turns off
all context banks and matches the GPU node's `iommus` stream (`0` mask
`1`) to an S2CR of type bypass. The first power-up prints the registers
and the valid match entries found (stale Linux state or reset values: the
device run tells).

The alternative, if bypass turns out impossible or the security cost too
high: msm's IOMMU path with an own context bank. That is LinuxKPI's
missing IOMMU API (domain alloc/attach, map/unmap of scatterlists,
geometry, fault handler: about 300 lines over one MMU-500 context bank)
plus an io-pgtable for ARM LPAE stage-1 (about 600 lines, or Linux's
`io-pgtable-arm.c`, 1300 lines, on LinuxKPI) and msm's `msm_iommu.c` as it
is; with `adreno-smmu-priv` (TTBR0 switching for per-process tables) about
1500 lines in all. Not needed for the first runs.

## Decision B: power and clocks

**(a) The GPU is off when FreeBSD starts.** The boot menu's kernel
(`out-pro1x.12nobtslim/config-7.1.0-pro1x.12nobtslim`) has
`CONFIG_DRM_MSM=y`, `CONFIG_SM_GPUCC_6115=y`, `CONFIG_ARM_SMMU=y`,
`CONFIG_QCOM_RPMPD=y`: msm binds the GPU as a component of the display,
but powers it only at the first open of a DRM file (`msm_drv.c:318, 355`,
`adreno_device.c:58`), whose first step is `adreno_load_fw()`
(`adreno_device.c:82`). The initramfs
(`initrd.img-7.1.0-pro1x.12nobtslim`, 453 entries) has no `lib/firmware`,
so that fails and the GPU is never powered; the GDSCs and clocks Linux
does not use are turned off after boot (`clk_disable_unused`, genpd's
power-off of unused domains); and at the kexec `device_shutdown()` runs
`adreno_shutdown()` (`adreno_device.c:278`) ->
`pm_runtime_force_suspend()` (line 366). What survives is the GPU clock
controller's configuration that Linux's gpucc probe wrote (PLL
configuration, branch wake/sleep counts) and Linux's last RPM vote for
VDDCX.

**(b) The native power-up**, as Linux 6.12 does it for the A610 with its
GMU wrapper (`a6xx_pm_resume()`, `a6xx_gpu.c:1840-1868`): the OPP (VDDCX
level, then the core clock's rate, line 1863), the GMU wrapper's device
(its CX power domain through a device link, `a6xx_gmu.c:1622-1628`, line
1865), the GX power domain (1866), the six clocks of the GPU node (1868).
`msm_freebsd_bsd.c` programs it from these sources:

| Part | Linux 6.12 source | Registers (gpucc@0x5990000, gcc@0x1400000) |
| --- | --- | --- |
| PLLs, at the first use (gpucc's probe) | `gpucc-sm6115.c:49-61, 106-117, 481-482`; `clk-alpha-pll.c:391` (configure), `:66` (register layout) | PLL0 at 0x0: L 0x3e, alpha 0, alpha_hi 0x80, CONFIG_CTL 0x4001055b, outputs main/aux/aux2, VCO 0 (1-2 GHz); PLL1 at 0x100: L 0x21, alpha 0x55555555, alpha_hi 0x55, CONFIG_CTL 0x4001055b, outputs main/aux, VCO 2 (0.5-1 GHz); the post dividers gfx3d takes 1 |
| gpucc's probe, the rest | `gpucc-sm6115.c:485-489` | CX GMU CBCR 0x1098 wakeup 0xf, sleep 0xf; GX GFX3D CBCR 0x1054 FORCE_MEM_CORE, FORCE_PERIPH_ON |
| PLL rate, enable | `clk-alpha-pll.c:734` (set_rate: L, 40-bit alpha, VCO, ALPHA_EN; a running PLL latches the update, `:681`), `:526` (enable: BYPASSNL, 5 us, RESET_N, lock, OUTCTRL) | |
| gfx3d RCG (0x101c) | `gpucc-sm6115.c:210-235`; `clk-rcg2.c:396, 111` | 320/465 MHz: PLL1_OUT_AUX (source 3) at 640/930 MHz, divider 2 (CFG 3); 600..980 MHz: PLL0_OUT_AUX2 (source 2) at twice the rate; CMD UPDATE, polled 500 us; the new PLL on while switching (CLK_OPS_PARENT_ENABLE) |
| GMU RCG (0x1120) | `gpucc-sm6115.c:195-208`; `clk-rcg2.c:1216` (shared ops: re-enabled with the configuration found) | left as found; its GCC input (GPLL0, GPLL0_DIV) voted if that is its source; parked on XO if it is one of the GPU's PLLs |
| CX GDSC (0x106c) | `gpucc-sm6115.c:397-405`; `gdsc.c:403` (init), `:259` (enable), `:317` (disable), `:139` | init: HW_CONTROL and SW_OVERRIDE off, wait times EN_REST 2, EN_FEW 8, CLK_DIS 2; on: SW_COLLAPSE 0, status in gds_hw_ctrl 0x1540 bit 31 (1.5 ms); off (votable): SW_COLLAPSE 1, 500 us |
| GX GDSC (0x100c) | `gpucc-sm6115.c:407-418` | after CX; on: GPU_GX_BCR 0x1008 asserted 1 us, IO clamp 0x1508 bit 0 off, SW_COLLAPSE 0, status bit 31 of 0x100c; off: SW_COLLAPSE 1, 500 us, clamp on |
| GPU clocks (the node's `clocks`) | `gpucc-sm6115.c:237-382`; `clk-branch.c:62, 43`; GCC: 7.1's `gcc-sm6115.c:1529, 2349` | core GX_GFX3D 0x1054 (halt skip), iface AHB 0x1078 (delay), gmu CX_GMU 0x1098 (halt), xo CXO 0x109c (halt), mem_iface GCC_BIMC_GPU_AXI 0x71154 (delay, HW gating), alt_mem_iface GCC_GPU_MEMNOC_GFX 0x3600c (voted, HW gating) |
| SMMU clocks | gpucc 0x5000 (voted); GCC 0x36018 (halt), 0x3600c; 7.1's `gcc-sm6115.c:2364` | with CX, before the SMMU is touched |
| GPLL0 to the GPU | 7.1's `gcc-sm6115.c:2302, 2319` | GCC 0x79004 bits 15, 16 |
| VDDCX level | the OPPs' `required-opps` (`sm6115.dtsi`: 320 MHz LOW_SVS 64 ... 980 MHz TURBO_NO_CPR 416); rpmpd: 7.1's `drivers/pmdomain/qcom/rpmpd.c:75-80, 866-867` | resource `rwcx` 0, key `vlvl`, active and sleep set, `swen` 1 first: `qcom_rpm_write()` of qcom_dsp, as its PAS proxy vote sends it |

Recommendation, implemented: the native sequence (no FreeBSD clock
framework exists for this SoC, and the GPU is off, so there is nothing
to inherit), with two cautions as defaults:

- **The CX vote is not sent** (`hw.msm.cx_vote=0`). The RPM keeps one vote
  per master: FreeBSD's would replace what Linux voted for all of the
  application processor (display, UFS, ...), whose needs FreeBSD does not
  know. Instead the GPU runs only at its lowest OPP (`hw.msm.max_freq`,
  default 320 MHz, LOW_SVS), which the level Linux left voted covers as
  long as the display it set up runs (MDSS needs at least that). With
  `hw.msm.cx_vote=1` the vote is `max(OPP level, hw.msm.cx_floor)`,
  default floor 416 (TURBO_NO_CPR, never below anything Linux may have
  voted); then `hw.msm.max_freq=0` allows all OPPs of the speed bin.
- **No runtime suspend** (`hw.msm.autosuspend=0`): from the first open
  the GPU stays powered until unload (or until a hang recovery cycles it,
  which needs the CX collapse and so the SMMU set up again; that path is
  run gpu5). Linux's 66 ms autosuspend with `hw.msm.autosuspend=1`.

## What `msm.ko` contains

`build.sh msm` (also part of `all`): `msm-patches/*.patch` applied once to
`$WORK/gpu/linux-6.12` (`patch -p1`, marks `.pro1x-applied-*`), then
`msm/Makefile` in the cross build's environment, objects in
`$WORK/gpu/obj`, `out/gpu/modules/msm.ko`, `kldxref`, the symbol check.
The module exports nothing (`EXPORT_SYMS=NO`).

| Part | Files |
| --- | --- |
| msm, GPU part | `msm_drv.c` (the DRM driver, its ioctls, the headless probe), `msm_gem*.c` (objects, PRIME, shrinker, submit, VMAs), `msm_gpu.c`, `msm_ringbuffer.c`, `msm_submitqueue.c`, `msm_fence.c`, `msm_io_utils.c`, `msm_rd.c` and `msm_perf.c` (debugfs: the command stream dump, counters) |
| adreno | `adreno_device.c`, `adreno_gpu.c`, `a6xx_catalog.c`, `a6xx_gpu.c`, `a6xx_gmu.c` (the GMU wrapper's init; a real GMU's code is built but not reached), `a6xx_hfi.c` |
| zap shader loader | `drivers/soc/qcom/mdt_loader.c` |
| Left out | KMS and display (`msm_kms.c`, `msm_fb.c`, `msm_atomic*.c`, `msm_fbdev.c`, `msm_mdss.c`, `disp/`, `dsi/`, `dp/`, `hdmi/`), `msm_debugfs.c` (includes the display's snapshot code; its GPU part is set up by `msm_freebsd.c`), `msm_iommu.c` (decision A), `msm_gpu_devfreq.c`, `a6xx_gpu_state.c` (no `CONFIG_DRM_MSM_GPU_STATE`), a2xx..a5xx |
| FreeBSD glue | `msm/msm_freebsd.c` (LinuxKPI side), `msm/msm_freebsd_bsd.c` (FreeBSD side), `msm/msm_freebsd.h` (between them), `msm/lkpi/` (shim headers, first in the include path), freebsd/dsp's `lkpi_platform.c`/`lkpi_platform_bsd.c` (Linux platform drivers on simplebus, built in with `-DLKPI_PF_MODULE=msm -DLKPI_PF_NO_QRTR -DLKPI_PF_NO_SMMU_BYPASS`) |
| Config | drm-kmod's `kconfig.mk` (the drm headers' structures must match `drm.ko`), plus `DRM_MSM`, `ARCH_QCOM`, `QCOM_SCM`, `QCOM_MDT_LOADER` |
| Depends on | `drmn`, `dmabuf`, `linuxkpi`, `firmware`, `lindebugfs`, `qcom_dsp` (`MODULE_DEPEND`) |

### The patches

| Patch | |
| --- | --- |
| `msm-patches/0001-drm-msm-a6xx-without-iommu.patch` | decision A: `a6xx_create_private_address_space()` returns `-ENODEV` without `gpu->aspace`, `a6xx_set_pagetable()` returns without `ctx->aspace` |
| `msm-patches/0002-drm-msm-build-on-linuxkpi-and-drm-kmod.patch` | `pid_t` pids (LinuxKPI's), `spinlock_t idr_lock`, `drm_vma_node_unmap(obj)` and shmem truncation as drm-kmod has them, no mapping gfp mask, `drm_sched_job_init()`'s client id argument of drm-kmod, `dev_platform_data()` for the adreno config, `#undef ROP_COPY/ROP_XOR` (drm-kmod's fb.h) |
| `drm-kmod-patches/0002-drm-devices-not-on-pci.patch` | `hw.dri.N.busid` `platform:msm` and `dev.drm.N.PCI_ID` 0:0 for a device whose newbus parent is no PCI bus; drm-kmod read a `struct pci_dev` that is not there |
| `drm-kmod-patches/0003-drm-gem-get-put-pages-on-linuxkpi.patch` | `drm_gem_get_pages()`/`drm_gem_put_pages()` on LinuxKPI's shmem (wired pages of the object's VM object); msm links against them; with the carveout they are not called (untested) |
| freebsd/dsp (in place) | `lkpi/linux/platform_device.h`: `remove_new`, `platform_data` in the platform device, `devm_ioremap_resource()`, `platform_get_irq_byname()`, `platform_device_info`'s fields; `lkpi_platform.c`: `remove_new`, `platform_get_irq_byname`; `lkpi_platform_bsd.c`: `LKPI_PF_NO_QRTR`, `LKPI_PF_NO_SMMU_BYPASS` (ath10k_snoc unchanged) |

No LinuxKPI patch (`freebsd/patches/`) was needed: everything LinuxKPI
lacks is in `msm/lkpi/` or `msm_freebsd.c`.

### The shims and how faithful they are

| Linux interface | Here | Fidelity |
| --- | --- | --- |
| platform devices | freebsd/dsp's glue; adreno's and a GMU wrapper driver (`gmuwrap`, registered before adreno: `a6xx_gmu_wrapper_init()` finds it with `of_find_device_by_node()`); `dev->driver` set to the driver's name and `dev_pm_ops` | as Linux for these nodes |
| the headless DRM device | `platform_driver_register()` of a driver without a device tree table (msm's `"msm"`) makes its device: no `of_node`, the GPU's newbus device for DMA and sysctls, DMA not cache coherent; Linux 6.12 does this only for imx5 (`adreno_device.c:240`) | Linux's headless path, for the SM6115 |
| component framework | one master, its match list, `component_bind_all()`/`unbind_all()`; binds when all components are there | enough for one GPU; no aggregate devices |
| runtime PM | per-device usage count, enable, active; the driver's `runtime_resume`/`suspend`; device links with `DL_FLAG_PM_RUNTIME`; autosuspend on a delayed work; force suspend/resume | synchronous (Linux's async get/put run in the caller); no idle callback, no irq-safe mode, no parent counting beyond links; autosuspend only with `hw.msm.autosuspend=1` |
| generic power domains | `dev_pm_domain_attach_by_name()`: the node's `power-domains`/`power-domain-names` on gpucc's GDSCs as virtual devices; `GENPD_NOTIFY_OFF`; synced power-off | the two GDSCs only; GX's parent CX counted |
| clocks | `devm_clk_get()`, `devm_clk_bulk_get_all()` by `clock-names` on the native helper; a clock of another provider or index is a dummy with a warning | the GPU node's six and the SMMU's three; rates only for gfx3d |
| OPPs | the `operating-points-v2` table with `opp-supported-hw` and `required-opps`' `opp-level`; ceil/floor/exact; `set_opp`: level up first, then rate, level down after | one table (the GPU's); no voltages, no interconnect bandwidth (the node has no interconnects); capped by `hw.msm.max_freq` |
| nvmem | `nvmem_cell_read_variable_le_u32()`: the cell's reg and bits read bytewise from the QFPROM's region, as Linux's qfprom; `hw.msm.speedbin` overrides | the one cell msm reads |
| interconnect, devfreq, cooling, LLCC, OCMEM, AOSS QMP, RPMh cmd-db | absent: `of_icc_get()` NULL, no frequency scaling, LLCC slices fail, `qmp_get()` fails | as Linux on a system without them; devfreq is a real gap (no DVFS) |
| IOMMU, io-pgtable | types only; `device_iommu_mapped()` false; `msm_iommu_*()` stubs | decision A |
| SCM, PAS | Linux's `qcom_scm_pas_init_image()` (metadata in contiguous uncached memory, kept with its context until released), `mem_setup`, `auth_and_reset`, `shutdown`, `supported`, `is_available` on qcom_dsp's calls, under other names (qcom_dsp exports FreeBSD functions of these names) | as Linux's; no `set_remote_state` (a5xx), no `gpu_init_regs` (a7xx); the crypto engine's clock Linux votes around PAS calls is not voted (as for the DSPs) |
| firmware | LinuxKPI's `request_firmware()` (`/boot/firmware/<name>`), `request_firmware_into_buf()` as a partial read | |
| DMA | `dma_alloc_attrs()` with `DMA_ATTR_NO_KERNEL_MAPPING`: contiguous wired pages without a VM object, write combining, the physical address as the DMA address; else `dma_alloc_coherent()` | the carveout's pages can be moved into a user mapping's VM object by LinuxKPI's `vmf_insert_pfn()` (the GEM fault); freeing them at unload skips pages still in one |
| GEM fault | `vmf_insert_pfn()` takes the VM object's lock (LinuxKPI calls `fault` with it unlocked) | |
| interrupts | `devm_request_irq()` on the platform glue's interrupts (LinuxKPI's only knows PCI's), freed by devres | |
| `capable(CAP_SYS_RAWIO)` | `PRIV_IO`, or `hw.msm.unprivileged_submit=1` | see decision A |
| hrtimers | `HRTIMER_MODE_ABS` deadlines on `ktime_get()` started as the time left | |
| module parameters | drm-kmod's (`compat.linuxkpi.msm_*`, `hw.msm.*`); string ones (`vram`) with a handler that keeps a copy | |
| others | `memparse()`, `idr_alloc_u32()`, `readl_poll_timeout()`, `in_range()`, `circ_buf`, `SYSTEM_SLEEP_PM_OPS`/`RUNTIME_PM_OPS`, `SZ_2G`..`SZ_256G` as 64-bit constants (LinuxKPI's overflow: its `SZ_4G` is 0), an ELF32 header for `mdt_loader.c`, `assert()` for the generated register headers (KASSERT) | |

Tunables: `msm_freebsd_bsd.c`'s header (`hw.msm.max_freq`, `cx_vote`,
`cx_floor`, `smmu`, `speedbin`, `autosuspend`, `unprivileged_submit`,
`selftest`) and msm's own module parameters under `hw.msm` (`vram`,
`hang_debug`, ...). Read-only: `hw.msm.cx_wanted`, `hw.msm.cx_voted`.

## Firmware

| File | From | Needed by |
| --- | --- | --- |
| `/boot/firmware/qcom/a630_sqe.fw` | linux-firmware (`qcom/a630_sqe.fw`, LICENSE.qcom); not in this repository: `rootfs.sh` copies it from the build host's linux-firmware or linux-firmware's tag 20260916 (README.md "In the image") | `adreno_load_fw()` at the first open (gpu2 on) |
| `/boot/firmware/qcom/sm6115/Fxtec/QX1050/a610_zap.mbn` | the phone's firmware (`rootfs.sh` copies `qcom/sm6115/Fxtec/QX1050` to `/usr/local/share/pro1x/firmware`); the name is the Pro1X device tree's `zap-shader` `firmware-name` | `zap_shader_load_mdt()` at the first hardware init, PAS id 13 |

The image has `/boot/firmware/ath10k` as a link into
`/usr/local/share/pro1x/firmware`, and the same for `qcom`
(`rootfs/boot/firmware/qcom -> /usr/local/share/pro1x/firmware/qcom`);
`rootfs.sh` copies `a630_sqe.fw` there.

## Device runs

`pro1x.test=gpu pro1x.gpu=<run>` in `/host/freebsd/cmdline` runs
`rootfs/usr/local/libexec/pro1x-gputest` (in the style of
`pro1x-dsptest`): `PASS gpu-RUN:` and `FAIL gpu-RUN:` lines, bounded
waits, the summary line `=== gpu-RUN: PASS|FAIL (...)`, modules from
userdata's `freebsd/modules` first, its programs `pro1x-gpu` and
`egl-pbuffer` (`build.sh tools`) from `freebsd/gpu-tools`. Load order:
`kldload firmware linuxkpi linuxkpi_video dmabuf lindebugfs drm qcom_dsp
msm` (the image's `/boot/modules` has both module sets and `linker.hints`,
README.md "In the image"). The ioctls of gpu2 to gpu4 are
`pro1x-gpu param` and `pro1x-gpu submit`; Mesa's part of gpu4 is
`egl-pbuffer render /dev/dri/renderD128` (EGL on GBM on the render node:
EGL's surfaceless platform would look for the GPU in libdrm's device
list, which has no msm on FreeBSD, README.md, and fall back to llvmpipe).

| Run | Does | Expected on the console | The script checks |
| --- | --- | --- | --- |
| gpu1 `power` (120 s) | `hw.msm.selftest=1` (kenv before the load), `kldload msm`: before msm's drivers attach, CX, the SMMU, GX, the clocks with gfx3d at 320 MHz, a register write and read back, all off again; then msm attaches (no power) | `msm: selftest: GPU power-up ...`; `msm: VDDCX level 64 wanted, not voted`; `msm: selftest: CX GDSC 0x... (status 0x8...)` (bit 31 set); `msm: GPU SMMU at 0x59a0000: sCR0 ... IDR0 ... (N SMRs, M context banks), the GPU's stream 0/0x1` and any valid `SMRn`; `msm: GPU SMMU: client bypass, sCR0 now ...` (bit 0 set); `msm: selftest: GX GDSC ...`; `PLL1 mode 0x...` (bit 31 lock, bit 0 out), `gfx3d RCG cmd 0x... cfg 0x303`; `RBBM_STATUS 0x..., CP_SCRATCH_REG7 0x5a5a1234 -> 0x5a5a1234: the GPU's registers answer`; `msm: selftest: passed (0)`; then `gmuwrap0`, `adreno0` attach, `msm: nvmem cell speed_bin: 0x...`, `OPP 320000000 Hz, VDDCX level 64` (others "left out"), `using 16m VRAM carveout`, `VRAM: ...`, `no IOMMU, fallback to VRAM carveout!`, `Initialized msm 1.12.0 for msm on minor 0`; `/dev/dri/card0`, `/dev/dri/renderD128` | `kldload msm` succeeds; `dmesg` has `selftest: passed`; no `status stuck`, `failed to`; `sysctl hw.dri.0.busid` is `platform:msm`; `sysctl hw.dri.0.name` names msm; `Initialized msm 1.12.0 for msm on minor 0`; the device nodes exist; `kldunload msm` succeeds (and a second `kldload`) |
| gpu2 `zap` (180 s) | gpu1 without the selftest, then the first open (a tiny program: open `/dev/dri/renderD128`, `DRM_IOCTL_MSM_GET_PARAM` `MSM_PARAM_GPU_ID`/`CHIP_ID`): firmware, power-up, hardware init with the zap shader through PAS | `loaded qcom/a630_sqe.fw from new location`; no `Unable to load`; no `Unable to authorize the image`; no `Zap shader not enabled`; GET_PARAM `CHIP_ID` 0x06010000, `GPU_ID` 610, `MAX_FREQ` 320000000 | the ioctl answers with these values; `dmesg` has no `gpu hw init failed`, `Couldn't power up the GPU`; `hw.msm.cx_wanted` 64 |
| gpu3 `ring` (180 s) | gpu2, and the hardware init's own ring test: `a6xx_hw_init()` writes the CP's init packets (`a6xx_cp_init`), switches the CP out of secure mode after the zap shader and waits for it idle | no `CP initialization failed`, no `timeout waiting to drain ringbuffer`, no `hangcheck`; the GPU interrupt counted (`vmstat -i` has `gpu-irq`) | `MSM_PARAM_TIMESTAMP` answers twice with a rising value (the GPU's always-on counter); `GET_PARAM` `MSM_PARAM_SUSPENDS` 0 |
| gpu4 `submit` (300 s) | a submit from user space through libdrm (as root; or a user with `hw.msm.unprivileged_submit=1`): `GEM_NEW` a command buffer and a result buffer, `GEM_INFO GET_IOVA` (physical addresses), `GEM_NEW` mmap, a `CP_MEM_WRITE` of a pattern into the result buffer, `GEM_SUBMIT`, `WAIT_FENCE`; then Mesa's freedreno on GBM/EGL surfaceless (`test/egl-pbuffer.c` with `MESA_LOADER_DRIVER_OVERRIDE=msm`) | no `IOMMU support or CAP_SYS_RAWIO required!`; the fence signals within 1 s; the pattern in the result buffer; Mesa: `GL_RENDERER: FD610` | the pattern; `egl-pbuffer`'s `middle 255,0,0 corner 0,0,255: OK` with the FD610 renderer |
| gpu5 `cycle` (300 s) | `hw.msm.autosuspend=1`, gpu4 twice with a pause: the GPU suspends (GX, CX off) and comes back, the SMMU set up again | `MSM_PARAM_SUSPENDS` rises; the SMMU's lines again only if its state was lost; both submits pass | as gpu4, and the suspend count |
| gpu6 `cx` (300 s) | after gpu5, `hw.msm.cx_vote=1 hw.msm.max_freq=0`: all OPPs of the speed bin, VDDCX voted | `msm: VDDCX voted at level 416 (the GPU wants ...)`; `hw.msm.cx_voted` 416; `hw.qcom_rpm.refused`, `timeouts` 0 | gpu4 at the highest OPP |

What may go wrong first, and what the lines tell: a hang (no console) at
the selftest's first CX register write means the GPU clock controller
needs something else on (its AHB clock comes from GCC's
`gcc_gpu_cfg_ahb_clk`, critical in Linux); `status stuck at 'off'` of a
GDSC; a PLL `failed to enable` (lock); the scratch register reading back
0 or 0xdeadbeef (GX not up, a clock missing); the SMMU registers all
zero or the CX GDSC still off (its clocks). Each run is a reboot.

## Not done yet

- Never run on the phone (the runs above).
- No frequency scaling (devfreq), no thermal limit; one OPP by default.
- No IOMMU: any process allowed to submit can reach all memory through
  the GPU (decision A). Own context bank plus io-pgtable is the way out.
- libdrm's FreeBSD device enumeration (`drmGetDevices2()`) treats every
  DRM device as PCI, so it does not list msm; opening
  `/dev/dri/renderD128` by name works (the driver name comes from the
  kernel), EGL's device and surfaceless platforms fall back to llvmpipe
  (README.md, GPU 3).
- turnip with kernel iovas is untested; freedreno's softpin path with
  them is expected to work.
- `drm_gem_get_pages()` on LinuxKPI is untested (the carveout does not use
  it).
- Hang recovery (`a6xx_recover()`) needs the CX power domain to collapse
  and its notifier, which the power domain glue gives; untested.
