# LuneOS on the Pro1X

[LuneOS](https://webos-ports.org) (the community continuation of webOS,
built with Yocto/OpenEmbedded) boots from the same boot menu as Maemo Leste,
MeeGo and Kali ("LuneOS"): the initramfs loop-mounts `/host/luneos.img`,
and LuneOS runs with Leste's kernel, modules and firmware. No partition is
touched.

LuneOS has no generic arm64 image, but its PinePhone build is plain aarch64
userland for a mainline kernel, and its Mesa includes freedreno, the
Pro1X's GPU driver. The image is that build's root partition.

| Step | Script | What it does |
| --- | --- | --- |
| 10 | `10-luneos-image.sh` | downloads the PinePhone test build (wrynose, 2026-09-24; SHA-256 checked), extracts its root partition into `/host/luneos.img` and grows it to 8 GB |
| 20 | `20-pro1x-hardware.sh` | Leste's kernel modules and firmware (`/lib/firmware/updates`); rmtfs and tqftpserv for the modem (from Leste, `/usr/lib/pro1x`); audio UCM profile and mixer setup; the device adaptation `rootfs-overlay/usr/share/luneos/adaptations/pro1x/deviceinfo`; masks the PinePhone's eg25-manager; drops the PinePhone boot partition from fstab |

Both run as root on the phone in Maemo Leste:

    bash luneos/10-luneos-image.sh             # IMG=/host/luneos.img SIZE=8G
    bash luneos/20-pro1x-hardware.sh

The boot menu entry needs an initramfs with the current `tools/pro1x-boot`
(then a new boot image for slot b).

## Device adaptation

LuneOS's `luneos-device-config` reads
`/usr/share/luneos/adaptations/pro1x/deviceinfo` at boot (the name comes from
the device tree's `fxtec,pro1x`):

- `deviceinfo_compositor_geometry="1080x2160+0+0r90s1"`: the portrait panel
  turned to landscape with the keyboard towards the user (r270 was upside
  down on the device); the touchscreen follows.
- the touchscreen and the keys by name, since the PinePhone's input links
  shipped in the image do not exist here.

## Access

- The developer image's root has no password. Over the USB link (LuneOS's
  own gadget, moved to 172.16.42.1 like Leste and Kali; the computer is
  172.16.42.2) `ssh root@172.16.42.1` works without one; over Wi-Fi only
  with a key (`sshd_config.d/pro1x.conf`).
- Wi-Fi: connman; mobile data and calls: oFono (LuneOS's telephony only
  speaks oFono), with Leste's rmtfs and tqftpserv for the modem.

Booted (2026-10-25): the compositor and the lock screen come up in
landscape, reachable over the USB link. Untested so far: touch, keyboard
layout, sound (PulseAudio and audiod start; routing for the Pro1X's card
not done), Wi-Fi, the modem. The clock started at the RTC's raw time;
`pro1x-rtc.service` now sets it from the RTC plus the offset the phone's
systems share (`/host/pro1x-rtc-offset`, `pro1x-rtc-sync`), not yet tried
here.

Screenshots without a camera, from an SSH session with a terminal (`ssh -t`;
without one luna-send prints nothing):

    luna-send -n 1 -f luna://com.webos.surfacemanager/captureCompositorOutput \
      '{"output":"/tmp/capture.png","format":"PNG"}'

The PNG is in the panel's portrait orientation (turn it 90° counter-clockwise).
