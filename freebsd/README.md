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
from an mtree of the tarball). It is only the step on the PC from which
`rootzfs.sh` makes the ZFS pool: the root on the phone is ZFS only.
The RAM root (`mfsroot/etc/rc`) mounts userdata read only at
`/dev/pro1x-host`, imports the pool and reroots into it (`reboot -r`;
`patches/0012` keeps filesystems below `/dev` across the reroot, `0016`
makes the mount point). With the read-only `root.zfs`, `/var` and `/tmp`
are in memory (`rootfs/etc/rc.conf`): FreeBSD does not write to userdata. Login on the
console as root (no password). devd powers the phone off when the power
key is held for 2 s; powerd scales the CPU frequency.

`rootzfs.sh` turns `root.ufs` into `out/root.zfs`, the same system as a
ZFS pool (`pro1x`, the root in `pro1x/ROOT/default`, zstd compressed,
about half the size), with FreeBSD's own `zpool` and `zfs` in a FreeBSD
machine (`vm/vm.sh`): FreeBSD's `makefs -t zfs` stops at the image's hard
links when it runs on Linux. On the phone it is `/host/freebsd/root.zfs`;
the RAM root imports the pool read only and reroots into it (the kernel
has ZFS). Without a ZFS root (`root.zfs` or the writable `pool.zfs`
below) no system starts: there is no UFS fallback any more. Tried
in the VM (`vm/try-zfsroot.sh`): the pool from a memory disk, the reroot,
the Pro1X's system up to its login (2026-10-26); on the phone not yet.

### Writable ZFS, boot environments, a way back

FreeBSD only reads userdata: it is ext4, holds every system on the phone,
and FreeBSD's ext2fs must not write it. A writable pool lives in a file
on it instead, `freebsd/pool.zfs`, which FreeBSD writes through the
file's own blocks, never through ext4:

1. **On Leste**, `tools/pro1x-freebsd-pool/pro1x-freebsd-pool create
   [--size 8G]` makes `pool.zfs` from `root.zfs`: every byte written (no
   holes or unwritten extents, which Linux would read as zeros whatever
   FreeBSD wrote), ZFS's end labels moved to the new end, then
   `chattr +i` (nothing on Linux can write, move or delete it). Next to it
   `pool.map`: its inode, size and its blocks on the partition (FIEMAP).
   ext4 keeps metadata every 2 GiB, so a pool of some GB is in several
   ranges.
2. **The RAM root** (`mfsroot/etc/pro1x-pool.sh`) reads the map with
   userdata mounted read only, checks that it belongs to that very file
   (inode, size), unmounts userdata (ext2fs holds it exclusively even
   read only), makes a GEOM nop provider over each range and
   concatenates them. The nops are shared (`patches/0029`,
   `kern.geom.nop.shared`): GEOM allows one exclusive writer per
   provider, so ordinary nops over several ranges of one partition could
   not all be written. Only with all four ZFS labels of a pool `pro1x`
   there is the pool imported read-write, else everything is undone and
   the read-only `root.zfs` starts as before. The pool grows
   into the file (`autoexpand`).
3. **Boot environments**: the root is `pro1x/ROOT/<name>`, the pool's
   `bootfs` unless `pro1x.be=<name>` is in `freebsd/cmdline`; at boot,
   `b` + Enter within 5 s lists them to choose one. root's home is
   `pro1x/root`, outside them, so going back keeps it. With the writable
   root `/var` is on it too (pkg's database, logs; `rootfs/etc/rc.conf`
   by kenv `pro1x.rootrw`).
4. **Before package changes** a boot environment of the running system:
   `pro1x-pkg` (root's shell: `alias pkg=pro1x-pkg`) runs
   `bectl create pkg-<date>` before `pkg install/upgrade/delete/
   autoremove` and keeps the last five. pkg itself has no hooks for this:
   calling `/usr/sbin/pkg` directly skips it. Should an upgrade leave
   FreeBSD unable to start: `b` at boot and the `pkg-...` environment, or
   in the RAM root (`pro1x.root=ram`, Enter within 10 s for a shell)
   `bectl activate pkg-...`, or `zfs rollback`.

Tried in the VM (2026-10-27): an ext4 userdata like the phone's (same
features, fragmented on purpose), the pool file in three ranges; wrong
maps (inode, ranges shifted by 4 KiB) refused; the pool imported
read-write through the shared nops, grown to the file, 300 MiB written,
scrubbed without errors, read back after a new import; boot
environments created, activated, removed; afterwards `e2fsck -fn` clean
and every other file on it unchanged. Not tried: the reroot into it
(the VM's test pool has no system) and anything on the phone. Making
`pool.zfs` on the phone writes 8 GB to userdata: only with the user's
go.

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
genmon; as on the Nokia N9, dimmed after the display time-out of 30 s
(`pro1x.display_timeout`) and 3 s later, or on a short press of the
power key, the N9's stand-by screen: the time, unread notifications and
the charging state on black, in the panel's low power mode at a low
brightness with only the rows around the content driven (partial mode;
`pro1x.standby_partial=0` drives all of them), the panel off while the
proximity sensor is covered (once
the DSP stack runs: `hexagonrpcd_enable` and `pro1x_proximity_enable`,
both off by default until tested), woken
up by a double tap or the power key; the keyboard light on with key
presses and off 30 s after the last one, as on the N950
(`tools/pro1x-standby`, sleeping on X's idle time alarms until something
happens; with `pro1x.standby=off` black after 3 minutes, since scfb has
no DPMS), `startx hildon`, `startx harmattan` (the Nokia N9's own user
interface, `harmattan-session`) or
`startx xfce`; without an argument `pro1x.desktop=hildon` (or `harmattan`) in
`freebsd/cmdline` picks that one. "Hildon" in Xfce's menu and among the
N9's applications, "Xfce" in Hildon's and the N9's, "MeeGo" in Xfce's and
Hildon's, or N9 and Xfce in Hildon's Settings, Desktop (`apps/pro1x-look`),
switch without leaving X (`pro1x-desktop hildon|harmattan|xfce`):
`~/.xinitrc` runs the desktops one after the other on the same server.
A short press on the power key goes to the stand-by screen (or blanks
the screen) or brings the desktop back, held
for 2 s it powers the phone off (`pro1x-powerkey`). The RGB status LED
shows charging (orange) and full (green), as on Leste
(`rc.d/pro1x_statusled`).

`freebsd/cmdline` on userdata (it overrides `pro1x.env`, `patches/0018`;
`kern.smp.disabled=0` starts all eight CPUs): `pro1x.root=ram` stays in
the RAM root; `pro1x.test=panel` steps through brightness, low power
mode, partial mode and panel off/on with times on the console; `pro1x.test=1` makes
the full system report, cycle the
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
| display | `qcom_dpu` (`0017`): Linux's msm keeps the display running across kexec (`device/kernel/patches/0005`), FreeBSD points its scanout at the bootloader's framebuffer (SMMU context of the display bypassed): without it the screen stays black, since msm drives the display from the boot menu on. `0022`: DCS commands to the panel the way msm sends them (command DMA between frames, low power mode): the brightness as `backlight(4)` (`backlight(8)`, 0-100 %), `dev.qcom_dpu.0.display` (panel off/on), `dev.qcom_dpu.0.lpm` (the panel's idle mode, 8 colours at a reduced refresh), `dev.qcom_dpu.0.partial` (partial mode: only rows "first last" driven), as the N9's panel driver did for its stand-by screen |
| watchdog | `qcom_wdt` (`0004`): the boot menu arms the APSS watchdog before the jump, FreeBSD feeds it; a hang resets the phone into the boot menu |
| GPIOs | `qcom_tlmm_sm6115` (`0006`): GPIOs and their interrupts; on them `gpiokeys` (Shift, Ctrl, Alt, Fn, Home, camera key, the slider switch), `gpioiic` (the touchscreen's bus), `gpioled` (keyboard backlight) |
| touchscreen | `goodix` (`0007`), GT9286, multi-touch through evdev |
| keyboard | `geniiic` (`0008`: device tree attachment of FreeBSD's GENI I2C driver, QUP wrapper bus) and `pro1xkbd` (`0009`): the 8x8 matrix behind the AW9523B, console keyboard and evdev; the boot menu keeps the I2C engine's clocks on before the jump |
| PMIC | `qcom_spmi` (`0011`): the SPMI arbiter (register access, interrupts), `qcom_pon` (power and volume-down keys: evdev, devctl), `qcom_rtc` (the seconds counter plus the offset Leste's `pro1x-rtc` keeps; the boot menu hands it over from `/host/pro1x-rtc-offset`; its alarm as the suspend's wake-up, `dev.qcom_rtc.0.wakealarm`, `0035`, `pm/README.md`) |
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
| display | the framebuffer Linux leaves running (`qcom_dpu`); brightness, panel off and low power mode by DCS commands (untested on the phone) |
| GPU (Adreno 610) | none: FreeBSD's DRM has no msm driver; Mesa's software renderer |
| touch, keyboard, keyboard light, slider | yes (`goodix`, `pro1xkbd`, `gpiokeys`, `gpioled`) |
| status LED, vibration | yes (`qcom_lpg`, `aw86224`) |
| battery, charging | state of charge (`pro1x_battery`); the charger itself runs as the bootloader set it |
| clock across power off | yes, with Leste's offset (`qcom_rtc`); wake alarms: alarmd's into the RTC's alarm, which ends a suspend (`pm/README.md`, untested on the phone) |
| CPU frequency, all CPUs | frequency yes; all eight CPUs from `kern.smp.disabled=0` (run 10 tests it) |
| power management (autosuspend, CPU idle states) | `powerd` only: CPUs idle with `wfi`, no deeper idle states, no suspend on arm64 (`docs/power-management.md`, FreeBSD) |
| storage | UFS on the link Linux brought up; no SD card |
| Hildon, Xfce | yes, switchable |
| USB (network to the PC, host) | not yet: FreeBSD's dwc3 is host only, and Linux takes the controller down at kexec |
| Wi-Fi | built, untested on the phone: ath10k over SNOC on the DSP stack (`dsp/README.md`: remoteproc, GLINK, QRTR, QMI) |
| Bluetooth | the chip only, for the FM radio (`fm/README.md`); no Bluetooth stack |
| modem: calls, SMS, GPS | built, untested on the phone: `pro1x-modemd` (QMI over QRTR through libqmi) as oFono's D-Bus API, so Leste's sphone runs unchanged (tried in the VM against a simulated modem); the voice of a call through `pro1x-audiod` (`pro1x-callaudio`); GPS through gpsd, location-daemon and liblocation (`modem/README.md`) |
| mobile data | built, untested on the phone: `qcom_ipa` (a port of Linux's IPA driver for the IPA v4.2: GSI, endpoints, the QMI handshake with the modem, QMAP with MAPv4 checksum offload, interfaces `qmapN`) and the sessions in `pro1x-modemd` (DPM, WDA, WDS; oFono's ConnectionManager); host tested against models of the hardware and the modem (`data/README.md`) |
| audio | built, untested on the phone: sound(4) as `snd_pro1x.ko` (`/dev/dsp`, `/dev/mixer`), the PCM through the ADSP by `pro1x-audiod` (ASM, ADM, AFE over `/dev/apr`), SoundWire and the codec from replay sequences of a Leste trace (recorded 2026-10-31); nothing heard before those (`audio/README.md`) |
| FM radio | built, untested on the phone: the Bluetooth UART (`qcom_geni_uart`), the chip and its receiver from user space (`pro1x-fmd`), the app; no sound (`fm/README.md`) |
| sensors, step counter | built, untested on the phone: the DSP stack (`dsp/`), `pro1x-steps`, `pro1x-proximity` (the proximity sensor on D-Bus only while the stand-by screen watches it) |
| cameras | built, untested on the phone: `qcom_camss.ko` (the CCI as iicbus buses, the camera clocks, pins and power domain, raw frames through CSIPHY, CSID and the TFE into memory, `/dev/camss0`), `pro1x-cam` (power, chip IDs, raw frames, JPEG with exposure and autofocus on the CPU; `camera/README.md`), a small Hildon camera app (built, staged for the next image) |

## Files

| File | |
| --- | --- |
| `build.sh` | builds everything on a Linux PC (clang, lld, libarchive): sources, cross toolchain, root filesystem, kernel, boot image |
| `PRO1X` | kernel config: only what the start needs, the frameworks for the SoC drivers and the Pro1X drivers (a GENERIC-based kernel hung early; its drivers for blocks without clocks are the suspects) |
| `pro1x.env` | static kernel environment: one CPU (PSCI under Qualcomm's hypervisor is untested), verbose boot |
| `mfsroot/etc/rc` | shows what the kernel found, then reroots into the ZFS root on userdata (`pool.zfs` read-write, else `root.zfs` read only); without one, Enter within 10 s for a shell, else reboot |
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
| `patches/0021-*` | LinuxKPI DMA for platform devices (the DSP stack's Wi-Fi, in progress) |
| `patches/0022-*` | `qcom_dpu`: brightness, panel off/on, sleep, low power and partial mode by DCS commands |
| `patches/0023-*` … `0028-*` | power, checked against the Nokia N9: watchdog fed every 15 s, held keys scanned every 30 ms, UFS Auto-Hibernate, the battery read by time with events for devd, the random harvester at 1 Hz once seeded, the touchscreen's sleep (`docs/power-management.md`) |
| `patches/0029-*` | shared GEOM nops (`kern.geom.nop.shared`): the writable pool's ranges inside userdata |
| `patches/0035-*` | `qcom_rtc`'s alarm as a wake-up (`dev.qcom_rtc.0.wakealarm`), `power_wakeup` for the suspend (`pm/README.md`) |
| `mfsroot/etc/pro1x-pool.sh` | the writable pool through the file's blocks, boot environment menu |
| `rootfs.sh`, `rootfs/` | the full system's root image and its configuration |
| `rootzfs.sh` | the root image as a ZFS pool |
| `vm/` | a FreeBSD/arm64 VM on the PC (qemu): packages, ZFS image, tries |
| `pkgfetch.py`, `mkspec.py` | packages with dependencies, checked; the image's owners and modes |
| `hildon/` | Leste's Hildon desktop cross-compiled for FreeBSD |
| `fm/` | the FM radio: the Bluetooth UART, the WCN3950's receiver, its tests (`fm/README.md`) |
| `camera/` | the cameras: CCI, clocks, pins, CAMSS, `pro1x-cam`, its tests (`camera/README.md`) |

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
