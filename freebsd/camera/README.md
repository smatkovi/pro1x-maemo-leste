# The cameras under FreeBSD

The Pro1X has a 12 MP rear camera (Sony IMX582, 48 MP quad Bayer used
binned, with a Dongwoon DW9800 voice coil for focus) and an 8 MP front
camera (Samsung S5K4H7). Under Leste they run on mainline Linux's CAMSS
with libcamera 0.7.2 and `pro1x-camera` (`scripts/48-camera-stack.sh`,
`apps/pro1x-camera`). FreeBSD has none of it: no CAMSS, no V4L2 for
anything but USB (webcamd), no driver for the camera's I2C controller
(CCI), no clock driver for this SoC. This directory builds the pieces the
FreeBSD way, from Linux's drivers for this SoC (the Leste kernel tree,
`build/linux`):

| Stage | What | Here | State |
| --- | --- | --- | --- |
| 1 | the sensors: power, MCLK, reset, the CCI, chip IDs | `qcom_camss.ko` (`qcom_cci`, `/dev/camss0`), `pro1x-cam id` | built; host tested (CCI protocol against a model, clock encodings); not run on the phone |
| 2 | raw frames: CSIPHY, CSID, the TFE's RDI write master | `qcom_camss.ko` (`camss_stream.c`, `/dev/camss0` streaming ioctls, mmap), `pro1x-cam raw` | built; host tested (clock rates against camss.c's formulas); not run on the phone |
| 3 | pictures: raw -> debayer -> PNG/JPEG, exposure, focus; the app | `pro1x-cam shot` (`-a`: autofocus), `focus`; `app/pro1x-camera` (Hildon) | built; host tested (RAW10, demosaicing, PNG and JPEG decoded by Pillow, the focus measure); the app only built; not run on the phone |

Nothing here has run on the phone (unreachable until 2026-10-31). What can
be checked on the PC is: `make -C freebsd/camera/test`.

Where the numbers come from: every register offset, bit, frequency table
and register sequence is generated from the Linux sources by
`gen/gen-tables.py` (`gen-tables.py LINUX_TREE`), so it can be
regenerated and compared; the device tree (`sm6115-fxtec-pro1x.dts`,
`sm6115.dtsi`) is read at run time for everything board specific.

## The hardware, as the device tree has it

| | rear | front |
| --- | --- | --- |
| sensor | IMX582 `camera@10`, CCI `i2c-bus@0` | S5K4H7 `camera@2d`, CCI `i2c-bus@1` |
| chip ID | reg 0x0016 = 0x0582 | reg 0x0000 = 0x487b |
| CSI-2 | CSIPHY 0 (`port@0`), 4 lanes, 456.8 / 286.4 MHz | CSIPHY 2 (`port@2`), 4 lanes, 367 MHz |
| MCLK | `GCC_CAMSS_MCLK0_CLK` 19.2 MHz, gpio20 `cam_mclk` | `GCC_CAMSS_MCLK2_CLK` 19.2 MHz, gpio27 |
| reset | gpio21, active low | gpio66, active low |
| rails (order of `imx582_power_on`/`s5k4h7_power_on`) | avdd18 (PCA9534 pin 0), avdd28 (pin 4), vio (PM8008 LDO7 1.8 V), vana (LDO4 2.8 V), vdig (LDO1 1.1-1.2 V) | vio (LDO7), vana (LDO6 2.8 V), vdig (LDO2 1.05-1.056 V) |
| lens | DW9800 `lens@c`: vio (LDO7), vdd (LDO5 2.8 V) | none |

The CCI (`cci@5c1b000`, "qcom,msm8996-cci") needs the camera power domain
`GCC_CAMSS_TOP_GDSC`, `GCC_CAMSS_TOP_AHB_CLK` and `GCC_CAMSS_CCI_0_CLK`
(37.5 MHz, `assigned-clock-rates`), its pins gpio22/23 and gpio29/30 in
function `cci_i2c`. The PM8008 (0x08, its regulators at 0x09) and the
PCA9534 (0x21) sit on the keyboard's I2C bus (`i2c1`, FreeBSD's
`geniiic`); the PCA9534's pins 2 and 3 are the touchscreen's rails and pin
1 the caps lock LED, so only pins 0 and 4 are changed. The PM8008's reset
is gpio26 (active low).

What Linux leaves at the kexec: nothing of the camera. The boot menu's
initramfs loads none of its modules (`initramfs/hooks/pro1x-loop`: goodix,
the watchdog, btrfs, overlay), and Linux's clock framework turns off and
parks what nobody uses. So FreeBSD has to switch on everything itself; it
reads and reports each register before it changes it. Linux's GCC driver
does configure GPLL10 (the TFE's PLL, L = 0x3c) at every boot, so it
only needs to be voted on later (stage 2).

## Stage 1: the sensors

### qcom_camss.ko

`sys/arm64/qualcomm/camss/`, one module, loaded by the test (`kldload
qcom_camss`):

- `camss_gcc.c`: the global clock controller's camera part, as Linux's
  `clk-branch.c`, `clk-rcg2.c` (`clk_rcg2_shared_ops`), `clk-alpha-pll.c`
  and `gdsc.c` do it: a branch's enable bit and its halt bit; an RCG's
  source and divider (`2 * h - 1`) and M/N/D from Linux's frequency
  table, written with the root forced on and then UPDATE; a PLL the
  source needs voted on (only if Linux configured it: L set); the GDSC
  with `gdsc_init`'s wait times, SW_COLLAPSE cleared, PWR_ON waited for.
  Without a rate an RCG is left as found and its rate reported. Tables:
  `camss_gcc_sm6115.h` (from `gcc-sm6115.c` and the dt-bindings header).
- `camss_tlmm.c`: the pin states of the device tree (`pinctrl-0`) as
  `pinctrl-msm` sets them (function = index in the pin's list of
  `pinctrl-sm6115.c`, drive, bias); a reset line as a GPIO output.
  FreeBSD's TLMM driver has no pin control.
- `qcom_cci.c`: Linux's `i2c-qcom-cci.c` (`cci_v2_data`): reset, the
  masters' timing (fast mode), each message one queue run (SET_PARAM,
  WRITE up to 11 bytes or READ up to 12, REPORT), a register read as a
  WRITE then a READ with a STOP between (as Linux), completion polled in
  `IRQ_STATUS_0`, an error halts and resets the master, a timeout the
  controller. Each master is a `qcom_ccim` device with an iicbus(4)
  child, so the buses are `/dev/iicN` (i2c(8) works on them).
  `sysctl dev.qcom_cci.0`: `hw_version`, `xfers`, `nacks`, `errors`,
  `timeouts`.
- `qcom_camss.c`: `/dev/camss0` on the `camss@5c11000` node:
  `CAMSS_IOC_INFO` (the port graph: which sensor on which CSIPHY with how
  many lanes, its CCI bus and `/dev/iicN`, address, reset line, MCLK and
  rate, link frequencies, rotation, every `-supply` resolved to "PCA9534
  pin n" or "PM8008 LDO n" with its bus and voltage range, the lens),
  `CAMSS_IOC_MCLK` (the sensor's pins and its MCLK at the device tree's
  rate, with the power domain), `CAMSS_IOC_GPIO` (only the reset lines
  the device tree names). `sysctl dev.qcom_camss.0.gcc`: every camera
  clock, PLL and GDSC as they are.
- `camss_smmu.c`: a copy of `dsp/`'s SMMU bypass (stage 2), so that the
  camera does not need `qcom_dsp.ko` (whose loading talks to the secure
  monitor and shares memory with the modem).

### pro1x-cam

`tools/pro1x-cam/`, the board's and the sensors' part in user space:

    pro1x-cam info              what /dev/camss0 read from the device tree
    pro1x-cam [-v] id [CAM]     power up, chip ID over the CCI, the lens, power down
    pro1x-cam power CAM on|off  power only (then i2c -f /dev/iicN ... by hand)
    pro1x-cam reg [-8] CAM REG [VALUE]

The power sequence (`power.c`) is Linux's: the PM8008 out of reset, each
rail in the driver's order with 2 ms between (a PM8008 LDO: its voltage
only set when outside the device tree's range, to the lowest grid step
at or above the minimum as the regulator core picks it, then ENABLE bit
7; a PCA9534 pin: output register then configuration register, other
pins kept), the MCLK, 3 ms, reset deasserted, 20 ms; off in reverse. The
lens: its rails, 5 ms, CTL = 0. The sensors' tables (`sensor_tables.h`)
are generated from `imx582.c`, `s5k4h7.c`, `dw9800.c`.

### Device run

Unattended, as the other FreeBSD tests: `pro1x.test=cam pro1x.cam=id` in
`/host/freebsd/cmdline`; `rootfs/etc/rc.local` runs
`rootfs/usr/local/libexec/pro1x-camtest`, which reports on the console
(the RAM console keeps it) and reboots. A newer `pro1x-camtest` and
`pro1x-cam` in `/host/freebsd/camera/` and the module in
`/host/freebsd/modules/` are taken first: only with the read-only
`root.zfs` (`pro1x.pool=off`), since the writable pool unmounts userdata.
`$WORK/cam-test/` has them laid out like `/host/freebsd/`, with the
command lines (`build-camera.sh` makes it; `modules/` there holds only
`qcom_camss.ko`, so it can be copied next to other tests' modules):

| Run | cmdline | Expected |
| --- | --- | --- |
| cam-id | `pro1x.test=cam pro1x.cam=id pro1x.pool=off hint.qcom_wdt.0.lifetime=180` | `camss_gcc: cci: GDSC 0x58004: ... -> 0x8...` (PWR_ON), `gcc_camss_top_ahb_clk on`, `gcc_camss_cci_0_clk on ..., 37500000 Hz` (or a refused GPLL0 AUX2: then 19.2 MHz and a slower bus), `camss_tlmm: cci: gpio22 cci_i2c: CTL ... -> ...` (function 1), `qcom_cci0: CCI HW version 0x...`, two `qcom_ccim` with iicbus/iic each; `/dev/camss0`; `info` with both sensors, their rails resolved (pca9534 pins 0/4 on the keyboard bus, pm8008 ldo1/2/4/5/6/7); `rear: IMX582 chip ID 0x0582: ok`, `rear: lens ... status 0x00: ok`, `front: S5K4H7 chip ID 0x487b: ok`; `nacks 0 errors 0 timeouts 0` |

| cam-raw | `pro1x.test=cam pro1x.cam=raw pro1x.pool=off hint.qcom_wdt.0.lifetime=300` (`pro1x.cam_sensor=rear` or `front`: one camera) | the id run, then per camera: `stream: sensor 0 CSIPHY0 4 lanes, 1920x1080 RAW10, link 286400000 Hz ...: CPHY 240000000, timer 100000000, CSID 192000000, TFE 153600000 Hz`, `camss_gcc: camss: gpll10 ... voted on` (if it was off), `qcom_smmu`-style `camss_smmu: camss: stream 0x800 ...`, `CSIPHY0 HW version`, `CSID0 ... reset done`, `TFE0 HW version`, `stream on: ...`; then `rear: frame N: means RGGB ...` for 8 frames with plausible levels (black about 64, not all 0 or 1023), `kept frame ...`, `8 frames, 0 with bus errors, 0 timeouts`; `stream off: 10 frames` |
| cam-app | `pro1x.test=cam pro1x.cam=app pro1x.pool=off hint.qcom_wdt.0.lifetime=240` | the id run, then Hildon (`startx hildon`) and `pro1x-camera` for 30 s: its log (no `No /dev/camss0`, no `No frames`), the processes, and the screen in `/dev/pro1xshot` (xwd, gzip): the rear camera's viewfinder |
| cam-jpeg | `pro1x.test=cam pro1x.cam=jpeg pro1x.pool=off hint.qcom_wdt.0.lifetime=480` | the raw run, then `rear: exposure N lines, gain G`, `rear: focus P (contrast C)`, `rear: frame N as /tmp/pro1x-cam/rear.jpg: 4000x3000, quality 90` (turned by 180 degrees), the same for the front camera without focus |

What comes back: the console (RAM console, as for every run) and, for
raw and jpeg, `/dev/pro1xshot`: a tar archive of the smallest files that
fit 3 MiB (the `.txt` statistics, the previews, `-small.jpg`, a raw frame).
On Leste, as root, right after the reboot:

    python3 pro1x-camfetch -o /root/fbsd-cam [--png]   # --png: .raw10 also as 16-bit PNG

With the writable pool (`pool.zfs`) the run also copies everything in
full size to `/var/db/pro1x-cam/<date>/` (FreeBSD's own; Leste cannot
read ZFS). FreeBSD never writes userdata.

If it hangs: the watchdog resets the phone after 180 s and the RAM console
ends at the last step, each register access being reported before it is
made. A chip ID read that NACKs with everything clocked points at the
rails (`pro1x-cam -v` lists each PM8008/PCA9534 access) or the MCLK.

Needs: a PRO1X kernel of this source tree (the module uses only
exported kernel functions, all 97 of them defined in the current kernel:
`out/kernel.bin` does) and a root image with this `rc.local` and
`pro1x-camtest` (the next `rootfs.sh`/`rootzfs.sh`; the image of
2026-10-30 09:05 has no `cam` branch, `pro1x.test=cam` runs the generic
test there); until then the run can only be started by hand from a shell
(`kldload /dev/pro1x-host/freebsd/modules/qcom_camss.ko`,
`sh /dev/pro1x-host/freebsd/camera/pro1x-camtest id`). The TLMM pins
the camera uses (gpio20-30, 66) are outside `gpio-reserved-ranges`
(0-3, 14-17); camss_tlmm refuses reserved ones anyway.

## Stage 2: raw frames

The "ISP-lite" path Linux's CAMSS uses for this SoC (CAMSS_2290):
CSIPHY -> CSID (RDI 0) -> the TFE's RDI 0 write master (7) -> memory, as
`camss-csiphy-3ph-1-0.c` (`lane_regs_qcm2290`), `camss-csid-340.c` and
`camss-vfe-340.c` program it, in Linux's order (`camss_stream.c`):

1. the clocks of the camss node (by `clock-names`) at the rates camss.c's
   formulas pick from its `*_res_2290` tables (`camss_pick_rate`: the
   first table rate above link frequency / 4 * 1.05, resp. pixel rate *
   bpp / 64 * 1.05 for the TFE, as the GCC table has it, if its source can
   run): rear 4000x3000 (456.8 MHz link, 576 MHz pixel rate) and the
   others give CPHY 240 MHz (GPLL0), timer 200 or 100 MHz (GPLL0 AUX2),
   CSID 192 MHz (GPLL6; 240 MHz from GPLL0 if GPLL6 cannot run), TFE 153.6
   MHz (GPLL10, voted on). The buses (top AHB, AHB, AXI, camnoc) as found;
   `hw.qcom_camss.axi_hz` (loader tunable) sets the AXI RCG to one of its
   table's rates if frames overflow
2. the power domain, our votes for the multimedia SMMU TBUs' GDSCs
   (`hlos1_vote_mm_snoc_mmu_tbu_rt/nrt`, which no device tree node
   claims, so Linux may have collapsed them), the node's four SMMU
   streams (0x400, 0x800, 0x820, 0x840) without translation
   (`camss_smmu.c`), since FreeBSD hands the TFE physical addresses
3. CSIPHY: hardware version, reset; CSID: reset (RST_DONE polled);
   TFE: global reset (RST_DONE polled)
4. stream on from the memory end: the write master (frame mode, PLAIN64
   packer, frame increment = stride * height) with the first two buffers
   (IMAGE_ADDR, REG_UPDATE), the CSID (receiver: lanes, lane assignment
   0x3210, PHY_NUM_SEL = CSIPHY + 1, ECC correction; RDI 0: RAW10, data
   type 0x2b, VC 0), the CSIPHY's lanes (`csiphy_lanes_enable` with the
   settle count of `csiphy_settle_cnt_calc`); user space then starts the
   sensor
5. frames: `TFE_IRQ_STATUS_0`/the bus status polled; BUF_DONE of subgroup
   RDI0 completes the oldest buffer, the next queued one goes to the write
   master (`vfe_buf_done`); overflow and violation status recorded per
   frame (`CAMSS_BUF_ERROR`)

Buffers: `TFE_BUS_IMAGE_ADDR` is 32 bits, so physically contiguous memory
below 4 GiB (contigmalloc), cached, written back and invalidated before
the TFE gets a buffer, invalidated before user space reads it; mapped with
mmap(2) of /dev/camss0 (buffer i at offset i * bufsize). They are kept
until a stream needs other sizes, not freed at STREAMOFF.

ioctls: `CAMSS_IOC_STREAMON` (sensor, size, RAW10, link frequency, pixel
rate, 2-8 buffers; returns stride, buffer size, the rates and settle
count), `CAMSS_IOC_DQBUF` (with a timeout; index, sequence, time, status),
`CAMSS_IOC_QBUF`, `CAMSS_IOC_STREAMOFF` (also on the last close),
`CAMSS_IOC_REGDUMP` (CSIPHY, CSID, TFE registers while streaming).

`pro1x-cam raw [-m MODE] [-n N] [-o PREFIX] CAM`: the camera powered, the
stream started, the sensor's registers (global sequence, mode, frame
length, exposure maximum, gain minimum, MODE_SELECT = 1), N frames with
the means of the four Bayer positions, the maximum and the saturated
share of each; the last one kept as `PREFIX.raw10` (as the TFE wrote it),
`PREFIX.txt` (size, stride, pattern, statistics) and
`PREFIX-preview.png`. Default modes are the ones whose raw frame fits
`/dev/pro1xshot`: rear 1920x1080 (2.6 MB), front 1632x1224 (2.5 MB).

## Stage 3: pictures

`pro1x-cam shot [-a] [-f POS] [-m MODE] [-q Q] [-r DEG] -o FILE.jpg CAM`,
all on the CPU (FreeBSD has no ISP; under Leste libcamera's software ISP
does it on the GPU):

- exposure: up to 12 rounds aiming the mean above black at 18 % of the
  range, the exposure lines first, then the analogue gain (codes as
  libcamera's sensor helpers for these sensors model it: IMX582 gain =
  1024 / (1024 - code), S5K4H7 gain = code / 32)
- focus (`-a`, rear only): pro1x-camera's `findFocus()`: positions 0 to
  1000 in steps of 100, then +-80 in steps of 20 around the best, one frame
  dropped after each move, the contrast (sum of neighbour differences in
  the middle half, on the green of each 2x2 quad) measured on the next;
  the lens moved in DW9800_CTRL_STEPS steps of 1 ms, parked at 0 at the
  end, as the Linux driver does; `-f POS`: a fixed position (default 100,
  the driver's `default_pos`)
- the picture: RAW10 unpacked, black level 64 (the tuning's 4096 at 16
  bits), bilinear demosaicing, grey world white balance (the Leste tuning
  has no colour matrix either), the 99.5th percentile to white, gamma
  1/2.2, rotated (`-r`; default pro1x-camera's for Hildon's landscape:
  rear 180, front 0), as baseline JPEG (`jpeg.c`: Annex K tables, 4:2:0,
  quality `-q`, default 90), and `FILE-small.jpg` at half size

`pro1x-cam focus CAM POS`: the lens to POS, its status, back to 0.

The picture's code (`shot.c`: exposure, focus sweep, demosaicing,
JPEG) is shared by `pro1x-cam shot` and the app.

### The camera app

`apps/pro1x-camera` (Leste's) is built on Qt, GStreamer and libcamera,
which has no FreeBSD backend; `app/pro1x-camera.c` is a small Hildon
(GTK 2) app on pro1x-cam's code instead, as on Fremantle: the viewfinder
fills the window (the smaller mode, rear 1920x1080, front 1632x1224,
demosaicked at half size, turned as pro1x-camera turns it for Hildon's
landscape), a tap focuses (rear: the same contrast sweep, in the
viewfinder), "Take picture", Enter or the camera key take one at full
size with exposure and the focus found into `~/MyDocs/DCIM/<date>.jpg`,
"Switch camera" changes cameras. A worker thread owns the camera; GTK is
only touched from the main loop. `app/build-app.sh` builds it against
`hildon/build.sh`'s sysroot (`build-camera.sh` calls it); it is not yet a
part of `hildon/build.sh` (that build was running), so it is not in the
image: for the device run it comes from `/host/freebsd/camera/`. Built,
not run anywhere: no viewfinder without the phone.

## Host tests

`make -C freebsd/camera/test` (ASan, UBSan):

- `test_cci`: `qcom_cci.c`'s protocol against a model of the CCI and two
  sensors: the queue words (Linux's layout), the chip IDs, writes and
  reads at every length up to the limits (11 / 12 bytes), a NACK (master
  halted and reset, the bus fine afterwards), the masters kept apart
- `test_calc`: every entry of Linux's camera frequency tables encoded
  and decoded again, all entries of one source implying the same source
  rate (GPLL0 600 MHz, AUX2 300 MHz, GPLL10 1152 MHz = L 0x3c, GPLL6
  768/384 MHz, ...; the OPE's GPLL8 entries excepted, Linux re-rates
  GPLL8 for them); the words for MCLK 19.2 MHz, CCI 37.5 MHz, TFE 153.6
  MHz; the stream's rates for every sensor mode against camss.c's
  formulas (and without GPLL6); the PM8008 voltages for the device tree's
  ranges; the CSIPHY settle count, stride, lane assignment; the ioctl
  structures' size
- `test_image` and `check_image.py`: a synthetic scene mosaicked in all
  four Bayer orders, packed as CSI-2 RAW10 and unpacked exactly,
  demosaicked (full and half size) with the right hues, rotated; PNG
  (8-bit RGB, 16-bit grey) and JPEG (also at sizes not a multiple of 16)
  decoded by Pillow, the JPEG within 1.2 levels of the PNG; the focus
  measure higher on a sharp texture than on the blurred one

## Building

`freebsd/build.sh` builds `qcom_camss.ko` into `out/modules` and
`pro1x-cam` into `out/dsp-tools` with the rest (so `rootfs.sh` puts them
into the image). For the device runs without a full build:

    WORK=$WORK bash freebsd/camera/build-camera.sh   # module (under .kbuild-lock) + tools -> $WORK/cam-test

## Files

| File | |
| --- | --- |
| `gen/gen-tables.py` | the register tables from the Linux sources |
| `sys/arm64/qualcomm/camss/qcom_cci.c` | the CCI, its masters as iicbus(4) buses |
| `sys/arm64/qualcomm/camss/qcom_camss.c` | `/dev/camss0`: the device tree's cameras, MCLK, reset lines |
| `sys/arm64/qualcomm/camss/camss_stream.c` | the raw frame path (stage 2) |
| `sys/arm64/qualcomm/camss/camss_gcc.c`, `camss_gcc_sm6115.h` | camera clocks, PLLs, power domain |
| `sys/arm64/qualcomm/camss/camss_tlmm.c`, `camss_tlmm_sm6115.h` | pin functions, reset lines |
| `sys/arm64/qualcomm/camss/camss_smmu.c` | SMMU streams without translation |
| `sys/arm64/qualcomm/camss/qcom_camss_ioctl.h` | the ioctl interface |
| `sys/modules/qcom_camss/Makefile` | the module |
| `tools/pro1x-cam/` | the tool: power (`power.c`), sensors and lens (`sensor.c`), streams (`capture.c`), pictures (`image.c`, `jpeg.c`) |
| `tools/pro1x-camfetch` | on Leste: the pictures from `/dev/pro1xshot` |
| `app/pro1x-camera.c`, `app/build-app.sh`, `app/pro1x-camera.desktop` | the Hildon camera app |
| `../rootfs/usr/local/libexec/pro1x-camtest`, `../rootfs/etc/rc.local` | the device runs |
| `build-camera.sh` | module and tools into `$WORK/cam-test` |
| `test/` | host tests |

## Known limits and risks

- **Register access with a clock off hangs the bus.** Each step is
  printed before it is made; the watchdog ends a hang.
- **PM6125 S6A** feeds the PM8008's LDO1/2 (`vdd-l1-l2-supply`). It is an
  RPM regulator FreeBSD does not touch here: voting it through
  `hw.qcom_rpm.regulator` would send its `regulator-min-microvolt`, 0.304
  V. It is assumed on as the RPM keeps it; to check on Leste:
  `/sys/class/regulator/*/{name,state,microvolts}`.
- **The PCA9534 is shared** with the touchscreen's rails: read, modify,
  write of single bits only.
- **CCI at 19.2 MHz**: if GPLL0's AUX2 output were off, the CCI's RCG
  cannot be set to 37.5 MHz and the bus runs at about half its speed with
  the same timing values (the attach says so).
- **Bus bandwidth**: Linux votes the camera's interconnect paths (`ahb`,
  `hf_mnoc`, `sf_mnoc`: icc_res_2290) at the RPM; FreeBSD has no
  interconnect driver. The display keeps the real time NoC running; if
  frames overflow (`TFE0 bus overflow`) or never arrive with everything
  else in order, this is the place to look, with the AXI rate
  (`hw.qcom_camss.axi_hz`).
- **SMMU**: the four streams get context banks with translation off, as
  for the display and the DSPs (camss_smmu.c is dsp/'s code); whether
  Qualcomm's hypervisor allows the new match entries is untested.
- **The TBUs' GDSCs**: voted on and never voted off.
- **Polling**: no interrupts are used (CCI, CSID, TFE): simpler to bring
  up, a little CPU while streaming.
- **Buffers stay allocated** from the first stream until the module is
  unloaded or a stream needs other sizes; a process keeping its mapping
  across a size change would see freed memory.
- **Colours**: grey world without a colour matrix; the sensors' Bayer
  orders are the Linux drivers' (`SRGGB10`, `SGRBG10`), not checked
  against the mounting (flips) yet.
- **`/dev/pro1xshot` is 3 MiB**: full size pictures only reach Leste
  through a raw scratch file, not made yet (the pool's map mechanism
  would do: a second immutable file with its own map and nops).
