# Handoff: Maemo Leste on the F(x)tec Pro1X -- status 24.10.2026, 17:15

To follow along in the browser: <https://claude.ai/artifact/S8E6wTczpWF4kZie3P8wXd>

This handoff is for a session **on archbang** (192.168.1.21, user
`sebastian`). The build tree, artifacts, backups and `fastboot` access live
there. The phone itself is no longer a reliable place to work: the earlier
sessions ran *on* Sailfish and died with every boot attempt.

## Update 18:25 -- LESTE BOOTS (from slot B)

18:24:15 reboot into slot B, 18:24:31 the g_cdc gadget shows up
(`0525:a4aa`), shortly afterwards `ssh root@172.16.42.1`: Devuan 5 / Maemo
Leste, kernel 7.1.0+, `/` = `/dev/loop0` from `/host/maemo-leste.img`, Xorg +
hildon-desktop running, keyboard (aw9523), Goodix touch, haptics, panel (msm
DRM, fb0), modem/adsp/cdsp remoteprocs up.
Missing: `qcom/a630_sqe.fw` (GPU), `goodix_9286_cfg.bin`, `regulatory.db`.
On the PC the gadget interface is called `enp19s0u1u1` (not `enx*`/`usb*`);
set `172.16.42.2/24` by hand.

**Why the images were rejected:** with an invalid/zeroed dtbo the ABL looks
for the DTB **appended to the kernel** (strings in the ABL: "Single appended
DTB found", "kernel image does not have appended DTB"); it uses the DTB
section of header v2 only together with a valid dtbo (SoC DTB + board
overlay, that is how Sailfish boots). A rejection shows up as fastboot 5-9 s
after the reboot.

**Working recipe** (`test-appended/out/boot-leste.img`, sha256
72057d38...):

* kernel = `cat out/Image.gz test-boardid/out/sm6115-fxtec-pro1x.dtb`
  (DTB with an additional `qcom,board-id = <0 0>` via fdtput -- whether that
  is needed is untested)
* initramfs without `usr/lib/modules/*/kernel` (everything boot-relevant is
  `=y`), `depmod` rerun, xz `--check=crc32 -6`: 4.9 MiB instead of 11.2 MiB
  (`test-slim/out/initrd.img-7.1.0+`; whether that is needed is untested)
* `mkbootimg --header_version 0`, offsets as before, no `--dtb`,
  then `avbtool add_hash_footer` as in `50-build-bootimg.sh`
* `dtbo_b` zeroed, `vbmeta_b` = `out/vbmeta-disabled.img`
  (`mkvbmeta.py`, flags=2, padded to 64 KiB; whether that is needed is
  untested)

`50-build-bootimg.sh` still builds the old (non-booting) v2 variant.
(Fixed since commit 3ee4a26: the script now builds the v0 recipe above.)

**Variants tested in slot B** (all written with `dd` from Sailfish):

| Time | Image | dtbo_b | vbmeta_b | Result |
|---|---|---|---|---|
| 17:48 | Sailfish `boot_a` | `dtbo_a` | factory | boots (Sailfish) |
| 17:59 | v2, DTB section | 0 | flags=2 | fastboot after 7 s |
| 18:06 | v2 + board-id | 0 | flags=2 | fastboot after 5 s |
| 18:14 | v2 + board-id + slim initrd | 0 | flags=2 | fastboot after 9 s |
| 18:24 | **v0, DTB appended to kernel** + board-id + slim | 0 | flags=2 | **Leste runs** |

**Back to Sailfish:** bootloader, `fastboot set_active a`, `fastboot
reboot` (repeat the commands until OKAY; `/opt/android-sdk/platform-tools/
fastboot` usually gets there faster). Sailfish then asks for the LUKS code on
the device; until then `pam_nologin` blocks all non-root SSH logins.
Leste does not mark slot B as "successful" -- after a few boots the ABL falls
back to A by itself.

## Update 17:50 (session on archbang) -- H1 refuted

**Slot B boots.** Test: `boot_a.img` and `dtbo_a.img` from
`/userdata/dualboot-backup/` copied with `dd` to `boot_b`/`dtbo_b` (hashes
checked after `drop_caches`: 583b77f7..., be54fca8...), `fastboot set_active
b`, reboot 17:48:30 -- Sailfish reports `androidboot.slot_suffix=_b` at
17:49:11. That puts **H2** in play: our Leste boot image does not boot, the
slot was never the problem. The active slot is `b` (Sailfish) afterwards,
`boot_a`/`dtbo_a` unchanged.

**Fastboot cannot write here.** On the CPU USB2 controller (bus 7, behind the
built-in Genesys hub 05e3:0608) about 10 % of the commands arrive garbled
(`unknown command`, `GetVar Variable Not found`, `Invalid Slot` for the same
command). `fastboot flash boot_X` with 96 MiB reproducibly fails with
`Requested download size is more than max allowed`, with `-S 32M` the
download hangs. Short commands (`set_active`, `reboot`) work with retries.
**Consequence:** never flash Leste to `boot_a` -- there is no way back via
fastboot. Write only with `dd` from Sailfish, test only in slot B.

**Correction to "kernel format":** Sailfish's 4.19 kernel has
`text_offset = 0x80000`, not 0. Ours has 0 -- according to the NixOS U-Boot
recipe the ABL accepts 0, so probably not the cause. Unpacked size of both
kernels ~43 MiB, no difference.

**Differences from the working NixOS route** (candidates for H2):
NixOS flashes `vbmeta` with verification disabled (`mkvbmeta.py`, flags=2)
**and** erases dtbo; we had the factory vbmeta **with** a zeroed dtbo. NixOS
uses header v0 with the DTB appended to `Image.gz`, we used header v2 with a
DTB section. Sailfish's DTB has `qcom,board-id = <0 0>`, ours does not
(NixOS' U-Boot DTB does not either). Next test in slot B: Leste image +
`vbmeta_b` from `mkvbmeta.py` + zeroed `dtbo_b`.

## Status in one sentence

Everything is built and ready, **but slot B does not boot** -- two attempts,
both using up all six ABL retries and falling back to Sailfish automatically.
Sailfish itself is undamaged.

## Current state (measured, not guessed)

The phone runs Sailfish OS 5.0.0.78 from **slot A**, reachable as
`defaultuser@192.168.1.15` (archbang's SSH key is installed, `sudo` without a
password).

GPT slot attributes (Qualcomm bit layout: priority 48-49, active 50, retry
51-53, successful 54, unbootable 55):

| Partition | hi16 | Prio | Active | Tries | Successful | Unbootable |
|---|---|---|---|---|---|---|
| `boot_a` | 0x007f | 3 | yes | 7 | yes | no |
| `dtbo_a` | 0x007f | 3 | yes | 7 | yes | no |
| `vbmeta_a` | 0x107f | 3 | yes | 7 | yes | no |
| `boot_b` | 0x0082 | 2 | no | **0** | **no** | **yes** |
| `dtbo_b` | 0x0040 | 0 | no | 0 | yes | no |
| `vbmeta_b` | 0x1040 | 0 | no | 0 | yes | no |

On the phone:

| Path | Size | Content |
|---|---|---|
| `/userdata/maemo-leste.img` | 24 GiB (3.5 GB used) | Leste rootfs, ext4 clean, 6291456 blocks |
| `/userdata/boot-leste.img` | 96 MiB | boot image with AVB footer, identical to `boot_b` |
| `/userdata/dualboot-backup/` | 243.7 MB | `boot_a/b`, `dtbo_a/b`, `vbmeta_a/b`, `misc`, `devinfo`, `frp`, `keystore`, GPT of all 6 LUNs |
| `/userdata/60-flash-slot-b.sh` | -- | flash script (requires the argument `flash`) |
| `~defaultuser/reboot-bootloader.py` | -- | reboot into the bootloader via the `reboot(2)` syscall |

On archbang under
`/run/media/sebastian/2e638a7f-26db-4e89-9446-81d688464798/pro1x-leste/`:

| Path | Content |
|---|---|
| `repo/` | git repo, identical to <https://github.com/smatkovi/pro1x-maemo-leste> (public) |
| `out/Image.gz` | Linux 7.1 + 14 Pro1X patches, 16.1 MiB |
| `out/initrd.img-7.1.0+` | 11.2 MiB, zstd, with loop-root hooks |
| `out/sm6115-fxtec-pro1x.dtb` | 80 KiB |
| `out/boot-leste.img` | 96 MiB, header v2 + AVB footer |
| `out/maemo-leste.img` | 24 GiB, **same geometry** as the copy on the phone (rsync delta possible) |
| `out/modules/` | modules 7.1.0+ |
| `build/linux/` | kernel tree, patches applied |
| `build/nixos-pro1x/` | reference port (patches, config, UCM, U-Boot recipe) |
| `backup-slot-a/` | mirror of the device backups, `SHA256SUMS.rel` verified |
| `switch-and-connect.log`, `switch2.log`, `ramboot.log` | logs of the three attempts |

## What was tried twice

**Attempt 1, 16:00:51** -- `boot_b` with a mkbootimg image without AVB
structures, `dtbo_b` zeroed, `fastboot --set-active=b`, reboot. No USB gadget
within 5 min. Afterwards Sailfish from slot A, `boot_b` at tries 0 /
unbootable.

**Attempt 2, 17:01:33** -- identical, but with an AVB hash footer
(`avbtool add_hash_footer --algorithm NONE --partition_name boot
--partition_size 100663296`), modelled exactly on the working `boot_a`.
Same result: no gadget, fallback, `boot_b` unbootable again.
According to the user the device "did not get past fastboot".

**Intermediate attempt** -- `fastboot boot out/boot-leste.img` (RAM boot,
would have been risk-free): the image is transferred (98304 KB OKAY), then
`Booting FAILED (remote: 'unknown command')`. **The Pro1X ABL does not know
`fastboot boot`.** On this device, testing necessarily means flashing.

## What this rules out

* **Slot B firmware** -- all `*_b` partitions are bit-identical to slot A
  (`xbl`, `xbl_config`, `rpm`, `tz`, `hyp`, `abl`, `modem`, `dsp`,
  `keymaster`, `devcfg`, `qupfw`, `imagefv`, `uefisecapp`, `featenabler`,
  `bluetooth`).
* **Kernel format** -- `boot_a` and our image both start with gzip (`1f8b`);
  the decompressed image has the magic `ARM\x64`, `text_offset = 0x0`, an EFI
  stub is present. Exactly what the ABL expects according to the NixOS port.
* **Initramfs compression** -- zstd, and the kernel has `CONFIG_RD_ZSTD=y`.
* **AVB structure** -- present and verified from attempt 2 on: `AVBf` at
  partition end - 64, `AVB0` (512 B) at offset 28692480, algorithm NONE,
  sha256 hash descriptor for partition `boot`. Structurally identical to
  `boot_a`.
* **Boot image offsets** -- read from `boot_a` and independently confirmed by
  `lib/bootimg.nix` of the NixOS port: base 0x0, kernel 0x8000,
  ramdisk 0x1000000, second 0xf00000, tags 0x100, dtb 0x1f00000, page size
  4096, header v2.
* **Size** -- 96 MiB image into a 96 MiB partition, payload 27.4 MiB.

## Open hypotheses, by priority

**H1 -- slot B fundamentally does not boot on this device.** In favour: the
note in the NixOS port (`lib/artifacts.nix`): *"First install: U-Boot into
boot_a (slot b does not boot on this phone)"* -- someone with a working port
saw the same thing. And two of our attempts are consistent with it.
*Counter-argument:* the user remembers having started Sailfish from slot B
once, back then with a dead keyboard. The DTBO finding (below) fits that, but
there is no proof that slot B ever booted.

**H2 -- our boot image does not boot at all**, regardless of the slot.
Candidates: the loop-root initramfs (`root=/dev/loop0` is only created by our
`local-top` script), the adopted NixOS kernel config, or the zeroing of the
DTBO.

**H1 and H2 can be separated with a single experiment** -- see below.

**H3 -- vbmeta.** The NixOS port flashes its own vbmeta with verification
disabled (`scripts/mkvbmeta.py` in `build/nixos-pro1x/`). Our
`vbmeta_a`/`vbmeta_b` are unchanged factory images (signed, flags 0x0).
Sailfish boots with them, our image maybe not.

## The decisive next test

Flash the Leste image into the **active slot A** -- the slot that
demonstrably boots. If it boots there, the image is good and H1 is
confirmed. If it does not boot there either, H2 is in play and the slot was
never the problem.

Important: `dtbo_a` has to be cleared for this test as well, otherwise the
ABL lays the Sailfish overlay over our mainline DTB. Both are written back
from the backup afterwards.

```bash
cd /run/media/sebastian/2e638a7f-26db-4e89-9446-81d688464798/pro1x-leste

# 0. verify the backups BEFORE anything is written
( cd backup-slot-a && sha256sum -c SHA256SUMS.rel )

# 1. phone into the bootloader (running Sailfish, slot A)
ssh defaultuser@192.168.1.15 'sudo python3 ~/reboot-bootloader.py'

# 2. wait until fastboot shows up
until fastboot devices | grep -q .; do sleep 2; done

# 3. Leste into slot A, clear the DTBO
fastboot flash boot_a out/boot-leste.img
fastboot erase dtbo_a
fastboot --set-active=a
fastboot reboot

# 4. wait for signs of life: USB Ethernet (enx*/usb*) and ttyACM0
#    screen /dev/ttyACM0 115200     -> kernel log and login prompt
#    ssh root@172.16.42.1           -> password pro1x-leste
```

**Way back, always and in every case** (device in the bootloader; if
necessary hold the power key for 10-15 s, then the key combination or again
`reboot-bootloader.py` from Sailfish):

```bash
fastboot flash boot_a backup-slot-a/boot_a.img    # 96 MiB, with footer+vbmeta
fastboot flash dtbo_a backup-slot-a/dtbo_a.img    # 24 MiB, contains aw9523b@5b
fastboot --set-active=a
fastboot reboot
```

If the device no longer gets into the bootloader at all, EDL remains -- the
GPTs of all six LUNs are in the backup for that, but it is the last resort.

## If H1 is confirmed: U-Boot instead of switching slots

Then dual boot does not go through two slots but through **one** boot image
with a menu. `build/nixos-pro1x/packages/uboot/` and `lib/artifacts.nix` show
the recipe; the pitfall is documented there as well:

> U-Boot's arm64 Image header has text_offset = -0x80000000; the stock ABL
> rejects that (straight back to fastboot). Linux has 0 there.

So: build U-Boot, zero the 8 bytes at offset 8, gzip it, append the DTB,
package it with mkbootimg using the same offsets, put it into `boot_a`. After
that U-Boot boots both kernels as files from `userdata`:

* Entry 1 -- Sailfish: extract kernel and ramdisk from
  `backup-slot-a/boot_a.img` (offsets in the header,
  `repo/scripts/lib/bootimg-params.py` reads them), cmdline from
  `backup-slot-a/cmdline-slot-a.txt`
* Entry 2 -- Leste: `out/Image.gz`, `out/initrd.img-7.1.0+`,
  `out/sm6115-fxtec-pro1x.dtb`, cmdline as below

## Technical details that would otherwise have to be worked out again

**Kernel.** The branch `qx1050-7.1` of `sm6115-mainline/linux` is *identical
to upstream v7.1* (commit `8cd9520d35a6`) -- the device support lives
entirely in the 14 patches of the NixOS port and has to be applied in **this
order**: touch, egpio, dsi, panelgap, dpugap, audio, q6voice, camera,
keyboard, leds, power, haptics, bluetooth, ipa. Built with `LLVM=1` (clang 23,
no aarch64 GCC needed on the host), the config is
`build/nixos-pro1x/device/kernel/config.aarch64`. Module version `7.1.0+`.

**Loop root.** There is no free partition for a second rootfs (`userdata` is
the last partition of the LUN, followed by 40 sectors), and modern
initramfs-tools no longer knows `loop=`. So Leste lives as an image file on
`userdata`, and three scripts in `repo/initramfs/` do the rest:
`local-top/pro1x-loop-root` (find the backing FS by UUID, mount it on
`/host`, attach the image as `/dev/loop0`), `init-bottom/pro1x-move-host`,
`hooks/pro1x-loop` (`losetup` into the initramfs). Verified: the scripts,
`losetup`, `blkid`, `mountpoint` and busybox are contained in the initramfs.

**Cmdline of the boot image:**

```
root=/dev/loop0 rootfstype=ext4 rootwait rw
pro1x.backing=UUID=cffbd472-3eed-4cb3-b73e-d8088ac7fcdd
pro1x.image=/maemo-leste.img
loglevel=7 ignore_loglevel earlycon=msm_geni_serial,0x4a90000
console=ttyMSM0,115200n8 console=ttyGS0,115200 console=tty1
```

`tty1` comes last and therefore is `/dev/console` (the output stays on the
panel). UFS device names differ between Halium 4.19 and mainline, so the
backing FS is looked up by UUID, never by `/dev/sdaN`.

**Access in Leste.** root password `pro1x-leste` (change it!), archbang's
public key in `/root/.ssh/authorized_keys`, `sshd` starts automatically
(`S03ssh`). `usb0` statically on **172.16.42.1/24**, the PC side is
172.16.42.2/24, no DHCP. `getty` on `ttyGS0` (inittab tag `T2`) and
`ttyMSM0` (`T0`) -- Wi-Fi is **not** preconfigured, Maemo manages `wlan0`
through its own stack. The g_cdc gadget is built in
(`CONFIG_USB_CDC_COMPOSITE=y`) and binds by itself; it provides `usb0` and
`ttyACM0` on the PC.

**DTBO.** `dtbo_a` (flashed by Sailfish) has **1** entry and contains
`aw9523b@5b` (keyboard matrix), `idea_keys`, `:kbd_backlight`, Goodix touch.
`dtbo_b` is in factory state with **37** entries, and entry 0 has none of
these nodes; the Sailfish overlay is not among the 37 at all (no SHA256
match). That explains the dead keyboard the user remembers from an earlier
slot B start. For Leste this is irrelevant: the mainline DTB is in the boot
image and describes the hardware itself -- checked:
`fxtec,pro1x-aw9523-keypad` with the complete 64-key `linux,keymap`,
`qcom,wcn3990-wifi`, `qcom,wcn3950-bt`, `awinic,aw86224`,
`fxtec,pro1x-charger-icl`, `qcom,sm6115-mpss-pas`/`-adsp-pas` with the
firmware paths `qcom/sm6115/Fxtec/QX1050/*.mbn`, `sony,imx582`,
`samsung,s5k4h7`, `dongwoon,dw9800`, `qcom,qcm2290-camss`.

**Firmware blobs** come from `sm6115-mainline/firmware-fxtec-qx1050` and are
installed under `/lib/firmware` in the rootfs image.

**Pitfalls that cost time.** UFS has **4 KiB sectors** -- GPT dumps with
`bs=512` cut off the partition entries. Sailfish has **no `truncate`**
(busybox), set image sizes with `python3`. The binfmt registered on Arch
points at the **dynamically** linked `qemu-aarch64`, whose host libraries
are missing inside the chroot; `40-make-rootfs.sh` registers
`qemu-aarch64-static` for the duration of the build and restores the state
afterwards. The first transfer of the 24 GiB image via rsync takes hours
(full apparent size, 1.5 MB/s) -- instead `resize2fs -M`, a zstd stream, and
grow it again on the device; **after that** rsync with `--inplace` is the
right tool, since the geometry of both sides already matches.

## Scripts in the repo

| File | Where | Purpose |
|---|---|---|
| `00-slot-ab-check.sh` | device, root | compare A/B pairs (read-only) |
| `10-backup-slot-a.sh` | device, root | back up boot/dtbo/vbmeta/GPT (read-only) |
| `20-fetch-sources.sh` | build host | fetch the kernel branch and the Leste image |
| `30-build-kernel.sh` | build host | apply patches, build kernel + modules |
| `40-make-rootfs.sh` | build host, sudo | build the rootfs image + initramfs |
| `50-build-bootimg.sh` | build host | pack the boot image v2 **including the AVB footer** |
| `55-deploy-rootfs.sh` | device, root | first deployment of the rootfs image |
| `56-update-rootfs.sh` | device, root | updates as rsync delta |
| `60-flash-slot-b.sh` | device, root | writes: boot image into the inactive slot |
| `70-switch-slot.sh` | PC | switch slots via fastboot |
| `80-switch-and-connect.sh` | PC, detached | switch, boot, USB link, SSH test |
| `81-ramboot-and-connect.sh` | PC | RAM boot test -- **useless on this device**, the ABL does not know `fastboot boot` |
| `lib/bootimg-params.py` | both | read Android boot headers v0-v2 |
| `lib/reboot-bootloader.py` | both, root | reboot into the bootloader |

## Sources

* <https://github.com/sm6115-mainline/linux> -- kernel, branch `qx1050-7.1`
* <https://github.com/sm6115-mainline/firmware-fxtec-qx1050> -- firmware
* <https://github.com/nyymi609/fxtec-pro1x-nixos> -- device patches, config,
  U-Boot recipe, hardware status
* <https://maedevu.maemo.org/images/arm64-generic/> -- Leste images
* <https://leste.maemo.org/Leste_FAQ> -- Leste does not use Halium
* <https://gist.github.com/Kabouik/94e9e60d84c0eeae9d48e18c11ed419e> -- dual boot on the Pro1
