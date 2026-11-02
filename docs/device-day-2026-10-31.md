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
| `fix-ownership.sh check\|fix\|undo [ROOT]` | phone | The images built before 2026-10-31 (bb39fa8/0f271a9) left `/`, `/etc`, `/usr`, the overlays' files, the modules and the firmware owned by uid 1000 (`user`, `kali`, `alarm`) or group-writable: a way to root. `check` lists them, read only; `fix` gives our paths back to root and writes an undo list first; `undo` puts that back. Our paths: `owned-paths.txt` next to it (`mk-owned-paths.sh` writes it from the repository). `deploy.sh tools` puts both into `/root/devday/` and `/host/devday/`. |
| `check-initrd.sh INITRD [alarm]` | PC | Checks that the initramfs `prepare` made carries the trial's parts. The watchdog's arming is skipped silently without `qcom-wdt.ko` or `devmem`. Passes on pro1x.5's initramfs. |
| `kexec-test.sh check\|load\|go\|after K` | PC | Starts the kernel of `$WORK/out-K` once from the running Leste with kexec, boot_b untouched (block 18). `go` and `after`'s dump only with `OK=yes`. |
| `probe.sh install\|arm\|status\|off\|watch` | PC | A kernel flashed once with a way back that needs no hands while the PC is on the cable (block 18.1): the phone's `pro1x-probe` service (sysinit) puts the saved boot_b back about 15 s into the candidate's start and ends its trial; `watch` takes the dump on 900e and collects. |
| `crashdump.py DIR...` | PC | Decodes a `qdl ramdump` directory: the PMIC's reset reasons, TZ's interrupt counters and log, the NoC errors (`docs/crash-dumps.md`). Reads files only. |
| `test/run.sh` | PC | Tests the helpers above kexec-test.sh against a fake phone (fake ssh/scp): 39 checks. |
| `test/fix-ownership.sh` | PC, sudo | Tests `fix-ownership.sh` on two fake roots made as the old builds made them (check, fix, check, undo): 47 checks; `CLEAN=<a correctly built image, mounted read-only>` adds 2. |

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
| 2b | Ownership fix (the old images' uid 1000 owners) | should | 10 min | fix | - |
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
| 17 | The MeeGo session's problems of 22:30 (mce +pro1x6, helpers, libraries) | should | 40-60 min | session stop, reboot | tests in MeeGo |

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
   `sudo -n qdl ramdump -o ~/ps/pro1x-runs/2026-10-31/ramdump-<time> 'OCIMEM*,CODERAM*,DATARAM*,MSGRAM*,PMIC*,PMON*,RST*,FSM*,*.cmm'`
   (since 11-01 with the RPM's data RAM and the PMIC's history), then
   `python3 -I tools/devday/crashdump.py <that directory>` (docs/crash-dumps.md).
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
   checks passed", and so must `bash $D/test/fix-ownership.sh` (sudo).

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
8. **Ownership, read only.** Every system image on the phone was built
   before bb39fa8/0f271a9 (Leste of 2026-10-24, its MeeGo copy, Kali,
   Arch, LuneOS). Their `/`, `/etc`, `/usr`, `/etc/init.d`, the overlays'
   files, the modules and the firmware probably belong to uid 1000
   (`user` in Leste), or are group-writable: a way to root. Step 6's
   `deploy.sh tools` put `fix-ownership.sh` into `/root/devday/`.
   ```sh
   bash $D/phone.sh ssh leste "stat -c '%U %n' / /etc /usr /etc/init.d"
   bash $D/phone.sh ssh leste 'sh /root/devday/fix-ownership.sh check' \
       > ~/ps/pro1x-runs/2026-10-31/01-ownership-leste.txt
   tail -3 ~/ps/pro1x-runs/2026-10-31/01-ownership-leste.txt
   ```
   - Expected from the old image: `user /`, `user /etc`, and so on. The
     check lists our paths as `owner ... -> 0(root):0(root)` or
     `mode 0775 -> 0755`, each with the step it came from, and ends in
     `== N of our paths to fix`. Block 2b fixes them.
   - `root` everywhere and `== 0 of our paths to fix ..., 0 by hand` means
     the image is fine.
   - "owned by a user ..., not ours" lists the rest that belongs to a
     uid >= 1000 outside the homes, with its package. The fix leaves these
     alone. Look at them by hand.
   - `/host` there, uid 1000 with 0771, is userdata's root: Sailfish's
     `/data`, which Android gives its `system` user (uid 1000). The fix
     leaves it alone. It does mean that `user` can rename or delete the
     images in `/host`: that is the owner's decision, not today's (compare
     `host-ls.txt` from block 2).

   The other systems' images, from Leste (as root, read only, never the
   image of a system that runs). Their reports go to `/root/devday/`:
   ```sh
   own() {   # own IMAGE [MOUNT OPTIONS]
       mkdir -p /mnt/own && mount -o loop,ro,${2:-noload} "$1" /mnt/own || return
       n=/root/devday/ownership-$(basename "$1" .img).txt
       sh /root/devday/fix-ownership.sh check /mnt/own > "$n"; tail -1 "$n"
       umount /mnt/own
   }
   own /host/meego-leste.img; own /host/kali.img; own /host/luneos.img
   own /host/arch.img subvol=@     # Btrfs: the root subvolume (still ext4: without)
   ```
   Then on the PC: `bash $D/collect.sh 01-ownership
   /root/devday/ownership-meego-leste.txt /root/devday/ownership-kali.txt
   /root/devday/ownership-luneos.txt /root/devday/ownership-arch.txt`.
   `/host/harmattan.img` is not one of ours: it is Nokia's tree, and its
   user has uid 1000 on purpose (`meego/37-harmattan-user.sh`).

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

## Block 2b: the ownership fix (should; 10 min) [OK]

Only if block 1, step 8 listed our paths. It changes owners and modes, no
contents. It needs no reboot. Afterwards a system's paths are as in a
correctly built image: the images of 2026-10-31 give nothing in `check`.
Before changing anything, `fix` writes every path's owner and mode to
`/root/devday/fix-ownership-undo-<time>.txt`. It changes only the paths
in `owned-paths.txt` (the overlays' files, the copies' destinations, the
directories above them). It never changes the homes, `/root`'s contents,
`/var/lib`, `/tmp`, or what is "not ours".

1. [OK] Leste, on the phone as root:
   ```sh
   sh /root/devday/fix-ownership.sh fix | tee /root/devday/ownership-fix-leste.txt
   ```
   It prints the check, the undo list's name, every change, and the
   check after it. That check must end in `== 0 of our paths to fix`.
2. [OK] The other images, from Leste (read-write, never while that
   system runs):
   ```sh
   fixown() {   # fixown IMAGE [MOUNT OPTIONS]
       mkdir -p /mnt/own && mount -o loop${2:+,$2} "$1" /mnt/own || return
       sh /root/devday/fix-ownership.sh fix /mnt/own |
           tee "/root/devday/ownership-fix-$(basename "$1" .img).txt"
       umount /mnt/own
   }
   fixown /host/meego-leste.img; fixown /host/kali.img; fixown /host/luneos.img
   fixown /host/arch.img subvol=@
   ```
   Arch's snapper snapshots (`/.snapshots`) keep the old owners. After a
   rollback to a snapshot taken before today, run the fix again (in Arch:
   `sudo sh /host/devday/fix-ownership.sh fix`).
3. On the PC: `bash $D/collect.sh 02b-ownership
   /root/devday/ownership-fix-leste.txt
   /root/devday/ownership-fix-meego-leste.txt` (and the others).

**The way back:** each fix's undo list.
```sh
sh /root/devday/fix-ownership.sh undo /root/devday/fix-ownership-undo-<time>.txt
```
A list holds its root (`/`, or `/mnt/own`) and the image that was
mounted there. For an image, mount it read-write on `/mnt/own` first,
then umount it after the undo. `undo` refuses a list of another image.

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

   **One change at a time:** before the flash, in the running Leste,
   `touch /host/pro1x-fs-defaults`, so the trial tests the new kernel
   with the old mount options and the plain loop. The new options come in
   step 6a, after the trial.
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
6a. **[OK] the new file-system options:** in Leste `rm /host/pro1x-fs-defaults`,
   reboot, then `findmnt -n -o OPTIONS / /host` (lazytime, commit=30) and
   `losetup -l -O NAME,DIO,LOG-SEC` (DIO 1 for images with 4 KiB blocks).
   If Leste does not come up: start another system from the menu (Kali,
   Arch) and `touch /host/pro1x-fs-defaults` there; they use the same loop
   path, so if none comes up, ways back 2.
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

### 6b. mce +pro1x5: the power key in the stand-by mode (found 19:19-19:30)

With mce +pro1x3 (and the fresh image's +pro1x4) a short press of the
power key did not switch the display on; about 5 s after the last press
only the stand-by screen came (dark), and only a press then gave the
desktop (`~/ps/pro1x-runs/2026-10-31/pk-*.txt`). Cause and fix:
[`patches/lpm/README.md`](../patches/lpm/README.md), "Fixed in +pro1x5";
the package is in the pre-release
[leste-hildon-2026.10.31.1](https://github.com/smatkovi/pro1x-maemo-leste/releases/tag/leste-hildon-2026.10.31.1)
(untested on the phone). The low power mode itself (behaviour, settings,
the stand-by screen) is documented in `patches/lpm/README.md`, the
panel's side in `docs/power-management.md`, "The panel's low power modes".

1. On the phone, as root: `pro1x-update` (read the plan: mce
   1.10.17+pro1x5), then `pro1x-update --yes`. Or with the package from
   the release copied to the phone, with services blocked: the package's
   postinst would restart mce (`invoke-rc.d mce restart`), and a
   restarted mce leaves the display off (`scripts/49-hidpi.sh`):
   ```sh
   [ -e /usr/sbin/policy-rc.d ] || { printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d; chmod 755 /usr/sbin/policy-rc.d; G=1; }
   apt install --allow-change-held-packages ./mce_1.10.17+pro1x5_arm64.deb
   [ -z "${G:-}" ] || rm /usr/sbin/policy-rc.d
   apt-mark hold mce
   ```
   So the new mce runs only after the next reboot. Each reboot while
   slot b's pro1x.5 trial is pending costs a try (see "Rollback"):
   install it now and let it come with a reboot that is due anyway.
2. [OK] after that reboot, [HAND]:
   - Let the display time out: dimmed, then the stand-by screen (the
     clock, 20 %) for a minute, then black.
   - Power key in the black: the display on at once, with the lock screen;
     about 5 s untouched, then the stand-by screen.
   - Power key on the stand-by screen: on again, with the lock screen.
   - Double tap on the stand-by screen: on, with the lock screen.
   - Keyboard slide opened in the black or on the stand-by screen: the
     desktop.
   - While the stand-by screen shows:
     `dbus-send --system --print-reply --dest=com.nokia.mce /com/nokia/mce/request com.nokia.mce.request.get_display_status`
     says `lpm` (+pro1x3/4 said `off`).
   - New with +pro1x5, as the proximity sensor now takes effect: the
     sensor covered for 5 s on the stand-by screen makes it black;
     uncovered, the stand-by screen comes back, but probably black
     (hildon-desktop draws again only on "on": `patches/lpm/README.md`,
     "Open"). If the stand-by screen lasts only about 5 s right after
     the time-out, the sensor reads "near" when claimed.
   - `collect.sh 06b-mce-pro1x5`.
3. The way back: `mce_1.10.17+pro1x4_arm64.deb` (leste-hildon-2026.10.31)
   the same way, with `--allow-downgrades` added, then a reboot.

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

Use `pro1x-alarmboot` 1.5 (pre-release `leste-alarmboot-2026.10.31.3`: the
N9's alarm screen, then off again as the N9; its screens count as an
alarm's for mce, as the system UI's dialog does); after installing it,
log out of Hildon or reboot. The charger off (it keeps the phone on).

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
   snooze. The power key during "Switching off": stays on (`staying on:
   the power key` in the log, before it `the power key` or
   `display_status_ind off: the power key`); left alone
   for more than the display time-out while the screens show, the display
   stays on. Optional:
   portrait with the keyboard closed, MeeGo (its copy with the 1.2
   package and the new `01pro1x-meego`, see the document: the N9's own
   alarm screen, then off). Arch (the packages of `arch-hildon-2026.10.31.3`,
   all of them, with `pro1x-alarmboot` 1.5; `pro1x-desktop meego`): the
   same steps as Leste.
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
   - Ownership (block 2b): `stat -c '%U %n' / /etc /usr` shows `root`,
     and `sudo sh /host/devday/fix-ownership.sh check` ends in
     `== 0 of our paths to fix`. If not: `fix` [OK], as in block 2b.
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
   The packages are those of `arch-hildon-2026.10.31.3` (all 153). Its
   `libhildonfm` no longer has `/etc/xdg/user-dirs.defaults`, xdg-user-dirs'
   file, where step 40 had put Maemo's MyDocs lines. Put xdg-user-dirs' back
   (WLAN needed; `pro1x-update --yes` does the same):
   ```sh
   sudo pacman -Sw --noconfirm xdg-user-dirs &&
     { ! grep -q MyDocs /etc/xdg/user-dirs.defaults || sudo rm /etc/xdg/user-dirs.defaults; } &&
     { [ -e /etc/xdg/user-dirs.defaults ] || sudo pacman -S --noconfirm xdg-user-dirs; }
   ```
   Ownership (block 2b): `sudo sh /host/devday/fix-ownership.sh check`
   ends in `== 0 of our paths to fix`. If not: `fix` [OK], as in block 2b.
   Then the first look [HAND]: the screen's rotation, touch, the Phosh home
   bar (known bug: a layer surface of height 0), the clock (rtc offset),
   the FM radio. GPS on demand and the blanking as in block 10.
4. `SYS=arch bash $D/collect.sh 11-arch`. Back to Leste [OK].

## Block 12: FreeBSD (2-4 h; G1 and dsp1/3/4/6/7a must, others should or could)

Sources:
- `freebsd/README.md`, `freebsd/pm/README.md` (runs 1-26),
  `freebsd/dsp/README.md`, `freebsd/data/README.md`,
  `freebsd/modem/README.md` (incl. stage 5), `freebsd/audio/README.md`,
  `freebsd/fm/README.md`, `freebsd/camera/README.md`
- `tools/pro1x-freebsd-pool/README.md`

Rules for the block:
- FreeBSD only after block 4, and the trial must have ended. **Its start
  from the menu needs kernel 7.1.0-pro1x.12 or later** (patch 0040: the
  kexec hands UFS over, no rebind; 12.2) in a boot image prepared from
  the repository as of 2026-10-31 evening. A boot image of pro1x.10 or
  .11 prepared now refuses FreeBSD and starts Leste (`last-boot`:
  `not starting freebsd: no arm_smmu.kexec_bypass`). pro1x.5's boot_b of
  10-29 still rebinds UFS and hangs (G1 of 2026-10-31): no FreeBSD on it.
- **Each run = set the cmdline, [OK] `pro1x-boot reboot freebsd`, wait,
  Leste comes back by itself, `collect.sh fbsd-<run>`.** The menu arms
  the APSS watchdog (about 32 s) right after FreeBSD's kernel is loaded,
  before `/host`'s umount, and reloads it just before `kexec -e`; a hang
  from there on (umount, Linux's shutdown, the jump, FreeBSD before its
  `qcom_wdt` runs) ends in the watchdog's reset into the menu's default
  (Leste: the one-shot choice is used up). The RAM console shows how far
  FreeBSD got; the menu's own steps are in the pstore (`<3>`
  `pro1x-loop-root:` lines) and in `/host/pro1x-boot/last-boot`.
  No bite of the APSS watchdog is on record for this phone yet: if one
  lands in the crash-dump mode (`05c6:900e`), ways back 4.
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
| pm18-26 | energy: jack by interrupt, autosleep, drain, vlow/vmin, what wakes it (and Wi-Fi's interrupts associated, idle), Wi-Fi power save, GPS off after its app, the notification light, drawing on the CPU (transitions, blur, llvmpipe's threads) | `freebsd/pm/README.md` runs 18-26 | needs the rebuild (image + hildon stage) | could; pm24-26 short | second phone for pm19/25; an hour each for pm20 | 15 min-3 h |
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
  lost. Its `-n` only loads and unloads. [OK] for each. It needs
  pro1x.12 too (12.2), and the copy in `/root/devday` from 2026-10-31's
  deploy is the old one (it rebinds UFS): copy the script again.
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
  (the DPU still scanning out with the panel off is the first suspect);
  pm22 with Wi-Fi associated and idle: `vmstat -i` twice a minute apart,
  about ten a second from the copy engines = the beacons reach the host
  (no beacon filter or ARP/NS offload in ath10k's code: README).
  Hildon on FreeBSD now runs Clutter at 30 frames a second and Qt Quick's
  basic render loop (llvmpipe draws on the CPU); in the VM the task
  navigator costs about 23 CPU seconds per entry against 1 s for an app
  opening, nothing while idle; pm26 measures the transitions, the blur and
  `LP_NUM_THREADS` on the phone.

### 12.2 The menu's FreeBSD start since G1 of 2026-10-31

**What happened (G1, 21:32, pro1x.5, logs in
`~/ps/pro1x-runs/2026-10-31/fbsd-G1-2/`):** the phone hung for over ten
minutes without USB and without a reset, until the power key. The menu
then still rebound UFS before the jump: unbind, its IOMMU group to
`identity`, bind, wait for the partition. The pstore of that start ends
at 11.1 s with UFS's register dump and `ufshcd_abort: cmd was completed,
but without a notifying intr, tag = 0`.

**The cause, from the dump and the 7.1 sources:**
- Confirmed:
  - The identity domain took effect. The dumped DMA addresses are
    physical: the command descriptor at 0xfde20000 lies in the CMA area
    (0xfdc00000, 32 MiB), the request list at 0x1_02dde000. Under the
    SMMU they would be 36-bit IOVAs near 0xf_ffff_ffff.
  - The rebound host brought the link up. Link startup completed, and
    UIC commands complete by interrupt, so the interrupt line works.
  - The aborted command is the first NOP OUT, a device-management
    command on the reserved tag 0. Its request UPIU is all zeros. The
    host finished it: the doorbell is clear and UTRLCNR bit 0 is set.
    But nothing came back into the memory Linux allocated: the OCS is
    still 0xf, the response is all zeros, and IS has no completion. So
    the host never saw the descriptor's interrupt bit. The interrupt was
    not lost: the host's DMA after the rebind went somewhere else.
- Not known: whether the identity domain (the SMMU bypass for this
  stream) or the second probe of the host makes its DMA go astray. The
  rebind has never worked on the phone. On 10-29 it failed earlier, at
  the PHY (`-19`, fixed by 0016). Before that, the menu did not rebind.
- Likely, for the hang: nothing at error level follows the abort (no
  device or host reset, no `NOP OUT failed`). So the probe never
  finished: the async scan holds UFS, the 30 s `find_backing` gave up,
  and the `reboot -f` that followed waited for the probe in the kernel's
  shutdown (`wait_for_device_probe`). The watchdog was armed only after
  all this, just before `kexec -e`, so nothing reset the phone.

**What changed:**
- `device/kernel/patches/0040`, kernel 7.1.0-pro1x.12 (= pro1x.11 +
  0040). Built 2026-10-31 in `$WORK/out-pro1x.12`, not flashed. A new
  parameter, `/sys/module/arm_smmu/parameters/kexec_bypass` (device
  names). At a kexec, after the devices' own shutdown, the SMMU's
  shutdown switches translation off in the context bank of each named
  device's stream (SCTLR.M = 0, the way arm-smmu-qcom emulates bypass).
  FreeBSD's `ufshci` reports exactly that: `MMU off: identity`.
- `initramfs/local-top/pro1x-loop-root`:
  - UFS is no longer unbound. It stays bound and translated, and the
    menu writes `4804000.ufshc` into that parameter before `kexec -e`.
    FreeBSD gets UFS with the link of the first probe (kept by 0004).
  - A kernel without the parameter does not start FreeBSD; Leste starts
    instead.
  - The watchdog is armed right after `kexec -s -l`, before `/host`'s
    umount, and reloaded to its full time just before `kexec -e`.
  - If `kexec -e` comes back, the parameter is cleared and the watchdog
    stopped; a one-shot start then gets Leste's watchdog as usual.
  - KolibriOS gets the same watchdog steps; UFS is untouched there.
- `freebsd/dsp/tools/pro1x-kexec-freebsd.sh` (dsp6a): the same hand-over,
  with no rebind of Leste's own root. Copy it to the phone again.
- PC tests:
  - `bash tests/boot-menu-kexec/run.sh`: the script's FreeBSD, KolibriOS
    and Leste paths in a sandbox, the order of the watchdog, the umount,
    the parameter and `kexec -e`, and the way back.
  - `bash tests/pro1x-boot-trial/run.sh`: still passes.

**Before the first FreeBSD run:**
0. **pro1x.12 crashed in all three trial starts (22:56-23:05): block 18.**
   Do not flash it again; the bisect there decides which kernel follows.
1. Flash pro1x.12 like block 4, with `K=pro1x.12` (OUT=$WORK/out-pro1x.12)
   and its own trial. It also carries 0033-0039, which no phone has run
   yet. Or flash pro1x.12 instead of pro1x.10 in block 4.
2. After prepare, check the boot image. The first must end in "the
   trial's parts are there"; the second must print 1 or more:
   ```sh
   bash $D/check-initrd.sh $OUT/initrd.img-$KVER alarm freebsd
   zcat $OUT/Image.gz | grep -ac arm_smmu.kexec_bypass
   ```
3. On the phone, after the start: `ls -l /sys/module/arm_smmu/parameters/kexec_bypass`
   (it must exist, mode 644; `cat` shows `(null)`).

**What G1 shows next:**
- FreeBSD's RAM console has the `ufshci` attach. Look for
  `stream 0x100: SMR... S2CR ... (translate)`, then `context bank N: ...
  SCTLR 0 (MMU off: identity)`, `keeping the link that is up`, and the
  root on ZFS.
- If `ufshci`'s first commands time out there the same way (no
  completion, descriptors untouched), the bypass itself is what breaks
  UFS's DMA. Then FreeBSD must translate UFS's stream with page tables of
  its own; no Linux-side change helps then.
- If FreeBSD never starts, the pstore's last `pro1x-loop-root:` line and
  `last-boot`'s `starting freebsd: ... loaded` show whether the menu got
  to the jump.

Untested: all of it on the phone. That covers patch 0040 on real
hardware (the context bank write, the hypervisor), the bypass for UFS,
FreeBSD's `ufshci` on a link the menu did not touch, and a watchdog bite
on this phone.

### 12.3 G1 again, 2026-11-01 01:46 (kernel 7.1.0-pro1x.12nobtslim)

`~/ps/pro1x-runs/2026-11-01/fbsd-G1-3*` (kernel.bin of 10-31, abaa2729)
and `fbsd-G1-4-k1029*` (kernel.bin-2026-10-29, d77cb918, put back
afterwards), both collected with `CLEAR=1`:

- The menu got to the jump: the pstore ends with `pro1x-loop-root:
  starting freebsd: watchdog armed` (11.7 s, 9.5 s). `kexec_bypass` was
  there (patch 0040), UFS was not rebound.
- FreeBSD was entered: the RAM console's marker said
  `PRO1X-FBSD:ENTERED` both times (every collect before had `none`), but
  there was **no console text** at all and no screenshot.
- The watchdog armed by the menu bit about 32 s later (TZ's crash
  handling at 47.65 s after power-on), 900e, the dump reset the phone
  into Leste: with the PC on the cable the way back needs no hands now
  (G1 of 10-31 hung for ten minutes). Without it the phone stays in 900e
  until a long press of the power key: `qcom_scm.download_mode` is
  already off, so the bite going to 900e is the firmware's policy.
- Both FreeBSD kernels end the same way, so it is not the 10-31 build.
  FreeBSD wrote its console at 0x5ca01000 on 10-25 and 10-27 (Linux
  pro1x.1-.3 era); since then the Linux side changed (0004, 0005, 0040,
  the bisect kernel's patches, the menu). Next: find where FreeBSD stops
  between its entry and the console (its proof-of-life stripe, or marker
  words written along locore/initarm), then compare with the hand-over.

### 12.4 FreeBSD from the menu boots to its RAM root (2026-11-01 07:30-08:12)

The marker without console text (12.3) was FreeBSD's own: a DDR dump
(`qdl ramdump` with `DDRCS0_0*`, 2 GiB, 97 s; `tools/devday/fbsd-ddr.py` finds
the kernel, at 0x48000000, checks its text against kernel.bin and reads boot
variables by symbol; `fbsd-frames.py` decodes the trapframes on the boot
stack) showed `boothowto` with RB_MULTIPLE|RB_VERBOSE (so initarm reached
cninit), `ramcons_buf` set but `ramcons_pos` 0, and the stack full of nested
data aborts. The outermost: `ramcons_cninit`, FAR offset 15, ESR 0x96000061
(an alignment fault on a write): memcpy() of the magic into Device memory.
Three FreeBSD fixes (freebsd/patches/0040-0042), each found by the next run:

| Run | Kernel | Result |
|---|---|---|
| fbsd-G1-5-ddr (07:38) | kernel.bin of 10-31 | the dump above |
| fbsd-G1-6-ramcons (07:49) | + 0040 (ramcons byte writes) | 51 KB of console: device attach up to cpufreq on cpu1, `Fatal data abort` far 0x3c in cpufreq_attach (cpu1 has no pcpu: kern.smp.disabled=1), panic, reboot after 15 s |
| fbsd-G1-7 (08:08) | + 0041 (cpufreq on running CPUs), 0042 (SPMI summary IRQ via the MPM's pin map: GIC SPI 183) | through all of the device attach, the RAM root mounted, its rc runs ("enter within 10s for a shell else reboot") |

What is left (run 7's console):

- **UFS:** `ufshci0: SMMU ... context bank 2: ... SCTLR 0 (MMU off:
  identity)` and `keeping the link that is up` (0040 works), then at the
  first query `UTP transfer request list notification is not ready`,
  `Invalid response code = 0xff` (OCS success), `ufshci_ctrlr_fail`: no
  da0, so the rc cannot import the pool on userdata. CAP 0x1587031F (64-bit
  addressing). Next: what the UTRD and the UCD hold after that query (a
  DDR dump of a run that stops there), and what Linux's shutdown at kexec
  still does to the controller after 0004.
- `goodix0: unable to allocate the interrupt`, `aw862240: chip id 0000`.
- The RAM console: the reset flips bits in it, some into a NUL;
  `ramconsole.py` now reads to where the text ends and clears the whole
  console (before, the first NUL, 207 bytes in, ended the output).

### 12.5 UFS works (2026-11-01 09:58-10:10)

The diagnostic (freebsd/patches/0043, run fbsd-G1-8-ufsdiag) showed the DMA
was fine all along: UTRL at 0x1021da000, UCD at 0x102200000, the UTRD's OCS 0
and a QUERY RESPONSE UPIU in the right place, for WRITE ATTRIBUTE IDN 0x0a
(bRefClkFreq), with a failed query response code (0xfd/0xff). The Samsung
KM8F8001JM-B813 (UFS 2.1) refuses that write, and ufshci gave up the whole
controller. 0044: read it first, write only when it differs, go on when
refused (as Linux). Run fbsd-G1-9-refclk: UFS device spec 2.1, 6 LUNs,
256 GB, da0-da5 attached, GEOM sees them.

Open: the console of run 9 ends in the middle of the CAM probe (`pass5 at
ufshci`), and the phone restarted at once (PMIC: power-off reason PS_HOLD,
the SoC reset itself, no crash dump). Run 10 keeps the raw RAM console
before it is cleared. Also: the FreeBSD keymap on the Pro1X has no `|`; the
RAM root has no termcap.

### 12.6 The root image, the menu's watchdog (2026-11-01 18:23-19:00)

- The RAM root's rc looked for `/dev/gpt/userdata` before the UFS disks
  were tasted (`GEOM: new disk da5` came after `rc done`) and rebooted:
  it waits up to 30 s for the label now, and writes `dmesg -a` (with what
  rc printed) to `/dev/pro1xshot` before every reboot and reroot (1bdd280).
  Run fbsd-startx-1: `Trying to mount root from zfs:pro1x/ROOT/default`,
  the login on the console.
- X does not come up: all three desktops end in "could not connect to X
  server: connection timed out", and there is no `/var/log/Xorg.0.log`.
  `tools/devday/fbsd-diag.sh` (on userdata as `freebsd/diag.sh`, run as
  `sh /dev/pro1x-host/freebsd/diag.sh`) keeps what it needs in the shot
  buffer and brings the USB gadget up. Run fbsd-diag-2: Xorg's own
  "Cannot open log file /var/log/Xorg.0.log", with /var a writable tmpfs.
  Every package file in the root image was owned by uid 65534: the
  packages' tarballs carry that number next to uname root, and makefs
  took the number. So the setuid Xorg.wrap ran Xorg as nobody (the same
  for dbus-daemon-launch-helper). rootfs.sh drops the numbers where a name
  is given (463a3e1); root.zfs of 2026-11-01 evening has it. Until then,
  as root on the console, `startx hildon -- /usr/local/libexec/Xorg`
  goes past the wrapper.
- The gadget in that run: dwc3dci attached and said `connected (run)`,
  usbus0 had its CDC Ethernet device, but the PC could not enumerate it
  (`device not accepting address, error -62`, `device descriptor
  read/64, error -110` then `-71`, `unable to enumerate USB device`), so
  no ue0 after 30 s. The device controller does not finish SET_ADDRESS /
  the first control transfers on the phone (pm/README.md stage 4 was
  tested in qemu only). Open.
- The gadget's `kldload` failed: uether depends on miibus, which the
  kernel lacks. build.sh builds `mii` now, pro1x_usbnet loads it first
  (d8ae214); until the next root.zfs, `miibus.ko` is in
  `freebsd/modules/` on userdata.
- Run fbsd-diag-1: the same code that reached the login at 18:23 was
  bitten at 18:42 in bufinit (900e; `fbsd-diag-1-ramdump`: text and
  rodata intact, the console ends cleanly after `avail memory`). The menu
  arms the APSS watchdog for the jump (32 s, the hardware's longest), and
  qcom_wdt attaches after about 1200 console lines, each drawn on the
  framebuffer synchronously while cold, at the CPU clock Linux left.
  patches/0045: the RAM console feeds the watchdog on every line while
  cold; `debug.verbose_sysinit=0` in pro1x.env drops about 900 of those
  lines (`debug.verbose_sysinit=1` in freebsd/cmdline brings them back).
  pro1x.env is built into the kernel by config, so a change to it needs a
  build without `KERNFAST` (`NOCLEAN=1` is enough). Later, perhaps: the
  CPUs at their highest clock in pro1x-loop-root before `kexec -e` (a
  boot_b of its own).
- That was not the cause. Run fbsd-diag-4 (20:15, kernel ebee541a, which
  had reached the login twice) was bitten at the same place with only
  2.3 KB of console (verbose sysinit off): a hang, not slowness. In both
  dumps the saved frames are bufinit -> bq_insert -> __mtx_lock_sleep,
  bqempty's lock owned by thread0, bq_len 10624 and 8736 of 50513: the
  loop stops 4-5 MB into the buffer headers, which start at PA
  0x1_0124_2000, the first large use of memory above 4 GB. The memory
  maps of good and bad runs are the same, so the headers are at the same
  place every time. `fbsd-boottest.sh physmem2500 6 'pro1x.root=ram
  hw.physmem=2500M'`: 6 of 6 starts past bufinit (without the limit 2 of
  6 hung today). `hw.physmem=2500M` (all below 4 GB) stays in
  freebsd/cmdline for now. Open: what in the hand-over makes stores above
  4 GB stall (Linux uses all 8 GB); the next bitten run is dumped with
  DDRCS0_1 too (fbsd-boottest.sh does).
- X itself: FreeBSD's scfb 0.0.7_2 calls xf86DisableRandR(), which
  xorg-server 21.1.25 no longer has, once Option "Rotate" is set: Xorg
  died with ld-elf's "Undefined symbol", the console left dark and deaf
  (run fbsd-diag-3). freebsd/scfb/ builds it without the call in the VM
  (8244dff), rootfs.sh puts it over the package's. With it (from
  userdata, `-modulepath`) X and the desktop came up on the phone.
- devd never ran: `/usr/local/etc/devd/pro1x_typec.conf` line 8 had an
  escaped quote inside the action, which devd's lexer does not know
  (83b83ed). Without devd no power key and no input hotplug for X.
- `startx hildon` gave Xfce: startx runs xterm instead of `~/.xinitrc`
  when it gets client arguments, so `~/.xinitrc` saw none and fell back
  to Xfce. `pro1x.desktop=` in freebsd/cmdline (or kenv) picks it.
- The touchscreen attached (GT9286) without its interrupt, the irq-gpios
  mapping did not resolve: patches/0046 maps interrupts-extended's cells
  as pro1x_kbd does (c7270b6). Not yet tried on the phone.
- `tools/devday/fbsd-x.sh` (userdata `freebsd/x.sh`): devd (from a
  mended copy while the image has the old rule), the desktop through
  kenv, X with userdata's modules; its log and Xorg's in the shot buffer.
- Leste, separately: 47 s into a boot the ADSP's handover interrupt fired
  every 240 ms ("Handover signaled, but it already happened"), and about
  three minutes later dsme's watchdog bit (pretimeout at 188 s, 900e)
  during a 3 GB copy; the next boot did the same until
  `echo stop > /sys/class/remoteproc/remoteproc1/state`. Open.

### 12.7 Hildon on the phone (2026-11-01 21:30 - 11-02 10:00)

- The touchscreen sits on gpioiic (GPIOs 6/7), and neither iicbb nor
  gpioiic passed resources up: its interrupt could not be allocated
  (patches/0049; 0046 and 0047 were steps on the way, 0047 also polls at
  60 Hz when there is no interrupt).
- 0041's pcpu checks asked cpulist, so no CPU had its frequency driver
  and powerd did not run (patches/0048): now 300-1804 MHz, powerd up.
- `kern.smp.disabled=0` in freebsd/cmdline: all eight CPUs, no problem
  seen so far (with `hw.physmem=2500M`).
- Touch direction: tools/devday/fbsd-touchcal.sh read the raw corners
  (raw y along the screen's width, raw x against its height, the
  image's matrix was right), tools/devday/fbsd-ptrlog showed X's pointer
  at the raw position anyway: X's TransformationMatrix was read but did
  not apply; libinput's CalibrationMatrix does. On the way: devd's
  moused.conf ran a moused for every input/eventN, which moved the
  pointer a second time through sysmouse (rc.conf
  `moused_nondefault_enable="NO"`).
- Hildon programs died on hildon_gtk_widget_set_theme_size: the image had
  FreeBSD's GTK, not the stage's Maemo GTK. cpio -p kept the packages'
  newer files (now -u), and mkspec.py took the packages' mtree link
  target for libgtk-x11-2.0.so.0 (now type and link come from the tree).
  Root image 6c3b15af: Terminal and Music run, the touchscreen lands
  where it is touched.
- Open on the phone: gPodder and Pure Maps do not start, MeeGo Maps
  shows an empty map (no network yet), scrolling lags (hildon-desktop
  composites with llvmpipe, scfb rotates every frame in software,
  tearing), the camera has no /dev/camss0 (qcom_camss not loaded), the
  vt console is not rotated, the USB gadget does not enumerate, the hang
  above 4 GB without `hw.physmem`.

### 12.8 What ends the sessions, the DSP ladder, the speed (2026-11-02 13:00-14:30)

- **No hangs, no crashes of the system:** all eight FreeBSD runs of the
  day end in an orderly shutdown (`Waiting for ... vnlru`, `syncer`,
  `Syncing disks ... done`); the RAM console only loses its last few
  hundred bytes at the reset, which looked like a hang. X never ended
  before (no final dump in any shot). In the image only devd's
  `pro1x-powerkey` asks for a shutdown: `shutdown -p` 2 s after a PRESS
  without RELEASE; with the cable in, the powered-off phone starts again,
  into the menu's default, Leste. hildon-home's and PulseAudio's signal 6
  come during rc.shutdown's kill, not before. Since patches/0050 and 0051
  the RAM console has the power key's edges with the time since boot,
  `pro1x: powering off|rebooting` at the start of the way down, and what
  programs write to `/dev/console`; a release edge that never comes is
  seen by a 250 ms poll, and `dev.qcom_pon.0.power_key` is the key now
  (the root image's `pro1x-powerkey` checks it before powering off from
  the next image on).
- **The DSP ladder, first run on the phone** (runs `fbsd-dsp-*` in
  `~/ps/pro1x-runs/2026-11-02/`, `hw.physmem=2500M` added): dsp1 `load`
  and dsp7a `rpm` pass (SCM, SMEM 0x46000000 with hosts 1, 2, 5, three
  SMP2P edges, rmtfs's region shared; l8, l16, l17, l23 voted on and
  off, nothing refused); dsp6 `modem`: tqftpserv, pd-mapper and rmtfs
  listed, the modem ready over SMP2P after 261 ms, its GLINK edge and
  QRTR up, DMS answers `qmicli --dms-get-revision`. The only failures:
  TrustZone answers "not supported" to the PAS query and starts the DSPs
  all the same (Linux never asks; now only logged), and the SoC id is
  444, Linux's `QCOM_ID_SM6115` (417 in the test was wrong). dsp7b
  `wifi`: WLFW (service 69) listed, `ath10k_snoc` loads, its probe fails
  at the first 8 KiB copy-engine ring, `failed to alloc CE dest ring 1:
  -12`: LinuxKPI treats the non-coherent WCN3990 core like a coherent
  PCI device, bus_dma bounces unaligned buffers through two pages and
  gives aligned ones no cache maintenance. patches/0052: Linux's DMA for
  devices without `dma-coherent` (uncached coherent memory, physically
  addressed streaming buffers, clean/invalidate by direction).
  **With it (14:03-14:11) the Wi-Fi works:** ath10k0 within 4 s
  (firmware WCN3990 hw1.0 api 5, board-2.bin, the rails l8/l16/l17/l23
  on), a scan finds the networks around, wpa_supplicant joins the
  network of userdata's wpa_supplicant.conf (WPA2-PSK, CCMP, keys in the
  firmware), dhclient gets a lease, the gateway answers ping: dsp7c 25
  passed, 0 failed but the PAS query. Left: the firmware has no MAC
  address (`invalid MAC address; choosing random`, a new one each boot),
  rc's netif tries wlan0 before the modem runs (`SIOCIFCREATE2`, harmless)
  and the first wpa_supplicant (devd, at ath10k's attach) fails before
  wlan0 is up; no Hildon UI to choose a network (no icd2 on FreeBSD).
  For the desktop: `pro1x.wifi=on` in freebsd/cmdline, fbsd-x.sh starts
  `fbsd-wifi.sh` beside X.
- **The speed:** a whole-screen rotation (2160 x 1080, the shadow to the
  framebuffer) measured on the phone's cores (Leste, `freebsd/scfb/test`):

  | | A73 (CPU 4-7) | A53 (CPU 0-3) |
  |---|---|---|
  | xorg's shadowUpdateRotatePacked (column walk) | 14 ms | 76 ms |
  | scfb_rotcw.h (blocks, NEON) | 16 ms | 37 ms |
  | Scale 2 (1080 x 540, each pixel 2 x 2) | 9 ms | 13 ms |

  X's one thread on an A53 costs it five times; FreeBSD's scheduler does
  not tell the clusters apart, so `fbsd-x.sh` puts Xorg on CPUs 4-7.
  `x.sh ... half` runs X at 1080 x 540 (scfb `Option "Scale" "2"`,
  Hildon unscaled through a `PRO1X_DISPLAY_CONF` in /tmp): a quarter of
  the pixels for X, the compositor's llvmpipe and every copy between
  them. `fbsd-x.sh` samples `top -H` every 6 s into the shot (both
  sessions of a boot), to see which threads the CPU goes to.

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

## Block 17: the MeeGo session's problems of 22:30 (should; 40-60 min) [OK] [HAND]

The user's findings in the MeeGo session (snapshot:
`~/ps/pro1x-runs/2026-10-31/meego-issues/collect.txt`). Causes and fixes
are in the commits 4a1ed33, 160ed35, 28461f4, 4a42630 and b42a173 and in
`meego/README.md`; everything here is untested on the phone. All steps run
in the **MeeGo boot**, as root, on MeeGo's own copy
(`/host/meego-leste.img`) and the shared `/host/harmattan.img`; nothing
changes in Leste's image except where step 8 says so.

| # | Finding | Cause | Fix |
|---|---|---|---|
| 1 | Bluetooth cannot be switched off | Harmattan's bluetoothd (bluez 4.91, maemo6 plugin) powers the adapter as mce's Bluetooth radio bit says; our radiostates module said "on" for every radio and dropped the switch's request. The status menu's row and the settings applet are Nokia's (conn-dui-settings-bt), not harmattan-qt5's. Not seen: why the switch did not stick (bluetoothd's stored mode was last written on 10-25) | mce +pro1x6: the bit off at the start, as on Leste, kept and signalled; the bus policy for bluez 4's agent calls (`meego/bluez4/`) |
| 2 | WLAN works, no icon; Firefox offline | sysuid's icon reads contextkit's `Internet.*`, which Harmattan's own icd2 provided (`com.nokia.icd2.context`); Leste's icd2 has none. Firefox: the Wi-Fi was really gone 22:24:52-22:33:46, and Firefox stays offline | `meego/icd2-context/`; Firefox's `network.manage-offline-status` false. Open: the status menu's row (IAP ids differ between Leste's and Harmattan's GConf) |
| 3 | Ö1 stutters | not found: same PulseAudio set-up as Leste; at the same time the Wi-Fi dropped (signal about -71 dBm, auth time-outs for 9 minutes) and Firefox started | `pro1x-wlan-powersave` now honours `/etc/pro1x-wlan-powersave-off`; test recipe in step 7 |
| 4 | Tearing | Xorg 21.1.7's rotation shadow: one buffer, redrawn without vblank; no page flips on a rotated CRTC; no TearFree in 21.1 | none yet: TearFree backport (16 xserver-master commits) applies to 21.1.7, not built (`meego/README.md`) |
| 5 | No sneak peek | mce +pro1x5 not installed; `60-pro1x.ini`'s `ModulesUser=radiostates` replaced 50-pro1x.ini's modules (no proximity, LEDs, keyboard light, vibration); nothing opened sysuid's low power mode view; double tap needs kernel 0033 | mce +pro1x6 with `harmattan-lpm`; `39-leste-mce.sh BUILD=0` writes the full list |
| 6 | No USB network for 10 minutes | the PC saw no device on the port until 22:28:52 and the phone's charger no input until t=556 s, while g_cdc and usb0 were up at t=21 s: the cable or VBUS, not the boot | none; ask whether the cable was replugged then. Check: step 6 |
| 7 | Brightness not settable | the slider writes Harmattan's GConf (its own gconfd on the system bus); Leste's mce reads Leste's | `meego/compat/harmattan-compat.c` passes the level to mce. Open: the display time-out setting has the same split |
| 8 | UI turned 180° | 3de3559 applied iio-sensor-proxy's mount matrix in n9-sensors; sensord wants libssc's frame | the flip removed (`meego/sensors/n9-sensors.c`) |
| 9 | "Only 8 GB" | RAM 7.5 GB is right; storage: MyDocs is a directory in harmattan.img (7.8 GB) | `/host/MyDocs`, when it exists, bound over MyDocs (step 5) |
| 10 | NFS Shift incomplete | 758 of its 1188 images are PVRTC; Mesa/freedreno has no PVRTC: every upload `GL_INVALID_ENUM` | `meego/pvrtc/`: decoding GLES libraries, the game started through `pvrtc-run` |
| 11 | Copies at the apps' edges | not found: every MeeGo Touch window is 960x480 (target `custom`, autodetect), a root capture of the home screen is clean | none; captures in step 7 |

1. PC, in the repository (pull first): the files and the package.
   ```sh
   git archive HEAD meego rootfs-overlay/usr/local/sbin/pro1x-wlan-powersave |
     bash $D/phone.sh ssh meego 'mkdir -p /root/meego-1031 && tar -C /root/meego-1031 -x'
   DEB=$WORK/out/mce-pro1x6/mce_1.10.17+pro1x6_arm64.deb   # or from leste-hildon-2026.10.31.2
   bash $D/phone.sh ssh meego 'cat > /root/meego-1031/mce_1.10.17+pro1x6_arm64.deb' < $DEB
   sha256sum $DEB; bash $D/phone.sh ssh meego 'sha256sum /root/meego-1031/*.deb'
   ```
   The two sums must be equal (3b5c6ac7...c274).
2. [OK] Stop the session (the screen goes dark; Leste's desktop does not
   start in the MeeGo boot): `rc-service pro1x-meego stop`.
3. mce +pro1x6 and its settings, with services blocked (a restarted mce
   leaves the display off):
   ```sh
   cd /root/meego-1031
   [ -e /usr/sbin/policy-rc.d ] || { printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d; chmod 755 /usr/sbin/policy-rc.d; G=1; }
   apt install --allow-change-held-packages ./mce_1.10.17+pro1x6_arm64.deb
   [ -z "${G:-}" ] || rm /usr/sbin/policy-rc.d
   apt-mark hold mce
   cat /etc/hostname                     # must say meego
   BUILD=0 bash meego/39-leste-mce.sh    # 60-pro1x.ini: 50's list;radiostates;harmattan-lpm
   ```
4. First, for item 8: `stat -c %y /usr/lib/pro1x-meego/n9-sensors`. The
   fix removes the axis flip of 3de3559 (2026-10-31 09:19); a binary
   older than that never had the flip, and then the 180 degrees have
   another cause: skip `41` (or put the flip back in n9-sensors.c) and
   tell the agents. Then the helpers and Harmattan's libraries (38 needs
   the network once, for the Debian tree's `libgles-dev`):
   ```sh
   H=/mnt/harmattan; mountpoint -q $H || mount -o loop /host/harmattan.img $H
   H=$H bash meego/41-leste-helpers.sh      # n9-sensors (rotation), icd2-context,
                                            # bus policies, Firefox's pref
   D=/host/debian-armhf H=$H bash meego/38-hardware-adaptation.sh   # brightness, PVRTC
   install -m 755 meego/40-harmattan-session.sh /usr/lib/pro1x-meego/harmattan-session
   [ ! -e /usr/local/sbin/pro1x-wlan-powersave ] ||
     install -m 755 rootfs-overlay/usr/local/sbin/pro1x-wlan-powersave /usr/local/sbin/
   ```
5. [OPT] [OK] MyDocs on /host (copies MyDocs out of the image, at most
   its 2.6 GB; the image's copy stays as it is):
   `du -sh $H/home/user/MyDocs`, then
   `mkdir /host/MyDocs && cp -a $H/home/user/MyDocs/. /host/MyDocs/ && chown 1000:1000 /host/MyDocs`.
   The way back: `rm -r /host/MyDocs` after copying anything new back.
6. [OK] Reboot into MeeGo (`pro1x-boot next meego`, reboot): the new mce
   reads its modules only at its start. [HAND] Item 6: the cable in from
   the start and not touched. `bash $D/phone.sh wait 3`: answering within
   about a minute means the 22:20 boot's ten minutes were the cable. If
   not, once it answers: `dmesg | grep -i "input in"` (the charger saw
   USB at t=20 s in Leste, at t=556 s in the MeeGo boot of 22:20).
7. Tests [HAND], then `collect.sh 17-meego-fixes`:
   - Item 8: landscape with the keyboard towards you upright, both
     portraits the right way round.
   - Item 1: Bluetooth off after the start (`hciconfig hci0`: DOWN;
     `bluetoothd.log` without "Network is down", since the session
     switches the adapter on before bluez 4 starts);
     Settings > Bluetooth on, then off: `hciconfig` follows,
     `dbus-send --system --print-reply --dest=com.nokia.mce /com/nokia/mce/request com.nokia.mce.request.get_radio_states`
     has bit 8 set while on (63 instead of 55). If the switch still
     springs back: `dbus-monitor --system "interface='org.bluez.Adapter'"`
     while switching, and `cat /mnt/harmattan/var/lib/bluetooth/*/config`.
   - Item 2: the WLAN icon in the status bar while connected;
     `dbus-send --system --print-reply --dest=com.nokia.icd2.context /org/maemo/contextkit/Internet/NetworkState org.maemo.contextkit.Property.Get`
     says `connected`. Firefox after a Wi-Fi drop and reconnect: a reload
     loads.
   - Item 7: Settings > Display, the slider: `cat /sys/class/backlight/5e94000.dsi.0/brightness` follows.
   - Item 5: let the display time out: dimmed, then the stand-by screen
     (sysuid's: time, notifications) at 20 %, after a minute black;
     covered for 5 s black. Power key: the lock screen. The double tap
     only with kernel pro1x.11 or later (0033) in this boot: `uname -r`.
   - Item 10: NFS Shift from the grid: cars and tracks textured.
     `grep pvrtc /mnt/harmattan/var/log/pro1x-session/*.log` or start it by
     hand with `PVRTC_TRACE=1` (`meego/pvrtc/pvrtc-run`): "over libGLES..."
     and "N levels decoded". Neither line: the game opens GL by path.
   - Item 9: Settings > Storage shows /host's size (if step 5 was done).
   - Item 3: Ö1 for 5 minutes, Firefox closed; every 5 s
     `iw dev wlan0 station dump` (signal, tx retries) and, as user,
     `pacmd list-sink-inputs`. If it stutters: [OK]
     `iw dev wlan0 set power_save off` and again.
   - Item 11: with an app that shows the copies in front:
     `su user -c "DISPLAY=:0 XAUTHORITY=/home/user/.Xauthority xwd -root -silent" > /tmp/root.xwd`
     and the same with `xwd -id <id>` of its window
     (`xwininfo -root -tree`): copies in the window's own image are the
     application's or EGL's, only in the root's mcompositor's or X's.
8. Leste's own image, at the next Leste boot: before 45 made MeeGo's
   copy, both shared one image, so Leste may have the same
   `60-pro1x.ini`. `cat /etc/mce/mce.ini.d/60-pro1x.ini`; if its
   `ModulesUser` is `radiostates` only, `BUILD=0 bash meego/39-leste-mce.sh`
   there too (the hostname is not meego: no harmattan-lpm), then the next
   reboot. mce +pro1x6 for Leste through `pro1x-update` (same as +pro1x5
   there).
9. The way back: mce +pro1x5 (leste-hildon-2026.10.31.1) as in 6b with
   `--allow-downgrades`, and `60-pro1x.ini` with `ModulesUser=radiostates`
   again; the other parts by running steps 1 and 4 with
   `git archive 4a1ed33^` instead of HEAD (before these fixes),
   then a reboot. `/host/MyDocs` is used only while it exists.

## Block 18: pro1x.12's crash, the bisect by kexec (must; 15-20 min per run) [OK]

**What happened (22:56-23:05):** 7.1.0-pro1x.12 on trial; all three starts
the same: Leste came up (USB gadget), `Bluetooth: ... command tx timeout`
on the screen, red LED, no ssh, about 31 s later the crash-dump mode. The
dumps (`ramdump-2300`, `-2303`, `-2305`), decoded in `docs/crash-dumps.md`:
the trial's APSS watchdog bit (TZ's `NSWDogBite`), TZ's own watchdog
barked, the RPM's bit, and both NoCs logged an error at 0x06080040 (the
modem's register block); TZ started its crash handling 46.3-47.0 s after
power-on, 25 s after Linux had started the DSPs. The pstore was lost
(Sailfish in between). Ranked suspects there: the Bluetooth UART set
(0023, 0024, 0029, 0030), then SLIMbus (0018-0021), then 0038/0039.

**State:** boot_b = pro1x.5 (`fe9798b9...`), slot b, no trial, Leste.

**The test:** `tools/devday/kexec-test.sh` starts a kernel once from the
running Leste: no flash, no trial, boot_b untouched. A hang ends in the
watchdog's reset (into boot_b's pro1x.5) or in 900e, which `after` OK=yes
leaves with the dump; the next Leste has the candidate's console in the
pstore. Candidates (built 11-01): `pro1x.12nobt` (without 0023/0024/0029/
0030), `pro1x.12no29` (without 0029), `pro1x.12nobtslim` (nobt and without
0021). Never boot Sailfish in between (it clears the pstore).

Per run, `K` = the kernel's name, with `D`, `WORK`, `PHONE` as above:

1. `PHONE="$PHONE" WORK=$WORK OUT=$WORK/out-$K bash scripts/65-kernel-update.sh prepare`
   (modules, initramfs; writes no partition; not for pro1x.5 or pro1x.12,
   they are prepared).
2. `bash $D/kexec-test.sh check $K`: no FAIL.
3. `bash $D/kexec-test.sh load $K` (nothing restarts; `unload` takes it back).
   The table's EXTRA and NOSERIAL go in front of check and load: the
   command line is fixed at load.
4. **[OK]** `OK=yes bash $D/kexec-test.sh go $K`
5. **[OK] for its way back** `OK=yes bash $D/kexec-test.sh after $K`:
   waits; up: watches it for 5 minutes, then collects (`kexec-$K-up`);
   900e: takes the dump (resets the phone); back on pro1x.5: collects
   (`kexec-$K`). Then `grep -n 'pretimeout\|stall\|Bluetooth\|remoteproc\|Call trace' ~/ps/pro1x-runs/2026-10-31/kexec-$K*/pstore.txt`.

| Run | K | EXTRA | Stays up | Goes down |
|---|---|---|---|---|
| 0 | pro1x.5 | `ignore_loglevel initcall_debug`, and `NOSERIAL=1` | kexec works: run 1 | as on 10-28 (0.23 s): the pstore names the initcall; stop here (then only a flash with trial is left) |
| 1 | pro1x.12 | (default) | not reproduced under kexec: run 2 anyway | its pstore: what Linux did before the bite |
| 2 | pro1x.12nobt | | the Bluetooth set: run 3 | not that set alone: run 4 |
| 3 | pro1x.12no29 | | 0029 | 0023/0024/0030 |
| 4 | pro1x.12nobtslim | | SLIMbus (0021) | neither: 0038/0039 next |

Stop as soon as a pstore names the cause. A kernel that runs under kexec
still gets its own trial before it replaces pro1x.5 in boot_b. FreeBSD
(block 12) waits for that kernel.

### 18.1 Run 0's result and the probe instead (11-01, 00:00-00:30)

Run 0 (pro1x.5 into itself) stopped about 0.41 s into the new kernel, and
its pstore had 2498 corrected and 158 unrecoverable ECC blocks: something
(the DSPs, still running) wrote into the RAM under the new kernel. kexec
from a running Leste is no test here; runs 1-4 go by flash instead, with a
way back that needs no hands while the PC is on the cable: `tools/devday/probe.sh` and the phone's
`pro1x-probe` service (`docs/crash-dumps.md`, "Probing a kernel").

1. Once: `bash $D/probe.sh install` (OpenRC sysinit in Leste).
2. `prepare` as above (not for pro1x.12), then `bash $D/probe.sh arm
   7.1.0-$K` (boot_b, still pro1x.5, saved as the image to put back).
3. **[OK]** `PHONE=... WORK=$WORK OUT=$WORK/out-$K bash scripts/65-kernel-update.sh flash`
   (trial, 3 tries), then reboot.
4. `DAY=2026-11-01 bash $D/probe.sh watch probe<N>-$K 7.1.0-$K 10`: 900e
   takes the dump and collects from the pro1x.5 that follows; up: 10
   minutes, then collects and reboots (into pro1x.5, boot_b is back).

Dry run 00:28 (armed for the running pro1x.5, rebooted): the service
started 15.5 s into the boot and had boot_b back (same bytes, cmp, trial
ended) at 19.6 s; pro1x.12's starts lasted about 31 s before the hang.
The one way it cannot cover: a candidate that hangs before 15 s takes the
trial's three tries and lands in Sailfish (LUKS code).

Order changed: pro1x.12 itself first (the pstore its earlier starts lost),
then `no29` before `nobt` (the dumps' NoC error at the modem block and the
18:56 modem fatal point at the WCN3950's shared power, which 0029 cuts).
mce +pro1x6 and the full ModulesUser list run from the dry run's boot on:
nothing else in Leste's user space changes between the runs.

**Result (runs 1-7, 00:30-01:37, `docs/crash-dumps.md` from "Run 1"):**
pro1x.12, nobt and nobtslim all went down at 41-42 s; a heartbeat in the
kernel log showed user space alive up to the watchdog's bark, and the
watchdog core sending no ping. The cause was the boot menu's trial
watchdog, which the kernel never fed (qcom_wdt bound before the arming),
racing dsme. Fixed in the initramfs (e3f60f1); nobtslim with it passed
run 7 and, at 01:45, a regular trial (confirmed by pro1x-boot-trial).
On the way: 0023/0024/0029/0030 break Bluetooth's setup, 0021's SLIMbus
NGD times out; neither caused the crash.

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
