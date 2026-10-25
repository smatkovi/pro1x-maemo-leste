# Kali Linux on the Pro1X

Kali boots from the same boot menu as Maemo Leste and MeeGo ("Kali Linux"):
the initramfs loop-mounts `/host/kali.img` instead of Leste's image, and
Kali runs with Leste's kernel, modules and firmware. No partition is
touched.

| Step | Script | What it does |
| --- | --- | --- |
| 10 | `10-make-rootfs.sh` | debootstrap of kali-rolling (arm64, natively on the phone in Leste) into `/host/kali.img` |
| 20 | `20-pro1x-hardware.sh` | Xfce desktop and Kali's default tools; Leste's kernel modules and firmware (`/lib/firmware/updates`, which no Kali package touches); the Pro1X stack from scripts/46 in `/usr/lib/pro1x` (hexagonrpcd, libssc, iio-sensor-proxy with SSC, pro1x-gnss, pro1x-steps); systemd units and settings (`rootfs-overlay/`) |

Both run as root on the phone in Maemo Leste:

    bash kali/10-make-rootfs.sh                  # IMG=/host/kali.img SIZE=16G
    SSH_PUBKEY=/path/key.pub bash kali/20-pro1x-hardware.sh

The boot menu entry needs an initramfs with the current
`tools/pro1x-boot` (scripts/50-build-bootimg.sh, then flash slot b).

## Setup

- User `kali`, password `kali` (Kali's default; change it with `passwd`).
  Lightdm logs in automatically; Xfce runs in landscape with the
  touchscreen turned along and doubled user interface size.
- SSH over the USB link as on Leste (`ssh kali@172.16.42.1`): password
  logins are only accepted from 172.16.42.0/24, keys from anywhere.
- WLAN and mobile data: NetworkManager. Mobile data goes through
  ModemManager (QRTR modem, rmtfs and tqftpserv as on Leste), e.g.
  `nmcli c add type gsm ifname '*' con-name mobile apn <apn>`.
- Sensors (iio-sensor-proxy), GPS (gpsd from pro1x-gnss), step counter
  (`org.pro1x.Steps`), audio (Leste's UCM profile), Bluetooth.

Untested so far: the first boot itself, and mobile data (no SIM).
