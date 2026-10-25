# MeeGo Harmattan on the Pro1X

The boot menu entry "MeeGo Harmattan" starts a Maemo Leste system without
its desktop; Leste stays the hardware layer (kernel, Xorg, mce, dsme, icd2,
PulseAudio, modem services) and Harmattan's user interface runs on top of it
from an N950 rootfs image (`/host/harmattan.img`) in a chroot. That Leste
system is MeeGo's own copy (`/host/meego-leste.img`, step 45), independent
of the Leste system the "Maemo Leste" entry boots; without the copy the
entry uses the Leste image.

## How it fits together

| Step | Script | What it does |
| --- | --- | --- |
| 20 | `20-debian-armhf.sh` | Debian trixie armhf tree with glibc, Mesa (freedreno) and current X libraries |
| 30 | `30-graft-debian-runtime.sh` | Grafts that runtime into the Harmattan tree (Harmattan's "armel" is hard-float ARMv7, ABI-compatible with armhf); converts prelinked RELA back to REL; `ld-linux.so.3` stub (`ldcompat/`) |
| 35 | `35-nokshim.sh` | Nokia EGL extensions (`EGL_NOK_image_shared`, lock_surface2, ...) on top of Mesa (`nokshim/`) |
| 36 | `36-aegis-permissive.sh` | libcreds without the Aegis kernel module: every credential check passes, tokens translate; aegis-exec as a plain user switch (`aegis/`) |
| 37 | `37-harmattan-user.sh` | Harmattan's user gets Leste's uid 1000 |
| 38 | `38-hardware-adaptation.sh` | Touch input in the N9's format for Qt and mcompositor (`touch/`), the battery from the kernel's power supply class in place of the N9's BME (`bme/`), timed's wake alarms translated for the Pro1X's RTC (`../tools/pro1x-rtc`), small binary patches |
| 39 | `39-leste-mce.sh` | Leste's mce closes Harmattan's lock screen (`patches/`) and has Harmattan's radio states, mapped onto the device mode (`mce/`) |
| 41 | `41-leste-helpers.sh` | Leste-side helpers of the session: the Pro1X's sensors, slide switch and side keys as the N9's input device (`sensors/`), dsme's IP heartbeat service (`iphb/`), PulseAudio's main volume interface for the volume bar (`mainvolume/`), BME's event file (`bme/`), Nokia's positioning daemon for QtLocation on Leste's gpsd (`positioning/`), Leste applications in Harmattan's application grid (`leste-apps/`) |
| 40 | `40-harmattan-session.sh` | Mounts and starts the session (installed as `/usr/lib/pro1x-meego/harmattan-session`, run by the `pro1x-meego` OpenRC service in the MeeGo boot); runs Harmattan's own upstart jobs (`run_job`) |
| 45 | `45-meego-image.sh` | MeeGo's own copy of the Leste system underneath (`/host/meego-leste.img`), so that the MeeGo boot no longer shares the Leste system's image; run it last |

Steps 20-38 and 41 work on the mounted image (`H=/mnt/harmattan`); 39 and
41 also build on Leste. `30-graft-debian-runtime.sh undo` restores the
original Harmattan files.

## Status

Works: GPU-accelerated MeeGo Touch UI in landscape (960x480), touch with
scrolling and edge swipes (minimise, close), lock screen, display
blanking, status bar clock, hardware keyboard and slide state, WLAN
(Leste's icd2), applications through applauncherd including Python/PySide
ports, music library (tracker) and playback, orientation sensor, volume
keys with Harmattan's volume bar (like on the N9 only while media plays or
during a call: ohmd's volume_mode), battery level and charging state. Session jobs started from Harmattan's
upstart files: timed, ohmd, policy-cfd, qmkeyd, meego-im-uiserver, ngfd,
mfeedbackd, ohm-session-agent and the rest of group-load-launcher.

## Towards parity with Leste

In this order:

1. Sensors: orientation, keyboard slide state (done)
2. Harmattan's session daemons from its own upstart jobs, audio playback,
   volume keys (done)
3. Battery (done)
4. Connectivity UI: Harmattan's radio states API in Leste's mce (built,
   active after the next reboot; to test: flight mode in the status menu)
5. Bluetooth: Harmattan's own bluetoothd 4.91 (its UI speaks bluez 4's
   API) runs for the session in place of Leste's bluez 5, which comes back
   when the session ends; the adapter is named Pro1X. Open: Bluetooth audio
   (Leste's PulseAudio only knows bluez 5); discovery and pairing still to
   be tried with a device in range
6. GPS (QtLocation through `positioning/` and Leste's gpsd; gpsd only starts
   once Leste's location disclaimer is accepted; tested with injected NMEA)
7. Cameras: Leste's camera app (pro1x-camera on libcamera) starts from
   Harmattan's application grid, like every other Leste application with a
   menu entry (Firefox ESR, gPodder, the Hildon applications...);
   `leste-apps/` writes their entries, keeps them in step with installed
   packages and starts them on Harmattan's request
   (/etc/pro1x-meego/leste-apps leaves some out or adds others). Harmattan's
   own camera application needs the N9's camera stack and stays out

Open details: the edge swipe's "sneak peek" animation renders black
(compositing in general works; ruled out: server grab, unredirection,
the dim shader, window alpha), tearing (Xorg 21.1's modesetting driver has no TearFree;
the screen is rotated and scaled), MIT-SHM BadAccess when applications go
to the background.
