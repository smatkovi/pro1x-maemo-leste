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

## The full system

`rootfs.sh` builds `out/root.ufs` (UFS) from the release's
`base.txz` with the Pro1X configuration in `rootfs/` (owners and modes
from an mtree of the tarball). On the phone it goes to
`/host/freebsd/root.ufs`. The RAM root (`mfsroot/etc/rc`) mounts userdata
read only at `/dev/pro1x-host`, attaches the image as `md1` and reroots
into it (`reboot -r`; `patches/0012` keeps filesystems below `/dev` across
the reroot, `0016` makes the mount point). The root stays read only, `/var` and `/tmp` are in memory
(`rootfs/etc/rc.conf`): FreeBSD does not write to userdata. Login on the
console as root (no password). devd powers the phone off when the power
key is held for 2 s; powerd scales the CPU frequency.

`rootzfs.sh` turns `root.ufs` into `out/root.zfs`, the same system as a
ZFS pool (`pro1x`, the root in `pro1x/ROOT/default`, zstd compressed,
about half the size), with FreeBSD's own `zpool` and `zfs` in a FreeBSD
machine (`vm/vm.sh`): FreeBSD's `makefs -t zfs` stops at the image's hard
links when it runs on Linux. With `root.zfs` next to `root.ufs` on the
phone, the RAM root imports the pool read only and reroots into it (the
kernel has ZFS); `pro1x.rootfs=ufs` in `freebsd/cmdline` keeps UFS. Tried
in the VM (`vm/try-zfsroot.sh`): the pool from a memory disk, the reroot,
the Pro1X's system up to its login (2026-10-26); on the phone not yet.

The desktop: `rootfs.sh` also puts packages from pkg.freebsd.org into
the image (`PKGS`, default Xorg with `scfb` on the framebuffer turned to
landscape, libinput, Xfce; `PKGS=` for the base system only). The index
is checked against the release's trusted key, every package against the
index (`pkgfetch.py`); what is only needed to build or for optional parts
(LLVM, Samba, Qt, Python, Perl) stays out (`PRUNE`). Owners and modes come
from the tarballs' mtrees (`mkspec.py`); the packages' users and groups,
GSettings schemas and MIME database are set up at build time, the image
loader list and font cache at boot (`rootfs/usr/local/etc/rc.d/pro1x_setup`,
which also fills root's home, a tmpfs, from `/usr/local/share/pro1x/root`).
Hildon, Leste's desktop, is in the image too when `hildon/build.sh` has
built it (`hildon/README.md`). As root on the console: `startx` (Xfce,
scaled twice for the 400 dpi screen, the battery in the panel through
genmon; as on the Nokia N9, after the display time-out of 30 s
(`pro1x.display_timeout`) and 3 s more, or on a short press of the power
key, the N9's stand-by screen: the time, unread notifications and the
charging state on black, woken up by a double tap or the power key,
`tools/pro1x-standby`; with `pro1x.standby=off` black after 3 minutes,
since scfb has no DPMS), `startx hildon` or
`startx xfce`; without an argument `pro1x.desktop=hildon` in
`freebsd/cmdline` picks Hildon. "Hildon" in Xfce's menu and "Xfce" in
Hildon's (`pro1x-desktop hildon|xfce`) switch without leaving X:
`~/.xinitrc` runs the desktops one after the other on the same server.
A short press on the power key goes to the stand-by screen (or blanks
the screen) or brings the desktop back, held
for 2 s it powers the phone off (`pro1x-powerkey`). The RGB status LED
shows charging (orange) and full (green), as on Leste
(`rc.d/pro1x_statusled`).

`freebsd/cmdline` on userdata (it overrides `pro1x.env`, `patches/0018`;
`kern.smp.disabled=0` starts all eight CPUs): `pro1x.root=ram` stays in
the RAM root; `pro1x.test=1` makes the full system report, cycle the
status LED and the vibration motor, start X for 40 s
(`pro1x.test=switch`: switching to the other desktop after 20 s), keep a
screenshot in `/dev/pro1xshot` (`patches/0014`: at 0x5cc00000, for Linux
to read after the reboot) and reboot (`rootfs/etc/rc.local`);
`hint.qcom_wdt.0.lifetime=N` lets the watchdog reset the phone N seconds
after boot (`patches/0013`), so that unattended tests always end.

## Drivers

| | |
| --- | --- |
| console | `vt_simplefb` on the bootloader's framebuffer; RAM console (`0003`) |
| display | `qcom_dpu` (`0017`): Linux's msm keeps the display running across kexec (`device/kernel/patches/0005`), FreeBSD points its scanout at the bootloader's framebuffer (SMMU context of the display bypassed): without it the screen stays black, since msm drives the display from the boot menu on |
| watchdog | `qcom_wdt` (`0004`): the boot menu arms the APSS watchdog before the jump, FreeBSD feeds it; a hang resets the phone into the boot menu |
| GPIOs | `qcom_tlmm_sm6115` (`0006`): GPIOs and their interrupts; on them `gpiokeys` (Shift, Ctrl, Alt, Fn, Home, camera key, the slider switch), `gpioiic` (the touchscreen's bus), `gpioled` (keyboard backlight) |
| touchscreen | `goodix` (`0007`), GT9286, multi-touch through evdev |
| keyboard | `geniiic` (`0008`: device tree attachment of FreeBSD's GENI I2C driver, QUP wrapper bus) and `pro1xkbd` (`0009`): the 8x8 matrix behind the AW9523B, console keyboard and evdev; the boot menu keeps the I2C engine's clocks on before the jump |
| PMIC | `qcom_spmi` (`0011`): the SPMI arbiter (register access, interrupts), `qcom_pon` (power and volume-down keys: evdev, devctl), `qcom_rtc` (the seconds counter plus the offset Leste's `pro1x-rtc` keeps; the boot menu hands it over from `/host/pro1x-rtc-offset`) |
| battery | `pro1x_battery` (`0015`): Leste's `pro1x-adc-battery` ported (PMI632 ADC voltage, gauge current, OCV table, charge counting); `dev.pro1x_battery.0.*`, `hw.acpi.battery.*`, `hw.acpi.acline` |
| CPU frequency | `qcom_cpufreq_hw` (`0010`): the firmware's frequency table per cluster for cpufreq(4) and powerd |
| vibration | `aw86224` (`0019`): the AW86224 haptics on the keyboard's I2C bus as `/dev/led/vibrator` (led(4): on/off and blink patterns), continuous drive with the Leste driver's register values; drive levels `dev.aw86224.0.*` |
| status LED | `qcom_lpg` (`0020`): the PMI632's light pulse generator, `/dev/led/status_red`, `_green`, `_blue` (led(4)), duty cycle `dev.qcom_lpg.0.<colour>` |
| UFS | `ufshci` (`0005`), on the link Linux left up: the controller's PHY, clocks and regulators need Qualcomm drivers FreeBSD lacks. Linux keeps UFS powered across kexec (`device/kernel/patches/0004`), and the boot menu rebinds it with an identity mapping first, since FreeBSD does not drive the SMMU in front of it and hands the controller physical addresses. The attach reports the SMMU's view of the stream |

## Compared with Leste

What the Pro1X does under Leste (the main README's table) and where
FreeBSD stands. Everything here is untested on the phone so far.

| Leste | FreeBSD |
| --- | --- |
| display | the framebuffer Linux leaves running (`qcom_dpu`); no brightness, no panel off yet (needs DSI commands, next) |
| GPU (Adreno 610) | none: FreeBSD's DRM has no msm driver; Mesa's software renderer |
| touch, keyboard, keyboard light, slider | yes (`goodix`, `pro1xkbd`, `gpiokeys`, `gpioled`) |
| status LED, vibration | yes (`qcom_lpg`, `aw86224`) |
| battery, charging | state of charge (`pro1x_battery`); the charger itself runs as the bootloader set it |
| clock across power off | yes, with Leste's offset (`qcom_rtc`); no wake alarms |
| CPU frequency, all CPUs | frequency yes; all eight CPUs from `kern.smp.disabled=0` (run 10 tests it) |
| power management (autosuspend, CPU idle states) | `powerd` only: CPUs idle with `wfi`, no deeper idle states, no suspend on arm64 (`docs/power-management.md`, FreeBSD) |
| storage | UFS on the link Linux brought up; no SD card |
| Hildon, Xfce | yes, switchable |
| USB (network to the PC, host) | not yet: FreeBSD's dwc3 is host only, and Linux takes the controller down at kexec |
| Wi-Fi, Bluetooth, modem, GPS | no: all run through the modem and ADSP firmware (remoteproc, QMI over GLINK, ath10k over SNOC), none of which FreeBSD has |
| audio | no: the codecs sit behind the ADSP (APR, LPASS macros, SoundWire) |
| sensors, step counter | no: ADSP (FastRPC, libssc) |
| cameras | no: CAMSS and the sensors' CCI, none in FreeBSD |

## Files

| File | |
| --- | --- |
| `build.sh` | builds everything on a Linux PC (clang, lld, libarchive): sources, cross toolchain, root filesystem, kernel, boot image |
| `PRO1X` | kernel config: only what the start needs, the frameworks for the SoC drivers and the Pro1X drivers (a GENERIC-based kernel hung early; its drivers for blocks without clocks are the suspects) |
| `pro1x.env` | static kernel environment: one CPU (PSCI under Qualcomm's hypervisor is untested), verbose boot |
| `mfsroot/etc/rc` | shows what the kernel found, then reroots into `root.ufs` on userdata; without it, Enter within 10 s for a shell, else reboot |
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
| `patches/0012-*` | reroot keeps the filesystems below `/dev` |
| `patches/0013-*` | `qcom_wdt` lifetime for unattended tests |
| `patches/0014-*` | `/dev/pro1xshot`, data for Linux behind the RAM console |
| `patches/0015-*` | `pro1x_battery`, the battery's state of charge |
| `patches/0016-*` | `/dev/pro1x-host`, the mount point below `/dev` (devfs has no mkdir) |
| `patches/0017-*` | `qcom_dpu`, the display handed over from Linux |
| `patches/0018-*` | `freebsd/cmdline` overrides the kernel config's environment (`pro1x.env`) |
| `patches/0019-*` | `aw86224`, the vibration motor |
| `patches/0020-*` | `qcom_lpg`, the RGB status LED |
| `rootfs.sh`, `rootfs/` | the full system's root image and its configuration |
| `rootzfs.sh` | the root image as a ZFS pool |
| `vm/` | a FreeBSD/arm64 VM on the PC (qemu): packages, ZFS image, tries |
| `pkgfetch.py`, `mkspec.py` | packages with dependencies, checked; the image's owners and modes |
| `hildon/` | Leste's Hildon desktop cross-compiled for FreeBSD |

The root filesystem is the release's `/rescue` (one static binary for sh,
init, mount, ifconfig...) from `base.txz`, built into the kernel
(`MFS_IMAGE`).

## Known limits and risks

- Nothing appears before the console starts; the green stripe tells
  "entered" from "never reached".
- Linux's msm display driver (built in) runs the display from the boot
  menu on and would switch it off on its way out of kexec; kernel patch
  0005 keeps it on, `qcom_dpu` takes it over. After `fastboot boot` the
  bootloader's splash setup is still there.
- The bootloader appends its own arguments to the command line, which
  FreeBSD then ignores; settings go into `pro1x.env`.
- `rc` reports and reboots after 10 s unless Enter is pressed, which
  starts a shell on the console (keyboard drivers untested so far).
- Every driver can be switched off for a test with a hint in
  `freebsd/cmdline`, e.g. `hint.ufshci.0.disabled=1`.
- UFS only works when Linux started FreeBSD (kexec from the boot menu);
  after `fastboot boot` nobody has set up the controller.
