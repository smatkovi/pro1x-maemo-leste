# Arch Linux ARM on the Pro1X

Arch Linux ARM (the base of Kupfer, the Arch distribution for phones, which
has no Pro1X profile) boots from the same boot menu as Maemo Leste, MeeGo,
Kali and LuneOS ("Arch Linux ARM"): the initramfs loop-mounts
`/host/arch.img`, and Arch runs with Leste's kernel, modules and firmware.
No partition is touched.

| Step | Script | What it does |
| --- | --- | --- |
| 10 | `10-make-rootfs.sh` | Arch Linux ARM's generic aarch64 root filesystem (signature checked against the Arch Linux ARM build key) into `/host/arch.img` |
| 20 | `20-pro1x-hardware.sh` | without Arch's kernel package; Phosh (with phoc and squeekboard) as the phone shell, NetworkManager, ModemManager, BlueZ, PipeWire, gpsd; Leste's kernel modules and firmware (`/usr/lib/firmware/updates`); the Pro1X stack from scripts/46 in `/usr/lib/pro1x` (hexagonrpcd, libssc, iio-sensor-proxy with SSC, pro1x-gnss, pro1x-steps), and the modem's rmtfs, tqftpserv and qrtr-ns from Leste where Arch Linux ARM has none; Kali's systemd units for them; `rootfs-overlay/` (phoc's output: landscape, scale 2; `phosh.service`, which starts Phosh for `alarm` without a display manager, as Debian's does: Arch's phosh package has no system unit) |

Both run as root on the phone in Maemo Leste:

    bash arch/10-make-rootfs.sh                  # IMG=/host/arch.img SIZE=16G
    SSH_PUBKEY=/path/key.pub bash arch/20-pro1x-hardware.sh

The boot menu entry needs an initramfs with the current
`tools/pro1x-boot` (scripts/50-build-bootimg.sh, then flash slot b). From
then on, further systems on Leste's kernel can be added without a new boot
image: one line `key|Label|/image.img|` each in `/host/pro1x-boot/extra`.

## Setup

- User `alarm` (Arch Linux ARM's), password `147147` (digits, for Phosh's
  lock screen; change it with `passwd`); root's password is Arch Linux
  ARM's default `root`. Phosh starts for `alarm` on boot.
- SSH over the USB link as on Leste (`ssh alarm@172.16.42.1`): password
  logins are only accepted from 172.16.42.0/24, keys from anywhere.
- WLAN and mobile data: NetworkManager (Phosh's settings); mobile data
  through ModemManager (QRTR modem, rmtfs and tqftpserv as on Leste).
- Sensors (iio-sensor-proxy), GPS (gpsd from pro1x-gnss), step counter
  (`org.pro1x.Steps`), audio (Leste's UCM profile), Bluetooth.

## Desktops

Besides Phosh, three X11 sessions, each started for `alarm` on VT 7 by its
unit; `pro1x-desktop phosh|hildon|meego|harmattan` (as root) picks one:

| Session | Step | What it is |
| --- | --- | --- |
| `hildon` | `40-hildon.sh`, `41-hildon-apps.sh` | Maemo Leste's Hildon desktop with its apps, as on Leste and in the FreeBSD packages (Leste's sources at the revisions `freebsd/hildon` pins, Leste's theme, icons and sounds) |
| `meego` | the same, `42-meego-look.sh` | Hildon with the Nokia N9's look as on Leste and FreeBSD (`patches/meego`): the home views, the launcher and the open applications as pages in a ring, the edge gestures, no title bars but the N9's thin status bar, the one-column app menu, the N9's icon shapes and rolling wheels for dates and times; the Events page (`tools/pro1x-events`: the weather, notifications, the calendar's appointments, a Mastodon timeline; Settings, Events) and the N9's volume bar (`tools/pro1x-volume-osd`); the MeeGo clock and the other MeeGo apps with the N9's theme |
| `harmattan` | `30-meego.sh` | the N9's own user interface ported to Qt 5 (smatkovi/harmattan-qt5: mcompositor, the home screen, the system UI with its status menu, the settings, the N9's keyboard), its touch and event sounds |

Step 42 also builds the FM radio (`apps/pro1x-fm`, "FM Radio" in the
launcher: the WCN3950's receiver through `/dev/radio0`, the kernel's
radio-qca-fm from the shared kernel and its modules, docs/fm-radio.md; its
sound through PipeWire's PulseAudio server, as on Leste through
PulseAudio) and its test tools `pro1x-fm-test` and `pro1x-fm-audio-test`
(`tools/pro1x-fm`).

The N9's theme, fonts and sounds are Nokia's: step 30 takes them from the
phone's own Harmattan image, nothing of them is in this repository. The
apps leave out the web engines (QtWebEngine has no Arch Linux ARM package,
QtWebKit would build for hours) and so Jib and Dorian.

## Building again, on the phone or on the PC

Steps 40 and 41 build a part again when its revision or this repository's
patches for it change (`patches/hidpi`, `meego`, `fixes`, the FreeBSD ones
41 takes); `REBUILD="part ..."` builds parts again anyway, and 41 takes
`ONLY="part ..."` for just these. Bringing an image built before the MeeGo
look's steps 2 to 4 (2026-10-28) up to date, in Leste with the image on
`/mnt/arch`:

    bash arch/40-hildon.sh        # libhildon, hildon-status-menu, hildon-desktop
    ONLY="calendar-backend rtcom-eventlogger rtcom-eventlogger-plugins" \
        bash arch/41-hildon-apps.sh
    bash arch/42-meego-look.sh    # the Events page, the volume bar, FM radio

or, without building, the packages of a release (`arch-hildon-*`, made by
`OUT=/dir bash arch/42-meego-look.sh`) with `PKGS=/dir bash
arch/42-meego-look.sh`; `MNT=/` runs 42 in the running Arch itself.
`arch/pc-chroot.sh` makes a chroot of Arch Linux ARM on the PC (qemu) for
the same builds there.

## Btrfs and snapshots

`15-btrfs.sh` (as root in Leste, the image not mounted) copies the ext4
image into a Btrfs image with the subvolumes `@` (root), `@home`,
`@snapshots` (`/.snapshots`), `@log` (`/var/log`) and `@pkg` (pacman's
cache), and sets up snapper with snap-pac: a snapshot before and after
every pacman transaction (installs and updates), up to 50 kept. The ext4
image stays as `/host/arch-ext4.img`.

- `pro1x-snapshot-menu.path` writes the snapshots to
  `/host/pro1x-boot/snapshots/arch` whenever `/.snapshots` changes; the
  boot menu then has "Arch Linux ARM: snapshots", a second menu with all
  of them, newest first, eight to a page ("More ..." for the next).
- A snapshot boots read-only with its changes in RAM (an overlay; gone at
  the next start). `pro1x-rollback <number>` makes it Arch's root for good
  (the one before stays as `@.before-rollback-<date>`).
- From another system: `pro1x-boot next arch@<number>`, `pro1x-boot list`
  shows the numbers.
- This needs a boot image whose initramfs mounts Btrfs roots and knows
  the snapshot menu (`tools/pro1x-boot`, `initramfs/`).

## If it hangs

The first start goes through `pro1x-boot next arch` (once; the boot
menu's default stays Leste), so every way back is a reboot:

- systemd feeds the SoC's watchdog (`qcom_wdt`, as dsme does on Leste):
  a hung system resets the phone after 30 s; a kernel panic reboots after
  10 s.
- Until `pro1x-boot-ok` has been run once (over SSH or in a terminal),
  `pro1x-boot-guard.timer` reboots 15 minutes after the start, for a boot
  that came up without USB networking or WLAN.
- What neither covers (a hang in the boot menu or before the kernel has
  its watchdog driver): holding the power key restarts the phone, and
  the menu starts Leste after 5 s.

First boot (2026-10-28): Arch comes up on Leste's kernel in about a
minute, with SSH over USB, NetworkManager, ModemManager, the DSP daemons
and the watchdog; phoc draws with the GPU (freedreno, FD610). Phosh does
not stay up yet: its home bar asks for a layer surface of height 0
anchored only at the bottom, and phoc ends the connection ("height 0
requested without setting top and bottom anchors"). The clock starts at
1970 until a saved RTC offset exists (`/host/pro1x-rtc-offset`);
`pro1x-rtc.service` waits for the RTC (rtc0, a module's, about 17 s into
the boot) before it puts the offset on top.

Untested so far: everything on the phone (the first boot, the screen's
rotation, touch), and mobile data (no SIM).
