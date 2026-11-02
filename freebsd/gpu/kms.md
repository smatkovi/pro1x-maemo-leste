# KMS for the display Linux leaves running (step GPU 4)

Goal: X on the GPU. Today Xorg runs `scfb` on the framebuffer at
0x5c000000, turns every frame in software and hildon-desktop composites
with llvmpipe. With this step `msm.ko` also offers KMS, so that
`/dev/dri/card0` is one DRM device with the display and the Adreno 610:
Xorg's `modesetting` driver draws with glamor (EGL on GBM on that device:
Mesa's freedreno), turns the screen with RandR on the GPU, and hands its
clients freedreno through DRI3.

State (2026-11-02): **built, not run on the phone**. `build.sh msm` builds
`msm/msm_freebsd_kms.c` into `msm.ko`, every undefined symbol resolves
against the PRO1X kernel and the modules (`build.sh`'s check);
`freebsd/patches/0053` compiles with the PRO1X kernel's flags and
`-Werror -Wall` (the patched file, outside `freebsd/src`); the kernel and
the image were not rebuilt.

| Part | Files |
| --- | --- |
| KMS (GPL-2.0, in `msm.ko`) | `msm/msm_freebsd_kms.c` (the DRM objects, the commit, vblank); its FreeBSD side in `msm/msm_freebsd_bsd.c` (tunables, the eventhandler, the poll's callout, the VRAM below 4 GiB) |
| msm's hook | `msm-patches/0003-drm-msm-freebsd-kms-for-the-running-display.patch`: `msm_drm_init()` tries `msm_freebsd_kms_init()` where it would make the device headless, `msm_drm_uninit()` calls `msm_freebsd_kms_fini()` |
| kernel (BSD-2-Clause) | `freebsd/patches/0053-qcom_dpu-MDSS-interrupt-DPU-interrupts-for-KMS.patch`: qcom_dpu takes the MDSS interrupt, hands the DPU's to the eventhandler `qcom_dpu_mdp_intr` (`sys/arm64/qualcomm/qcom_dpu.h`) |
| X | `freebsd/rootfs/usr/local/etc/X11/xorg-kms.conf.d/10-pro1x-kms.conf` (a second configuration directory; the image's `xorg.conf.d` stays scfb's), `xrandr` at the session's start (`~/.xinitrc`), `tools/devday/fbsd-x.sh ... kms` |
| device runs | `pro1x-gputest` `kms1` .. `kms4` (below), `test/pro1x-gpu.c` (`pro1x-gpu kms`), `test/egl-pbuffer.c` (`egl-pbuffer x11`) |

Tunables: `hw.msm.kms=1` turns it on (default 0: the device stays headless
as in `msm-port.md`, the GPU runs are unchanged); `hw.msm.vram=256m` (the
carveout holds every buffer, scanout ones too: 16m, msm's default, is less
than two screens); `hw.msm.kms_irq=0` polls instead of taking the
interrupt; `hw.qcom_dpu.irq=0` (kernel) leaves the MDSS interrupt alone.
Read only: `hw.msm.kms_info` (mode, pipes, vblank source),
`hw.msm.kms_vblanks`, `kms_flips`, `kms_flips_late`, `kms_underruns`,
`dev.qcom_dpu.0.irq`, `dev.qcom_dpu.0.intr_*`.

## What Linux leaves running, and what the handover knows

The boot menu's Linux 7.1 keeps msm's display on at the kexec
(`device/kernel/patches/0005-msm-keep-display-across-kexec.patch`:
`msm_kms_shutdown()` returns when `kexec_in_progress`). FreeBSD's
`qcom_dpu` (`freebsd/patches/0017`, `0022`;
`sys/arm64/qualcomm/qcom_dpu_handover.c`) attaches to the
`qcom,sm6115-mdss` node and

- turns translation off in the display streams' SMMU context banks
  (`SCTLR.M=0` for the mdss node's `iommus` streams 0x420/0x2, 0x421/0x0),
  so the DPU fetches physical addresses;
- points every scanning source pipe (VIG0 at 0x4000, DMA0 at 0x24000 of the
  "mdp" registers, those with a non-zero `SSPP_SRC0_ADDR`) at the
  `/chosen` simple-framebuffer (0x5c000000, 1080 x 2160, stride 4320,
  a8r8g8b8) and writes `CTL_0`'s `CTL_FLUSH` (0x1018) with the pipes' bits
  (VIG0 bit 0, DMA0 bit 11) and `CTL` (bit 17): the next frame shows the
  console, which `vt_simplefb` draws;
- sends DCS commands over the DSI controller (0x5e94000: brightness as
  `backlight(4)`, display on/off, idle mode, partial mode, sleep) between
  frames of video, after checking `DSI_CTRL` (enable, `VID_MODE_EN`),
  `DSI_TRIG_CTRL` (DMA by software) and `DSI_CMD_DMA_CTRL` (low power).

Nothing else of the pipeline is touched: INTF_1's timing engine, CTL_0's
layer mixer set-up, LM_0, DSPP_0, PINGPONG_0, the DSI PHY and its PLL, the
clocks and the RPM votes stay as Linux left them.

**Video mode.** The panel (`boe,bf060y8m-aj0`, 7.1's
`drivers/gpu/drm/panel/panel-boe-bf060y8m-aj0.c:499`) is driven with
`MIPI_DSI_MODE_VIDEO | MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM`,
and qcom_dpu finds `DSI_CTRL.VID_MODE_EN` set (it refuses DCS otherwise).
So an update is not a frame transfer: the timing engine sends a frame every
16.7 ms from wherever the pipe points, and a "flip" is a new source address
and a flush, taken by the hardware at the next frame start. KMS checks
`DSI_CTRL` again and gives up in command mode (that would need `CTL_START`
per frame and PINGPONG_0's done interrupt, bit 8; not written).

## Design

**Where.** In `msm.ko`'s own DRM device, so Mesa and Xorg see one "msm"
device with KMS and the render node (Mesa loads freedreno by the driver's
name; `drmGetDevices2()` would not find a second, platform, device on
FreeBSD, README.md). msm's real KMS (`msm_kms.c`, `msm_atomic.c`,
`disp/dpu1`) would need the DPU's resource manager, its clock, interconnect
and power handling, kthread workers and a DSI host with PHY and PLL: far
more than a running display needs. So `msm_freebsd_kms.c` is a minimal KMS
of its own on drm-kmod's atomic helpers, plugged in where msm makes the
device headless (`msm-patches/0003`; Linux 6.12's `msm_drv.c:276-283`
clears `DRIVER_MODESET`/`DRIVER_ATOMIC` there). drm-kmod has no
`drm_simple_kms_helper.c` and only a stub of `drm_gem_framebuffer_helper.c`
(no `drm_gem_fb_create()`), so the objects and the framebuffer creation are
written out (about 300 lines).

**Objects.** One CRTC, one primary plane (XRGB8888 and ARGB8888, modifier
LINEAR only, so GBM allocates linear scanout buffers), one DSI encoder,
one DSI connector (`DSI-1`, always connected, 68 x 136 mm as the Linux
panel driver says), one mode. No cursor plane (X draws its pointer), no
scaling, no positioning: the plane covers the mode exactly
(`drm_atomic_helper_check_plane_state(..., DRM_PLANE_NO_SCALING,
DRM_PLANE_NO_SCALING, false, true)`), the pipe's geometry (`SSPP_SRC_SIZE`,
`OUT_SIZE`, `OUT_XY`) stays Linux's. Framebuffers up to 4096 x 4096 are
accepted (X's front buffer is 2160 x 1080 when the screen is turned), the
pitch a multiple of 8 below 64 KiB.

**The mode** comes from INTF_1's timing registers (0x6a800 + offset),
inverting `dpu_hw_intf_setup_timing_engine()` (6.12's `dpu_hw_intf.c:99`)
and `drm_mode_to_intf_timing_params()` (`dpu_encoder_phys_vid.c:43`; DSI:
no borders, sync polarities 0):

| Register | Linux writes | KMS reads |
| --- | --- | --- |
| `INTF_HSYNC_CTL` 0x008 | `(hsync_period << 16) \| hsync_pulse_width` | htotal, hsync width |
| `INTF_DISPLAY_HCTL` 0x03c | `(hsync_period - h_front_porch - 1) << 16 \| (h_back_porch + hsync_pulse_width)` | hdisplay = end - start + 1, front porch |
| `INTF_VSYNC_PERIOD_F0` 0x00c | `vsync_period * hsync_period` | vtotal |
| `INTF_VSYNC_PULSE_WIDTH_F0` 0x014 | `vsync_pulse_width * hsync_period` | vsync width |
| `INTF_DISPLAY_V_START_F0` 0x01c | `(vsync_pulse_width + v_back_porch) * hsync_period + hsync_skew` | first active line |
| `INTF_DISPLAY_V_END_F0` 0x024 | `(vsync_period - v_front_porch) * hsync_period + hsync_skew - 1` | vdisplay, front porch |
| `INTF_HSYNC_SKEW` 0x04c, `INTF_TIMING_ENGINE_EN` 0x000 | | skew; bit 0 must be set (else: no KMS) |

The pixel clock is not in the DPU (it is the DSI PLL's); it is computed for
the panel's 60 Hz (`panel-boe-bf060y8m-aj0.c:215-232`: 1236 x 2192 at
162559 kHz, exactly 60 Hz). The panel driver's mode gives
`HSYNC_CTL 0x04d40018`, `DISPLAY_HCTL 0x04af0078`, 2709312, 1236, 19776,
2689535: KMS should report `"1080x2160": 60 162559 1080 1116 1140 1236 2160
2176 2177 2192`. A `top_gap`/`bottom_gap` of the Linux panel driver would
show in the registers and so in the mode.

**What is checked first** (any failure: `kms: ... no KMS` on the console and
the device stays headless, the GPU still works): `hw.msm.kms=1`; the
`qcom,sm6115-dpu` node; `DSI_CTRL` (dsi node reg + 4) enabled in video
mode; INTF_1's timing engine on and a sane mode; every pipe with a source
address shows the simple-framebuffer's address (so qcom_dpu has turned the
SMMU's translation off; otherwise physical addresses would be translated)
with `SSPP_SRC_FORMAT` 0x000236ff or 0x000237ff (linear XRGB/ARGB8888,
`dpu_hw_sspp.c:210` with `mdp_format.c`'s entries), `SSPP_SRC_UNPACK_PATTERN`
0x03020001 and the mode's size; the VRAM carveout below 4 GiB.

**Scanout.** Every buffer is an msm GEM object, and without an IOMMU (GPU 2,
decision A) every one is in msm's VRAM carveout: physically contiguous, its
"iova" (`msm_gem_get_and_pin_iova(obj, NULL, ...)`) the physical address.
The pipe's `SSPP_SRC0_ADDR` (0x14) has 32 bits and the display's SMMU
translates nothing, so with `hw.msm.kms=1` the carveout is allocated below
4 GiB (`msm_bsd_vram_alloc()`; the phone's RAM starts at 0x40000000, so
3 GiB qualify). `prepare_fb` pins the object and refuses one above 4 GiB,
`cleanup_fb` unpins it. A commit writes, for each scanning pipe,
`SSPP_SRC0_ADDR` = address + offset + src_y * pitch + src_x * 4,
`SSPP_SRC_YSTRIDE0` (0x24, bits 15:0) = pitch and `SSPP_SRC_FORMAT` (0x30)
= the found format without `SRCC3_EN` (bit 8: buffers are shown opaque,
ARGB8888 as XRGB8888), then `CTL_FLUSH` = the pipes' bits | bit 17 (6.12's
`dpu_hw_ctl.c:26, 47`; `dpu_hw_ctl_trigger_flush_v1()`, line 134, writes
`CTL_FLUSH` last as well). Dumb buffers (`msm_gem_dumb_create`: pitch
aligned to 32 pixels, `MSM_BO_SCANOUT | MSM_BO_WC`) and GBM's buffers from
freedreno are both such objects; PRIME from another device does not occur
(msm refuses imports without an IOMMU, and there is no other GPU).

**Flip completion.** The hardware takes a flush at the frame start and
clears the bits in `CTL_FLUSH`; 6.12's `dpu_encoder_phys_vid_vblank_irq()`
(`dpu_encoder_phys_vid.c:328-357`) counts a kickoff done when the pending
flush bits read back clear. KMS does the same: `atomic_flush` writes the
flush first and then arms the commit's event (a vblank in between makes the
event a frame late, never early); each vblank sends it once `CTL_FLUSH`
shows none of its bits (`hw.msm.kms_flips`; a vblank with the bits still
set counts in `kms_flips_late`). The commit tail is
`drm_atomic_helper_commit_tail_rpm()`'s order (the CRTC's vblank on before
its planes are flushed) with `drm_atomic_helper_wait_for_flip_done()`
instead of waiting for a vblank, so a buffer is unpinned only once the pipe
fetches from the next. Turning the CRTC off (DPMS off, the last client
gone, `pro1x-gpu`'s `RMFB`, unload) points the pipes back at the console's
framebuffer, flushes and waits up to 40 ms for the flush, then sends the
event: the panel stays on and shows the console.

**Vblank: the interrupt.** INTF_1's vsync is bit 27 of the DPU's top
interrupt (`MDP_SSPP_TOP0_INTR`; `dpu_6_3_sm6115.h:96-97`: underrun 26,
vsync 27), whose `INTR_EN`/`INTR_STATUS`/`INTR_CLEAR` are at 0x010, 0x014,
0x018 of the "mdp" registers (`dpu_hwio.h:14-16`; the DPU 6.x set,
`dpu_hw_interrupts.c:67`). The DPU's interrupt line goes into the MDSS
top (0x5e00000), an interrupt controller in Linux's device tree
(`sm6115.dtsi`: the mdss node has `interrupts = <GIC_SPI 186
IRQ_TYPE_LEVEL_HIGH>`, `interrupt-controller`; the mdp node
`interrupt-parent = <&mdss>; interrupts = <0>`, dsi0 `<4>`); its
`HW_INTR_STATUS` (0x010, `msm_mdss.c:23, 78-100`) says which block
interrupts, and it has no mask of its own: a block's own enables decide.
The GIC SPI is a resource of the mdss node, which `qcom_dpu` owns in
newbus, so `freebsd/patches/0053` takes it there:

- At attach, before the interrupt is set up, every enable is cleared (DPU
  `INTR_EN`, `INTR2_EN` 0x008, `HIST_INTR_EN` 0x01c, INTF_1's own 0x6a9c0,
  INTF_1's tear check 0x6d800; the DSI controller's masks in
  `DSI_INTR_CTRL`) and every status written 1 to clear: nothing interrupts
  until a consumer asks.
- The handler (an ithread handler: the consumer takes sleep mutexes)
  reads `HW_INTR_STATUS`. Bit 0, the DPU: all statuses acknowledged as
  `dpu_core_irq()` does (`dpu_hw_interrupts.c:240`; INTR, INTR2, HIST,
  INTF_1, tear), the enabled ones (`INTR_STATUS & INTR_EN`, `INTR2_STATUS &
  INTR2_EN`) handed to the eventhandler `qcom_dpu_mdp_intr`. Bit 4, DSI:
  its statuses taken into a latch the DCS code's waits read
  (`dsi_intr_status()`), written back to clear; on the error status the
  error interrupt is turned off, as Linux's `dsi_error()` does first
  (`DSI_IRQ_MASK_ERROR`, bit 25 of `DSI_INTR_CTRL` 0x10c, `dsi.xml`).
  A source still asserted after 1000 passes in a row gets all enables
  turned off and a line on the console.
- `msm.ko` registers for `qcom_dpu_mdp_intr` by name
  (`EVENTHANDLER_REGISTER`; the list is the kernel's,
  `EVENTHANDLER_LIST_DEFINE` in qcom_dpu), so it needs no symbol of the
  patch: it asks `dev.qcom_dpu.0.irq` first, and on a kernel without
  0053 (or with `hw.qcom_dpu.irq=0`, `hw.msm.kms_irq=0`) it polls. KMS sets
  `INTR_EN` bit 26 (underruns, counted) at set-up and bit 27 while DRM wants
  vblanks (`enable_vblank`/`disable_vblank`), and clears both before it
  deregisters, which waits for a running handler.

**Vblank: the poll.** Without the interrupt, a callout looks at INTF_1's
`INTF_FRAME_COUNT` (0x0ac; Linux sets `INTF_FRAME_LINE_COUNT_EN` to 3,
`dpu_hw_intf.c:238`) every quarter frame while vblanks are wanted, and
handles a vblank whenever it changed; if the counter does not run (checked
over 50 ms at set-up), a vblank per frame time. The frame counter is also
DRM's hardware vblank counter (`max_vblank_count` 0xffffffff, as
`dpu_kms.c:1213`) when it runs.

**qcom_dpu and vt.** Nothing has to step aside: qcom_dpu writes the pipes
only once, at its attach, long before `msm.ko`; afterwards it only sends
DCS commands over DSI, which KMS never touches, so brightness
(`backlight(8)`), `dev.qcom_dpu.0.display`, `lpm`, `partial` and `sleep`
keep working beside KMS (the stand-by screen's tools use them). vt stays
on `vt_simplefb` at 0x5c000000: KMS registers no fbdev (no
`drm_fbdev_*_setup()`, so drm-kmod's `vt_drmfb` never attaches), and
whenever no client framebuffer is on the plane the pipes show the console's
framebuffer again. While X runs, vt is in `KD_GRAPHICS` (Xorg's console
handling) and does not draw; console messages meanwhile land in the hidden
framebuffer. The only change to qcom_dpu is that it now owns the MDSS
interrupt (0053).

## X

FreeBSD's `xorg-server` 21.1.25 package (2026Q4) has the modesetting driver
with glamor and DRI3: `modesetting_drv.so` and `libglamoregl.so` (which
needs `libgbm.so.1`, `libepoxy.so.0`), the server needs `libxshmfence`
(DRI3) and `libgbm`. What the driver needs from the kernel, and what this
KMS gives:

| modesetting / glamor needs | here |
| --- | --- |
| `DRM_CAP_DUMB_BUFFER` (`KMS doesn't support dumb interface` otherwise), `CREATE_DUMB`/`MAP_DUMB` | msm's `dumb_create`/`dumb_map_offset` (VRAM, write combining) |
| the legacy KMS ioctls (`SETCRTC`, `PAGE_FLIP` with events, `ADDFB2` with modifiers, `WAIT_VBLANK`); atomic only with `Option "Atomic"`, off by default | the atomic helpers behind the legacy ioctls; `PAGE_FLIP` events and `WAIT_VBLANK` from the vblank above |
| GBM on the card's fd (glamor's EGL) | Mesa's GBM loads `msm_dri.so` by the driver name `msm`: freedreno |
| `IN_FORMATS` with LINEAR for scanout buffers | the plane's only modifier |
| PRIME for DRI3 (pixmaps to clients and back) | drm-kmod's PRIME; the render node and card0 are one device, so imports of its own buffers are the same object |
| a cursor plane | none: `SWcursor` (X draws the pointer) |

Rotation: the panel is portrait, the session landscape (as scfb's
`Rotate "CW"` and the MeeGo session's `xrandr --rotate right`). The plane
has no rotation property, so modesetting turns the screen through a
shadow buffer it allocates with glamor (a 1080 x 2160 GBM buffer): one GPU
copy per update instead of scfb's CPU rotation. Page flipping is off while
the screen is turned (modesetting's Present does not flip a rotated CRTC);
the vblank still times Present.

The configuration is a second directory,
`/usr/local/etc/X11/xorg-kms.conf.d/10-pro1x-kms.conf` (the image's
`xorg.conf.d` with scfb stays the default): `Driver "modesetting"`,
`Option "kmsdev" "/dev/dri/card0"`, `AccelMethod "glamor"`, `SWcursor`,
`Monitor-DSI-1` with `Option "Rotate" "right"` (xf86Crtc's monitor
option: turned at the server's start), `AutoAddGPU` off, and the
touchscreen's calibration matrix of `10-pro1x.conf` (the screen is turned
the same way). `~/.xinitrc` turns `DSI-1` right with `xrandr` if the server
has not (nothing happens on scfb, which has no `DSI-1`); `xrandr` is in
the image's packages now. `tools/devday/fbsd-x.sh hildon kms` loads the
modules (userdata's `freebsd/modules` first) with `hw.msm.kms=1
hw.msm.vram=256m`, takes userdata's Mesa (`freebsd/mesa`, out/gpu/mesa)
through `LD_LIBRARY_PATH` and friends if it is there, starts X with
`-configdir` (userdata's `freebsd/xorg-kms.conf.d` first) and turns the
screen with `xrandr` once X is up if needed.

## Device runs

`pro1x.test=gpu pro1x.gpu=kmsN` in `/host/freebsd/cmdline`
(`pro1x-gputest`, which sets `hw.msm.kms=1 hw.msm.vram=256m` unless the
command line has them, loads `firmware linuxkpi linuxkpi_video dmabuf
lindebugfs drm qcom_dsp msm` and checks `Initialized msm 1.12.0`,
`/dev/dri/card0`, `renderD128`, `hw.dri.0.busid` `platform:msm`). Needs
the kernel with `0053` for the interrupt (without it the runs pass with
the poll), the image with the GPU firmware and modules (or userdata's
`freebsd/modules`, `freebsd/gpu-tools`). Each run is a reboot.

| Run | Does | Expected on the console | The script checks |
| --- | --- | --- | --- |
| kms1 `pattern` (60 s) | `pro1x-gpu kms 10`: the KMS resources, two dumb buffers of the mode, the test pattern set with `SETCRTC` and shown 5 s (eight colour bars, white frame, red square top left, green top right, blue bottom left of the portrait panel), then 10 s of page flips with a white bar moving down, each waited for; the framebuffers removed | `qcom_dpu0: MDSS interrupt on, all sources off` (with 0053); `kms: DSI at 0x5e94000 in video mode (DSI_CTRL 0x...)` (bits 0, 1 set); `kms: INTF_1: "1080x2160": 60 162559 1080 1116 1140 1236 2160 2176 2177 2192 0x48 0x0`; `kms: VIG0: source 0x5c000000, 1080x2160 at 0,0, out 1080x2160 at 0,0, stride 4320, format 0x2...ff, unpack 0x3020001` (DMA0 likewise or source 0); `kms: INTF_1 frame counter N -> N+3 in 50 ms`; `msm: kms: the DPU's interrupts from qcom_dpu's MDSS interrupt`; `kms: 1080x2160, 1 pipe (VIG0), flush 0x20001, vblank INTF_1 vsync interrupt, frame counter`; from pro1x-gpu: `ok: connector DSI-1 connected, 1080x2160`, `plane ...: formats XR24 AR24`, `ok: SETCRTC: the test pattern ...`, `ok: N page flips, N events in 10.0 s: 60.0 flips/s` (about 60, a flip per frame); the console's text again at the end | `hw.msm.kms_info` not `off`; the DSI line; connector, SETCRTC, page flips |
| kms2 `vblank` (60 s) | kms1, then 120 `WAIT_VBLANK`s | `ok: 120 vblanks waited for in 2.00 s: 60.0 Hz`; `hw.msm.kms_vblanks` up by more than 120 (flips and waits), `kms_flips` about the flips, `kms_flips_late` small, `kms_underruns: 0`; `dev.qcom_dpu.0.intr_mdp` rising with them (`intr_mdp_none`, `intr_stray` 0) | the vblank line; the counters; no underrun; with 0053 the MDSS interrupts |
| kms3 `xorg` (90 s) | `Xorg :0 -configdir xorg-kms.conf.d`; `xrandr --query`; `egl-pbuffer x11` (a GLES client through DRI3); the screen kept (`xwd` into `/dev/pro1xshot`) | Xorg.0.log: `modeset(0): ... /dev/dri/card0`, `glamor X acceleration enabled on FD610`, `glamor initialized`, `Output DSI-1 connected`, the rotation; `DSI-1 connected primary 2160x1080+0+0 right (...)`; the client: `GL_RENDERER: FD610`, `middle 255,0,0 corner 0,0,255: OK` | glamor on FD6xx; `DSI-1` turned right; the client's renderer |
| kms4 `hildon` (90 s) | `startx` with `pro1x.desktop=hildon` and `-configdir xorg-kms.conf.d`, 30 s, the screen kept | as kms3 in the log; `hildon-desktop runs`; no `llvmpipe` thread in hildon-desktop (`procstat -t`), a DRM device open (`fstat`); `top -H` shows Xorg and hildon-desktop well below scfb's (devday 12.8: Xorg's rotation and llvmpipe's compositing took most of the A73s) | glamor on FD6xx; hildon-desktop without llvmpipe |

`pro1x.test=script` runs userdata's `freebsd/test.sh` instead, for a run
put together on the day (e.g. `fbsd-x.sh hildon kms`).

What may go wrong first, and what the lines tell:

- `kms: ... does not show the console's framebuffer`: qcom_dpu did not
  attach or the pipes carry other addresses: no KMS (correct: without the
  SMMU bypass our physical addresses would be translated).
- The pattern does not appear, the console stays: the flush is not taken
  (`kms: flush 0x... still pending`), or the pipe wants another bit; the
  `CTL_0 layer` value in the INTF_1 line says which pipes Linux staged.
- Garbage or a shifted picture: the stride (dumb pitch 4352 for 1080
  pixels) or the format; a black picture: alpha (the format is written
  without `SRCC3_EN`).
- Flips at 30/s or `kms_flips_late` high: the event armed after the
  vsync (harmless) or the flush taken a frame later than assumed.
- No events at all and `flip_done timed out` from DRM: no vblank: compare
  `dev.qcom_dpu.0.intr_mdp` (interrupt) with `hw.msm.kms_vblanks`; try
  `hw.msm.kms_irq=0` (poll).
- An interrupt storm (`MDSS interrupt stays asserted`): a source not
  cleared; boot with `hw.qcom_dpu.irq=0`.
- DCS commands failing with the interrupt on (`DCS 0x51: no CMD_DMA_DONE`):
  the handler's latch; `hw.qcom_dpu.irq=0` brings back the old path.
- `no ... bytes of contiguous memory for the VRAM below 4 GiB`: 256 MB
  contiguous could not be found; a smaller `hw.msm.vram` (128m holds the
  screens and little else), or load msm earlier.
- X: `glamor initialization failed` (GBM/EGL on card0: Mesa's freedreno
  missing, check `LD_LIBRARY_PATH`/userdata's Mesa); `KMS doesn't support
  dumb interface`; a picture turned the wrong way (the Monitor option, or
  `xrandr --rotate left`).

## Risks

- Never run: every register value above is read from Linux's sources and
  qcom_dpu's working handover, not observed under KMS.
- The interrupt path is new for the DSI side too: DCS waits now read the
  statuses through the handler's latch; whether the DSI statuses latch
  with their masks off (they are off now outside DCS transfers) is not
  known, but DCS sets the two it waits for while it sends.
- `INTF_FRAME_COUNT` taken as a 32-bit counter (as Linux's
  `max_vblank_count`); if it is narrower, DRM sees a jump at the wrap.
- VRAM: everything X and the clients allocate comes from one contiguous
  carveout (256 MB with KMS); hildon-desktop composites redirected windows
  (each full-screen window 9 MB): many windows may exhaust it
  (`msm_gem_new` fails, clients see `ENOMEM`). The IOMMU (msm-port.md's
  alternative) would lift this and the 4 GiB limit.
- Security as in GPU 2: submits need root or
  `hw.msm.unprivileged_submit=1`; a desktop session as a user needs the
  tunable, and any GPU submitter can reach all memory.
- libdrm's FreeBSD device list leaves msm out: Xorg is given the device
  (`kmsdev`), Mesa finds the driver by name; programs that enumerate
  devices (EGL device/surfaceless, some Wayland compositors) still fall
  back to llvmpipe.
- No DPMS of the panel itself: a CRTC turned off shows the console's
  framebuffer with the panel lit; switching the panel off stays with the
  stand-by screen's DCS sysctls (a later step could call qcom_dpu from
  `atomic_disable`).
- One fixed mode and the pipes Linux chose; another panel set-up (a
  `top_gap`, both pipes scanning) is followed as found, a command-mode
  panel is refused.
