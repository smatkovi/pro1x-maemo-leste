# Kernel updates

All the Linux systems the boot menu starts run boot_b's kernel: Maemo Leste,
the MeeGo copy, Kali, LuneOS and Arch Linux ARM. FreeBSD has a kernel of its
own (`/host/freebsd/kernel.bin`, started with kexec) and Sailfish OS stays in
slot a; neither is touched by a kernel update.

The package managers never change that kernel. Leste has no `linux-image`
meta packages any more, and Arch has no kernel package. So `apt upgrade` and
`pacman -Syu` are independent of it.

## Modules in one place

The kernel's modules are on userdata, in `/host/pro1x-modules/<version>`.
The initramfs (`initramfs/init-bottom/pro1x-shared-modules`) binds the
running kernel's directory onto `/lib/modules/<version>` of the system it
starts. Without that directory a system keeps its own modules (images made
before this).

- No image needs its own copy of the modules.
- An older Arch snapshot gets the modules of the kernel that boots it.
  Its own copy would be of the kernel it was taken under.
- The earlier kernels' modules stay, for a boot image flashed back.

For that, every kernel needs a version of its own. Build it with `KLOCAL`
(`7.1.0-pro1x.2` below); without it the version is `7.1.0+` each time.

## Steps

On the build host:

```sh
KLOCAL=-pro1x.2 OUT=$WORK/out-pro1x.2 WORK=$WORK bash scripts/30-build-kernel.sh
PHONE="ssh root@172.16.42.1" OUT=$WORK/out-pro1x.2 WORK=$WORK \
    bash scripts/65-kernel-update.sh prepare
```

`prepare` writes no partition. It:

1. puts the modules in `/host/pro1x-modules`, and the running kernel's too
   if they are not there yet;
2. installs this repository's initramfs scripts in Leste;
3. makes the initramfs for the new kernel in Leste;
4. builds the boot image (50-build-bootimg.sh) and puts it on the phone as
   `/host/boot_b-<version>.img`.

Then, with the phone's owner there:

```sh
PHONE=... OUT=... WORK=... bash scripts/65-kernel-update.sh flash
```

`flash` saves boot_b as `/host/boot_b-before-<version>.img` first, then writes
the new image into boot_b. It does not reboot.

`status` lists the module versions on the phone, the running kernel and the
saved boot images.

## Trial starts: back to slot a by itself

`flash` puts slot b on trial (`pro1x-boot trial <version> 3`). The phone's
bootloader then takes one try for every start of slot b while the slot is
not marked "successful". When none are left, it starts slot a, Sailfish
OS. It did exactly that on 2026-10-24, before `pro1x-boot` kept slot b
marked successful.

A start on trial is confirmed when it went well, and it has to end in a
reboot when it did not:

- `pro1x-boot-trial` (in every Linux system, from `66-boot-trial-install.sh`)
  waits for the user interface (Hildon, MeeGo, Phosh, LuneOS, X). Once it
  runs, it confirms the start (`/host/pro1x-boot/trial-ok`). Without one
  after ten minutes (`/host/pro1x-boot/trial-wait`), it reboots.
- The initramfs starts the SoC's watchdog for a start on trial. The kernel
  (`qcom_wdt`) feeds it until the system takes it over (dsme on Leste,
  systemd's `RuntimeWatchdogSec` on Arch, Kali, LuneOS), for at most 15
  minutes. A start that hangs thus resets the phone. A kernel panic reboots
  after 10 s (`panic=10`).
- The boot after a confirmed start ends the trial: slot b is successful
  again, with 7 tries (`pro1x-boot trial-check` in the initramfs).

So with 3 tries, a kernel that does not come up ends in Sailfish after the
third failed start, without the PC. Some details:

- Choosing FreeBSD in the menu counts as a good start: FreeBSD has its own
  kernel, and the Linux kernel got as far as the menu.
- `pro1x-boot trial-stop` ends a trial by hand, and `pro1x-boot list`
  shows it.
- Sailfish asks for its LUKS code; from there boot_b can be written back
  (see below).
- `TRIES=0 ... flash` writes without a trial.

## The way back

Btrfs snapshots do not help here. They cover Arch's root only, and boot_b
(kernel, initramfs, boot menu) is a partition of its own. Its way back is
the image `flash` saved:

- If the new kernel boots, write the saved image back from Leste:
  `TARGET=b IMG=/host/boot_b-before-<version>.img VBMETA= sh scripts/60-flash-slot-b.sh flash`.
  The old kernel's modules are still in `/host/pro1x-modules`.
- If it does not boot (fastboot after a few seconds, or a hang), write the
  saved image with fastboot from the PC. `flash` copied it there,
  `$OUT/boot_b-before-<version>.img`:
  `fastboot flash boot_b $OUT/boot_b-before-<version>.img`. Use
  /opt/android-sdk's fastboot; flashing through the PC's USB hub has failed
  before. Or boot Sailfish OS in slot a (`fastboot --set-active=a`) and
  write it from there.
