# Kali Linux on the Pro1X

Kali boots from the same boot menu as Maemo Leste and MeeGo ("Kali Linux"):
the initramfs loop-mounts `/host/kali.img` instead of Leste's image, and
Kali runs with Leste's kernel, modules and firmware. No partition is
touched.

| Step | Script | What it does |
| --- | --- | --- |
| 10 | `10-make-rootfs.sh` | debootstrap of kali-rolling (arm64, natively on the phone in Leste) into `/host/kali.img` |
| 20 | `20-pro1x-hardware.sh` | Xfce desktop and Kali's default tools; Leste's kernel modules and firmware (`/lib/firmware/updates`, which no Kali package touches); the Pro1X stack from scripts/46 in `/usr/lib/pro1x` (hexagonrpcd, libssc, iio-sensor-proxy with SSC, pro1x-gnss, pro1x-steps); systemd units and settings (`rootfs-overlay/`) |
| 25 | `25-mobile-data.sh` | only for an image made before 2026-10-30: the modem's start order for mobile data (rmtfs and ModemManager drop-ins, `pro1x-modem-wait`) and the APN database; in Kali (`sudo bash kali/25-mobile-data.sh`) or from Leste (`MNT=/mnt/kali`) |

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
  ModemManager's `qcom-soc` plugin: the modem on QRTR (started by rmtfs,
  tqftpserv serves its files, as on Leste), data through the IPA's
  `rmnet_ipa0`, on which ModemManager makes a link per connection
  (`qmapmux0.0`):
  `sudo nmcli c add type gsm ifname '*' con-name mobile gsm.auto-config yes gsm.pin-flags 2`,
  then `sudo nmcli --ask c up mobile`; or `gsm.apn <apn>` instead of
  `gsm.auto-config yes`. SIM PIN: NetworkManager asks for it (`--ask`,
  nm-applet in Xfce), or `mmcli -i any --pin=...` from a root shell (not
  through sudo, which logs the command line). How it fits
  together, the test and what can go wrong: [docs/kali-mobile-data.md](../docs/kali-mobile-data.md).
- Sensors (iio-sensor-proxy), GPS (gpsd from pro1x-gnss), step counter
  (`org.pro1x.Steps`), audio (Leste's UCM profile), Bluetooth.

The first boot worked (2026-10-25). Untested so far: mobile data (no SIM
until 2026-10-31).
