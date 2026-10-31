# Updating the phone's packages: `pro1x-update`

The packages of this repository come out as separate GitHub releases: Leste
(`leste-hildon-*`, `leste-mobile-data-*`, `leste-harmattan-*`,
`leste-waydroid-*`), Arch (`arch-hildon-*`) and FreeBSD (`freebsd-hildon-*`,
each a whole pkg repository). A later release often carries only the
packages that changed, so bringing a system up to date used to mean
collecting the newest of each package from several releases. Instead there
is one command per system:

    pro1x-update            # dry run: what would change, nothing is touched
    pro1x-update --yes      # do it (as root)

`tools/pro1x-update/pro1x-update` (POSIX sh) finds out which system it runs
on (Leste: `/etc/maemo_version`; Arch: `/etc/arch-release`; FreeBSD:
`uname`) and reads that system's manifest from the release
[`pro1x-latest`](https://github.com/smatkovi/pro1x-maemo-leste/releases/tag/pro1x-latest).
That release is only a fixed place for files that are replaced whenever a
new release is made; it is a pre-release and never "Latest". Everything
comes from GitHub over HTTPS. No web server or apt repository is involved.

## On the device day

Leste, as root (over USB: `ssh root@172.16.42.1`), Wi-Fi up:

    curl -fLo /usr/local/sbin/pro1x-update https://github.com/smatkovi/pro1x-maemo-leste/releases/download/pro1x-latest/pro1x-update
    chmod 755 /usr/local/sbin/pro1x-update
    pro1x-update                          # read the table and apt's plan
    pro1x-update --yes
    pro1x-update --with mobile-data       # once the SIM is in: see docs/leste-mobile-data.md
    pro1x-update --with mobile-data --yes
    reboot

Arch (the running Arch, as root): the same two `curl`/`chmod` lines, then
`pro1x-update` and `pro1x-update --yes`. FreeBSD (as root):

    fetch -o /usr/local/sbin/pro1x-update https://github.com/smatkovi/pro1x-maemo-leste/releases/download/pro1x-latest/pro1x-update
    chmod 755 /usr/local/sbin/pro1x-update
    pro1x-update && pro1x-update --yes

From a clone of the repository, `sh tools/pro1x-update/pro1x-update` does
the same. Images built from now on have it in `/usr/local/sbin`
(`scripts/45-pro1x-hardware.sh`, `arch/20-pro1x-hardware.sh`). If the
manifest was made with another `pro1x-update` than the one running, the
tool prints a note with the download link. A manifest of a newer format is
refused with the same link.

## Maemo Leste

`leste.manifest` lists, for every package of all `leste-*` releases, the
newest version (in Debian's version order, read from the package itself,
epoch included), its release, URL, SHA-256, group and policy. The tool:

1. compares each package with the installed version (`dpkg --compare-versions`)
   and decides what to do (table below);
2. downloads what it needs into `/var/cache/pro1x-update` (as non-root:
   `~/.cache/pro1x-update`), checks the SHA-256 against the manifest and the
   name, version and architecture against the package's control data;
3. shows installed -> new for each package, then apt's plan for all of them
   at once (`apt-get -s`): what apt also takes from Leste's archive, what it
   would remove, the recommended packages that stay out. Up to here it is
   the dry run;
4. with `--yes`: writes the versions before to
   `/var/lib/pro1x-update/<date>.before`, runs `apt-get update` (errors are
   reported but do not stop it; `--no-apt-update` skips it), and installs
   all packages in **one** `apt-get install`, so apt orders them itself.
   Then the group scripts run (mobile data, below). Finally every
   `+pro1x` rebuild of a Leste package is held (`apt-mark hold`), as
   `scripts/49-hidpi.sh` and the release notes hold them.

The install itself:

- **No service is started or restarted** while it runs. Where
  `/usr/sbin/policy-rc.d` is missing, a temporary one that refuses (exit
  101) is put there, as `49-hidpi.sh` does: a restarted mce leaves the
  display off, a restarted oFono drops a call. The new versions run after
  a reboot.
- **No recommended packages** (`--no-install-recommends`). The N9's Phone
  and Messages recommend ModemManager, which would take the modem from
  oFono, and Maps recommends geoclue. The dry run lists the recommended
  packages left out; install the wanted ones by hand.
- **Nothing is removed**: `apt-get --no-remove` stops if the update would
  remove a package. One case where that can happen: an archive `-dev`
  package that wants the archive's library exactly, e.g.
  `calendar-backend-dev`, which no release carries. The dry run shows
  `REMOVE` lines; `--allow-remove` lets apt go ahead.
- **Changed configuration files stay** (`--force-confold`, `--force-confdef`).
- `--allow-change-held-packages`: the earlier `+pro1x` builds are held.
- **The rest of the system is not upgraded.** Because of the holds, a
  later `apt upgrade` does not put the archive's versions back.

### What is installed

| policy | packages | installed |
|---|---|---|
| `always` | this repository's own tools (`pro1x-*`: `pro1x-events`, `pro1x-volume-osd`, `pro1x-look`, `pro1x-alarmboot`), not `pro1x-waydroid` | also where missing |
| `installed` | the rebuilds of Leste's packages (gtk2, libhildon1, hildon-desktop, hildon-home, hildon-status-menu, mce, liblocation0, location-ui, calendar-backend, conversations); the groups `harmattan` and `waydroid`; `ofono-scripts` | where installed, as `49-hidpi.sh`'s `install_ours`: of a source's packages those the system has. `--with GROUP` installs the group's missing ones |
| `extra` | `-dev`, `-dbgsym`, `-doc` | only where installed, also with `--with` |
| `switch` | `ofono` | only where a `+pro1x` build is installed already. The first switch from the archive's oFono needs `--with mobile-data` |

A package is never downgraded. If a newer version is installed (e.g. from
Leste's archive), the table says "kept: newer installed". `--skip PKG`
leaves a package out and `--add PKG` takes a missing one (shell patterns).
`-v` also lists what is up to date.

Groups (the release's kind: `leste-<group>-<date>`):

- `hildon`: the MeeGo look, HiDPI, the location and calendar fixes,
  Conversations. Updated by default.
- `mobile-data`: oFono with the IPA data path. `--with mobile-data` does
  the first switch from the archive's oFono the way the release says: the
  release's `ofono` and `ofono-scripts` go to `scripts/49-mobile-data.sh`
  (`DEBS=`), which also keeps the old oFono in `/root/ofono-orig`, holds
  the new one and replaces the boot service `pro1x-rmnet` by
  `pro1x-modem-wait`. The script and its two files come with the manifest
  (`leste-files.tar.gz`, SHA-256 checked) at the manifest's commit. Later
  oFono updates go through plain apt.
- `harmattan`: the N9's user interface as Leste packages.
  `--with harmattan` installs all 35, with their Qt 5 dependencies from
  the archive.
- `waydroid`: `pro1x-waydroid`. Waydroid itself is then
  `apt install waydroid` ([waydroid.md](waydroid.md)), not part of this.

### The way back

`/var/lib/pro1x-update/<date>.before` lists the versions before. The
archive's versions (`+4m7`) come back with
`apt-mark unhold PKG && apt install PKG=VERSION`. An earlier `+pro1x` build
comes back from its release (`apt install --allow-change-held-packages
./PKG_VERSION_arm64.deb`). For oFono, `/root/ofono-orig` and
[leste-mobile-data.md](leste-mobile-data.md).

## Arch Linux

`arch.manifest` (format 2) lists the newest package of each name across the
`arch-hildon-*` releases (pacman's version order), with each package's
`depend`, `provides` and `conflict` from its `.PKGINFO`. Since
arch-hildon-2026.10.30.1 these are all 152 parts of the Arch steps, which
depend on each other, so the tool picks as on Leste:

| group | packages | installed |
|---|---|---|
| `core` | the parts of arch-hildon-2026.10.30 (libhildon, hildon-status-menu, hildon-desktop, calendar-backend, rtcom-eventlogger, rtcom-eventlogger-plugins, pro1x-events, pro1x-volume-osd, pro1x-fm) and this repository's own `pro1x-*` (pro1x-sessions) | also where missing |
| `harmattan` | `harmattan-*`, qt-components, libngf, ngfd | where installed (registered in pacman); `--with harmattan` installs them all |
| `apps` | the rest: Hildon's apps and their libraries | where installed; `--with apps` |

To these it adds what they depend on in the set, whatever its group: by
name, or through a package's `provides` where nothing installed provides
it already (`pacman -T`). The dry run marks those
`install (for <package>)`. For the core alone that is 14 more (gtk2-maemo,
gconf, libosso, alarmd, clockd, libhildondesktop, ...). An installed
package is upgraded when the manifest has a newer version and never
downgraded. `--skip PKG` leaves one out; the dry run warns when another
one needs it. `--add PKG` takes one of the opt-in groups.

Then, as on Leste, the downloads are checked (SHA-256, `.PKGINFO`'s name
and version). The tool also checks the files they bring that already
exist:

- **Files without a package** (put there by steps 30 to 42 without
  pacman) are taken over. The dry run counts them.
- **Files of another package** stop the update before anything is
  installed, with a list. Two exceptions, which pacman handles itself: a
  package that a new one conflicts with (it is removed), and a package
  upgraded in the same run whose new version no longer has the file.
- **Conflicts**: where a new package conflicts with an installed one, the
  dry run says so (e.g. `gtk2-maemo replaces the installed gtk2`). pacman
  then removes the old one (`--ask 4`, which answers pacman's "remove
  it?" with yes; with `--noconfirm` alone pacman would stop).

The dry run ends with what pacman would also take from Arch Linux ARM
(`pacman -U --print`). With `--yes`: `pacman -U --noconfirm --needed
--overwrite '*'`, as the release says. After the check above only
nobody's files are left to overwrite. Then the session's settings, as
`arch/42-meego-look.sh`'s PKGS mode makes them: PipeWire's PulseAudio
server enabled for every user's session, MeeCast's icons where the Events
page looks for them, `systemctl daemon-reload`. The sessions' scripts and
units come with `pro1x-sessions`. snap-pac takes a Btrfs snapshot before
and after (`pro1x-boot next arch@<number>` boots the one before).

The packages are built against Arch Linux ARM of their date. If pacman
cannot find a dependency, or a program misses a library, run
`pacman -Syu` first: `--sysupgrade` does that before installing. Arch has
no holds and needs none: Arch Linux ARM has none of these packages
(where it has one of the name, the release's is `<name>-maemo`, which
provides and conflicts with it).

For an Arch image mounted in Leste (`/mnt/arch`), use
`PKGS=/dir bash arch/42-meego-look.sh` as before (`arch/README.md`).

## FreeBSD

Every `freebsd-hildon-*` release is a whole pkg repository, so updating
means pointing pkg at the newest one. The tool:

1. asks GitHub's API for the releases (pre-releases included) and takes
   the newest `freebsd-hildon-*` tag (by its date and number) whose assets
   include `meta.conf` and `packagesite.pkg`. A release still uploading is
   skipped. Without an answer from the API it takes the tag in
   `freebsd.manifest`;
2. shows the `url` in `/usr/local/etc/pkg/repos/pro1x.conf` and the new
   one, and from the new release's catalogue (`packagesite.pkg`) the
   installed packages with an older version (`pkg version -t`). This dry
   run leaves pkg's own database alone;
3. with `--yes`: keeps `pro1x.conf.before-pro1x-update`, rewrites only the
   `url` (or writes the file as the releases describe it, if it is
   missing), runs `pkg update -f -r pro1x` and puts the old file back if
   that fails, then `pkg upgrade -y -r pro1x`.

Packages the release adds are installed only as dependencies of upgraded
ones. Anything new on its own: `pkg install -r pro1x NAME`.

## Making the manifests (PC)

After every new release (any system):

    tools/release/mkmanifest.sh            # build them into build/manifest-cache/out, publish nothing
    tools/release/mkmanifest.sh --publish  # and upload them to pro1x-latest

It lists all releases (`gh api`) and downloads every package once into
`build/manifest-cache` (in `WORK`). It checks each package's SHA-256
against GitHub's digest of the asset and against the release's
`SHA256SUMS` where there is one; older releases have none and are checked
by the digest alone. The name, version and architecture come from the
package itself, not from the file name, which drops the Debian epoch. It
then picks the newest of each package. It warns when a later release has
an older version than an earlier one, or the same version with other
contents. Example: `harmattan-system-ui 1.2.24-2+0m7+pro1x1` differs
between leste-harmattan-2026.10.28 and .29; both are superseded by
`+pro1x2`. (The nine parts of arch-hildon-2026.10.30 were rebuilt with
the same versions in .30.1, so they give that warning too.) The policies
are in `leste_policy()` and `arch_group()`. The bundle
`leste-files.tar.gz` and the copy of `pro1x-update` come from
`git archive HEAD`, so `--publish` refuses
uncommitted changes to them and a HEAD that is not pushed. The manifests
are uploaded last, so that a phone reading a new manifest also finds its
files. The release's notes list every package with its version and
release.

Why not an apt repository in a release: apt needs all files under one
base URL, but the packages sit in several releases. An unsigned
repository would need `[trusted=yes]`, which is weaker than the manifest's
SHA-256 over HTTPS. A signed one needs a key. And `apt upgrade` would then
pull in this repository's packages on its own, including groups the
system does not use.

## Tested (2026-10-30/31, PC only)

- **Leste**, in a chroot of the official arm64 image (`scripts/leste-chroot.sh`),
  with the current releases:
  - the image as it is: the dry run planned 15 upgrades of archive
    packages to the `+pro1x` builds and the three own tools;
  - with leste-hildon-2026.10.27's packages installed and held first (as
    on the phone): the dry run planned 8 upgraded, 3 newly installed,
    nothing removed. `--yes` installed them in one apt run (the chroot's
    policy-rc.d refused every restart), and the new `+pro1x` builds were
    held. A second run found nothing to do;
  - `--with mobile-data --yes` ran `49-mobile-data.sh` from the bundle:
    oFono `+pro1x1` and `ofono-scripts` installed and held, the archive's
    oFono kept in `/root/ofono-orig`, `pro1x-modem-wait` in the runlevel;
  - `--with harmattan,waydroid`: dry run only. Its apt plan showed
    ModemManager among the recommended packages, which is why nothing
    recommended is installed;
  - from a clone (`scripts/leste-chroot.sh repo`) with the published
    manifest: nothing left to do. `--with waydroid --yes` installed
    `pro1x-waydroid` (not held) with weston from the archive;
  - without the chroot's own policy-rc.d: the tool put its own there for
    the install and removed it afterwards.
- **Arch**: on the PC with a stand-in `pacman` (its installed packages
  from a list, `-Qo` from the real one for those, installs only logged)
  and the real packages of arch-hildon-2026.10.30.1:
  - nothing registered: the core and its 14 dependencies, 24 packages,
    92 apps and 36 Harmattan parts left out;
  - with gtk2, an older libhildon, a newer rtcom-eventlogger registered:
    gtk2-maemo replaces gtk2 (gtk2's files on the PC allowed), libhildon
    upgraded, rtcom-eventlogger kept;
  - `--with harmattan` (55) and `--with apps,harmattan` (150);
  - a crafted package with `/usr/bin/ls` (stopped: belongs to coreutils);
  - `--yes`: `pacman -U --noconfirm --needed --overwrite '*' --ask 4` with
    the 19 packages, then the session's settings (stand-in `systemctl`).
- **FreeBSD**, on the PC with stand-ins for `pkg` and `fetch`: the tag
  from GitHub's API, the rewrite of `pro1x.conf` (and a new one), the old
  file back when `pkg update` fails, `freebsd.manifest` when the API does
  not answer. Then in the FreeBSD 15.1 VM (pkg 2.8.4, 171 packages from a
  local pro1x repository, 2026-10-31): `sh -n`, the dry run (14 older than
  freebsd-hildon-2026.10.30.5), `--yes` (`pro1x.conf` rewritten,
  `enabled: yes` kept; pkg upgraded 14 and installed 8 new dependencies,
  telepathy-ring, location-daemon, libqmi among them), a second dry run
  with nothing left. The VM's own `pro1x.conf` was put back afterwards.

Not tried on the phone.
