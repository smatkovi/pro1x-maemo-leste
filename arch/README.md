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

Untested so far: everything on the phone (the first boot, the screen's
rotation, touch), and mobile data (no SIM).
