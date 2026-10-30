# Maemo Leste as a second boot entry on the F(x)tec Pro1X

Goal: Sailfish OS stays untouched on slot A, Maemo Leste goes onto slot B as a
second system.

Status 24.10.2026: **Maemo Leste boots from slot B**, Sailfish stays in slot A,
and a boot menu in the initramfs chooses between the systems. How we got
there, every boot image variant that was tested: [docs/HANDOFF.md](docs/HANDOFF.md).

| Hardware | State on Leste | How |
|---|---|---|
| Display, GPU (Adreno 610) | works, glamor/GLES on FD610 | Mesa 25 from daedalus-backports, `a630_sqe.fw` |
| Touch, keyboard, keyboard light, slider | works | mainline drivers; mce `button-backlight` |
| Status LED (RGB) | works (charging, notifications) | DT split into red/green/blue:status, mce `led-sw` + `led-dbus` |
| Vibration | works | mce `evdevvibrator` |
| Wi-Fi | works, stable MAC | modem up via `rmtfs`/`tqftpserv`; MAC derived from the serial |
| Bluetooth | controller up | linux-firmware 2025 (`qca/cmbtfw12.tlv`); pairing not tested |
| Modem in oFono | detected; mobile data prepared | oFono 2.18 `+pro1x1` (`scripts/49-mobile-data.sh`) sets up the IPA data path itself (DPM port, WDA format, `rmnet0..3` with MAPv4 checksums); calls/SMS/data untested (no SIM), test steps in `docs/leste-mobile-data.md` |
| Loudspeaker, microphone | works | NixOS UCM profile, PulseAudio 48 kHz, suspend after 5 min (pops; fix in 7.1.0-pro1x.8 untested, `docs/audio.md`) |
| Earpiece, headphones | untested | |
| Battery, charging | works | upower |
| Accelerometer, light, proximity, compass | works | ADSP sensors: hexagonrpcd + libssc + iio-sensor-proxy 3.9 |
| Step counter | works (count while walking untested) | the ADSP's pedometer: libssc patch + `pro1x-steps` on D-Bus (`org.pro1x.Steps`) |
| Clock across power off, alarms that power the phone on | prepared (power-on by the RTC alarm untested) | the PMIC's RTC cannot be set: `pro1x-rtc` keeps the offset, `librtc-realtime` translates alarmd's and timed's wake alarms (`tools/pro1x-rtc`) |
| GPS | engine runs, positions arrive | `pro1x-gnss` (QMI LOC -> NMEA -> gpsd); liblocation and location-ui rebuilt with fixes of the privacy disclaimer and "enable positioning" dialogs (`patches/fixes/liblocation.patch`, `location-ui.patch`, in `scripts/49-hidpi.sh`; tried in the Leste chroot on the PC, `tests/leste-location`, not on the phone); outdoor fix untested |
| Cameras (12 MP rear, 8 MP front) | works | libcamera 0.7.2 + soft ISP on the GPU; `pro1x-camera` app with autofocus |
| Device lock code (CAL) | works | block2mtd file as `/dev/mtd1`, patched libcal |
| Crash logs | works | ramoops, `/sys/fs/pstore` |
| Double tap to wake | not working yet | goodix patch in the kernel, first test failed |

The key point: with an empty `dtbo` the stock ABL only accepts a **DTB appended
to the kernel** (header v0); the DTB section of header v2 is not enough.
`scripts/50-build-bootimg.sh` builds it that way.
`scripts/45-pro1x-hardware.sh` sets up the hardware support in the rootfs.

Slot B does boot on this device (proven with Sailfish's own `boot_a`/`dtbo_a`
copied there); the note in the NixOS port that slot B does not boot does not
apply here. Background on the old slot B question: [docs/slot-b-question.md](docs/slot-b-question.md).

## Starting point

| | |
|---|---|
| Device | F(x)tec Pro1X, `halium-qx1050`, SoC SM6115 (bengal) |
| Installed OS | Sailfish OS 5.0.0.78, kernel 4.19.157-perf-halium, Android HAL in an LXC container |
| SFOS rootfs | directory `/userdata/halium-rootfs` on `/dev/sda13` (225 GiB, ~129 GB free) |
| `/home` | `home.img` via dm-crypt (`home_encrypted`), 76 GiB, only ~9 GB free |
| Bootloader | unlocked (`androidboot.vbmeta.device_state=unlocked`, `verifiedbootstate=orange`) |
| Slots | A/B fully present, `_a` is active |
| `bootctl` | **not** installed on this port -> switching slots only via fastboot from a PC |
| UFS | six LUNs (sda..sdf), **4 KiB sectors** |

Full partition list: [`device/partition-map.txt`](device/partition-map.txt).
`userdata` (sda13) is the last partition of its LUN; between its end and the
end of the disk there are only 40 sectors. A partition of its own for Leste
would therefore only be possible by shrinking `userdata` -- and only offline.
That is why Leste lives as an ext4 image file on `userdata`, next to
`halium-rootfs`.

## Diagnostic results

### Slot B is technically intact

`scripts/00-slot-ab-check.sh` compares every `*_a`/`*_b` pair (content + hash):

* `xbl`, `xbl_config`, `rpm`, `tz`, `hyp`, `abl`, `bluetooth`, `modem`, `dsp`,
  `keymaster`, `devcfg`, `qupfw`, `vbmeta`, `imagefv`, `uefisecapp`,
  `featenabler`, `multiimgoem`: **populated and bit-identical to slot A**
* `mdtp`, `mdtpsecapp`, `cmnlib`, `cmnlib64`: empty in **both** slots (factory state)
* `boot_a` == `boot_b`: slot B currently holds the same SFOS hybris boot image
* `dtbo`: differs -- `dtbo_a` has 1 entry (from the SFOS flash),
  `dtbo_b` has 37 (factory state)

GPT slot attributes of `boot_b`: priority 3, retry 6, `SUCCESSFUL` set,
`UNBOOTABLE` not set -- only `ACTIVE` points at A. `vbmeta_a`/`vbmeta_b` are
unchanged factory images (signed, flags `0x0`); Sailfish boots anyway because
the bootloader is unlocked.

The NixOS port notes "slot b does not boot on this phone" -- that applies to
its U-Boot image. On this device Sailfish did boot from slot B, but without a
working keyboard: `dtbo_a` (flashed by Sailfish, 1 entry) contains
`aw9523b@5b` together with `idea_keys` and `kbd_backlight`, entry 0 of `dtbo_b`
(factory state, 37 entries) does not -- and the Sailfish overlay is not among
the 37 at all. See [docs/slot-b-question.md](docs/slot-b-question.md).

### Boot image parameters

Read from the running `boot_a` (`scripts/lib/bootimg-params.py`), stored in
[`device/boot_a-params.env`](device/boot_a-params.env):
header v2, page size 4096, base 0x0, kernel 0x8000, ramdisk 0x1000000,
tags 0x100, dtb 0x1f00000. Boot partition 96 MiB, dtbo 24 MiB.
The NixOS port uses the same offsets (`lib/bootimg.nix`) -- an independent
cross-check.

## Why mainline and not Halium

Maemo Leste does **not** use Halium/libhybris; it is a Devuan arm64 port on a
mainline kernel, and there is no official porting guide. Reusing the existing
4.19 Halium kernel would mean porting libhybris and droid-hal to Devuan -- a
lot of work and outside of what Leste supports.

Mainline on the Pro1X, on the other hand, is far along. Important: the branch
`qx1050-7.1` of `sm6115-mainline/linux` is **identical to upstream v7.1**
(commit `8cd9520d35a6`); the complete device support lives in the 14 patches
of the NixOS port, which have to be applied in that port's order.
Documented as working: display, Adreno 610, touch, hardware keyboard
including the slider, Wi-Fi, BT, audio, microphones, telephony/SMS/data, both
cameras, vibration, charging, suspend. Open/untested: GPS, SD card, NFC, video
codecs.

## What was built

| Artifact | Size | Content |
|---|---|---|
| `out/Image.gz` | 16.1 MiB | Linux 7.1 + 14 Pro1X patches, config of the NixOS port, built with clang/LLVM |
| `out/sm6115-fxtec-pro1x.dtb` | 80 KiB | matching device tree |
| `out/modules/` | 82 MiB | kernel modules 7.1.0+ |
| `out/initrd.img-7.1.0+` | 11.2 MiB | initramfs-tools with loop-root hooks |
| `out/maemo-leste.img` | 24 GiB (3.2 GiB used) | Leste rootfs (Devuan 5 daedalus, arm64-generic `20260426`) + kernel + modules + firmware |
| `out/boot-leste.img` | 27 MiB | Android boot image v2, fits into the 96 MiB partition |

The firmware blobs come from `sm6115-mainline/firmware-fxtec-qx1050`.

## Boot path

There is no free partition for a second rootfs, and modern initramfs-tools no
longer understands `root=...` with `loop=`. Hence three small scripts in
[`initramfs/`](initramfs/):

* `local-top/pro1x-loop-root` -- reads `pro1x.backing=` and `pro1x.image=` from
  the cmdline, waits for the UFS device, mounts it on `/host` and attaches the
  rootfs image as `/dev/loop0`. After that initramfs-tools finds its
  `root=/dev/loop0` as usual.
* `init-bottom/pro1x-move-host` -- moves the backing mount into the new root as
  `/host`, so the loop image does not hang off an orphaned mount (and so the
  Sailfish rootfs is reachable from Leste).
* `hooks/pro1x-loop` -- copies `losetup` into the initramfs.

Cmdline of the boot image:

```
root=/dev/loop0 rootfstype=ext4 rootwait rw
pro1x.backing=UUID=cffbd472-3eed-4cb3-b73e-d8088ac7fcdd
pro1x.image=/maemo-leste.img
loglevel=7 ignore_loglevel earlycon=msm_geni_serial,0x4a90000
console=ttyMSM0,115200n8 console=tty1
```

The device names (`/dev/sdaN`) differ between Halium 4.19 and mainline, so the
backing filesystem is always looked up by its UUID, never by a path.

## Access on first boot

Wi-Fi is not preconfigured (Maemo manages `wlan0` through its own stack,
`/etc/network/interfaces` leaves it on `manual`). Access therefore goes over
the same USB cable that is plugged in for fastboot anyway: the kernel has
`CONFIG_USB_CDC_COMPOSITE=y`, the g_cdc gadget binds by itself at boot and
provides Ethernet and a serial console at the same time.

* `usb0` statically on **172.16.42.1/24**, the PC side is 172.16.42.2/24
  (both sides static, no DHCP server in the image)
* `sshd` starts automatically (`S03ssh` in `rc2.d`), root login with a key is
  allowed (`PermitRootLogin prohibit-password`), the build host's public key
  is in `/root/.ssh/authorized_keys`
* serial console: `screen /dev/ttyACM0 115200` on the PC -- `agetty` on
  `ttyGS0`, and the kernel logs there as well (`console=ttyGS0,115200`).
  `/dev/console` stays `tty1`, so the output is also shown on the panel
* an additional `agetty` on `ttyMSM0` -- that is the debug UART and cannot be
  reached without taking the device apart, but it costs nothing
* the root password is set when the image is built (`ROOT_PW`, default
  `pro1x-leste`) -- **change it after the first login**

## Choosing the system

The initramfs of slot B shows a boot menu (Maemo Leste, MeeGo if
`/meego.img` exists on userdata, Sailfish OS). It works with any input that
still works: keyboard (arrows, digits, first letter, Enter), side keys
(volume chooses, power starts), a tap on the screen, or nothing (after the
countdown the preselected entry starts). From a running system:

```
pro1x-boot list               # systems, default, next, active slot
pro1x-boot next sailfish      # once, on the next start (switches the slot)
pro1x-boot default leste      # preselected entry
pro1x-boot timeout 5          # countdown in seconds, 0 = no menu
pro1x-boot slot a|b           # just switch the bootloader slot
```

Choosing Sailfish switches the bootloader to slot A and reboots. Back from
Sailfish: `pro1x-boot next leste` there (needs `sgdisk`), or with a PC:

```
./scripts/70-switch-slot.sh b      # on the PC, device in the bootloader
```

From either system you get into the bootloader with

```
sudo python3 scripts/lib/reboot-bootloader.py
```

(reboot(2) with `LINUX_REBOOT_CMD_RESTART2` and the argument `bootloader` --
busybox `reboot` takes no argument, and `systemctl reboot bootloader` only
exists from systemd 246 on.)

If booting slot B fails completely, the ABL counts down the retries and falls
back to slot A by itself -- Sailfish then comes back without any action.

`pro1x-boot slot` sets the Qualcomm A/B attributes like `fastboot set_active`,
but keeps "successful" set, because Sailfish never marks its own boot and
would otherwise lose its slot after seven starts.

## Plan

- [x] Inventory the device, check bootloader status and slot contents
- [x] Backups: `boot_a/b`, `dtbo_a/b`, `vbmeta_a/b`, `misc`, `devinfo`, `frp`,
      `keystore`, GPT (primary + backup) of all six LUNs -- on the device and
      on the build host, checksums verified on both sides
- [x] Extract the boot image parameters from `boot_a`
- [x] Build mainline kernel 7.1 + 14 device patches (clang/LLVM, no cross GCC)
- [x] Leste rootfs image with kernel, modules, firmware and loop-root initramfs
- [x] Build the boot image and verify it against the `boot_a` parameters
- [x] Transfer the rootfs image to the device (`/userdata/maemo-leste.img`,
      24 GiB, 3.5 GB used) and verify its content on the device
- [x] Put the boot image on the device (`/userdata/boot-leste.img`), not flashed yet
- [x] Settle whether slot B is bootable (it boots; the DTBO caused the dead keyboard)
- [x] Checked that the Leste DTB brings the hardware description itself (keypad
      with 64-key keymap, Wi-Fi, BT, haptics, MPSS/ADSP, cameras, charger)
- [x] Flash `boot_b`, zero `dtbo_b`, switch over -- Leste boots (v0 + appended DTB)
- [x] `pro1x-boot` and the initramfs boot menu, so switching works without a PC
- [x] Audio (UCM2 profiles of the NixOS port), sensors (hexagonfs), camera
- [x] oFono sees the modem (qrtrsoc)
- [x] Mobile data prepared: patched oFono over the IPA (`docs/leste-mobile-data.md`)
- [ ] Calls, SMS, mobile data with a SIM; earpiece, headphones, Bluetooth pairing
- [ ] Double tap to wake (kernel patch in place, does not wake yet)
- [ ] MeeGo as a third system (boot menu entry is ready)

## Risks

* Switching slots currently needs a PC with fastboot (no `bootctl` on the
  device) -- in the medium term a `qbootctl` on both systems solves this.
* `dtbo_b` can only suit one system: zeroed for Leste, or a copy of `dtbo_a`
  for Sailfish-on-B. Not both.
* `/home` has only ~9 GB free; the Leste image belongs in `/userdata`.
* A broken `boot_b` costs one boot attempt, not a brick -- the ABL counts down
  the retries and falls back to slot A, and `boot_a` is backed up.

## Scripts

| File | Where | Purpose |
|---|---|---|
| `scripts/00-slot-ab-check.sh` | device, root | compare A/B pairs (read-only) |
| `scripts/10-backup-slot-a.sh` | device, root | back up boot/dtbo/vbmeta/GPT (read-only) |
| `scripts/20-fetch-sources.sh` | build host | clone the kernel branch, fetch the Leste image |
| `scripts/30-build-kernel.sh` | build host | apply patches, build kernel + modules |
| `scripts/40-make-rootfs.sh` | build host, sudo | build the rootfs image, generate the initramfs |
| `scripts/45-pro1x-hardware.sh` | inside the rootfs, root | Mesa, firmware, modem services, mce/audio configuration, boot menu tools |
| `scripts/46-sensor-stack.sh` | inside the rootfs, root | hexagonrpcd, libqmi 1.34, libssc, iio-sensor-proxy 3.9, pro1x-gnss, pro1x-steps, pro1x-rtc |
| `scripts/47-libcal.sh` | inside the rootfs, root | libcal that accepts the block2mtd CAL (lock code) |
| `scripts/48-camera-stack.sh` | inside the rootfs, root | libcamera 0.7.2 with the Pro1X sensors |
| `scripts/49-apps.sh` | inside the rootfs, root | `pro1x-camera`, the Desktop applet (`pro1x-look`) |
| `scripts/49-hidpi.sh` | inside the rootfs, root | Hildon at the N900's size on 2160x1080 (`tools/pro1x-hidpi`); also the parts with bug fixes in `patches/fixes/` (liblocation, location-ui, calendar-backend) |
| `scripts/49-mobile-data.sh` | inside the rootfs, root | oFono rebuilt for mobile data over the IPA (`patches/fixes/ofono.patch`), `pro1x-modem-wait` instead of `pro1x-rmnet` |
| `scripts/50-build-bootimg.sh` | build host | pack the Android boot image (v0, DTB appended) |
| `scripts/55-deploy-rootfs.sh` | device, root | first deployment: shrink, stream, grow again (additive) |
| `scripts/56-update-rootfs.sh` | device, root | updates as rsync delta (`--inplace`) |
| `scripts/60-flash-slot-b.sh` | device, root | **writes**: boot image into the inactive slot |
| `scripts/65-kernel-update.sh` | build host | a new kernel for all Linux systems on the phone: modules to `/host/pro1x-modules`, initramfs, boot image (`prepare`); **writes** boot_b only with `flash` ([docs/kernel-update.md](docs/kernel-update.md)) |
| `scripts/70-switch-slot.sh` | PC | switch slots via fastboot |
| `scripts/80-switch-and-connect.sh` | PC, detached | waits for fastboot, switches, boots, sets up the USB link, tests SSH |
| `scripts/lib/reboot-bootloader.py` | both, root | reboot straight into the bootloader |
| `scripts/lib/bootimg-params.py` | both | read Android boot headers v0-v2 |

### Image transfer: when a zstd stream, when rsync

A first deployment via rsync is a flop: rsync reads the full apparent size of
24 GiB, measured at 1.5 MB/s -- a good four hours. Instead
`55-deploy-rootfs.sh` shrinks the filesystem with `resize2fs -M` to 3.8 GiB,
streams it zstd-compressed (measured ~7 MB/s of payload, about eleven
minutes) and grows it back to 24 GiB on the device. After that both sides
have the same geometry, and for every further change rsync is the right tool:
`56-update-rootfs.sh` transfers only the changed blocks with
`--inplace --no-whole-file`.

One more busybox pitfall: Sailfish has no `truncate`, so the image size on the
device is set with `python3`.

### Note on the build host (Arch)

No aarch64 GCC is needed, the kernel builds with `LLVM=1` (clang + lld).
For the chroot into the arm64 rootfs, the binfmt registered on Arch is **not**
enough: it points at the dynamically linked `qemu-aarch64`, whose host
libraries are missing inside the chroot (child processes fail with "required
file not found"). `40-make-rootfs.sh` therefore registers
`qemu-aarch64-static` for the duration of the build and restores the original
state afterwards.

## Sources

* <https://leste.maemo.org/Leste_FAQ> -- Leste does not use Halium
* <https://maedevu.maemo.org/images/arm64-generic/> -- arm64-generic images
* <https://github.com/sm6115-mainline/linux> -- kernel, branch `qx1050-7.1`
* <https://github.com/sm6115-mainline/firmware-fxtec-qx1050> -- firmware
* <https://github.com/nyymi609/fxtec-pro1x-nixos> -- device patches, config, UCM, hardware status
* <https://gitlab.com/postmarketOS/pmaports/-/merge_requests/5350> -- pmOS qx1050
* <https://gist.github.com/Kabouik/94e9e60d84c0eeae9d48e18c11ed419e> -- dual boot on the Pro1
