# The Adreno 610 GPU under FreeBSD

The SM6115's GPU is an Adreno 610. Linux drives it with `msm`; on FreeBSD
the plan is the same driver on top of drm-kmod, FreeBSD's port of Linux's
DRM core on LinuxKPI:

| Step | What | State |
| --- | --- | --- |
| GPU 1 | drm-kmod 6.12-lts (the DRM core, dma-buf) as arm64 modules for the PRO1X kernel; Linux v6.12's msm sources and their register headers | built (2026-11-02): modules link against the kernel and their dependencies with no unresolved symbol; not loaded on the phone yet |
| GPU 2 | `msm` (the GPU part: adreno, a6xx for the A610) on drm-kmod, the GPU clock controller, the SMMU | not started |

## What `build.sh` makes

`WORK=/path/to/freebsd-build bash freebsd/gpu/build.sh [modules|headers|all]`
(after `freebsd/build.sh` has built the kernel toolchain and the PRO1X
kernel once):

| File | |
| --- | --- |
| `$WORK/out/gpu/modules/dmabuf.ko` | dma-buf, dma-fence (with array, chain, unwrap), dma-resv, sync_file |
| `$WORK/out/gpu/modules/drm.ko` | the DRM core: GEM, PRIME, syncobj, the GPU scheduler (`drm_sched_*`), `drm_exec`, GPUVM, KMS and its helpers (MIPI DSI, panels, bridges, DP/HDMI helpers), fbdev emulation, `vt_drmfb` |
| `$WORK/out/gpu/modules/linuxkpi_video.ko` | from the base sources (`sys/modules/linuxkpi_video`): `drm.ko` depends on it |
| `$WORK/out/gpu/modules/lindebugfs.ko` | from the base sources: `drm.ko` depends on it (debugfs; its own dependency pseudofs is in the kernel) |
| `$WORK/out/gpu/modules/linker.hints` | for these four |
| `$WORK/gpu/linux-6.12/` | from linux-6.12.tar.xz (SHA-256 checked): `drivers/gpu/drm/msm` with `registers/`, `include/uapi/drm/msm_drm.h`, `include/linux/adreno-smmu-priv.h`, the SM6115 GPU clock controller (`drivers/clk/qcom/gpucc-sm6115.c` with gdsc, alpha PLL, RCG2, branch and the headers they include, `include/dt-bindings/clock/qcom,sm6115-gpucc.h`), `drivers/soc/qcom/mdt_loader.c` and its header |
| `$WORK/gpu/linux-6.12/generated/*.xml.h` | msm's register headers, made by `registers/gen_header.py` as msm's Makefile makes them (`gen_header.py --validate --rnn drivers/gpu/drm/msm/registers --xml drivers/gpu/drm/msm/registers/adreno/a6xx.xml c-defines > generated/a6xx.xml.h`, from the tree's top, for each of its ADRENO_HEADERS and DISPLAY_HEADERS) |

`linuxkpi.ko` itself is `freebsd/build.sh`'s (`out/modules`); the kernel
has the rest `drm.ko` depends on (pci, iicbus, iic, iicbb, mem, pseudofs).
The modules are built in the cross build's environment (`make.py
buildenv`) with `SYSDIR=$WORK/src/sys` and `KERNBUILDDIR` set to the
PRO1X kernel's objects (its `opt_*.h`), objects in `$WORK/gpu/obj`; the
kernel is not rebuilt. The build holds `$WORK/.kbuild-lock`. At the end
the script checks every undefined symbol of the four modules against the
kernel and the modules each depends on.

drm-kmod is cloned from github.com/freebsd/drm-kmod, branch 6.12-lts,
at 1d054ae55685 (the version for FreeBSD 15.1), with:

| Patch | |
| --- | --- |
| `drm-kmod-patches/0001-drm-acpi_iicbus-only-with-device-acpi.patch` | `drm.ko` registered `acpi_iicbus_driver`, which only kernels with `device acpi` have; the PRO1X kernel is FDT-only and would refuse to load it |

## Not done yet

- Nothing here has been loaded on the phone. `rootfs.sh` does not put
  `out/gpu/modules` into the image yet; `drm.ko` needs `linuxkpi.ko`
  (`out/modules`) found by the same `kldload`, so both directories have to
  be in `kern.module_path`, or all of them in `/boot/modules` with
  `kldxref` run there again.
- Only `dmabuf` and `drm` are built: no `ttm` (msm does not use it).
- For GPU 2: drm-kmod leaves out `drm_gem_get_pages()` and
  `drm_gem_put_pages()` (shmem-backed GEM pages, `#ifdef __linux__` in
  `drm_gem.c`), which `msm_gem.c` uses; the msm port has to bring its own.
  The Linux IOMMU and io-pgtable code msm's `msm_iommu.c` builds on has no
  LinuxKPI counterpart either.
