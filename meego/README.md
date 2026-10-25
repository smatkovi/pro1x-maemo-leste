# MeeGo Harmattan on the Pro1X

The boot menu entry "MeeGo Harmattan" starts Maemo Leste without its
desktop; Leste stays the hardware layer (kernel, Xorg, mce, dsme, icd2,
PulseAudio, modem services) and Harmattan's user interface runs on top of it
from an N950 rootfs image (`/host/harmattan.img`) in a chroot.

## How it fits together

| Step | Script | What it does |
| --- | --- | --- |
| 20 | `20-debian-armhf.sh` | Debian trixie armhf tree with glibc, Mesa (freedreno) and current X libraries |
| 30 | `30-graft-debian-runtime.sh` | Grafts that runtime into the Harmattan tree (Harmattan's "armel" is hard-float ARMv7, ABI-compatible with armhf); converts prelinked RELA back to REL; `ld-linux.so.3` stub (`ldcompat/`) |
| 35 | `35-nokshim.sh` | Nokia EGL extensions (`EGL_NOK_image_shared`, lock_surface2, ...) on top of Mesa (`nokshim/`) |
| 36 | `36-aegis-permissive.sh` | libcreds without the Aegis kernel module: every credential check passes, tokens translate (`aegis/`) |
| 37 | `37-harmattan-user.sh` | Harmattan's user gets Leste's uid 1000 |
| 38 | `38-hardware-adaptation.sh` | Touch input in the N9's format for Qt and mcompositor (`touch/`), small binary patches |
| 39 | `39-leste-mce.sh` | Leste's mce closes Harmattan's lock screen (`patches/`) |
| 41 | `41-leste-n9-sensors.sh` | The Pro1X's accelerometer and proximity sensor as N9 input devices for sensord (`sensors/`) |
| 40 | `40-harmattan-session.sh` | Mounts and starts the session (installed as `/usr/lib/pro1x-meego/harmattan-session`, run by the `pro1x-meego` OpenRC service in the MeeGo boot) |

Steps 20-38 and 41 work on the mounted image (`H=/mnt/harmattan`); 39 and
41 also build on Leste. `30-graft-debian-runtime.sh undo` restores the
original Harmattan files.

## Status

Works: GPU-accelerated MeeGo Touch UI in landscape (960x480), touch with
scrolling and edge swipes (minimise, close), lock screen, hardware
keyboard, WLAN (Leste's icd2), applications through applauncherd
including Python/PySide ports, music library (tracker), orientation
sensor.

## Towards parity with Leste

In this order:

1. Sensors: orientation (done), keyboard slide state (udev names)
2. Harmattan's session daemons from its own upstart jobs: timed (clock),
   qmkeyd (keys), meego-im-uiserver, ngfd/mfeedbackd (tones, haptics),
   ohmd + ohm-session-agent (audio policy: playback), mdecorator,
   clipboard, contactsd, commhistoryd
3. Battery: a BME server (`/tmp/.bmesrv`) fed from Leste's power supply
4. Connectivity UI: Harmattan's radio states API in Leste's mce
   (the status menu shows "flight mode" without it)
5. Bluetooth: bluez 4 (Harmattan) or bluez 5 (Leste)
6. GPS
7. Cameras

Open details: tearing (Xorg 21.1's modesetting driver has no TearFree;
the screen is rotated and scaled), MIT-SHM BadAccess when applications go
to the background.
