# FreeBSD on the Pro1X

A real FreeBSD kernel (15.1) on the F(x)tec Pro1X: its own kernel, not
Linux's. First milestone: the kernel boots on the screen, the root
filesystem is in RAM, a shell runs. FreeBSD has no drivers for most of the
phone (keyboard, touchscreen, Wi-Fi, modem, battery); this is a bring-up
project, with the drivers added one by one (see Drivers).

## How it starts

FreeBSD's arm64 kernel carries the Linux arm64 Image header
(`LINUX_BOOT_ABI`, in GENERIC), so anything that starts a Linux kernel
can start it and hand it the device tree. Two ways, neither touches a
partition:

- **From the boot menu** ("FreeBSD"): the Linux initramfs loads
  `/host/freebsd/kernel.bin` with `kexec -s` (the device tree it booted
  with goes along) and jumps into it. From there on only FreeBSD runs.
- **Without Linux**: `fastboot boot out/freebsd-boot.img` from the PC; the
  phone's bootloader starts the kernel from RAM, once (an Android boot
  image like Leste's: gzip kernel with the device tree appended).

The console is `vt_simplefb` on the framebuffer the bootloader leaves
running (the device tree's `simple-framebuffer` under `/chosen`,
1080x2160). Before it takes over, a green stripe at the top of the screen
shows that the kernel was entered at all
(`patches/0001-arm64-pro1x-proof-of-life.patch`).

FreeBSD has no network here either, so it also leaves traces where Linux
can read them after the next boot: the spare part of the bootloader's
splash memory behind the framebuffer, which nobody else writes and which
survives the reboot. The kernel puts `PRO1X-FBSD:ENTERED` at 0x5ca00000 on
entry, and the RAM console (`patches/0003`) keeps everything FreeBSD
prints from 0x5ca01000 on. `rc` reports what it found and reboots after
10 s. In Linux the region has to be read with `mmap` in aligned 8-byte
words (`read()` on `/dev/mem` fails there).

`freebsd/cmdline` next to the kernel on userdata adds kernel environment
to the command line, e.g. `hint.ufshci.0.disabled=1`.

## Drivers

| | |
| --- | --- |
| console | `vt_simplefb` on the bootloader's framebuffer; RAM console (`0003`) |
| watchdog | `qcom_wdt` (`0004`): the boot menu arms the APSS watchdog before the jump, FreeBSD feeds it; a hang resets the phone into the boot menu |
| GPIOs | `qcom_tlmm_sm6115` (`0006`): GPIOs and their interrupts; on them `gpiokeys` (Shift, Ctrl, Alt, Fn, Home, camera key, the slider switch), `gpioiic` (the touchscreen's bus), `gpioled` (keyboard backlight) |
| touchscreen | `goodix` (`0007`), GT9286, multi-touch through evdev |
| keyboard | `geniiic` (`0008`: device tree attachment of FreeBSD's GENI I2C driver, QUP wrapper bus) and `pro1xkbd` (`0009`): the 8x8 matrix behind the AW9523B, console keyboard and evdev; the boot menu keeps the I2C engine's clocks on before the jump |
| PMIC | `qcom_spmi` (`0011`): the SPMI arbiter (register access, interrupts), `qcom_pon` (power and volume-down keys: evdev, devctl), `qcom_rtc` (the seconds counter plus the offset Leste's `pro1x-rtc` keeps; the boot menu hands it over from `/host/pro1x-rtc-offset`) |
| CPU frequency | `qcom_cpufreq_hw` (`0010`): the firmware's frequency table per cluster for cpufreq(4) and powerd |
| UFS | `ufshci` (`0005`), on the link Linux left up: the controller's PHY, clocks and regulators need Qualcomm drivers FreeBSD lacks. Linux keeps UFS powered across kexec (`device/kernel/patches/0004`), and the boot menu rebinds it with an identity mapping first, since FreeBSD does not drive the SMMU in front of it and hands the controller physical addresses. The attach reports the SMMU's view of the stream |

## Files

| File | |
| --- | --- |
| `build.sh` | builds everything on a Linux PC (clang, lld, libarchive): sources, cross toolchain, root filesystem, kernel, boot image |
| `PRO1X` | kernel config: only what the start needs, the frameworks for the SoC drivers and the Pro1X drivers (a GENERIC-based kernel hung early; its drivers for blocks without clocks are the suspects) |
| `pro1x.env` | static kernel environment: one CPU (PSCI under Qualcomm's hypervisor is untested), verbose boot |
| `mfsroot/etc/rc` | shows what the kernel found (devices, disks, a read-only look at userdata), then reboots into the boot menu after 10 s (no keyboard yet) |
| `patches/0001-*` | proof of life: the green stripe and the entry marker |
| `patches/0002-*` | building on an Arch Linux host: glibc 2.43 (two bootstrap fixes) and a clang that reports more than one "version" line |
| `patches/0003-*` | RAM console in the splash memory; keeps the kernel config's environment when the bootloader passes bootargs |
| `patches/0004-*` | `qcom_wdt`, the APSS watchdog |
| `patches/0005-*` | `ufshci` from the device tree, reusing the link Linux brought up |
| `patches/0006-*` | `qcom_tlmm_sm6115`, GPIO and GPIO interrupt controller |
| `patches/0007-*` | `goodix`, the touchscreen |
| `patches/0008-*` | `geniiic` from the device tree, the QUP v3 wrapper as a bus |
| `patches/0009-*` | `pro1xkbd`, the keyboard matrix |
| `patches/0010-*` | `qcom_cpufreq_hw`, CPU frequency |
| `patches/0011-*` | `qcom_spmi`, `qcom_pon`, `qcom_rtc`: PMIC access, keys, clock |

The root filesystem is the release's `/rescue` (one static binary for sh,
init, mount, ifconfig...) from `base.txz`, built into the kernel
(`MFS_IMAGE`).

## Known limits and risks

- Nothing appears before the console starts; the green stripe tells
  "entered" from "never reached".
- Linux could switch the display off on its way out of kexec; then the
  `fastboot boot` way shows whether FreeBSD itself is fine.
- The bootloader appends its own arguments to the command line, which
  FreeBSD then ignores; settings go into `pro1x.env`.
- `rc` reports and reboots after 10 s unless Enter is pressed, which
  starts a shell on the console (keyboard drivers untested so far).
- Every driver can be switched off for a test with a hint in
  `freebsd/cmdline`, e.g. `hint.ufshci.0.disabled=1`.
- UFS only works when Linux started FreeBSD (kexec from the boot menu);
  after `fastboot boot` nobody has set up the controller.
