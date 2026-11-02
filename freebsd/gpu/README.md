# The Adreno 610 GPU under FreeBSD

The SM6115's GPU is an Adreno 610. Linux drives it with `msm`; on FreeBSD
the plan is the same driver on top of drm-kmod, FreeBSD's port of Linux's
DRM core on LinuxKPI:

| Step | What | State |
| --- | --- | --- |
| GPU 1 | drm-kmod 6.12-lts (the DRM core, dma-buf) as arm64 modules for the PRO1X kernel; Linux v6.12's msm sources and their register headers | built (2026-11-02): modules link against the kernel and their dependencies with no unresolved symbol; not loaded on the phone yet |
| GPU 2 | `msm` (the GPU part: adreno, a6xx for the A610) on drm-kmod, the GPU clock controller, the SMMU | not started |
| GPU 3 | Mesa 26.2.2 with freedreno (Gallium, OpenGL/GLES) and turnip (Vulkan) beside llvmpipe and softpipe, drop-in for the image's mesa-libs and mesa-dri (`mesa.sh`, below) | built (2026-11-02); llvmpipe and softpipe of this build render in the FreeBSD VM; freedreno untested (no GPU driver yet) |

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

## GPU 3: Mesa with freedreno (`mesa.sh`)

`WORK=/path/to/freebsd-build bash freebsd/gpu/mesa.sh` (after
`freebsd/hildon/build.sh` has made its sysroot and `rootfs.sh` has
fetched the package index, both once) cross-builds Mesa 26.2.2, the
version of the image's `mesa-libs` and `mesa-dri` packages, on the PC:
clang for `aarch64-unknown-freebsd15.1` against `$WORK/hildon/sysroot`,
meson with a cross file, in under 2 minutes on the PC (`JOBS=4`).
FreeBSD's packages have no freedreno (`mesa-dri` 26.2.2: llvmpipe,
panfrost, radeonsi, r300, r600, svga, RADV, lavapipe).

| | |
| --- | --- |
| Source | `mesa-26.2.2.tar.xz` from archive.mesa3d.org into `$WORK/dl`, SHA-256 as in the port's `graphics/mesa-dri/distinfo` (ports commit 29df97d33d31, 2026Q4) |
| Drivers | Gallium freedreno (`msm_dri.so`, `kgsl_dri.so`), llvmpipe, softpipe; Vulkan turnip (`libvulkan_freedreno.so`, `freedreno_icd.aarch64.json`) |
| Interfaces | EGL (x11, wayland, surfaceless, drm), GBM, GLX (dri), GLES 1 and 2, glvnd (`libEGL_mesa`, `libGLX_mesa`, `50_mesa.json`: libglvnd's `libEGL`, `libGL`, `libGLES*` stay the package's) |
| Built on | the sysroot plus `MESA_PKGS` (fetched with `pkgfetch.py`, recorded in `sysroot.list` as `build.sh` does) and, of `llvm19`, only LLVM's headers, `libLLVM` and its CMake files (`/usr/local/llvm19`) |
| `llvm-config` | `$WORK/gpu/mesa/hostbin/llvm-config`, a shell script with the answers of the package's `llvm-config19` (shared mode, `-lLLVM-19`, RTTI on, its component list from `LLVMConfig.cmake`) and paths in the sysroot: meson asks it, and the real one is a FreeBSD program |
| Run path | `/usr/local/llvm19/lib`, as the port links (libLLVM is not in ldconfig's path; the image has `libLLVM.so.19.1` there, `rootfs.sh`) |
| Result | `$WORK/out/gpu/mesa/usr/local/...` (36 MB), laid out as the two packages: `lib/libgallium-26.2.2.so`, `libEGL_mesa`, `libGLX_mesa`, `libgbm`, `lib/gbm/dri_gbm.so`, `lib/dri/*_dri.so` (links to `libdril_dri.so`), `libvulkan_freedreno.so`, `libdata/pkgconfig/{dri,gbm}.pc`, the headers, `share/drirc.d`, `etc/libmap.d/mesa.conf` |

The build tree (`$WORK/gpu/mesa`, about 550 MB at its largest) is deleted
at the end (`KEEP=1` keeps it). The script checks that every object in the
stage is a FreeBSD/arm64 ELF file and that each library it needs is in the
stage or the sysroot; all of them are in the image's package list as well
(turnip also needs `libudev.so.0`, `libxcb-keysyms.so.1`,
`libdisplay-info.so.4`). Nothing installs it into the image yet: copied
over the image's files (`cp -a $WORK/out/gpu/mesa/usr /`), it replaces
both packages' libraries. The package's DRI stubs for drivers this build
does not have (`radeonsi_dri.so`, `panfrost_dri.so`, ...) then point at a
libgallium without them and fail cleanly; `libvulkan_lvp.so` and
`libvulkan_radeon.so` do not use libgallium and keep working.

Mesa's own changes for FreeBSD, `mesa-patches/` (applied with `patch -p0`):

| Patch | |
| --- | --- |
| `patch-include_renderdoc__app.h` | the port's |
| `patch-src_vulkan_wsi_wsi__common__wayland.c` | the port's: `CLOCK_MONOTONIC_RAW` is `CLOCK_MONOTONIC_FAST` on FreeBSD |
| `patch-src_freedreno_drm_msm_msm__bo.c` | FreeBSD has no `ENODATA` (`ENOATTR`, as `src/util/os_file.c`); freedreno's msm backend did not compile |
| `patch-src_gallium_drivers_freedreno_freedreno__util.h` | FreeBSD has no `SYS_gettid`: `DBG()` takes `pthread_getthreadid_np()` |

meson runs with `--wrap-mode=nofallback`: Mesa would otherwise download
libarchive (for freedreno's command-stream decoder, a tool) and install
it.

`WORK=... VM="ssh ... root@host" bash freebsd/gpu/mesa.sh test` runs
`test/egl-pbuffer.c` in a FreeBSD/arm64 machine on the staged libraries
(`LD_LIBRARY_PATH`, `__EGL_VENDOR_LIBRARY_FILENAMES`): GLES 2 through EGL
surfaceless into a pbuffer, a triangle read back with `glReadPixels`. In
the test VM (2026-11-02):

    GL_RENDERER: llvmpipe (LLVM 19.1.7, 128 bits)
    GL_VERSION: OpenGL ES 3.2 Mesa 26.2.2
    loaded: /tmp/mesa-stage/usr/local/lib/libgallium-26.2.2.so
    middle 255,0,0 corner 0,0,255: OK
    GL_RENDERER: softpipe                      (GALLIUM_DRIVER=softpipe)
    middle 255,0,0 corner 0,0,255: OK
    MESA: error: fd_device_new:41: cannot get version: Inappropriate ioctl for device
    gbm_create_device(/dev/zero): backend drm  (MESA_LOADER_DRIVER_OVERRIDE=msm)

softpipe is not in the package's libgallium, so the second run is this
build's. freedreno, forced onto a file that is no DRM device, refuses it
and GBM falls back to its software path, without a crash.

### Which kernel interface freedreno needs

The answer for GPU 2, from Mesa 26.2.2's sources:

- Gallium freedreno (`src/freedreno/drm/msm`, `src/gallium/drivers/freedreno`)
  only uses the classic msm interface: `DRM_MSM_GET_PARAM`/`SET_PARAM`,
  `GEM_NEW`, `GEM_INFO` (`GET_IOVA`, `GET_OFFSET`, `SET_NAME`,
  `SET/GET_METADATA`), `GEM_CPU_PREP`, `GEM_MADVISE`, `GEM_SUBMIT`,
  `SUBMITQUEUE_NEW/CLOSE/QUERY`, `WAIT_FENCE`. There is no VM_BIND in it.
  It needs the driver name `msm` and major version 1; the minor version
  gates features (`FD_VERSION_*` in `freedreno_drmif.h`): 3 submit queues
  and GMEM base, 4 softpin (the submit path Mesa takes from there on,
  below it the old relocations), 5 robustness, 8 cached-coherent buffers,
  9 the VA size parameter.
- Turnip needs 1.6 (syncobj). It tries `MSM_PARAM_EN_VM_BIND` and falls
  back to the classic submit when the kernel refuses; without
  `DRM_CAP_SYNCOBJ_TIMELINE` (before Linux 6.15) it emulates timeline
  semaphores.
- Linux 6.12's msm is 1.12.0 (`MSM_VERSION_MINOR` in
  `$WORK/gpu/linux-6.12/drivers/gpu/drm/msm/msm_drv.c`; VM_BIND came
  later, with 1.13.0), so its classic interface is all Mesa 26.2 needs.
  msm should report its real version (1.12): reporting less turns
  features off.

### Not done yet

- Nothing of this is in the image, and freedreno has not met a GPU.
- libdrm: the image's 2.4.134 is enough (Mesa needs 2.4.109 and core
  libdrm only; freedreno does not use `libdrm_freedreno`, which the
  package does not have), so libdrm is not rebuilt.
- How Mesa finds the GPU: given a file descriptor (Xorg's DRI3, GBM on
  `/dev/dri/renderD128`) it asks the kernel for the driver's name (`msm`),
  which works. Enumerating devices does not, for a platform device:
  libdrm's FreeBSD code (`drmParseSubsystemType()`) takes every DRM device
  for a PCI one and reads the bus address from the sysctl
  `hw.dri.N.busid`, which it expects as `pci:dddd:bb:dd.f`; drm-kmod
  (`drm_sysctl_freebsd.c`, `drm_add_busid_modesetting()`) makes that
  string with `to_pci_dev(dev->dev)` for every device, which for msm's
  platform device reads a `struct pci_dev` that is not there. GPU 2 has
  to give msm a busid of its own (e.g. `platform:...`); then
  `drmGetDevice2()` fails cleanly and Mesa uses the driver name, but
  `drmGetDevices2()` still leaves msm out, so EGL's surfaceless and
  device platforms fall back to llvmpipe (Wayland clients, which get the
  device as a number, may too) until libdrm learns FreeBSD platform
  devices.
- The X server: X clients reach freedreno through DRI3, which the
  framebuffer driver `scfb` does not offer. With scfb, GLX and EGL on X
  stay on llvmpipe; freedreno for Hildon needs Xorg's modesetting driver
  with glamor on a KMS device (msm's display part), or rendering
  off-screen through GBM or EGL surfaceless.
