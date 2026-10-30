# Waydroid on Maemo Leste (Pro1X)

Android apps on Leste in one Hildon window: the package **pro1x-waydroid**
(`tools/pro1x-waydroid`, built by `scripts/mkdeb-waydroid.sh`) does the
preparation, then Waydroid itself is installed with apt and set up with
`waydroid init`.

**State 2026-10-30: built and tried in a chroot of Leste's arm64 image on
the PC; not run on the phone yet** (device test steps at the end).

## What the package does

| Part | What |
|---|---|
| `/etc/init.d/waydroid-container` | OpenRC service, enabled at boot: Waydroid's container manager (`waydroid container start` on the system bus, what upstream's `waydroid-container.service` does under systemd) under `supervise-daemon`. Before it starts the manager: the kernel modules of Waydroid's network (`veth`, `bridge`, `nf_nat`, `ip_tables`, `iptable_{filter,nat,mangle}`, `xt_MASQUERADE`, `xt_CHECKSUM`, `xt_tcpudp`; also `loop`, `fuse`, `overlay`), checks for binderfs and the cgroups, `pro1x-waydroid defaults`. Without Waydroid installed it does nothing and says so. Waydroid mounts binderfs and makes its binder devices itself (`/dev/binderfs`, links in `/dev`). Log: `/var/log/waydroid-container.log` (and Waydroid's own `/var/lib/waydroid/waydroid.log`) |
| `/usr/share/dbus-1/system-services/id.waydro.Container.service` | Waydroid's own D-Bus activation file is diverted (`dpkg-divert`, to `...service.waydroid`): when a session asks for the container manager and none runs, dbus-daemon starts it through the OpenRC service (`/usr/lib/pro1x-waydroid/dbus-activate`), as systemd does upstream. So the first Android start after `waydroid init` needs no manual service start |
| `/usr/bin/pro1x-waydroid` and the menu entry **Android** | a nested Wayland compositor (weston 10, X11 backend, kiosk shell) as one Hildon window below the title bar, Waydroid's session and full UI in it; closing the window stops Android. Details below |
| `/etc/apt/sources.list.d/pro1x-waydroid.list` (written by the postinst), `/usr/share/keyrings/pro1x-waydroid.gpg` | Waydroid's own repository for the Debian release under the Devuan one (daedalus: bookworm, excalibur: trixie) |
| `/etc/apt/preferences.d/pro1x-waydroid` | Waydroid's `libglibutil` over Leste's (see below); lxc without `lxc-templates`, `lxcfs`, `libpam-cgfs` |
| `/etc/dnsmasq.d/pro1x-waydroid` | Leste's dnsmasq binds the interfaces one by one (`bind-dynamic`) and leaves `waydroid0` and `lxcbr0` alone |
| trigger on `/etc/init.d/lxc-net` | lxc's `lxc-net` (a bridge `lxcbr0` with its own dnsmasq at boot) is taken out of the runlevel when lxc installs it; `WAYDROID_LXC_NET=keep` leaves it |
| Depends | `dnsmasq-base` (Waydroid's `waydroid-net.sh` runs dnsmasq, its package does not say so), `iproute2`, `kmod`, `weston`, `x11-xserver-utils` (xrandr), `python3`, `libx11-6`, `dbus`, `openrc`; Recommends `pulseaudio-utils`. Not `lxc` and `iptables`: Waydroid depends on them, and installed with it (after this package) lxc comes without the recommended packages the preferences file holds back |

Settings: `/etc/default/pro1x-waydroid` (comments there): `WAYDROID_STORE`,
`WAYDROID_WINDOW` (`window` or `fullscreen`), `WAYDROID_RENDERER` (`gl` or
`pixman`), `WAYDROID_DENSITY`, `WAYDROID_PROPERTY_DEFAULTS`,
`WAYDROID_LXC_NET`. A user's `~/.config/pro1x-waydroid.conf` may set the
window ones (read only when not running as root).

## Where Waydroid comes from, and why

- Devuan daedalus (Debian bookworm) and Leste's repositories have no
  Waydroid. Debian has it (1.6.3) only in forky, sid and
  trixie-backports (sources.debian.org, 2026-10-30), and those builds need
  Python 3.13.
- Waydroid's own repository, `https://repo.waydro.id/ bookworm main`, has
  `waydroid 1.6.3` (all), `libgbinder 1.1.45`, `python3-gbinder 1.3.1`
  (built for Python 3.11, as daedalus has) and `libglibutil 1.0.80` for
  arm64. Its index lists the arm64 packages in `binary-all`; apt takes
  them (tried).
- `waydroid` depends on `python3:any, lxc, python3-gbinder, python3-gi,
  gir1.2-gtk-3.0, python3-dbus, dbus, polkitd | policykit-1,
  pipewire-pulse | pulseaudio, iptables` -- **no systemd**; all are in
  daedalus (polkitd 122-3devuan4). Its maintainer scripts call
  `deb-systemd-helper` and `systemctl` only behind `|| true` or
  `[ -d /run/systemd/system ]`; its postinst's `waydroid upgrade -o`
  prints "not initialized" before `waydroid init`. It ships only a systemd
  unit, which is why this package has the OpenRC service.
- Upstream's install script (`curl https://repo.waydro.id | bash`) does not
  know Devuan's codenames (it takes `VERSION_CODENAME=daedalus` and gives
  up), so the package writes the source itself.
- **libglibutil**: Waydroid's `libgbinder` needs `libglibutil (>= 1.0.52)`;
  Leste has 1.0.35+4m7 and pins its repository at 1001
  (`/etc/apt/preferences.d/pin-daedalus`), so without help
  `apt install waydroid` fails. The preferences file gives
  repo.waydro.id's `libglibutil` 1002: after the package and `apt update`,
  libglibutil becomes 1.0.80 (also with a plain `apt upgrade`, before
  Waydroid is installed). The same library (Jolla's), newer; of the 153
  symbols of 1.0.35, 1.0.80 lacks two (`gutil_inotify_watch_get_type`,
  `gutil_time_notify_get_type`), and its only user in Leste's image,
  `libgofono` (for libosso-abook, libicd-network-ofono, cellulard), uses
  12 others, all present (checked with `nm -D`).

### The repository's key

`/usr/share/keyrings/pro1x-waydroid.gpg` is `https://repo.waydro.id/waydroid.gpg`
as downloaded on 2026-10-30 (sha256
`71fe05d735c812e15fe229bf10106b02b62561be8aa5280d63a58e25a5c0c5e2`):

    pub   rsa3072 2023-08-03 [SC] [expires: 2099-08-03]
          7CE0 331F 71E0 A238 BB10  02D7 0E40 6D18 1DCE E19C
    uid   Czarek Nakamoto <ci@mrcyjanek.net>
    sub   rsa3072 2023-08-03 [E] [expires: 2099-08-03]

What was checked:

- it signs the current `dists/bookworm/InRelease` (gpgv: good signature,
  2026-10-27; `SignWith: 7CE0331F71E0A238` in the Release file);
- the same fingerprint is on keyserver.ubuntu.com (with that UID) and on
  keys.openpgp.org (without UID), fetched by fingerprint on 2026-10-30;
- a user of the Whonix forum (thread "Waydroid download", 2024-01-05)
  reported the same fingerprint, created 2023-08-03, for the key
  repo.waydro.id served then: the key has not changed since at least
  January 2024.

Not available: a fingerprint published by the Waydroid project itself
(its issue 524 asking for that has been open since 2022). Kicksecure's
wiki still names the older key `0D27 43A2 4328 AE06 34DF 3557 959F E34E 90E5 1522`,
which no longer signs the repository. `scripts/mkdeb-waydroid.sh` checks the
file's sha256 and fingerprint before it packs it.

## Space

`waydroid init` downloads LineageOS 20 (Android 13) for arm64: the system
zip (870 MiB, 2026-09-27) and the MAINLINE vendor zip (141 MiB,
2026-10-22), unpacked to `system.img` (1902 MiB) and `vendor.img`
(402 MiB) in `/var/lib/waydroid/images`; each zip is deleted after it is
unpacked. So about **2.3 GiB stay, about 3 GiB are needed during the
download**. Android's `/data` (apps and their data) grows in
`~/.local/share/waydroid/data`.

Leste's root is the ext4 image `/host/maemo-leste.img` (24 GiB) on
userdata, mounted from `/dev/loop0`; `/host` is userdata itself (225 GiB,
Sailfish's partition). Check before `waydroid init`:

    df -h / /host

If the root is short, Waydroid's directory can live on userdata: before
`waydroid init`, as root,

    mkdir /host/pro1x-waydroid
    sed -i 's|^WAYDROID_STORE=.*|WAYDROID_STORE=/host/pro1x-waydroid|' /etc/default/pro1x-waydroid
    rc-service waydroid-container restart     # binds it to /var/lib/waydroid

(the service refuses when `/var/lib/waydroid` already holds a setup; move
its contents first). Android's `/data` can follow by hand (untested): as
the user, `mv ~/.local/share/waydroid /host/pro1x-waydroid/home-user && ln
-s /host/pro1x-waydroid/home-user ~/.local/share/waydroid`, with that
directory made and owned by `user` first.

## Install

On the PC (the release `leste-waydroid-2026.10.30`):

    gh release download leste-waydroid-2026.10.30 -R smatkovi/pro1x-maemo-leste -D /tmp/leste-wd
    (cd /tmp/leste-wd && sha256sum -c SHA256SUMS)
    scp /tmp/leste-wd/pro1x-waydroid_1.1_all.deb root@172.16.42.1:/root/

On the phone, as root, with a network connection, in two steps (the
preferences file must be there before Waydroid brings lxc):

    apt install /root/pro1x-waydroid_1.1_all.deb
    apt update
    df -h / /host                    # about 3 GiB free on / (or WAYDROID_STORE, above)
    apt install waydroid
    waydroid init                    # the ~1 GB download; MAINLINE vendor
    pro1x-waydroid check

Then **Android** in Hildon's menu. The first start boots Android (a minute
or more), later ones too: closing the window stops the container.

## The Android window

`pro1x-waydroid` (the menu entry runs it as the user):

- takes the screen size from `xrandr` and Hildon's title bar height from
  the theme in use (`/etc/hildon/theme/matchbox2/theme.xml`, scaled by
  pro1x-hidpi: 56 x 2.25 = 126 px), starts weston with that size
  (2160x954 in landscape) and its socket `wayland-pro1x-waydroid` in
  `$XDG_RUNTIME_DIR` (elogind's `/run/user/1000`; without it a private
  `/tmp/pro1x-waydroid-UID`);
- finds weston's X window (WM_CLASS `Weston Compositor`, also
  `X-Maemo-Wm-Class` of the menu entry, so the task switcher knows it),
  names it "Android", and if Hildon gave it another size starts weston
  once more with that size (weston's X11 backend cannot follow a resize);
- sets Android's density through `GRID_UNIT_PX` (Waydroid takes 20 times
  it): 160 times Hildon's scale (`HILDON_SCALE`, pro1x-hidpi), 360 dpi at
  2.25; `WAYDROID_DENSITY` overrides it;
- points `PULSE_RUNTIME_PATH` at the session's PulseAudio (from
  `pactl info`), which Waydroid binds into the container;
- runs `waydroid show-full-ui`, which becomes the session: it starts the
  container and shows Android's home once it is up;
- when the window is closed, runs `waydroid session stop`; when the
  session ends, closes the window (and shows the session's last line as a
  banner).

A second tap on the menu entry while the window is open shows Android's
home again. Logs in `$XDG_RUNTIME_DIR`: `pro1x-waydroid.log` (the start),
`pro1x-waydroid-weston.log`, `pro1x-waydroid-session.log` (Waydroid's
session); `pro1x-waydroid stop` stops it from a shell.

Input: weston gets the touch screen as X's core pointer and passes it on
as a mouse; `pro1x-waydroid defaults` (run by the service) sets
`persist.waydroid.fake_touch=*` so that Android treats it as touch in all
apps. The hardware keyboard reaches Android as key codes through weston;
Android's own layout for physical keyboards applies (Settings, System,
Keyboard), not the X server's Pro1X layout -- untested. Weston cannot
follow a rotation: the window keeps its size until Android is started
again. `WAYDROID_WINDOW=fullscreen` covers the whole screen without the
title bar (then the task switcher and closing only by keyboard or
`pro1x-waydroid stop`).

## GPU or software rendering

With the MAINLINE vendor image Waydroid picks the host's render node
itself: `/dev/dri/renderD128` (the msm driver, `msm_dpu`) gives
`ro.hardware.gralloc=gbm`, `ro.hardware.egl=mesa`,
`gralloc.gbm.device=/dev/dri/renderD128` and
`ro.hardware.vulkan=freedreno` -- Mesa's freedreno (and turnip) in
Waydroid's image on the Adreno 610. That is the default; nothing to set.
weston must then run its GL renderer (default `WAYDROID_RENDERER=gl`), on
the phone's Mesa 25 (daedalus-backports) through glamor/DRI3, so that it
can take Android's dma-bufs (`zwp_linux_dmabuf_v1`).

When Android stays black or SurfaceFlinger crashes (`waydroid logcat`),
switch to software rendering (as root; `waydroid upgrade -o` restarts a
running Android with it, in the open window):

    pro1x-waydroid software     # ro.hardware.gralloc=default, ro.hardware.egl=swiftshader, ro.hardware.vulkan=
    pro1x-waydroid gpu          # back: those properties removed again

Both edit `[properties]` of `/var/lib/waydroid/waydroid.cfg` and run
`waydroid upgrade -o`. Software rendering also works with weston's pixman
renderer (`WAYDROID_RENDERER=pixman`). What the Xperia 10 IV setup
(sfos-waydroid-hybris) needed does not apply here: it was a HALIUM vendor
with the host's Adreno blobs over libhybris; the Pro1X runs mainline
freedreno with Waydroid's MAINLINE vendor image. Its CHECKSUM problem
does not apply either: this kernel has `xt_CHECKSUM` (module).

## Kernel and system notes

- Binder: `CONFIG_ANDROID_BINDER_IPC=y`, `ANDROID_BINDERFS=y`; with
  binderfs the kernel makes no `/dev/binder` itself, Waydroid mounts
  binderfs on `/dev/binderfs` and links its devices into `/dev`. No
  ashmem: Waydroid sets `sys.use_memfd=true`.
- cgroups: Leste's OpenRC (0.45) mounts them `hybrid` (v1 controllers in
  `/sys/fs/cgroup/*`, v2 at `/sys/fs/cgroup/unified`). This kernel has no
  v1 `memory` and `cpuset` (`MEMCG_V1`, `CPUSETS_V1` off), so Android's
  init will complain about those; lmkd works with PSI (`CONFIG_PSI=y`).
  If the container does not start for cgroup reasons, try
  `rc_cgroup_mode="unified"` in `/etc/rc.conf` and reboot.
- Network: `waydroid-net.sh` makes the bridge `waydroid0`
  (192.168.240.1/24), NAT with `iptables-legacy` (Debian's iptables
  package has it; the kernel has `IP_NF_IPTABLES_LEGACY`), and its own
  dnsmasq for DHCP and DNS. Leste's dnsmasq (enabled at boot, listening on
  the wildcard address) kept that one from binding -- tried in a network
  namespace: "failed to create listening socket for 192.168.240.1: Address
  already in use"; with `/etc/dnsmasq.d/pro1x-waydroid` it binds. Android's
  DNS goes to Waydroid's dnsmasq, which asks Leste's resolver.
- No AppArmor in this kernel: Waydroid runs the container unconfined
  (lxc's postinst warns that `apparmor_parser` failed; harmless).

## Removal

    apt remove waydroid              # Waydroid; its data stays in /var/lib/waydroid, ~/.local/share/waydroid
    apt remove pro1x-waydroid        # service off the runlevel, source gone, D-Bus file back
    apt purge pro1x-waydroid         # also /etc/default/pro1x-waydroid, the dnsmasq and apt files
    umount /var/lib/waydroid         # only where WAYDROID_STORE was set (then remove that directory)
    rm -rf /var/lib/waydroid /home/user/.local/share/waydroid   # Android's images and data

`libglibutil` stays at 1.0.80 (Leste's 1.0.35 can be put back with
`apt install libglibutil=1.0.35+4m7` once nothing needs the newer one).

## What was tried on the PC (2026-10-30)

In a chroot of Leste's arm64 image (`scripts/leste-chroot.sh`, under
qemu-user):

- the package builds (`scripts/mkdeb-waydroid.sh`, key checked) and
  installs: weston and its libraries come along; the diversion, the
  runlevel entry and the apt source are made; `apt update` takes
  repo.waydro.id's signed index with the shipped key;
- `apt install waydroid` then installs waydroid 1.6.3, python3-gbinder
  1.3.1, libgbinder 1.1.45, libglibutil 1.0.80 (over Leste's), lxc 5.0.2
  without lxc-templates, lxcfs, libpam-cgfs, and the trigger takes
  lxc-net out of the runlevel; `apt upgrade` leaves libglibutil;
- the OpenRC service under `openrc-run`: without Waydroid it says so and
  is "started" without a daemon, stop and restart are clean; with Waydroid
  (and the chroot's own system bus) it runs `waydroid container start`
  under supervise-daemon, the manager owns `id.waydro.Container`, stop
  (SIGTERM) ends it; `dbus-activate` run as dbus would starts it through
  the service (6 s under qemu) and returns once the name is owned (the
  real activation through `dbus-daemon-launch-helper` cannot run under
  qemu-user: setuid);
- `pro1x-waydroid defaults`, `software`, `gpu` on a waydroid.cfg as
  `waydroid init` writes it: the properties change, `waydroid upgrade -o`
  writes them into `waydroid_base.prop` (with the PC's render node,
  Waydroid's automatic `gralloc=gbm`, `egl=mesa`); repeated runs change
  nothing; the service applies the defaults when it starts;
- removal, purge and reinstallation, with and without Waydroid installed:
  Waydroid's activation file comes back and goes away again;
- Leste's dnsmasq and Waydroid's in a private network namespace: the
  conflict without, no conflict with the drop-in (`dnsmasq --test` passes
  with Leste's configuration);
- the Android window under Xvfb (2160x1080) as the user `user`, with a
  stand-in for `waydroid` (`show-full-ui`, `session stop`, `status`):
  weston 10 comes up with the GL renderer (llvmpipe) in about a second at
  2160x954, its window is found and named "Android", a Wayland client
  (wayland-info) connects through the socket, `GRID_UNIT_PX=18`; when the
  session ends the window closes, when weston ends the session is stopped,
  a second start shows the UI again; pixman by setting, and the fallback to
  pixman when GL cannot start, work. Fullscreen needs a window manager
  (Xvfb has none).

Not tried on the PC: Android itself (binderfs on the PC's `/dev`, an arm64
Android under qemu-user), `waydroid init`, freedreno, the Hildon window
manager's placement, PulseAudio, the boot order on the phone. Under llvmpipe
on Xvfb weston offers no `zwp_linux_dmabuf_v1`; on the phone it has to.

## Device test (from 2026-10-31)

With the package and Waydroid installed and `waydroid init` done:

1. `pro1x-waydroid check` (as root): binderfs, PSI, `/dev/fuse`,
   cgroups (`hybrid`), the modules, render node `/dev/dri/renderD128
   (msm_dpu)`, waydroid set up (MAINLINE), service started, manager on the
   bus. Note every FAIL.
2. `rc-service waydroid-container status`; `cat /var/log/waydroid-container.log`;
   `tail -50 /var/lib/waydroid/waydroid.log`. After a reboot the service
   must be started without help.
3. Tap **Android**. Watch `/run/user/1000/pro1x-waydroid.log`,
   `pro1x-waydroid-session.log` and `pro1x-waydroid-weston.log`
   (renderer `gl`; the EGL extensions should
   list `EGL_EXT_image_dma_buf_import`, the GL renderer freedreno/FD610),
   and as root `waydroid log`,
   `waydroid logcat | grep -i -E 'surfaceflinger|gralloc|egl|fatal' | head -50`,
   `ls -l /dev/binderfs`, `lxc-info -P /var/lib/waydroid/lxc -n waydroid`.
   Time until Android's home.
4. GPU: `waydroid shell -- getprop ro.hardware.gralloc` (`gbm`),
   `waydroid shell -- dumpsys SurfaceFlinger | grep -i gles`
   (freedreno, FD610). Black window or crash loop: `pro1x-waydroid software`,
   close, start again.
5. Network inside Android: `waydroid status` (IP 192.168.240.x),
   `waydroid shell -- ping -c 3 192.168.240.1`, `waydroid shell -- ping -c 3 9.9.9.9`,
   a web page in Android's browser (DNS). On the host: `ip addr show waydroid0`,
   `iptables-legacy -t nat -S POSTROUTING`, `ss -lnup | grep :53` (Leste's
   and Waydroid's dnsmasq side by side).
6. Sound: a sound in Android (a video, the settings' sounds);
   `pactl list short sink-inputs` on the host.
7. Touch and keyboard: tap, scroll a list, long press (fake touch); type
   in Android's search with the hardware keyboard (layout, symbols, Shift,
   the Fn row); the on-screen keyboard should stay away while the hardware
   one is open.
8. The window: below Hildon's title bar without black or cut-off borders
   (otherwise the log shows the size Hildon gave it), title "Android", the
   task switcher; the close button stops Android (`waydroid status`:
   Session STOPPED, the container stopped).
9. Space after init: `df -h / /host`, `du -sh /var/lib/waydroid`.
