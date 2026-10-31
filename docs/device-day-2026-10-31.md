# Device day 2026-10-31: F(x)tec Pro1X and Nokia N950

One ordered list of everything the agents prepared for the phones since
2026-10-29, when the Pro1X became unreachable. The order follows risk and
dependency: the phone's state first, then the backup, then the one flash
of the day (kernel 7.1.0-pro1x.10), then the tests that need it, then the
other systems. Each block has a time estimate, what makes it stop, and the
way back.

The detailed steps stay in the documents named in each block. This page
says in which order to run them, what to copy, what to collect, and where
the user's OK or hands are needed.

## Legend and rules

- **[OK]**: needs the user's explicit, fresh OK right before the step.
  This covers any flash, kexec, reboot or power-off, any write to a
  partition, and creating the KolibriOS container or the FreeBSD pool
  (big writes to userdata). One OK may cover a named list of steps, such
  as "FreeBSD runs G1, pm1, pm2". Anything outside that list asks again.
- **[HAND]**: the user has to do something at the phone: the power key,
  a cable, headphones, the SIM, a call, an SMS, typing a PIN, looking at
  the screen.
- **[OPT]**: optional. Do it only if time allows. Every block also has a
  priority: **must**, **should** or **could**.
- Times are estimates for one person and one agent. They include the
  reboots, about 1-2 minutes per Leste start.
- Never use `StrictHostKeyChecking=no`. Never use `udevadm trigger`.
  Never use `pkill -f` or `pgrep -f`: their patterns match the agent's own
  ssh command. Never run `fastboot flash` for big images on this PC: the
  Bus 7 hub garbles it (`docs/HANDOFF.md`). Write partitions with `dd`
  from Leste or Sailfish OS.
- Collect after every run with `tools/devday/collect.sh <run-name>`, even
  when the run failed. Results go to `~/ps/pro1x-runs/2026-10-31/`.

## Variables (PC, in the repository)

```sh
cd /run/media/sebastian/2e638a7f-26db-4e89-9446-81d688464798/pro1x-leste/repo
WORK=/run/media/sebastian/2e638a7f-26db-4e89-9446-81d688464798/pro1x-leste
D=tools/devday
# the day's kernel: pro1x.10 (= pro1x.9 + 0028 alarm power-on + 0029-0032
# power fixes), built by the coordinator; pro1x.9 only as the fallback
K=pro1x.10; [ -f $WORK/out-$K/kernelrelease ] || K=pro1x.9
OUT=$WORK/out-$K; KVER=$(cat $OUT/kernelrelease)
# PHONE for the repository's scripts (64, 65, 69, 71): Leste's pinned key
PHONE="ssh -o BatchMode=yes -o StrictHostKeyChecking=yes \
 -o UserKnownHostsFile=$HOME/.ssh/known_hosts.pro1x-leste \
 -o HostKeyAlgorithms=rsa-sha2-512,rsa-sha2-256 root@172.16.42.1"
```

`bash $D/phone.sh ssh leste '<command>'` runs a command in Leste with the
same pinned key. For Kali and Arch it is `phone.sh ssh kali`, and so on.

## The helpers (tools/devday/)

| Script | Where it runs | What it does |
|---|---|---|
| `phone.sh usbnet` | PC | Puts the PC's 172.16.42.2/24 on `enp19s0u1u1` (`nmcli dev modify`). Every re-enumeration of the phone drops it. |
| `phone.sh pin SYS` | PC | Pins a system's host key once (accept-new, into `~/.ssh/known_hosts.pro1x-SYS` only), over the cable. |
| `phone.sh which`, `phone.sh wait [MIN]` | PC | Shows which system answers, or waits until one does. |
| `fetch.sh [list]` | PC | Fetches the newest of every package of all `leste-*` pre-releases, plus the Arch packages and the Whisperfish deb, into `$WORK/devday/`. Checks them against the releases' SHA256SUMS. |
| `deploy.sh BLOCK` | PC | Copies one block's files to the phone (scp, sha256 checked; a replaced file is kept as `.before-2026-10-31`). Installs and flashes nothing. Use `deploy.sh list BLOCK` to see the block. |
| `backup.sh` | PC | Reads the phone's boot, dtbo, vbmeta, misc and devinfo partitions, the GPTs, the slots' attributes and the state on `/host`. Writes them into `~/ps/pro1x-backups/2026-10-31/`. Writes nothing on the phone. |
| `collect.sh RUN [FILE...]` | PC | Collects logs, the RAM console, the screenshot and FILEs into `~/ps/pro1x-runs/2026-10-31/RUN/`, with `summary.txt`. |
| `ramconsole.py` | phone | Reads FreeBSD's and KolibriOS's RAM console and screenshot at 0x5ca00000, read only. `--clear` clears it afterwards. collect.sh uses it. |
| `leste-install.sh plan\|install` | phone | Installs the fetched packages group by group. By default it only upgrades packages that are installed already, and holds them. It restarts no service. |
| `check-initrd.sh INITRD [alarm]` | PC | Checks that the initramfs `prepare` made carries the trial's parts. The watchdog's arming is skipped silently without `qcom-wdt.ko` or `devmem`. Passes on pro1x.5's initramfs. |
| `test/run.sh` | PC | Tests all of the above against a fake phone (fake ssh/scp): 39 checks. |

`DRY=1` in front of deploy.sh, collect.sh or backup.sh prints the phone
commands without running them.

`tests/pro1x-boot-trial/run.sh` tests the trial and the fallback to slot a
(see "Rollback").

## Overview

| # | Block | Priority | Time | [OK] | [HAND] |
|---|---|---|---|---|---|
| 0 | PC preparation (evening before or morning) | must | 45 min | - | - |
| 1 | The phone's state: power on, first look | must | 15-30 min | - | power key, SIM |
| 2 | Backup | must | 10 min | - | - |
| 3 | Reference on the old kernel | should | 10 min | - | listen |
| 4 | Kernel 7.1.0-pro1x.10: prepare, flash, trial | must | 40 min | flash, 2 reboots | maybe power key |
| 5 | Audio and FM on pro1x.10 | must | 75-90 min | 2 reboots | headphones, headset, listen |
| 6 | Leste packages and user-space power parts | should | 25 min | reboot | - |
| 7 | Power measurements | should | 60-120 min | suspends | cable out, power key |
| 8 | Alarm power-on | should | 20 min | power-off | power key menu |
| 9 | Leste mobile data | must | 75 min | reboot | SIM, PIN, calls, SMS |
| 10 | Kali (rtc fix, mobile data) | should | 40 min | 2 reboots | PIN |
| 11 | Arch | could | 30 min | 2 reboots | screen, touch |
| 12 | FreeBSD | must (G1, dsp) / should / could | 2-4 h | every run | some runs |
| 13 | KolibriOS | should | 60-90 min | every run | some runs |
| 14 | Whisperfish | could | 20-40 min | - | Signal on the main phone |
| 15 | Waydroid | could | 60 min | - | touch, keyboard |
| 16 | N950 | should | 60-120 min | install, boots | cable, power key |

The total is about 10-14 hours, more than one day. If time runs short,
do blocks 0-5 and 9 first, then FreeBSD G1 and the dsp runs, then the
rest by priority. The N950 (block 16) can run alongside, during long
unattended Pro1X runs (FreeBSD's 10-25 minute pm runs, the power
measurements). Two devices on USB at the same time: the N950 uses its
own NetworkManager profile (192.168.2.x). Check after every replug that
`enp19s0u1u1` still has 172.16.42.2 (`phone.sh usbnet`).

---

## Rollback: what is armed, what is not (read before block 4)

### The mechanism

Since 2026-10-28, `65-kernel-update.sh flash` puts slot b on trial
(`pro1x-boot trial <kernel> 3`). Slot b then has 3 tries and is not
marked "successful". The bootloader takes one try for every start of
slot b. With none left, it marks slot b unbootable and starts slot a
(Sailfish OS, which waits for its LUKS code).

- A start is **confirmed** by `pro1x-boot-trial` in the system once its
  user interface runs. It writes `/host/pro1x-boot/trial-ok`. Choosing
  FreeBSD or KolibriOS in the boot menu also confirms.
- The **next** start's initramfs ends the trial (`trial-check`). Slot b
  is then successful again, with 7 tries.
- On a trial start (and on a one-shot `pro1x-boot next`), the initramfs
  arms the SoC's watchdog. qcom_wdt feeds it until dsme (Leste) or
  systemd takes it, for at most 15 minutes. A start with no user
  interface after 10 minutes reboots (`pro1x-boot-trial`). `panic=10`
  reboots after a panic.
- **What the watchdog cannot catch:** a kernel that hangs before the
  initramfs. Nothing resets that. The user holds the power key for about
  10 s, and each such start costs a try.

### Measured on the PC (`tests/pro1x-boot-trial/run.sh`, 28 checks)

These run the real `pro1x-boot` and `pro1x-boot-trial` against a GPT image
and a model of the bootloader. **The model is an assumption.** Qualcomm's
ABL, as its sources describe it: boot if successful; else take a try and
boot if one is left; else mark unbootable and start the other slot. The
phone itself has only shown "the tries counted down, then slot a"
(2026-10-24). With TRIES=3:

| Starts of the new kernel | Result |
|---|---|
| good | confirmed. The 2nd start ends the trial: 7 tries, successful. |
| hang, good | confirmed on the 2nd start, ended on the 3rd. |
| hang, hang, hang (late or early) | the 4th power-on is Sailfish OS. Slot b is unbootable. |
| **hang, hang, good** | **confirmed, yet the 4th power-on is Sailfish OS**: no try is left for the start that would end the trial. |
| a system without `pro1x-boot-trial` (the MeeGo copy, if 66 skipped it) | never confirmed. After 3 starts, Sailfish OS. |
| FreeBSD or KolibriOS from the menu | confirmed by the menu. |

So 3 tries survive **one** failed start, not two.

**TRIES=0 is "not armed".**
- On a slot that is successful, a kernel that hangs is started again at
  every power-on, with no fallback.
- Over a trial still pending (pro1x.5's), TRIES=0 is worse. The old
  trial's leftover tries keep counting for a kernel that hangs early. But
  a kernel that reaches the initramfs and hangs later has already marked
  slot b successful (the trial is "stale"), and there is no way back by
  itself.

### State at the start of the day (from the notes, verify in block 1)

- boot_b = 7.1.0-pro1x.5, flashed 2026-10-29 18:50 with the trial
  (3 tries). It has never started.
- Its way back is `/host/boot_b-before-7.1.0-pro1x.5.img` (= pro1x.4,
  confirmed on 10-29). The PC has it in `$WORK/out-pro1x.5/`.
- The phone hung in Leste's shutdown on 2026-10-29 19:10 (after the
  KolibriOS `-m` unbind deadlock). The next power-on is pro1x.5's first
  trial start.

### Verdict and proposal

**Arm it: flash pro1x.10 with the default trial (TRIES=3), nothing
extra.** `65 flash` arms it itself. The user is there, so the fallback to
Sailfish OS (LUKS code) is acceptable, and it is the only automatic way
back from a kernel that does not boot.

Before the flash:

1. pro1x.5 must have come up (block 1). Then its image, saved by `flash`
   as `boot_b-before-7.1.0-pro1x.10.img`, is known to boot.
2. `prepare` must have installed `pro1x-boot-trial` into **every** image.
   On 10-29 it left out `meego-leste.img`, because the Whisperfish chroot
   held it read-only. Before prepare, `/host/wf-leste` must not be
   mounted. Afterwards, `grep 'left out' $OUT/prepare.log` must be empty.
3. The boot menu scripts must be committed. Prepare takes
   `initramfs/` and `tools/pro1x-boot` from the working tree, and the
   alarm agent changed both. Check
   `git status --short initramfs tools/pro1x-boot tools/pro1x-boot-trial`
   is empty, then `bash tests/pro1x-boot-trial/run.sh` ends in "all cases
   passed".
4. **The boot image must carry the parts, not only the scripts.** The
   watchdog's arming in `pro1x-loop-root` is skipped silently when
   `devmem` or `qcom_wdt` is missing. After prepare:
   - `bash tools/devday/check-initrd.sh $OUT/initrd.img-$KVER alarm` must
     end in "the trial's parts are there". It checks pro1x-loop-root with
     trial-check, pro1x-boot with trial, trial-check and alarm-check,
     pro1x-bootmenu, sgdisk, kexec, `qcom-wdt.ko` and `devmem`.
   - On the phone, the confirming service must be in every system:
     `rc-update show default | grep pro1x-boot-trial` in Leste. In the
     images:
     `for i in kali luneos; do mount -o loop,ro /host/$i.img /mnt && ls -l /mnt/etc/systemd/system/multi-user.target.wants/pro1x-boot-trial.service; umount /mnt; done`
     and for Arch `mount -o loop,ro,subvol=@ /host/arch.img /mnt` (same
     `ls`). The MeeGo copy is covered by "no `left out` in prepare.log".
   This makes "armed" a checked fact, not an inference.
5. After the flash, `pro1x-boot list` must show
   `trial:   kernel 7.1.0-pro1x.10 on slot b, boot_b 3 tries left, not successful`.

While the trial runs, boot only Leste. One confirmed start, then one more
plain reboot [OK] ends it (`pro1x-boot list`: no `trial:` line, block 4).
Every reboot during a trial costs a try. That includes the FreeBSD and
KolibriOS runs, the alarm test, Kali and Arch. None of them before the
trial has ended.

Not proposed: TRIES=4 (it would survive two failed starts, at the price
of one more power-key cycle of a broken kernel). Not proposed: changing
any armed state on the phone before block 1.

### Ways back, in this order

1. **The new kernel runs, but something is wrong.** From Leste:
   `TARGET=b IMG=/host/boot_b-before-$KVER.img VBMETA= sh scripts/60-flash-slot-b.sh flash`
   [OK], then reboot [OK]. The old kernels' modules stay in
   `/host/pro1x-modules`.
2. **It does not come up.** Let the trial run out: power key [HAND] at
   each hang, up to 3 starts. Then Sailfish OS starts.
   - The user types the LUKS code [HAND].
   - Over ssh (`defaultuser`, sudo, on the Sailfish network address in
     the notes): `sudo dd if=/userdata/boot_b-before-$KVER.img of=/dev/disk/by-partlabel/boot_b bs=1M conv=fsync` [OK].
   - Then slot b again: `fastboot --set-active=b` from the bootloader.
     It is a short command that works with retries; use
     `/opt/android-sdk/platform-tools/fastboot`. Or run `pro1x-boot slot b`
     in Sailfish (`PRO1X_BOOT_HOST=/userdata`, needs sgdisk there).
     Either clears "unbootable".
3. **Confirmed but no try left** ("hang, hang, good"). Same as 2, but
   without the dd: slot b back, and the next start ends the trial.
4. **The bootloader's crash-dump mode** (USB `05c6:900e`, seen with
   FreeBSD on 10-29):
   `sudo -n qdl ramdump -o ~/ps/pro1x-runs/2026-10-31/ramdump-<time> 'OCIMEM*,CODERAM*,PMIC*,RST*,*.cmm,MSGRAM*'`.
   The phone resets itself afterwards and starts the menu's default
   (Leste). Then `phone.sh usbnet`.
5. **The bootloader itself.** `fastboot --set-active=a|b` and
   `fastboot reboot` only. The backup of block 2 and
   `$WORK/backup-slot-a/` (2026-10-24, with the GPTs of all six LUNs) are
   for EDL, the last resort.

---

## Block 0: PC preparation (evening before or morning; must; 45 min)

1. **Disk space.** Both disks must keep at least 10 GB free
   (`df -h / $WORK`). The backup needs about 245 MB on `/`. The FreeBSD
   root image (3 GB) only goes to the phone.
2. **Host keys.** These are pinned and kept (copied 2026-10-30 from the
   volatile `/tmp` and job directories):
   - `~/.ssh/known_hosts.pro1x-leste`: RSA only, from 2026-10-25. The
     MeeGo copy uses it too.
   - `~/.ssh/known_hosts.pro1x-arch`

   Kali, LuneOS and FreeBSD (usbnet) are pinned on their first boot with
   `phone.sh pin kali|luneos|freebsd`. FreeBSD's read-only root makes a
   new host key at every boot: re-pin it there by deleting its file.
   `bash $D/phone.sh keys` lists the pins.
3. **Packages:** `bash $D/fetch.sh` (again on the morning). It picks up
   anything released overnight, for example `pro1x-alarmboot`. Check the
   list in `$WORK/devday/FETCHED`. On 2026-10-30 it held:
   - 53 Leste packages in 5 groups
   - oFono `2.18.0-1+4m7.1+pro1x1`
   - `pro1x-waydroid_1.1`
   - the Arch packages of `arch-hildon-2026.10.30`
   - `whisperfish-leste_0.6.0.dev-1` (sha256 `44a43122…95fd`, the build
     with the stacking fix)
4. **Kernel:** `ls $WORK/out-pro1x.10/` should show `kernelrelease`,
   `Image.gz`, the dtb, `config-*` and `modules/`. Without them, use
   `K=pro1x.9` and tell the coordinator.
5. **Boot scripts committed, the trial test passes** (Rollback, point 3).
6. **KolibriOS rebuilt.** `deploy.sh list kolibri` warns when
   `build/*/kernel.bin` is older than the repository's last commit (it was
   on 10-30).
   ```sh
   make -C ~/ps/kolibrios-arm PLATFORM=arm64
   make -C ~/ps/kolibrios-arm PLATFORM=n950
   ```
7. **FreeBSD files:** `bash $D/deploy.sh list freebsd freebsd-pm freebsd-tests`
   with no MISSING. Other agents rebuild in `$WORK/freebsd` (the root
   image of 10-30 16:12, kernel 15:50, PRO1XPM kernel 19:11). Take the
   final state, and note the file times in the run log.
   - Runs pm 14-17 need a rebuild with `xhci.ko`, `pro1x_typec.ko` and
     `pro1x-usbrole` in the image (`freebsd/pm/README.md`). Without it
     they are left out.
8. **The helpers' own tests:** `bash $D/test/run.sh` must end in "all
   checks passed".

## Block 1: the phone's state (must; 15-30 min) [HAND]

Rollback: Rollback, ways back 2-4.

1. [HAND] **Power key ~10 s** until the phone is off (it hangs in Leste's
   shutdown since 10-29).
2. [HAND] **If mobile data, calls or the modem runs are wanted today:
   put the SIM in now, while the phone is off** (blocks 9, 10, 12, 13).
3. [HAND] Power on. Expected: the boot menu (5 s), then Leste. This is
   pro1x.5's first trial start.
4. PC:
   ```sh
   bash $D/phone.sh wait 10     # sets the USB address again every 20 s
   bash $D/phone.sh which       # "leste (pro1x-boot current: leste)"
   ```
5. If it does not come up, find out which case it is:
   - The screen hangs before the menu (a hang before the initramfs): power
     key [HAND], and try again. At most 3 starts, then Sailfish OS
     (ways back 2).
   - The menu appears but Leste hangs: the watchdog resets it by itself
     (within 15 min). Count the starts.
   - USB `05c6:900e`: ways back 4.
6. **The first look, before anything changes.** If the pstore (ramoops)
   survived the 10 s power-key reset, it holds the console of the hung
   shutdown of 10-29. It did survive the crash-dump resets, but a
   power-key hold may be a colder reset. An empty `pstore.txt` is then
   expected, not a failure of collect:
   ```sh
   bash $D/deploy.sh tools
   bash $D/collect.sh 01-first-boot /host/pro1x-boot /var/log/syslog
   ```
   In the summary, expect:
   - `uname -r` = 7.1.0-pro1x.5
   - the trial line: "kernel 7.1.0-pro1x.5 ... 2 tries left, not
     successful, start confirmed" once Hildon has run
   - the last-boot `entries:` line listing `kolibri` and `freebsd`
7. Check the Wi-Fi address (`ip -4 addr show wlan0`). It is needed in
   block 7.

## Block 2: backup (must; 10 min) — before anything is written

Nothing is written on the phone. If the backup fails, stop. Nothing is
flashed without it.

```sh
bash $D/backup.sh            # -> ~/ps/pro1x-backups/2026-10-31/
(cd ~/ps/pro1x-backups/2026-10-31 && sha256sum -c --quiet SHA256SUMS)
```

What it reads:
- boot_a/b (96 MiB each), dtbo_a/b (24 MiB each), vbmeta_a/b,
  vbmeta_system_a/b, misc and devinfo
- the primary and backup GPT of every UFS LUN, at its 4 KiB block size,
  plus `sgdisk -p`
- the slot attributes, decoded in `slots-decoded.txt`
- `/host/pro1x-boot/*`, `/host/pro1x-rtc-offset`, `/host/pro1x-display.conf`,
  the cmdline files of FreeBSD and KolibriOS, and the pool's label sums,
  in `host-state.tar`
- lists of `/host`, `/host/freebsd`, `/host/kolibri` and
  `/host/pro1x-modules`
- the sha256 of every `/host/boot_b-*.img`, matched against the PC's
  copies (`host-boot-images-on-pc.txt`)
- the cmdlines in boot_a and boot_b

The total is about 245 MB. Look at `slots-decoded.txt`: boot_b should be
active, with 2 tries, not successful, and not unbootable.

## Block 3: reference on the old kernel (should; 10 min) [HAND]

This runs on pro1x.5, before the flash.
- `docs/audio.md` step 0: listen to the loudspeaker's pop with the old
  amplifier order. `deploy.sh audio` puts the two test sounds in
  `/root/devday/`.
- `docs/fm-radio.md` step 0: `qrtr-lookup` must list the ADSP's SLIMbus
  service 769, version 1.
- [OPT] `docs/alarm-power-on.md` step 0: `sh /tmp/pro1x-boot pmic` with
  the repository's `tools/pro1x-boot/pro1x-boot` copied to `/tmp`.

```sh
bash $D/deploy.sh audio
bash $D/phone.sh ssh leste 'qrtr-lookup | grep -E "^ *769 "'
```

On the phone, as root, for the pop:
```sh
pa() { su user -c "XDG_RUNTIME_DIR=/run/user/\$(id -u) $*"; }
pa paplay /root/devday/pro1x-silence.wav      # 5 times, 6 s apart
pa paplay /root/devday/pro1x-beep.wav
```
Note what is heard in the run log (`~/ps/pro1x-runs/2026-10-31/notes.md`).

## Block 4: kernel 7.1.0-pro1x.10 (must; 40 min) [OK]

See `docs/kernel-update.md` and `docs/alarm-power-on.md` "On the phone".
Preconditions: blocks 1-2 done, Rollback points 1-3 checked.

1. Nothing may hold an image. On the phone:
   `findmnt | grep -E 'wf-leste|/mnt/kali|/mnt/arch'` must be empty.
2. **prepare.** It writes no partition. It does write
   `/host/pro1x-modules/$KVER`, the boot image `/host/boot_b-$KVER.img`,
   and the trial's parts into every image (66). About 10 min:
   ```sh
   PHONE="$PHONE" WORK=$WORK OUT=$OUT bash scripts/65-kernel-update.sh prepare 2>&1 | tee $OUT/prepare.log
   grep -n 'left out\|not there' $OUT/prepare.log     # must be empty
   bash $D/check-initrd.sh $OUT/initrd.img-$KVER alarm  # "the trial's parts are there"
   ```
   Also run the phone-side checks of "Verdict and proposal" point 4
   (`pro1x-boot-trial` in Leste's runlevel and in the images). If any of
   them fails, do not flash.

   prepare installs the repository's `initramfs/` into Leste before it
   makes the initramfs (`65-kernel-update.sh`, lines 90-95), so the boot
   image carries what is committed then. Since 2026-10-31 that includes
   the file systems' options (`docs/power-management.md`, "The owner's
   decisions"): `/host` with `lazytime,commit=30`, the ext4 roots'
   `ROOTFLAGS` `lazytime,commit=30`, Arch's `lazytime`, and the loop
   device with direct I/O for images with 4 KiB blocks. After the first
   start: `findmnt -n -o OPTIONS / /host` and `losetup -l -O NAME,DIO,LOG-SEC`.
   If a system does not come up because of it (a root that does not
   mount), the way back without reflashing is a file on userdata, from
   any system that runs: `touch /host/pro1x-fs-defaults` (old options,
   plain loop, from the next start).
3. **[OK] flash.** It saves boot_b as
   `/host/boot_b-before-$KVER.img` and copies it to `$OUT`, writes boot_b,
   and starts the trial with 3 tries:
   ```sh
   PHONE="$PHONE" WORK=$WORK OUT=$OUT bash scripts/65-kernel-update.sh flash
   bash $D/phone.sh ssh leste /usr/local/sbin/pro1x-boot list   # trial: ... 3 tries left
   ```
4. **[OK] reboot** with the user there:
   `bash $D/phone.sh ssh leste /sbin/reboot`. Then `phone.sh wait 10`.
   If it does not come back: ways back 2.
5. In Leste:
   - `uname -r` = $KVER
   - `pro1x-boot list` shows "start confirmed" once Hildon runs
   - then `bash $D/collect.sh 04-$K-first-start`
6. **[OK] one more reboot** ends the trial. `pro1x-boot list` then has no
   `trial:` line, and `slots.txt` in the collect shows boot_b with 7
   tries, successful.
7. The checks of `docs/fm-radio.md` (after the flash):
   - `cat /proc/asound/cards; lsmod | grep -E 'wcn399x|slimbus'`. If the
     card is missing, run `modprobe snd-soc-wcn399x` before any rollback.
   - `aplay -l`, a sound, the microphone
   - `dmesg | grep -i -E 'smmu|fault'`

Abort: ways back 1 (the kernel runs) or 2 (it does not).

## Block 5: audio and FM on pro1x.10 (must; 75-90 min) [HAND]

The order matters: the jack's gate comes before the UCM profile, and the
traces for FreeBSD come after the profile.

Some steps here and in block 6 need Wi-Fi on the phone:
`apt install evtest` in step 2, and `gcc` and `libglib2.0-dev` for 71.
Connect Wi-Fi first (status menu).

1. **Loudspeaker pop (patch 0026):** `docs/audio.md` steps 1-5. Use the
   wav files in `/root/devday/`. PulseAudio is still on the old 300 s
   timeout. Collect: `collect.sh 05-pop`, plus the trace output pasted
   into `notes.md`.
2. **Headset jack (patch 0027):** `docs/audio.md` "Test on the phone",
   steps 1-2, with the OLD profile.
   - Needs `apt install evtest` (Wi-Fi).
   - [HAND] Headphones in and out, then a headset; press its buttons.
   - **Gate:** "Headphone Jack" `on` with nothing plugged in means stop:
     no profile.
3. **UCM profile** (only after 2 passed):
   ```sh
   PHONE="$PHONE" bash scripts/64-ucm-install.sh
   bash $D/phone.sh ssh leste 'sh /root/devday/ucm-check.sh controls'
   ```
   Then [OK] reboot and run step 4 of `docs/audio.md` (the ports follow
   the jack). The way back is `64-ucm-install.sh restore` plus a reboot.
4. **FM sound (0018-0021):** `docs/fm-radio.md` stage 3, step 4.
   [HAND] Headphones in; their cable is the antenna.
   ```sh
   bash $D/phone.sh ssh leste 'pro1x-fm-audio-test 99.9 10'
   bash $D/collect.sh 05-fm-sound /tmp/fm.wav
   ```
   Then the "FM Radio" app.
5. **The Leste audio traces for FreeBSD** (`freebsd/audio/README.md`
   step 0). They go after the profile, with pro1x.10 for the jack
   thresholds. [HAND] Headphones in. On the phone:
   ```sh
   sh /root/devday/leste-audio-trace.sh                              # hph,spk,ear,mic -> /host/audio-trace.txt
   sh /root/devday/leste-audio-trace.sh jack /host/audio-trace-jack.txt   # unplug/plug when told
   # [OPT] last, it rebinds the card (a reboot may be needed for sound):
   sh /root/devday/leste-audio-trace.sh probe /host/audio-trace-probe.txt
   ```
   Then `collect.sh 05-audio-trace /host/audio-trace.txt /host/audio-trace-jack.txt`.
   On the PC, `trace2seq.py` as in `freebsd/audio/README.md` makes
   `pro1x-audio.seq`. It goes to `/host/freebsd/audio/` in block 12.
6. The PulseAudio 3 s switch comes only after the pop test passed. It is
   part of `71-power-userspace.sh` (block 6). If the pop test failed, do
   not install 71's `50-pro1x.pa`: give 71 `restore` for that file, or
   leave block 6's step 3 out.

## Block 6: Leste packages and the user-space power parts (should; 25 min)

The way back: `apt-get install --allow-downgrades` of the old versions.
The versions are in `/root/devday/leste-install-*.log`, the files in
`/var/cache/apt/archives`.

1. `bash $D/deploy.sh leste-debs`
2. On the phone:
   - `sh /root/devday/leste-install.sh plan`
   - read the plan
   - `sh /root/devday/leste-install.sh install`

   The groups go in order: GTK 2, libraries, mce and hildon-desktop and
   the status menu, the applications, the N9 look. Only installed
   packages are upgraded; installing new ones needs NEW=1, e.g.
   `pro1x-alarmboot` for block 8, or the harmattan-* packages in the MeeGo
   copy. No service is restarted.
3. **The user-space power parts for pro1x.10** (the coordinator's
   decision). This needs `gcc` and `libglib2.0-dev` in Leste:
   ```sh
   bash $D/phone.sh ssh leste 'apt-get install -y gcc libglib2.0-dev'
   PHONE="$PHONE" bash scripts/71-power-userspace.sh install
   ```
   It covers 50-pro1x.pa (3 s), pro1x-autosuspend, pro1x-rpmtest, and the
   Wi-Fi power save, also in the MeeGo copy, Kali and Arch, and since the
   audit of 2026-10-31 GPS on demand: gpsd in no Leste runlevel, and on
   Kali and Arch the GNSS engine (`pro1x-gnss.service`) only while gpsd
   runs, gpsd started by its socket instead of at boot. With the owner's
   decisions of the same day also: pro1x-gnss stopping the engine when
   nobody reads (compiled in Leste, against 46's libqmi), Bluetooth off
   until switched on (`bluetoothctl power on`), mce's LED patterns
   without the "device on" light, the display's time-out 2 minutes (Leste
   gconf default, Kali's Xfce, Arch's Phosh), `lazytime,commit=30` in the
   fstab's line for `/` (Leste and the MeeGo copy get one, Kali's and
   LuneOS's get the options), the write-back sysctl, also in luneos.img.
   The way back is `71-power-userspace.sh restore`. The patched
   location-daemon and mce (+pro1x4) come with the next Leste package
   release (49-hidpi.sh).
4. [OK] reboot, then `collect.sh 06-after-packages`.
   - Expected: Hildon up, `DISPLAY=:0 xset -q` "Monitor is Off" after the
     display blanks (mce pro1x3).
   - Wi-Fi and the status menu work.
   - In the MeeGo copy (`pro1x-boot next meego`, [OK] reboot): the N9 home
     comes up. Back to Leste afterwards.

## Block 7: power measurements (should; 60-120 min) [HAND]

Plan and table: `docs/power-management.md` "Measurements for 2026-10-31".
The procedure is "The notify_rpm test", run on pro1x.10, not pro1x.9.
The pro1x.9 against pro1x.10 comparison is [OPT], only if time allows.
- Over Wi-Fi: `PHONE` with `root@<Wi-Fi address>` and the same key.
- [HAND] Cable out and the phone lying still for everything that
  suspends, until 0032 has shown that USB comes back.
- A phone that does not wake: hold the power key [HAND] until it
  restarts. notify_rpm is off after every start.

1. Awake, display off: `pro1x-powerlog <name> 30`.
2. Suspends [OK for the series]:
   - `69-rpm-test.sh 60 norpm bt-down`
   - `69-rpm-test.sh 60 rpm bt-down`
   - if that woke: `600 rpm bt-down`
   - then `60 rpm bt-down sensors`, `60 rpm bt-up btwake`
   - over USB: `pro1x-rpmtest 60 norpm`, from a Wi-Fi shell (0032)

   Logs go to `~/ps/pro1x-runs/2026-10-31/power/`: put
   `OUTDIR=~/ps/pro1x-runs/2026-10-31/power` in front of 69.
3. The Bluetooth UART between sleeps (`docs/power-management.md`, that
   section): IBS counters, `runtime_status`. [HAND] A Bluetooth device.
4. [OPT] One hour of standby with `rc-service pro1x-autosuspend start`.
5. The audit's checks (`docs/power-management.md`, "Measurements for
   2026-10-31", steps 4-9; awake, no suspend, no reboot): gpsd not running
   in Leste; the pairs dt2w on/off, the "device on" LED on/off (look at the
   LED [HAND]) and Bluetooth up/down, 15 minutes each [OPT, as time
   allows]; jbd2 and the UFS gating from step 1's log; the cpuidle
   baseline. Before one suspend of 2, the LED's `brightness`.
6. Afterwards: [OK] reboot (USB back), `collect.sh 07-power`, and fill in
   the table in `docs/power-management.md`.

## Block 8: alarm power-on (should; 20 min) [OK] [HAND]

See `docs/alarm-power-on.md`, "The test on the phone". It needs pro1x.10
(0028) and `pro1x-alarmboot`:
- `NEW=1 PKG_GROUPS=4-apps sh /root/devday/leste-install.sh install`
  once the package is in a `leste-*` pre-release (`fetch.sh` again)
- or `dpkg -i` of the deb

Use `pro1x-alarmboot` 1.2 (pre-release `leste-alarmboot-2026.10.31`: the
N9's alarm screen, then off again as the N9); after installing it, log
out of Hildon or reboot. The charger off (it keeps the phone on).

Steps:
1. `pro1x-boot pmic`: "power-on sources" must name "RTC alarm".
2. The look alone, in the session's terminal as `user`:
   `pro1x-alarmboot -D ring`, `-D off`, `-L de -D ring`. A tap ends each.
3. An alarm 5 minutes ahead in Leste's clock. Then [OK][HAND] switch off
   from the power key menu.
   - Expected: the phone comes on about 60 s before the alarm, without a
     menu, and rings: Leste's tone, the N9 screen (black, time, name,
     Snooze, Stop) over the Hildon dialog.
   - Stop: tone and screen go; 2 s later "Switching off": tap it (stays
     on). `cat /tmp/pro1x-alarmboot.log` (`stop tapped`, `status 7`,
     `stop for alarm N (button 1)`, `staying on: kept on`). Nothing rings
     10 minutes later.
4. Again, Stop and then nothing: off after 10 s. Again, Snooze and
   nothing: off, on again a minute before the snoozed time, the N9
   screen. Again, unanswered: snoozed after 50 s, off, back for the
   snooze. Optional: the power key during "Switching off" (stays on),
   portrait with the keyboard closed, MeeGo (its copy with the 1.2
   package and the new `01pro1x-meego`, see the document: the N9's own
   alarm screen, then off).
5. Counter-check: a start by the power key shows the menu
   (`last-boot`: `alarm-check: none not-by-the-rtc`).
6. `collect.sh 08-alarm /host/pro1x-boot/last-boot /tmp/pro1x-alarmboot.log`
   (the log of the last run; /tmp may not outlive a switch off).

The FreeBSD part of the test belongs in block 12, after G1.

## Block 9: Leste mobile data (must; 75 min) [HAND]

See `docs/leste-mobile-data.md`.
- The SIM went in in block 1.
- **Order: what works without a SIM, then calls and SMS, then data.**
- Mobile data costs money; a few MB.
- Never put the PIN in a file or on a command line: Leste's PIN dialog
  [HAND].

1. `bash $D/deploy.sh ofono`. It puts oFono's packages into
   `/root/mobile-data/` and `leste-md.tar` into `/root/`.
2. On the phone:
   ```sh
   mkdir -p /root/pro1x-md && tar -C /root/pro1x-md -xf /root/leste-md.tar
   DEBS=/root/mobile-data bash /root/pro1x-md/scripts/49-mobile-data.sh
   ```
   Then [OK] `/sbin/reboot`.
3. Steps 1-3 of the document (IPA, QRTR services, the modem). Then step 4:
   the PIN on the screen [HAND].
4. **Step 5 [HAND]:** a call out and in with sphone (earpiece and
   microphone), an SMS out and in with Conversations. A second phone is
   needed.
5. Steps 6-9: data. The APN table for Austrian SIMs is in the document.
   Wi-Fi off for step 8.
6. Step 10 makes the logs (`/root/mobile-data-log-*`). Then
   `collect.sh 09-mobile-data /root/mobile-data-log-<date>`. The IAP entry
   holds the IMSI: keep it out of anything public.

The way back is the document's "Way back" (ofono-orig, unhold,
pro1x-rmnet).

## Block 10: Kali (should; 40 min)

Its trial must have ended (block 4, step 6).
1. `bash $D/deploy.sh kali` copies `/root/kali.tar` and the fixed
   `pro1x-rtc.service` (it waits for rtc0, commit a5a42b4).
2. On the phone (Leste), the fixed service into the image, then mobile
   data's preparation:
   ```sh
   mkdir -p /root/pro1x-kali && tar -C /root/pro1x-kali -xf /root/kali.tar
   mkdir -p /mnt/kali && { mountpoint -q /mnt/kali || mount -o loop /host/kali.img /mnt/kali; }
   install -m 644 /root/devday/kali/pro1x-rtc.service /mnt/kali/etc/systemd/system/pro1x-rtc.service
   MNT=/mnt/kali bash /root/pro1x-kali/kali/25-mobile-data.sh
   umount /mnt/kali
   ```
   [OPT] Do the same `install` into `luneos.img` (it uses Kali's file).
3. [OK] `pro1x-boot next kali && /sbin/reboot`. On the PC:
   `phone.sh usbnet; phone.sh pin kali`.
   - Kali logs in as `kali` (password `kali`) with sudo.
   - Checks: the clock is right (not 1970) and `systemctl status
     pro1x-rtc` is fine.
   - GPS on demand (block 6's 71): `systemctl is-enabled gpsd.socket
     gpsd.service pro1x-gnss.service` = enabled, disabled, enabled, and
     `pgrep -x pro1x-gnss` empty (`docs/power-management.md`,
     measurements step 4); the blanking, step 10.
4. `docs/kali-mobile-data.md` steps 2-7 (PIN by `nmcli --ask` [HAND]).
   Then `SYS=kali bash $D/collect.sh 10-kali /host/kali-mobile-<date>`.
5. Back: `sudo pro1x-boot next leste && sudo reboot` [OK].

## Block 11: Arch (could; 30 min)

See `arch/README.md`. Its trial must have ended.
1. `bash $D/deploy.sh arch` copies the packages to
   `/host/devday/arch-pkgs` and `arch.tar` to `/host/devday/`.
2. [OK] `pro1x-boot next arch && /sbin/reboot`. Arch's key is pinned
   (`known_hosts.pro1x-arch`, user `alarm`, sudo).
3. In Arch:
   ```sh
   mkdir -p ~/pro1x-arch && tar -C ~/pro1x-arch -xf /host/devday/arch.tar
   sudo PKGS=/host/devday/arch-pkgs MNT=/ bash ~/pro1x-arch/arch/42-meego-look.sh
   ```
   Then the first look [HAND]: the screen's rotation, touch, the Phosh home
   bar (known bug: a layer surface of height 0), the clock (rtc offset),
   the FM radio. GPS on demand and the blanking as in block 10.
4. `SYS=arch bash $D/collect.sh 11-arch`. Back to Leste [OK].

## Block 12: FreeBSD (2-4 h; G1 and dsp1/3/4/6/7a must, others should or could)

Sources:
- `freebsd/README.md`, `freebsd/pm/README.md` (runs 1-25),
  `freebsd/dsp/README.md`, `freebsd/data/README.md`,
  `freebsd/modem/README.md` (incl. stage 5), `freebsd/audio/README.md`,
  `freebsd/fm/README.md`, `freebsd/camera/README.md`
- `tools/pro1x-freebsd-pool/README.md`

Rules for the block:
- FreeBSD only after block 4. Its start from the menu needs the UFS PHY
  fix 0016 (pro1x.5 and later), and the trial must have ended.
- **Each run = set the cmdline, [OK] `pro1x-boot reboot freebsd`, wait,
  Leste comes back by itself, `collect.sh fbsd-<run>`.** A hang ends in
  the watchdog's reset into the menu's default (Leste). The RAM console
  shows how far it got.
- Run `CLEAR=1 collect.sh ...` (clears after saving) so the next run
  starts clean.
- **Every cmdline gets `pro1x.pool=off`.** The 8 GB `pool.zfs` of 10-29
  holds an old system, and with the pool the RAM root unmounts userdata:
  no sim-pin, wpa_supplicant.conf, modules or tools. The dsp and cam
  cmdlines have it already. Add it to the fm, audio, data and modem ones.
- Crash-dump mode (`05c6:900e`): ways back 4.

### 12.0 Deploy (20 min; 3.1 GB to userdata)

```sh
bash $D/deploy.sh freebsd freebsd-pm freebsd-tests freebsd-wifi
```

Then on the phone (Leste, root), the kernel and image in place, the old
ones kept:
```sh
cd /host/freebsd
mv kernel.bin kernel.bin-before-2026-10-31; cp kernel.bin-2026-10-31 kernel.bin
[ ! -f root.zfs ] || mv root.zfs root.zfs-before-2026-10-31; mv root.zfs-2026-10-31 root.zfs
cp cmdline cmdline-before-2026-10-31
```

[HAND] The user's own files are typed by the user, never by a script:
```sh
install -m 600 /dev/null /host/freebsd/sim-pin
read -rs P && printf '%s\n' "$P" > /host/freebsd/sim-pin; unset P
```
- `data.conf` follows `freebsd/data/README.md`: apn, username, password,
  auth, protocol, roaming=0, autoconnect=0.
- `modem-test.conf` (`to=`, `dial=`, `answer=1`) is [OPT].
- Delete `root.zfs-before-2026-10-31` once G1 passed: it frees 3 GB.

### 12.1 The order

Each run below is a `cmdline` line, or a file in `/host/freebsd/cmdlines/`:
`cp /host/freebsd/cmdlines/cmdline.dsp-load /host/freebsd/cmdline`.

| # | Run | cmdline | Kernel | Priority | [HAND] | Time |
|---|---|---|---|---|---|---|
| G1 | the full system on root.zfs | `pro1x.test=1 pro1x.pool=off hint.qcom_wdt.0.lifetime=300` | kernel.bin (PRO1X) | must, first | - | 6 min |
| pm1 | SMP | `kern.smp.disabled=0 pro1x.test=smp hint.qcom_wdt.0.lifetime=300` | kernel.bin-pm | should | - | 5 min |
| pm2 | idle, one CPU | see `freebsd/pm/README.md` run 2 | pm | should | cable out | 17 min |
| pm3, pm4 | idle with the cluster / with 8 CPUs | run 3, run 4 | pm | could | cable out | 17 min each |
| pm5 | wfi comparison | `pro1x.test=power pro1x.power_min=10 hint.qcom_wdt.0.lifetime=900` | pm | could | cable out | 13 min |
| pm6 | suspend | run 6 (modules in `/host/freebsd/pm/`, deployed) | pm | should | cable out | 19 min |
| pm7 | suspend until the power key | run 6 + `pro1x.suspend_key=1` | pm | could | press power when told | 20 min |
| pm8 | pm6 with 8 CPUs | run 8 | pm | could | cable out | 19 min |
| pm9, pm10 | USB probe, USB gadget (CDC Ethernet) | runs 9, 10 | pm | should | cable in; PC: address on the new interface | 3 + 9 min |
| pm11 | the full system over USB, ssh | `pro1x.usb=1 pro1x.pool=off` | kernel.bin | could, after pm10 | `phone.sh pin freebsd` | 8 min |
| pm12 | RTC alarm | run 12 | pm | should, after pm6 woke | cable out | 10 min |
| pm13 | alarm clock in Hildon (+ block 8's FreeBSD part) | run 13, `pro1x.desktop=hildon` | kernel.bin | could | set the alarm, `pro1x-suspend` | 13 min |
| dsp1 | load | `cmdline.dsp-load` | kernel.bin-dsp* | must | - | 5 min |
| dsp3/4 | ADSP | `cmdline.dsp-adsp` | dsp | must | - | 6 min |
| dsp7a | RPM | `cmdline.dsp-rpm` | dsp | must | - | 5 min |
| dsp6 | modem | `cmdline.dsp-modem` | dsp | must | SIM in | 8 min |
| dsp5 | sensors | `cmdline.dsp-sensors` | dsp | should | - | 8 min |
| dsp6v | modem with the CX vote (after 7a) | `cmdline.dsp-modemcx` | dsp | could | - | 8 min |
| dsp7b, dsp7c | Wi-Fi, WPA | `cmdline.dsp-wifi`, `cmdline.dsp-wpa` | dsp | should | an AP | 10 + 11 min |
| data | ipa, modem, wds | `pro1x.test=data pro1x.data=ipa\|modem\|wds pro1x.pool=off hint.qcom_wdt.0.lifetime=600` | kernel.bin | should (after dsp6) | SIM; data.conf for wds | 3 x 13 min |
| modem | registration, sms, call, gps | `pro1x.test=modem [pro1x.modem=sms\|call\|gps pro1x.modem_wait=300\|600] pro1x.pool=off hint.qcom_wdt.0.lifetime=900` | kernel.bin | should (after dsp6) | SIM, second phone, outdoors for gps | 4 x 10-15 min |
| audio | pcm, adsp, play, rec, codec, voice, jack | `cmdline.audio-*` + `pro1x.pool=off`; `pro1x-audio.seq` from block 5 into `/host/freebsd/audio/` | kernel.bin | should: pcm, adsp, play; could: rest | headphones; jack: plug when told | 7 x 8-13 min |
| fm | uart, bt, radio, audio, slim, afe | `cmdline.fm-*` + `pro1x.pool=off` | kernel.bin | should: uart, bt, radio; could: rest | headphones | 6 x 6-13 min |
| cam | id, raw, app, jpeg | `cmdline.cam-*` | kernel.bin | should: id, raw | light in front | 4 x 6-11 min |
| stage 5 | SMS in Conversations | by hand in Hildon (`freebsd/modem/README.md` stage 5) | kernel.bin | could | SIM, second phone | 20 min |
| pm14-17 | USB-C, host, USB 3, role switch | `freebsd/pm/README.md` runs 14-17 | needs the rebuild | could | cables, OTG, stick | 30 min |
| pm18-25 | energy: jack by interrupt, autosleep, drain, vlow/vmin, what wakes it, Wi-Fi power save, GPS off after its app, the notification light | `freebsd/pm/README.md` runs 18-25 | needs the rebuild (image + hildon stage) | could; pm24, pm25 short | second phone for pm19/25; an hour each for pm20 | 15 min-3 h |
| dsp6a | adopt Leste's modem (pro1x-kexec-freebsd.sh, never run) | `cmdline.dsp-adopt` | dsp | could, last | - | 8 min |
| pool | `pro1x-freebsd-pool remove` / `create --size 8G` | - | - | could, last | - | 15 min |

Notes:
- **dsp kernel:** dsp-test's kernel differs from out's in the header only
  (`freebsd/dsp/README.md`: when they differ, use the dsp-test one). It
  is deployed as `kernel.bin-dsp-2026-10-31`.
- **The pm kernel:** `cp kernel.bin-pm-2026-10-31 kernel.bin` for the pm
  block. Afterwards **put kernel.bin back** (`cp kernel.bin-2026-10-31
  kernel.bin`) and the cmdline (`freebsd/pm/README.md`).
- **The modules** must match the kernel at `/host/freebsd/kernel.bin`.
  deploy.sh takes `out/modules` (the joint build); the test directories'
  own copies only where `out/modules` has none.
- **dsp6a** stops ofono and kexecs from the running Leste. Leste's root is
  gone from there on, and the modem's EFS writes held in rmtfs memory are
  lost. Its `-n` only loads and unloads. [OK] for each.
- **The pool** writes 8 GB to userdata [OK]. No prepared run needs it.
- **Energy (the audit of 2026-10-31, `freebsd/pm/README.md` "The energy
  audit")**, in the joint image once rebuilt: the panel goes off 2 minutes
  into the stand-by screen (`pro1x.standby_off=never` keeps the N9's way;
  pm20 (a)/(b) set it), gpsd and the modem's GNSS stop when the last app
  is gone (pm24), the status LED is Maemo's notification light (pm25,
  `pro1x_led_enable="NO"` for the old battery colours), PulseAudio
  starts (it had no `default.pa`: no Hildon sounds before). To note in
  the runs already planned: pm9's probe with no cable (`GDSC on`,
  `master clock on` = Linux left the USB block running, waste for as long
  as FreeBSD runs); pm14's `3 of 3 interrupts` (else `pro1x_typec` polls
  4 times a second); pm6/pm20 (c) `vlow +0, vmin +0` with the modem off
  (the DPU still scanning out with the panel off is the first suspect).

## Block 13: KolibriOS (should; 60-90 min)

Sources: `~/ps/kolibrios-arm/docs/pro1x.md`, `power.md`, `storage.md`,
`asm.md`, `pro1x-network.md`, `pro1x-modem.md`, `pro1x-audio.md`,
`threads.md`, `apps.md`.

**Never the old `pro1x-kexec.sh -m`.** It unbound IPCRTR, and Linux hung
in that on 10-29. That is the copy from 10-28 in `/root/kolibri/`. The
new script stops ofono, leaves Linux's IPCRTR open and passes
`kolibri.ipcrtr=adopt`. Kernel patch 0022 (in pro1x.6 and later) also
fixes the unbind deadlock. It has never run on the phone.

1. Block 0 rebuilt KolibriOS. Then `bash $D/deploy.sh kolibri`. It copies:
   - the kernel as `/host/kolibri/kernel.bin-2026-10-31`
   - the new `pro1x-kexec.sh`, `mkefs.py` and `pro1x-read.py` to
     `/root/kolibri/`; the old script stays as
     `pro1x-kexec.sh.before-2026-10-31`
   - `kolibri-home`
2. On the phone:
   ```sh
   cp /host/kolibri/kernel.bin /host/kolibri/kernel.bin-before-2026-10-31
   cp /host/kolibri/kernel.bin-2026-10-31 /host/kolibri/kernel.bin
   ```
   Then check that the phone runs the repository's script:
   `sha256sum /root/kolibri/pro1x-kexec.sh` must equal
   `sha256sum ~/ps/kolibrios-arm/tools/pro1x-kexec.sh` on the PC.
3. **Runs without the modem: from the boot menu.** The initramfs starts
   no DSP and arms the watchdog. For each run:
   - write the line into `/host/kolibri/cmdline`, always with
     `kolibri.nomodem`
   - [OK] `pro1x-boot reboot kolibri`
   - back in Leste: `CLEAR=1 bash $D/collect.sh kos-<run>`. The RAM
     console and the PPM screenshot come along.

   | Run | `/host/kolibri/cmdline` | [HAND] | Priority |
   |---|---|---|---|
   | T1 | `kolibri.nomodem kolibri.noinput kolibri.lifetime=45` | look at the screen | must |
   | T2 | `kolibri.nomodem kolibri.lifetime=60` | keys, touch | should |
   | P1 | `kolibri.nomodem kolibri.lifetime=60 kolibri.psci=off` | keys, touch | should |
   | P2 | `kolibri.nomodem kolibri.powertest=3 kolibri.displayon` | - | should |
   | P3 | `kolibri.nomodem kolibri.powertest=10` | cable out; power key if dark | could |
   | P4 | `kolibri.nomodem kolibri.lifetime=180` | power key and letter keys as in power.md | could |
   | P5 | `kolibri.nomodem kolibri.powertest=3 kolibri.keytest=60` | cable out | could |
   | B1 | `kolibri.nomodem kolibri.psci=off kolibri.bench kolibri.run=/sys/sysbench kolibri.lifetime=60` | - | should |
   | S1 | `kolibri.nomodem kolibri.lifetime=90 kolibri.run=/sys/board` | - | should |
   | S2 | `kolibri.nomodem kolibri.lifetime=180 kolibri.run=/sys/kfar` | keyboard | could |

4. **The container (S3/S4) [OK]: it writes 512 MB to userdata.**
   - Create it: `kolibri-home create --host /host --size 512M`, then
     `kolibri-home status --host /host`.
   - S3: a boot-menu start without `kolibri.home`.
   - S4: `kolibri.home=rw` in the cmdline, writing in kfar and t_edit;
     back in Leste, `kolibri-home status` and `mount` as in
     `storage.md`.
   - The way back: `kolibri-home remove --host /host`.
5. **With the modem: only the new `pro1x-kexec.sh` from the running
   Leste.** Display on, SIM unlocked in Leste:
   ```sh
   cd /root/kolibri
   ./pro1x-kexec.sh -n -m          # loads and unloads only: no ofono stop, no jump
   ./pro1x-kexec.sh -m /host/kolibri/kernel.bin "kolibri.lifetime=300"     # [OK] M0
   ```
   - A hang before the jump, while ofono stops, has no watchdog. Hold the
     power key [HAND], and run no Wi-Fi tests until `-m` works.
   - Then, each with `-n` first: A1 `-m -a` (call audio [HAND], a second
     phone), and W1-W3 `-m -w /root/kolibri/wifi.conf`.
     `/root/kolibri/wifi.conf` is root-only: `ssid=`, `psk=`, `country=AT`.
     The user writes it.
   - No messenger linking until `rng: the SoC's PRNG` shows on the phone.
6. At the end, `/host/kolibri/cmdline` gets a harmless line back:
   `kolibri.nomodem kolibri.lifetime=60`.

## Block 14: Whisperfish (could; 20-40 min)

See `~/ps/whisperfish-leste/README.md` and `leste/test/chroot-test.sh`.

1. `bash $D/deploy.sh whisperfish` copies the deb (sha256 `44a43122…`)
   and the `leste/` tar to `/host/wf-leste/in/`.
2. **WF1, the stacked windows in a chroot**:
   - Unpack the tar to `/host/wf-leste/in/src`, then run
     `STEPS="start sleep:40 shot:start ..." bash /host/wf-leste/in/src/leste/test/chroot-test.sh /host/wf-leste/in/whisperfish-leste_0.6.0.dev-1_arm64.deb`.
   - It overlays `meego-leste.img` read-only: never while the MeeGo copy
     runs, and never during `65 prepare`. Unmount afterwards.
   - Collect `/host/wf-leste/shots`.
3. **WF2, the real link**: only if the user wants it now. It touches
   their Signal account [HAND]: Linked devices, then scan the QR.
   - `apt install /host/wf-leste/in/whisperfish-leste_0.6.0.dev-1_arm64.deb`
   - The way back: `apt remove`, unlink the device in Signal, delete
     `~/.config/be.rubdos` and `~/.local/share/be.rubdos`.

## Block 15: Waydroid (could; 60 min)

See `docs/waydroid.md`. It needs Wi-Fi and about 1 GB to download.

1. `bash $D/deploy.sh waydroid` puts the deb in `/root/`.
2. Then the document: `apt install /root/pro1x-waydroid_1.1_all.deb`,
   `apt update`, `df -h / /host`, `apt install waydroid`, `waydroid init`,
   `pro1x-waydroid check`. Then the device test, steps 1-9 [HAND].
3. The way back is the document's "Removal".

## Block 16: Nokia N950 (should; 60-120 min)

Sources: `~/ps/n950-maemo-leste` (`docs/install.md`), `~/ps/kolibrios-arm/docs/n950.md`.
- Nothing there has been tested on the phone yet.
- Access: ubiboot's maintenance mode exports the eMMC over USB.
  - Leste: `ssh root@192.168.2.15` (the PC gets 192.168.2.100 by DHCP),
    key-only. Use the image with the key,
    `~/ps/n950-leste-build/release-20261029/n950-leste-20261029-with-key.img`.
  - Pin it: `phone.sh pin n950`, then `SYS=n950 bash $D/collect.sh n950-...`.

1. **N1 install [OK][HAND]: it repartitions the eMMC.**
   - [HAND] The N950 is off. Plug the cable in while ubiboot starts (it
     exports the eMMC).
   - On the PC, `lsblk` to find the 14.8 GiB disk.
   - `sudo bash install/n950-install.sh /dev/sdX <image>`. It backs up
     the partition table, the first MiB, MyDocs and ubiboot.conf into
     `./n950-backup-<time>/`, then writes p1 (MyDocs, restored) and p4
     (Leste). Harmattan's p2 is never touched.
   - No restore procedure is documented. The backup is the way back
     (`sfdisk` + `mkfs.vfat` + untar).
2. **N2 the first start:** in ubiboot "Maemo", then "Leste, Linux 6.18".
   USB networking, then ssh. Collect.
3. **N3 FM** (WL1273) [HAND] headphones:
   - `n950-fm-test scan`
   - `n950-fm-test -k <MHz>; n950-fm-audio on`
   - `FM_AUDIO=bypass` if there is no sound
4. **N4 GPS:** `harmattan-gps start` (outdoors [HAND]).
5. **N5 KolibriOS:**
   - `SYS=n950 bash $D/deploy.sh n950` puts the rebuilt kernel at
     `/boot/kolibri-new.bin`, plus `n950-dss-dump.sh`.
   - Under Leste: `sh /root/devday/n950-dss-dump.sh > dss.txt`.
   - Swap the kernel, keeping the image's own:
     `cp /boot/kolibri.bin /boot/kolibri-test.bin; cp /boot/kolibri-new.bin /boot/kolibri.bin`.
     The image's own kernel is old (bb6da38, without the ATAG fix).
   - [OK] reboot, then ubiboot "KolibriOS (test)".
   - Photograph the screen [HAND].
   - The way back: power key, then `cp /boot/kolibri-test.bin
     /boot/kolibri.bin` under Leste.

## End of the day (15 min)

- The Pro1X back in a known state:
  - `pro1x-boot list`: default leste, no `next`, no trial.
  - `/host/freebsd/kernel.bin` and `cmdline` = the PRO1X kernel and a
    plain line (or `cmdline-before-2026-10-31`).
  - `/host/kolibri/cmdline` harmless; the RAM console cleared.
- `collect.sh 99-end-of-day`.
- Copy `~/ps/pro1x-runs/2026-10-31/` into the results notes. Fill in the
  measurement table in `docs/power-management.md`. Each block's document
  gets its results ("Tested on the phone 2026-10-31: ..."), committed by
  the agents of those parts.
