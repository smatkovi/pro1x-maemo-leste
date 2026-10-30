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
| 2 | raw frames: CSIPHY, CSID, the TFE's RDI write master | `qcom_camss.ko` (`camss_stream.c`), `pro1x-cam raw` | see "Stage 2" |
| 3 | pictures: raw -> debayer -> PNG/JPEG, focus, an app | `pro1x-cam shot`, `focus`, `af` | see "Stage 3" |

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
`/host/freebsd/modules/` are taken first. `$WORK/cam-test/` has them laid
out like `/host/freebsd/`, with the command lines
(`build-camera.sh` makes it):

| Run | cmdline | Expected |
| --- | --- | --- |
| cam-id | `pro1x.test=cam pro1x.cam=id hint.qcom_wdt.0.lifetime=180` | `camss_gcc: cci: GDSC 0x58004: ... -> 0x8...` (PWR_ON), `gcc_camss_top_ahb_clk on`, `gcc_camss_cci_0_clk on ..., 37500000 Hz` (or a refused GPLL0 AUX2: then 19.2 MHz and a slower bus), `camss_tlmm: cci: gpio22 cci_i2c: CTL ... -> ...` (function 1), `qcom_cci0: CCI HW version 0x...`, two `qcom_ccim` with iicbus/iic each; `/dev/camss0`; `info` with both sensors, their rails resolved (pca9534 pins 0/4 on the keyboard bus, pm8008 ldo1/2/4/5/6/7); `rear: IMX582 chip ID 0x0582: ok`, `rear: lens ... status 0x00: ok`, `front: S5K4H7 chip ID 0x487b: ok`; `nacks 0 errors 0 timeouts 0` |

If it hangs: the watchdog resets the phone after 180 s and the RAM console
ends at the last step, each register access being reported before it is
made. A chip ID read that NACKs with everything clocked points at the
rails (`pro1x-cam -v` lists each PM8008/PCA9534 access) or the MCLK.

Needs: the kernel the module was built against (`build-camera.sh` says
whether `out/kernel.bin` is it) and a root image with this `rc.local` and
`pro1x-camtest` (the next `rootfs.sh`/`rootzfs.sh`); until then the run
can only be started by hand from a shell (`kldload` the module from
`/host/freebsd/modules`, `sh /dev/pro1x-host/freebsd/camera/pro1x-camtest id`).

## Host tests

`make -C freebsd/camera/test` (ASan, UBSan):

- `test_cci`: `qcom_cci.c`'s protocol against a model of the CCI and two
  sensors: the queue words (Linux's layout), the chip IDs, writes and
  reads at every length up to the limits (11 / 12 bytes), a NACK (master
  halted and reset, the bus fine afterwards), the masters kept apart
- `test_calc`: every entry of Linux's camera frequency tables encoded
  and decoded again, all entries of one source implying the same source
  rate (GPLL0 600 MHz, AUX2 300 MHz, GPLL10 1152 MHz = L 0x3c, GPLL6
  768/384 MHz, ...); the words for MCLK 19.2 MHz, CCI 37.5 MHz, TFE
  153.6 MHz; the PM8008 voltages for the device tree's ranges; the CSIPHY
  settle count; the ioctl structures' size

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
| `tools/pro1x-cam/` | the tool: power, sensors, lens, frames |
| `tools/pro1x-camfetch` | on Leste: the pictures from `/dev/pro1xshot` |
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
