# Alarms that switch the phone on

An alarm set in Maemo Leste's (or FreeBSD's) clock rings even when the
phone was switched off: the PMIC's RTC alarm powers the phone on, the boot
menu sees that and starts the system that set the alarm at once, without
the menu, and that system rings. It looks as on the Nokia N9 when an
alarm switched that on: a black screen with the time, the alarm's text,
Snooze and Stop. Once the alarm is over the phone switches itself off
again, as the N9 did ([below](#what-the-n9-did-when-an-alarm-switched-it-on)):
"Switching off" for 10 s, during which a tap, a key or the power key
keeps it on; a call, the charger or the phone being used keep it on too.
`after-alarm=ask` asks instead.

Status (2026-10-31): built and tested on the PC (below); the power-on
itself is untested on the phone. To be flashed with kernel pro1x.10;
`pro1x-alarmboot` 1.4 (the N9 look, off again as the N9; 1.4: the power
key told apart from a display time-out) in the pre-release
`leste-alarmboot-2026.10.31.2` (1.1 in `leste-alarmboot-2026.10.30.1`
asked instead); Arch: `arch-hildon-2026.10.31.1`.

## Is it possible on this hardware?

Yes, as far as the sources show; one register is to be checked on the
phone (step 1 of the test).

| Question | Answer | Source |
| --- | --- | --- |
| Which PMIC has the power-on (PON) block and the RTC? | PM6125, SPMI slave 0: PON at 0x800 (`qcom,pm8998-pon`, a GEN2 PON), RTC counter at 0x6000, RTC alarm at 0x6100 (`qcom,pm8941-rtc`) | `arch/arm64/boot/dts/qcom/pm6125.dtsi` (mainline 7.1); the vendor kernel's `pm6125.dtsi` (LineageOS `android_kernel_fxtec_sm6115`) has the same addresses |
| Is the RTC alarm a power-on source? | `PON_TRIGGER_EN` (0x880), bit 2 (`PON_RTC` in the vendor kernel's `enum pon_trigger_source`). Neither the vendor kernel (`qpnp_pon_trigger_config()` has no caller) nor mainline writes it: it is as the PMIC and the boot loaders leave it. Qualcomm's own RTC driver of the same PMIC generation (`qpnp-rtc.c`, sm8150) switches the alarm off at shutdown unless a power-off alarm is wanted (`qcom,qpnp-rtc-alarm-pwrup`, `poweron_alarm`): an alarm left on powers the device on, with nothing else configured. The Pro1X's vendor kernel uses `rtc-pm8xxx`, which leaves it on. | vendor sources; `pro1x-boot pmic` shows the bit on the phone ("power-on sources") |
| Does the alarm survive the power off? | Yes: the RTC is in the PMIC's always-on part (it keeps counting while off; `pro1x-rtc` relies on that). Linux's `rtc-pm8xxx` shutdown writes only an nvmem/UEFI offset (none here); the RTC core keeps a set wake alarm; Leste's `hwclock --systohc` at shutdown gets -ENODEV (no `allow-set-time`) and touches nothing. FreeBSD's `qcom_rtc` leaves it on too ("as Linux keeps it", `freebsd/pm/README.md`); FreeBSD's alarmd switches it off only when no alarm needs it (`patches/alarmd/0006`). | `drivers/rtc/rtc-pm8xxx.c`, `drivers/rtc/interface.c` |
| How is "powered on by the RTC" seen? | `PON_REASON1`, bit 2 (GEN2 at 0x8c0, GEN1 at 0x808); `WARM_RESET_REASON1` (0x8c2) 0 means a cold start. The vendor kernel reads them at probe, after XBL and ABL, and logs "Power-on reason: Triggered from RTC (RTC Alarm Expiry)": they are still valid then. ABL reads them too (`GetPonReason`) but clears only the reboot reason (`ClearResetReason`, the soft reset spare register), not these. | vendor `qpnp-power-on.c` (`PON_OFFSET`, `qpnp_pon_reason[]`); Qualcomm ABL `LinuxLoader.c` |
| What does ABL boot after an RTC power-on? | The active slot's boot partition, as for any power-on: boot_b, the menu. ABL's "alarm boot" (`androidboot.alarmboot=true`) is a different thing, a reboot reason (0x03) Android writes before a reboot. The only special case of a cold start is off-mode charging (`androidboot.mode=charger` when the charger caused it), which loads the same boot image; this kernel ignores `androidboot.*`. The start is one of the slot's tries, as any start (the trial below is not changed). | Qualcomm ABL (`QcomModulePkg`: `UpdateCmdLine.c` `TargetPauseForBatteryCharge`, `LinuxLoader.c`); the Pro1X's own ABL is F(x)tec's build of it, not public |

## What the N9 did when an alarm switched it on

In short: it rang in its "acting dead" mode on a black screen (a large
title, a line of text, Snooze, Stop), snoozed itself after a minute
without an answer, and after Stop, or after Snooze until the snoozed
time, switched itself off again. It asked nothing; to use the phone one
held the power key. Put together from Harmattan's open source packages
and from the N9's own firmware as the owner extracted it on the PC
(PR1.3, `DFL61_HARMATTAN_40.2012.21-3`, `~/n950/extract/n9-root`): its
data files only (start scripts, D-Bus service files, package lists,
style sheets, theme pictures, message catalogues read with `lconvert`),
as a reference; no Nokia program was taken apart, and nothing of the
firmware is in this repository or its packages.

1. **Switched on.** A clock alarm carries timed's boot flag: the device
   is powered on and booted into the acting-dead state 60 seconds before
   the trigger time if it is off (timed 2.54, `libtimed.3`,
   `setBootFlag()`). At boot `getbootstate` says `ACT_DEAD`, and
   `/etc/init/rc-default.conf` goes to runlevel 5.
2. **Acting dead.** `/etc/init/runlevel-switch.conf` marks the state
   (`/tmp/ACT_DEAD`) and starts the X session, which in that state
   (`xsession.conf`: `XSESSION_ACTDEAD_STARTING`) starts only the group
   `xsession/group-actd-daemons.conf`: D-Bus, timed, the ohm session
   agent, the vibra and key daemons, mthemedaemon, applauncherd in boot
   mode, mfeedbackd, ngfd (tones) and the charging animation
   (`act-dead-charging-indicator`, its style a black background), and
   neither the compositor nor the system UI nor the home screen.
3. **The ringing screen.** timed hands the alarm to its notification
   service, D-Bus `com.nokia.voland` (`usr/share/dbus-1/services/
   com.nokia.voland.service`: `/usr/bin/reminderservice`, package
   visualreminders), with the clock's plugin for it
   (`/usr/lib/voland/plugins/libclockalarm.so`, package
   clockreminderplugin). The clock's style sheet for that notification
   (`usr/share/themes/base/meegotouch/libclockwidgets/style/
   libclockwidgets.css`, `AlarmNotification`) gives, in the N9's pixels
   (0.1 mm, the theme's unit): a title in bold 64 (bottom margin 12), a
   text in light 32 (42 from the sides), a Snooze button and a Stop
   button, each 408 x 64 with a bold 32 white label, Snooze 40 below
   what is above it on the dialog's negative button picture
   (`meegotouch-dialog-button-negative`: black with a grey rim), Stop 36
   below on the positive one (dark blue with a lighter blue rim); the
   open base theme styles the clock's query dialog as
   `LargeButtonsQueryDialog`, "as specified in the Clock ALG", with its
   buttons one below the other (`mmessageboxstyle.css`). No compositor
   ran under it, so all around was black. The clock's catalogue
   (`usr/share/l10n/meegotouch/clock_*.qm`) has the button words
   (`qtn_cloc_alarm_snooze`, `qtn_cloc_alarm_stop`) and the name of an
   alarm without one (`qtn_cloc_alarm_title`); the German ones are what
   the N9's German catalogue has for them. When the screen was locked in
   normal use, the alarm showed on the lock screen instead (the plugin's
   `libclockalarm.css`: a large panel with the alarm's title in bold 48
   and a subtitle in light 26).
4. **Without an answer.** timed's dialogs snooze themselves after a
   minute unless the event says otherwise (`libtimed.3`,
   `suppressTimeoutSnooze()`; timed's own default snooze is 300 s, the
   clock's alarms bring theirs, 10 minutes as the clock sets them). The
   user guide (issue 1.2, "Snooze an alarm") adds that turning the phone
   face down snoozes it too (issue 1.0: that it mutes it).
5. **Off again, without a question.** dsme starts an acting-dead boot
   with a shutdown pending (dsme 0.62.3, `set_initial_state_bits()`:
   `ACT_DEAD` sets `shutdown_requested`) and stays in acting dead only
   while an alarm rings or is due within 2 minutes (`alarmtracker.c`,
   libdsme's `dsme_snooze_timeout_in_seconds()`, 120 s) or the charger
   is connected (`select_state()`). After Stop the N9 therefore switched
   itself off within seconds (`SHUTDOWN_TIMER_TIMEOUT`, 2 s); after
   Snooze too, and the RTC switched it on again a minute before the
   snoozed time. With the charger connected it stayed in acting dead,
   showing the charging animation (`shutdown.conf`: a USB charger at
   shutdown means a "minireboot" into `ACT_DEAD`; unplugged, dsme ends
   it after 15 s).
6. **Switched on by the power key only.** In acting dead mce asks dsme to
   power up after a press of its "medium" length (mce 1.11.20,
   `powerkey.c`: `MCE_STATE_ACTDEAD` → `request_powerup()`; the N9's
   `/etc/mce/mce.ini`: `PowerKeyMediumDelay=1000`, 1 s, with the
   power-on LED pattern meanwhile). dsme switches to runlevel 2,
   `runlevel-switch.conf` marks `USER`, stops the acting-dead session
   and starts the normal one with the boot progress indicator.
7. **No question anywhere.** No start script asks; none of the N9's 99
   English message catalogues has a "switch on?" text; the charging
   animation's style knows only battery icons. The user guide (issues
   1.0 and 1.2, and the copy on the phone, `/usr/share/userguide`)
   explains setting, snoozing and removing alarms, not the phone that is
   off. The owner's N9 clock screenshots (`~/ps/n9-clock-ref`, private)
   show the clock and the alarm editor, not the ringing screen. The N900
   asked: Fremantle's `osso-systemui-alarm` shows
   `cloc_fi_power_up_note_description` for 15 s after an alarm in acting
   dead (Leste's system UI has that plugin, but Leste here is never in
   acting dead).

Sources: Nokia N9 User Guide, issue 1.2
(<http://altehandys.de/downloads/man-no-n9-00.pdf>, p. 89) and issue 1.0
(<https://one.nz/MEDIA_CustomProductCatalog/m5670124_N9_UG_en.pdf>);
Harmattan PR1.2 open source packages timed 2.54+0m7, dsme 0.62.3+0m7,
libdsme 0.61.6+0m6, mce 1.11.20+0m7, meegotouchtheme 0.24.13-1+0m7 (base
theme: `mmessageboxstyle.css`, `commonlabelstyle.css`, `constants.ini`:
"32px" = 3.2 mm); the N9's firmware's data files named above; Leste's
`osso-systemui-alarm` 0.3.3 for the N900's question.

### What the Pro1X shows

The Pro1X starts its full system for an alarm (no acting-dead mode), and
the Hildon system UI rings it; `pro1x-alarmboot` puts the N9's screens on
top ([how](#how-it-works), step 5):

| Screen | Picture (PC, Xvfb, 2160 x 1080 at 403 dpi) |
| --- | --- |
| the alarm rings: time, text, Snooze (black), Stop (blue) | `alarmboot-ring-{en,de}-{landscape,portrait}.png` |
| after it (the default): "Switching off" / "Wird ausgeschaltet", 10 s to keep it on (a tap anywhere, Keep on) | `alarmboot-off-*.png` |
| `after-alarm=ask`: "Switch the phone off again?" / "Telefon wieder ausschalten?", Switch off, Keep on | `alarmboot-ask-*.png` |
| "Switching off" while it shuts down | `alarmboot-switching-*.png` |

The pictures are in the owner's `~/ps/pro1x-screens/` (`*-n9assets.png`
with the N9's font and button pictures installed); make them with
`tests/alarm-boot/screens.sh shots`.

- **Sizes:** the N9's pixels are 0.1 mm; at the Pro1X's 403 dpi one is
  1.59 screen pixels, so the alarm screen is as large in mm as on the N9.
  The dpi is X's (the screen's width in mm), `-d` otherwise; where that
  is unknown or absurd the N9's 480 pixels (its short side) take three
  quarters of the short side.
- **Landscape and portrait:** everything is centred in one column, as the
  N9 had it in either; the window follows the screen: when hildon-desktop
  has turned it to portrait (the keyboard closed, the phone upright, as
  with the MeeGo look) the screens are laid out in portrait.
- **Fonts and pictures:** Nokia Pure Text where installed (fontconfig),
  else Open Sans (Leste's `fonts-open-sans`), else Sans; the N9's
  `meegotouch-dialog-button-{negative,positive}[-pressed].png` from an
  installed blanco theme (`/usr/local/share/themes/blanco`, as the
  private meego-hildon-assets install it, or `/usr/share/themes/blanco`,
  or `-t`), else the buttons drawn alike (2 px rim, a gradient: black
  with a grey rim, dark blue with a blue rim, corner radius 19). Nothing
  of Nokia's is in the package.
- **Words:** English and German by the session's language (`LANG`),
  short labels of our own that say what the N9's clock and shutdown
  screen said (Snooze, Stop, Alarm, Switching off, and their German
  counterparts); the optional question is new (the N9 had none).
- **Order and contents:** the time as the title and the alarm's name as
  the text, Snooze above Stop: the style sheet gives the sizes and
  distances, not which label shows what nor the buttons' order, so these
  are ours.
- **Off again, as the N9** (the owner's choice for 1.2; 1.0 and 1.1
  asked, and kept the phone on without an answer): what the N9's dsme did
  by itself after an acting-dead alarm. The N9 kept the phone on only by
  its power key (held, it switched on fully) and in its charging state;
  here the phone stays on with a tap or a key on "Switching off", the
  power key (Linux's input devices with `KEY_POWER` where readable, and
  everywhere the display going off, to the stand-by screen or locking,
  which a press does: Leste's mce says "off", FreeBSD's `pro1x-standby`
  "lpm"; while these screens show no time-out can do that, since 1.4
  they keep the display on as a video player would: mce's
  `req_display_blanking_pause` every 30 s (mce's pause lasts 60 s), X's
  idle time reset every 2 s, which `pro1x-standby` times out by (its
  stand-by screen comes 3 s after dimming at the earliest); "dimmed" is no
  power key. From its start until the alarm's screen X's idle time alone,
  so that FreeBSD's stand-by screen, which stays above everything, cannot
  come before the alarm rings; mce's time-outs stay as they are, the
  alarm dialog asks mce for the display),
  during a call (mce's `get_call_state`, oFono's calls), with the charger
  connected, and when the phone was used: input before the alarm rang or
  after its end that was not on these screens (XInput 2's raw key, button
  and touch events: input from a device, not what only resets X's idle
  time). Input on the Hildon dialog while it rang counts
  as the alarm's. The old question: `after-alarm=ask` (or `-Q`).
- **The snooze's wake-up first:** with an alarm still to come (alarmd's
  queue: an `ACTDEAD` or `BOOT` alarm not switched off; MeeGo: timed's next
  boot-up event), it switches off only once the RTC's wake-up is set in
  the future (`/sys/class/rtc/rtc0/wakealarm` against `since_epoch`, the
  RTC's own time; FreeBSD `dev.qcom_rtc.0.wakealarm`). alarmd sets it at
  once when its queue changes, a snooze too (`server.c`: 60 s before the
  alarm, `POWERUP_COMPENSATION_SECS`); not set within 15 s, the phone
  stays on.
- **Unanswered:** the N9's dialog snoozed itself after a minute. Over the
  Hildon dialog the screen snoozes the alarm after 50 s (before the
  dialog's tone stops at 60 s and it stops listening), the same way as a
  tap, then switches off; its own ringing snoozes after a minute (three
  times, then it stops). In MeeGo timed snoozes by itself.
- **A look without an alarm:** `pro1x-alarmboot -D ring` (or `ask`,
  `off`, `switching`; `-L de`), in the session as its user; a tap prints
  0 (the upper button) or 1 and ends it.

## Which systems

The boot menu starts whichever system set the RTC alarm (the owner
detection needs nothing from the systems) and leaves the note
`/host/pro1x-boot/alarmboot` (FreeBSD: the kernel environment); what
rings then is each system's own.

| System | Alarm clock | Switched on by its alarm | N9-look screens | Tested |
| --- | --- | --- | --- | --- |
| Maemo Leste | clock-ui, meego-clock (alarmd, the system UI's alarm dialog) | yes: alarmd sets the RTC (`pro1x-rtc`, `librtc-realtime`) | yes: the alarm screen over the dialog, then off again as the N9 | PC (stub alarmd, test bus, Xvfb), the package built in the image's chroot; phone 2026-10-31 |
| MeeGo (N9 UI on Leste's copy) | the N9's clock (timed and its own alarm screen) | yes: timed through `librtc-realtime` | the N9's own ringing screen; then off again as the N9 ("Switching off" in the N9 look, `-T`) | PC (timed's signal on a test bus); phone open |
| FreeBSD | clock-ui, meego-clock (alarmd, the system UI) | yes: `qcom_rtc`'s wakealarm, `kenv pro1x.alarmboot` | yes, as Leste | PC; the FreeBSD VM against the real alarmd and system UI (`freebsd/vm/try-alarmboot.sh`: Stop, Snooze, three runs; 1.1 with the question, 1.2's switch-off not yet in the VM) |
| Arch | in the Hildon and MeeGo-look sessions (`hildon`, `meego`): clock-ui and meego-clock on alarmd and the system UI's alarm dialog, started by Arch's `hildon-session` since 2026-10-31; not in Phosh (GNOME Clocks sets no RTC alarm) nor in the N9 UI session (`harmattan`: no alarm service) | yes: alarmd as the user `alarm` sets the RTC through `librtc-realtime` (package `pro1x-alarmboot`), the menu starts Arch, which starts the desktop `pro1x-desktop` chose (the alarm rings only in a Hildon one) | yes, as Leste; switching off through logind (Arch has no dsme), calls through ModemManager, the power key kept from logind while switching off (it reaches X) | PC (the Arch build in a chroot of Arch Linux ARM; its alarmd, system UI and `pro1x-alarmboot` under qemu, see below); phone open |
| Kali | none installed | no | no | sources |
| LuneOS | its own clock (sleepd's alarms) | open: whether sleepd leaves an RTC alarm set when LuneOS is switched off; if it does, the menu starts LuneOS for it | no, by choice: LuneOS keeps its own alarm look (and has no switch-off question) | not yet (needs the phone: an alarm in LuneOS's clock, then `cat /sys/class/rtc/rtc0/wakealarm` before switching off) |
| KolibriOS | none (the ARM port has no alarm clock and no RTC alarm) | no | no | sources (smatkovi/kolibrios-arm) |
| Sailfish OS (slot a) | its own clock (timed, its own acting-dead handling) | its own: with slot a active ABL boots Sailfish, which handles its alarm itself; our menu (slot b) cannot hand an alarm start to it (a slot switch is a reboot: the RTC power-on reason is gone) | no (Sailfish is not changed) | not tested |

In the images built on the PC the parts are there by default:

- **Leste:** `scripts/40-make-rootfs.sh` runs `49-alarmboot.sh` after
  `49-hidpi.sh`: the `pro1x-alarmboot` package built by `mkdeb-tools.sh`
  (`ONLY=pro1x-alarmboot`, its build dependencies only) and installed
  with apt, so that `pro1x-update` upgrades it. `45-pro1x-hardware.sh`
  puts `pro1x-boot` and the boot menu, `46-sensor-stack.sh` `pro1x-rtc`
  in place.
- **MeeGo:** `meego/45-meego-image.sh` copies that Leste system, with the
  package and `Xsession.d/01pro1x-meego` (from `45-pro1x-hardware.sh`'s
  `rootfs-overlay`).
- **Arch:** `arch/42-meego-look.sh` builds `pro1x-alarmboot` with
  `librtc-realtime` and the files above (`ONLY=pro1x-alarmboot` for it
  alone), `arch/43-packages.sh` packages it (step 42's parts) and the
  sessions' `pro1x-sessions` with the new `hildon-session`.
- **FreeBSD:** `freebsd/hildon/build.sh` puts `pro1x-alarmboot` and
  `rc.d/pro1x_alarmboot` into the Hildon stage (package `pro1x-alarmboot`,
  in hildon-meta's list), `freebsd/rootfs.sh` copies the stage and
  `freebsd/hildon/rootfs` (`hildon-session`) into the image; with the
  private assets (`HARMATTAN=`) the N9's blanco theme and Nokia Pure go to
  `/usr/local/share/themes` and `/usr/local/share/fonts/nokia`, where
  `pro1x-alarmboot` finds them.

## How it works

1. **The alarm is set.** clock-ui's alarms have `ALARM_EVENT_ACTDEAD`;
   alarmd puts the RTC alarm 60 s before the next such alarm. On Leste
   through `/dev/rtc0` and `librtc-realtime` (`tools/pro1x-rtc`), on
   FreeBSD through `dev.qcom_rtc.0.wakealarm`.
2. **Power off.** Nothing to do. (Leste's `/etc/init.d/pro1x-rtc` used to
   set a one-shot `pro1x-boot next` for the running system at a shutdown
   with an alarm; that also sent starts by the power key there, and is
   gone.)
3. **The RTC powers the phone on.** XBL, ABL, boot_b's kernel, the
   initramfs.
4. **The boot menu** (`initramfs/local-top/pro1x-loop-root`) asks
   `pro1x-boot alarm-check` first. It reads the PMIC through its regmap in
   debugfs (`/sys/kernel/debug/regmap/0-00/registers`, a 9-byte line per
   register; no module and no kernel patch needed, the menu runs before
   `qcom-pon` and `rtc-pm8xxx` load; only reads): the PON subtype, the
   reasons, the power-on sources, the RTC counter and the alarm.
   - **Whose alarm?** The RTC has one alarm, and the system that set it
     owns it. `/host/pro1x-boot/rtc-alarm` notes the alarm's count at each
     start (`count owner [used]`). A count that changed since the
     previous start was set by the system that ran in between
     (`/host/pro1x-boot/current`, which the menu writes for every start,
     FreeBSD's and KolibriOS's too). So FreeBSD, which never writes
     userdata, needs nothing at its shutdown, and a system that only left
     another one's alarm alone does not take it over.
   - **The alarm's start:** powered on by the RTC (`PON_REASON1` bit 2),
     the counter at most 10 minutes past the alarm's count, the owner
     installed, this alarm not used for a start before. Then the owner
     starts at once (no menu, a snapshot owner `arch@N` as its snapshot),
     with the watchdog of a one-shot choice; a pending `pro1x-boot next`
     stays for the next start (unless it names the same system).
   - It notes `/host/pro1x-boot/alarmboot` (`system=`, `rtc_alarm=`,
     `rtc_now=`, `due=`: the alarm's real time, count + the shared RTC
     offset + 60 s), removed again at every other start. FreeBSD gets
     `pro1x.alarmboot=1 pro1x.alarmdue=<time>` in its kernel environment.
   - Anything else (power key, charger, reboot, PMIC unreadable): the menu
     as before. Every start logs one line to `/host/pro1x-boot/last-boot`,
     e.g. `alarm-check: none not-by-the-rtc owner=leste late=-3500 pon=80
     warm=00 poff=.. sources=86 rtc=... alarm=... on=1 last=leste prev=...`
     or `alarm-check: leste late=14 due=... pon=04 ...`.
5. **The system rings.**
   - **Leste:** alarmd and the system UI ring as always (a clock-ui alarm
     has `ALARM_EVENT_POSTPONE_DELAYED`: even one that comes late after a
     slow start rings, up to a day late): their tone, their snoozes, their
     queue. `pro1x-alarmboot` (package `pro1x-alarmboot`, started by
     `Xsession.d/02pro1x-alarmboot`, before the system UI, so that it sees
     the dialog of an alarm that is due already come up; what it does goes
     to `/tmp/pro1x-alarmboot.log`) watches the alarm dialog
     (`alarm_dialog_status` on the system bus) and shows the N9's screens
     ([what they are](#what-the-pro1x-shows)):
     - **While the system UI's alarm dialog rings** (status 5): the N9's
       alarm screen as a full-screen window over it (hildon-desktop's
       stacking layer 10, above the screen lock's 9), with the time and
       text of the alarm that rings (from alarmd). A tap answers through
       the system UI itself: mce's `sig_device_orientation_ind` with
       `face_up`, then `face_down` (the orientation as mce gives it, so
       nothing turns), which the ringing dialog takes as the phone turned
       over: it snoozes, stopping its tone as with its own button. For
       Stop, once that dialog has gone (status 7), alarmd gets the Stop
       button as the answer (`alarmd_ack_dialog`, the index of the
       event's Stop action): alarmd takes the last answer, so the snoozed
       alarm ends stopped (a repeating one moves to its next day, a single
       one is switched off), as when a snoozed alarm is stopped later.
       Should the dialog not have gone 3 s after a tap, the window steps
       aside for good and the Hildon dialog's own buttons are used. Should
       hildon-desktop not have given the window the screen's size within
       2 s (seen in the FreeBSD VM for a second program's window in the
       same session: no answer to its configure request, GTK's drawing
       area left at 1 x 1), it becomes an override-redirect window, which
       hildon-desktop shows blurred under the alarm dialog's dimming but
       which works the same. The
       dialog stops its tone after a minute and rings again every 5
       minutes (3 times); while it is silent (status 6) it no longer
       listens for the face-down signal, so the window is away then and
       the Hildon dialog shows; it comes back when the dialog rings again.
       (Answering alarmd directly while the dialog rings would be simpler
       but is not safe: alarmd closes the system UI's dialog only for a
       deleted alarm, and the system UI then drops the alarm's
       notification without closing it, so its repeating tone could go on;
       `alarm_remove()` in osso-systemui-alarm.)
       Unanswered for 50 s, the screen snoozes the alarm that way itself
       (the N9's dialog snoozed after a minute).
     - **Should no dialog come** within 2 minutes of the alarm's time
       while alarmd still has an alarm then (or does not answer), it rings
       by itself: the same N9 alarm screen and a tone through PulseAudio.
       Where alarmd has the alarm, Snooze and Stop go to alarmd as from
       its dialog (a snooze sets the RTC); without alarmd, Snooze rings
       again after 10 minutes and the phone stays on. Unanswered it
       snoozes after a minute (3 times, then it stops). Without a display
       it still rings (the tone alone) and keeps the phone on.
     - **Once the alarm is over** (the dialog gone 2 s, or its own
       ringing ended): "Switching off" for 10 s, then dsme's shutdown, as
       the N9 did ([what keeps it on](#what-the-pro1x-shows): a tap, a
       key, the power key, a call, the charger, the phone in use; an alarm
       still to come needs its RTC wake-up set first). A snoozed alarm
       switches the phone on again a minute before its time. Another alarm
       ringing meanwhile takes over. With `after-alarm=ask` in
       `/etc/pro1x-alarmboot.conf` (group `[pro1x-alarmboot]`, or `-Q`)
       it asks instead, as 1.1 did: "Switch the phone off again?", no
       answer in 60 s keeps the phone on (then its own ringing rings 5
       minutes and does not snooze by itself).
     - An alarm that was deleted after the RTC was set (Leste's alarmd
       leaves the RTC alarm then) does not ring; the phone switches off
       all the same.
   - **FreeBSD:** `rc.d/pro1x_alarmboot` (package `pro1x-alarmboot`)
     writes the note `/var/run/pro1x-alarmboot` and starts the Hildon
     session (`startx hildon`, as root, as on the console), which starts
     `pro1x-alarmboot` before the system UI (log
     `/tmp/pro1x-alarmboot.log`). The same screens; alarmd's queue is kept
     only on the writable pool (`pro1x.rootrw`), with the read-only root
     `pro1x-alarmboot` rings by itself after 2 minutes; switching off is
     `shutdown -p now`; the charger from `hw.acpi.acline`; the
     configuration file is
     `/usr/local/etc/pro1x-alarmboot.conf`.
   - **MeeGo** (the N9's user interface on Leste's copy, system key
     `meego`): the N9's own timed rings with the N9's own alarm screen (its
     reminder service, from the N950 image under `/host/harmattan.img`);
     timed's wake alarms reach the RTC through `librtc-realtime` too
     (`meego/38-hardware-adaptation.sh`). Nothing is put over that screen.
     `pro1x-alarmboot` (started by `Xsession.d/01pro1x-meego`, which ends
     Leste's X session there before `02pro1x-alarmboot`; log
     `/tmp/pro1x-alarmboot.log`) runs with `-T`: timed's signal
     `com.nokia.time.next_bootup_event` is 1 while a boot-up alarm is in
     its dialog (timed's `machine_t::calculate_bootup()`); once it is
     something else (stopped or snoozed: the next boot-up alarm's time,
     or 0) it switches off 2 s later as above, the RTC wake-up first if
     that is a time to come. timed snoozes an unanswered alarm itself. Its
     own ringing should no such signal come in 2 minutes (it does not ask
     Leste's alarmd there).
   - **Arch** (the `hildon` and `meego` sessions): as Leste. Arch's
     `hildon-session` starts `pro1x-alarmboot` (log
     `/tmp/pro1x-alarmboot.log`), then alarmd (with
     `LD_PRELOAD=/usr/lib/pro1x/librtc-realtime.so`: the RTC keeps no real
     time) and the system UI, as the session's user `alarm`; the package
     `pro1x-alarmboot` brings what that needs: the D-Bus policy for alarmd
     (`/usr/share/dbus-1/system.d/pro1x-alarm.conf`: Arch's alarmd package
     has Leste's below `/usr/etc`, which the bus does not read, and for
     the user `user`), alarmd's queue directory (`/var/cache/alarmd`,
     tmpfiles.d) and rtc0 for the group `alarm` (udev). Switching off
     goes through logind's `PowerOff` (no dsme); `hildon-session` tells
     alarmd that the desktop is up (`com.nokia.HildonDesktop.ready`, as
     Leste's `Xsession.post/21hildon-desktop-wait`, and `init_done` as its
     `99initdone`: alarmd shows no alarm
     before, its "limbo"); while switching off the
     power key is kept from logind (an inhibitor), so it reaches X and our
     window keeps the phone on; calls are ModemManager's. The alarm rings
     only when the desktop that comes up is Hildon's (`pro1x-desktop
     hildon` or `meego`).
   - **The others:** [below](#which-systems).

Kernel patch 0028 (`device/kernel/patches/0028-power-reset-qcom-pon-...`,
in pro1x.10) adds what the running systems may want: `qcom-pon` logs the
reasons at boot (`dmesg | grep "power-on reason"`) and shows them in
`/sys/bus/platform/drivers/qcom-pon/*/` as `pon_reason`,
`warm_reset_reason`, `poff_reason`; `rtc_power_on` reads the RTC bit of
`PON_TRIGGER_EN`, and writing 1 or 0 changes only that bit (a
read-modify-write; no power key, reset, S2 or PS_HOLD setting). The boot
menu does not need it.

`pro1x-boot pmic` (root, any Linux system with debugfs) prints all of it:

```
power-on reason: 0x04 (RTC alarm), cold boot (warm reset reason 0x00), power-off reason 0x.. (PON subtype 04)
power-on sources: 0x86 (SMPL, RTC alarm, power key)
RTC: counter 50000014, alarm at 50000000 (on, in -14 s)
the alarm's owner (count owner [used]): 50000000 leste used
this start by the alarm: system=leste rtc_alarm=50000000 rtc_now=50000014 due=... 
```

## On the phone, 2026-10-31

What goes onto the phone: kernel pro1x.10 (with 0028; built together with
the power patches 0029 and up), the boot image `65-kernel-update.sh
prepare` makes from this repository (the initramfs scripts and
`pro1x-boot` above), Leste's `pro1x-rtc` service (prepare installs it with
`pro1x-boot`, `66-boot-trial-install.sh`) and the `pro1x-alarmboot`
package. **Writing boot_b needs the owner's OK on the day.**

0. Optional, before flashing, with pro1x.9 running: the register reading
   alone. Copy `tools/pro1x-boot/pro1x-boot` to `/tmp` and run
   `sh /tmp/pro1x-boot pmic` as root: a start by the power key shows
   `power-on reason: 0x80 (power key)`; "power-on sources" must name
   "RTC alarm".
1. Build and prepare (PC, Leste running):
   ```sh
   KLOCAL=-pro1x.10 OUT=$WORK/out-pro1x.10 WORK=$WORK bash scripts/30-build-kernel.sh
   PHONE="ssh root@172.16.42.1" OUT=$WORK/out-pro1x.10 WORK=$WORK \
       bash scripts/65-kernel-update.sh prepare
   ```
2. With the OK: `... 65-kernel-update.sh flash` (saves boot_b as
   `/host/boot_b-before-7.1.0-pro1x.10.img`, writes, puts slot b on trial
   with 3 tries), then reboot. The trial and the way back are those of
   [kernel-update.md](kernel-update.md): a start that does not come up
   ends in Sailfish after three tries; back with
   `TARGET=b IMG=/host/boot_b-before-7.1.0-pro1x.10.img VBMETA= sh scripts/60-flash-slot-b.sh flash`
   or `fastboot flash boot_b $OUT/boot_b-before-7.1.0-pro1x.10.img`.
3. The package, from the pre-release `leste-alarmboot-2026.10.31.2`
   (1.4, the N9 look and off again as the N9; built by `scripts/mkdeb-tools.sh`,
   `ONLY=pro1x-alarmboot`; `pro1x-update` sees it too):
   ```sh
   gh release download leste-alarmboot-2026.10.31.2 -R smatkovi/pro1x-maemo-leste -D /tmp/alarmboot
   (cd /tmp/alarmboot && sha256sum -c SHA256SUMS)
   scp /tmp/alarmboot/pro1x-alarmboot_1.4_arm64.deb root@172.16.42.1:/root/
   # in Leste, as root (1.0's Xsession.post/48pro1x-alarmboot goes)
   apt install /root/pro1x-alarmboot_1.4_arm64.deb
   ```
   then log out of Hildon or reboot. FreeBSD gets `pro1x-alarmboot` 1.4
   and the new `hildon-session` with the next FreeBSD Hildon release.
   MeeGo's copy (`/host/meego-leste.img`, from Leste with the copy
   mounted, e.g. on `/mnt/ml`): the same package (`chroot /mnt/ml apt
   install /root/pro1x-alarmboot_1.4_arm64.deb` after copying it there)
   and the new `rootfs-overlay/etc/X11/Xsession.d/01pro1x-meego` into its
   `/etc/X11/Xsession.d/`; a newly made copy (`meego/45-meego-image.sh`)
   has both from Leste.

## The test on the phone

1. `pro1x-boot pmic` (root): "power-on sources" names "RTC alarm". If
   not: `echo 1 > /sys/bus/platform/drivers/qcom-pon/*/rtc_power_on`, and
   after the next power off and on, `pro1x-boot pmic` again: whether the
   PMIC kept it (if not, the rc scripts would have to set it at every
   start; FreeBSD has no such switch yet).
2. **Leste:** first the look alone: in the session's terminal (as
   `user`) `pro1x-alarmboot -D ring`, then `-D off` and `-L de`: the
   screens fill the display, a tap prints 0 or 1 (`-D off` does not
   switch off). Then clock, an alarm 5
   minutes ahead. `cat /sys/class/rtc/rtc0/wakealarm` is a count (the
   RTC's own, not the real time) and `pro1x-boot pmic` says `alarm at ...
   (on, in ~240 s)`. Switch off with the power key menu. Expected: about
   60 s before the alarm the phone switches itself on, no boot menu,
   Leste starts, at the alarm's time Leste's alarm tone and the N9's
   alarm screen (black, the time, the alarm's name, Snooze, Stop) over
   the Hildon alarm dialog.
   Without the charger (it keeps the phone on).
   - a) Stop: the tone stops, the screen goes, 2 s later "Switching off":
     tap it (keeps on). `cat /tmp/pro1x-alarmboot.log`: `stop tapped`,
     `alarm dialog status 7`, `stop for alarm N (button 1)`, `switching
     off in 10 s`, `staying on: kept on`; the clock: a single alarm
     switched off, a repeating one at its next day; nothing rings 10
     minutes later.
   - b) Again (alarm 5 minutes ahead, power off, on by the alarm): Stop,
     then nothing: after 10 s "Switching off" and the phone is off.
   - c) Again: Snooze, nothing: the log (`ssh` before it goes) says `an
     alarm is still to come` or `the RTC's wake-up: in ... s`; the phone
     goes off and switches itself on a minute before the snoozed time,
     rings with the N9 screen.
   - d) Again, unanswered: after 50 s the screen snoozes it (`no answer:
     snoozing`), the phone goes off and comes back for the snooze.
   - e) The power key during "Switching off": stays on (`staying on: the
     power key`). With the charger connected: stays on (`the charger`).
   - f) Portrait: the keyboard closed, the phone upright while it rings:
     the screen turns with hildon-desktop, everything in one column.
   - Should a tap leave the Hildon dialog on screen: the log says `the
     alarm dialog did not go`; the face-down signal did not reach the
     system UI (`dbus-monitor --system "interface='com.nokia.mce.signal'"`
     while tapping); its own buttons work; it switches off after it all
     the same unless the screen was used elsewhere.
3. **FreeBSD:** the same from FreeBSD's Hildon (`startx hildon`; with
   the writable pool alarmd keeps the alarm, else `pro1x-alarmboot`
   rings 2 minutes after its time), then `shutdown -p now`. Expected: the
   phone switches on, FreeBSD starts at once (kexec), Hildon comes up by
   itself, it rings, then "Switching off" (`shutdown -p`).
4. **Counter-checks:** switched on by the power key before an alarm: the
   menu as always (`last-boot`: `alarm-check: none not-by-the-rtc`). A
   reboot right after an alarm's start: the menu (`alarm-used-already`).

If it does not work:

| What happens | Where to look |
| --- | --- |
| The phone stays off | `pro1x-boot pmic` before the power off: the alarm `on`, in the future? "power-on sources" with "RTC alarm"? (step 1) |
| It comes on, but the menu shows | `/host/pro1x-boot/last-boot`, the `alarm-check:` line: `pmic-unreadable` (no debugfs, no `0-00` regmap), `not-by-the-rtc` (the `pon=` value: another reason bit), `not-this-alarm` (`late=` beyond 600 s or negative), `owner-not-installed`, `alarm-used-already`; `/host/pro1x-boot/rtc-alarm` (who owns it) |
| The system starts, nothing rings | Leste: `/host/pro1x-boot/alarmboot` there? `pro1x-alarmboot -v` by hand in the session's terminal; alarmd's queue (`alarmclient -i`, package alarmclient); FreeBSD: `kenv pro1x.alarmboot`, `/var/run/pro1x-alarmboot`, `/var/log/startx.log` |
| "Switching off" does not come | `/tmp/pro1x-alarmboot.log`: it waits for the alarm dialog to show and go (or for its own ringing to end); `staying on: ...` says why it stayed on (`the charger`, `a call`, `the phone was used`, `the power key`, `the RTC's wake-up for the next alarm is not set`) |
| The Hildon alarm dialog rings without the N9 screen | `/tmp/pro1x-alarmboot.log`: no `alarm dialog status 5` (started too late or not at all: `/etc/X11/Xsession.d/02pro1x-alarmboot`), `no clock alarm with Snooze and Stop rings` (alarmd did not give the ringing alarm), `did not go` (a tap was not taken: it stepped aside) |

## Tested on the PC

`tests/alarm-boot/run.sh` (2026-10-30, 92 checks, 0 failures):

- `pro1x-boot alarm-check` on faked PMIC registers (a 65536-line file of
  the regmap's form, read with `dd` at 9 * register as on the phone),
  under the PC's sh and under the initramfs's own busybox 1.35 (aarch64,
  through qemu-user): an alarm set in Leste, the same alarm a second time,
  a start by the power key, FreeBSD's alarm left alone by Leste
  afterwards, an Arch snapshot as the owner and one that is gone, an
  owner that is not installed, 20 minutes late, before the alarm, no
  alarm, the GEN1 layout and a count above 2^31, the PMIC unreadable or
  of another form; `pro1x-boot pmic`'s report.
- The addresses against Linux 7.1 (`rtc-pm8xxx`'s `pm8941_regs`,
  `pm6125.dtsi`) and kernel patch 0028's.
- `initramfs/local-top/pro1x-loop-root` itself, in the initramfs (its
  busybox, its `/scripts/functions`) in a bubblewrap sandbox with
  `blkid`, `mount`, `losetup`, `kexec` and the menu stubbed: the menu after
  a power-key start; Arch at once after its alarm, with the watchdog, the
  one-shot choice kept; that choice (FreeBSD) at the next start by kexec
  without the alarm words; FreeBSD's alarm with `pro1x.alarmboot=1
  pro1x.alarmdue=...`; a snapshot's alarm; an old `next` for the alarm's
  system taken.
- Kernel 0028 built with `W=1` without warnings (test kernel
  7.1.0-pro1x.10-alarm, pro1x.9 + 0028); `pro1x-alarmboot` built for
  FreeBSD against the Hildon sysroot and for Leste in its chroot.

```sh
INITRD=$WORK/out-pro1x.5/initrd.img-7.1.0-pro1x.5 LINUX=$WORK/build/linux \
    bash tests/alarm-boot/run.sh
```

`tests/alarm-boot/screens.sh` (2026-10-31): `pro1x-alarmboot` 1.2 built on
the PC against a stub of alarmd's libalarm (`tests/alarm-boot/stub`), in
an Xvfb and with a D-Bus daemon of its own as the system bus: `shots`
makes the screens' pictures (both languages, landscape and portrait);
`test` checks the answers, 29 checks, 0 failures with `OWN=1` (1.4,
2026-10-31): status 5
puts the N9 screen over the dialog with the alarm's text; a tap on Stop
sends `face_up` and `face_down`; status 7 gives alarmd the Stop button
(`ack 77 1`) and "Switching off" follows 2 s later; a tap keeps the
phone on; status 6 takes the screen away, status 5 brings it back; a tap
without a status after it steps aside after 3 s and leaves alarmd to the
system UI; with no alarm to come it switches off after 10 s (asks dsme);
the same alarm a second time does nothing; a click elsewhere after the
alarm ("the phone was used") keeps it on; while switching off
`display_status_ind` "dimmed" does not, "lpm" (`pro1x-standby`'s
stand-by screen, FreeBSD's power key) does; 15 s without input while the
screens show, X's idle time stays under 2 s (no stand-by time-out), and
from the start, 5 s before the alarm rings; in MeeGo's mode (`-s meego`) timed's
`next_bootup_event` 1 puts nothing over timed's screen, its end with the
next alarm's time switches off only with that alarm's RTC wake-up set
(on the PC it is not: stays on after 15 s); `-Q` asks as 1.1 did; without
a dialog it rings itself after 90 s with the alarm's text, its Snooze
goes to alarmd (`ack 79 0`) and switching off begins, a dialog that comes
after all takes over, and unanswered for 50 s the screen snoozes it. The
Leste package built by `49-alarmboot.sh` in the chroot of the official
image (`gcc -Wall`, no warnings) and installed there with apt over the
previous version.

`tests/alarm-boot/arch-chroot.sh` (2026-10-31, 1.3, 7 checks, 0 failures):
Arch's side in `arch/pc-chroot.sh`'s chroot of Arch Linux ARM (qemu) with
the packages of arch-hildon-2026.10.30.1 and the new `pro1x-alarmboot`
and `pro1x-sessions` installed, on an Xvfb of the PC: a system bus of its
own (the chroot's `dbus-daemon` with Arch's policy files), alarmd as the
user `alarm` with `librtc-realtime` (it owns `com.nokia.alarmd` through
`pro1x-alarm.conf`; the PC's rtc0 is `/dev/null` for the chroot then),
Arch's system UI, the desktop's ready and `init_done` signals; an alarm as
clock-ui sets one: the system UI's dialog rings, the N9 screen covers it,
Stop: the system UI took the face-down signal, its dialog went, alarmd
got the Stop and switched the alarm off, "Switching off", a tap keeps the
phone on (`~/ps/pro1x-screens/alarmboot-arch-chroot-*.png`; without a
window manager the system UI's dialog stays partly above our screen
there). It also found that alarmd, started in a session, waits for
`init_done` (its "limbo") before it shows any alarm: `hildon-session`
sends it.

`freebsd/vm/try-alarmboot.sh` (2026-10-30 with 1.1, the FreeBSD VM, its Hildon
session on Xvfb with the real alarmd and osso-systemui-alarm 0.3.3, the
N9's theme and fonts from the private assets installed): an alarm as
clock-ui sets one, due in 30 s; the system UI's dialog rings, the N9
screen covers it; a tap on Stop: the system UI took the face-down
signal, the dialog went, alarmd got the Stop and the single alarm was
switched off; a tap on Snooze: the alarm snoozed by 10 minutes; each
time the question, Keep on. Three runs in one session: the second and
third program's window was not sized by hildon-desktop (1 x 1) and fell
back to the override-redirect window as described, which worked the
same (shown blurred under the dialog's dimming). The pictures:
`~/ps/pro1x-screens/alarmboot-vm-freebsd-*.png`.

Again on 2026-10-31 with 1.4 and `pro1x-standby` (`STANDBY=`, built from
the repository's source, time-out 4 s, `RUNS="stop power"`): it started
3 s after `pro1x-alarmboot`, about 27 s before the alarm rang, and
neither dimmed nor showed its stand-by screen in the 53 s until the alarm
was over (before it, while it rang, during "Switching off"); it dimmed
5 s after the tap that kept the phone on. Its SIGUSR1 (the power key, as
`pro1x-powerkey` sends it) during "Switching off": `display_status_ind
lpm: the power key`, `staying on: the power key`, the VM stayed on.

```sh
bash tests/alarm-boot/screens.sh shots      # OUT=~/ps/pro1x-screens
OWN=1 bash tests/alarm-boot/screens.sh test
# in the FreeBSD VM, with the packages installed, the source in /tmp:
sh freebsd/vm/try-alarmboot.sh /tmp/pro1x-alarmboot.c
# with pro1x-standby and the power key (its source in /tmp too)
RUNS="stop power" STANDBY=/tmp/pro1x-standby.c sh freebsd/vm/try-alarmboot.sh
```

Not tested: anything on the phone (the PMIC registers through debugfs,
the power-on itself, ABL's behaviour after it, the ringing and the
switch-off question in a real session), and FreeBSD's side in the VM
(the VM was in use).

## Limits

- One RTC alarm for all systems: the system that set it last owns it.
  Sailfish OS (slot a) keeps its own alarms; an alarm while slot a is
  active starts Sailfish.
- `alarm-check` does not look at the warm reset reason: should a later
  restart within 10 minutes of an alarm still show the RTC bit in
  `PON_REASON1` (whether the PMIC keeps it over a reset is to be seen on
  the phone), and that alarm's count be a new one (an alarm that rang
  while the phone was on), that start goes straight to the system that
  ran anyway, and `pro1x-alarmboot` there asks to switch off. Each count
  starts a system only once.
- The N9 screen answers the Hildon alarm dialog through its "turned
  face down" snooze (osso-systemui-alarm 0.3.3: only while the dialog
  rings, from anyone on the system bus); a system UI without it makes
  the screen step aside after the first tap. Between the rings (its tone
  stops after a minute) the Hildon dialog shows. Stop is that snooze
  followed by alarmd's Stop: for a moment the RTC is set for the snooze,
  then for the next alarm. Only clock alarms with Snooze and Stop get
  the N9 screen (not calendar reminders).
- Leste's alarmd leaves the RTC alarm of a deleted alarm set: the phone
  then switches on for nothing, `pro1x-alarmboot` does not ring (alarmd
  has no such alarm) and only asks to switch off again.
