# Audio on the Pro1X (Leste)

The playback path: PulseAudio opens `hw:Pro1X,0` (MultiMedia1, q6asm), the
DSP routes it to `RX_CODEC_DMA_RX_0`, the LPASS RX macro sends it over
SoundWire to the WCD9370, whose headphone PA (HPHL/HPHR) drives both the
headphone jack and two Awinic AW87339 class-K amplifiers (I2C 0x59 and
0x5a, enable lines GPIO 85 and 98, `Spk0`/`Spk1` in the mixer) for the
loudspeakers. The UCM profile is the NixOS port's (`Speaker` switches
`Spk0 OUT Switch`/`Spk1 OUT Switch` on, `Headphones` off) with this
repository's headset jack patch (below); PulseAudio runs
at 48 kHz, S16 (`rootfs-overlay/etc/pulse/daemon.conf.d/50-pro1x.conf`).
With kernel patches 0034/0035 music takes a second front end,
MultiMedia4 (`hw:Pro1X,8`), with periods of 640 ms instead of 20 ms
("Music with a deep buffer", at the end).

The kernel for the flash on 2026-10-31 is 7.1.0-pro1x.9 (`$WORK/out-pro1x.9`,
patches 0001-0027): pro1x.8 (0001-0026, the loudspeaker pop below) plus the
headset jack's thresholds (0027). Both tests below run on it.

## The loudspeaker pop (patch 0026, 7.1.0-pro1x.8, untested)

Every power-up of the card popped in the loudspeaker, so
`rootfs-overlay/etc/pulse/default.pa.d/50-pro1x.pa` lets PulseAudio keep
the card open for 300 s after a sound instead of 5 s. That costs about
100 mA and 400 interrupts a second for those 5 minutes and keeps
`pro1x-autosuspend` from suspending (`docs/power-management.md`). With a
kernel that has this patch the file now picks 3 s by itself (below,
"The PulseAudio switch").

What the kernel did (up to pro1x.7): the amplifier's DAPM widget was a PGA
without a register, the same tier as the codec's `HPHL PGA`/`HPHR PGA`,
and DAPM breaks the tie by register (-1 before `WCD937X_ANA_HPH`, 0x3009).
So the amplifiers were enabled before the codec's headphone PA was
switched on (class-H, PA enable, 20 ms settling). In the device tree the
amplifiers hang off the RX macro's `HPHL_OUT`/`HPHR_OUT`, beside the codec,
so nothing but that sort order places them. The AW87339's output stage
starts switching about 40 ms after the enabling write (datasheet V1.3:
TON typ. 40 ms; its start-up plot), while the stream was started about
25 ms after it. Two explanations fit, and only the phone can tell them
apart:

- the PA's turn-on step reaches the amplifier's AC-coupled input
  (9 kOhm / 100 nF) during its turn-on;
- the output stage comes up in the middle of the sound's first 15 ms.

Patch 0026 (`device/kernel/patches/0026-asoc-aw87339-*.patch`):

- The amplifier is an output driver widget (`POST_PMU`/`PRE_PMD`, as
  mainline's aw8738 and simple-amplifier, and as Xiaomi's wcd937x hooks up
  the same amplifier family): on after the codec's PGAs and their
  settling, off before them. Its output stops within the disabling write
  (datasheet shutdown plot).
- A `POST` widget (as rt5640's "HP Post") holds the stream start until the
  turn-on time has passed after the enable, before the DAIs are unmuted
  and the stream is triggered. Both amplifiers are enabled first, so the
  two waits add up to one turn-on time. A sound after idle starts about
  40 ms later than before.
- The register values are unchanged.

Module parameters, changeable at runtime, each for the next power-up
(`/sys/module/snd_soc_aw87339/parameters/`):

| Parameter | Default | Meaning |
|---|---|---|
| `on_delay_ms` | 40 | the wait after the enable (datasheet TON typ. 40 ms); 0 = the order change alone |
| `off_gpio` | Y | the enable line low while off (vendor kernel, aw87xxx v0.2.0); N keeps it high and shuts down by software only (Awinic's aw87xxx v0.1.14 for the AW873x9) |

Not covered: the wait only runs for a stream start. A pin switch while a
stream runs (the UCM device switching from headphones to the speaker)
enables the amplifier without it; the stream is then already playing, as
before. After a system resume the stream comes back without the wait too.

Sources: Awinic's AW87339 datasheet V1.3 (June 2018: TON, the start-up and
shutdown plots, SYSCTRL, the input high-pass); the Pro1X's vendor kernel
(LineageOS `android_kernel_fxtec_sm6115`: `techpack/audio/asoc/codecs/awinic/aw87xxx.c`
v0.2.0, the `aw87xxx_mode_switch_*` controls in `bengal.c`) and its
`mixer_paths.xml`; Xiaomi's `techpack/audio/asoc/codecs/wcd937x/wcd937x.c`
(`AW87xxx_SPK`); Awinic's aw87339 reference driver (the power-down write).
Copies of all of them on the build host: `$WORK/dl/aw87339/`.

## Test on the phone (with 7.1.0-pro1x.9, which contains 0026)

Two files from the build host (1 s of digital silence, and a 100 ms
1 kHz beep with 5 ms ramps), so that anything heard with the first is
power sequencing and the second shows a cut or clicking start:

    python3 -I -c 'import math,struct,wave
    def wav(p, n, a):
        w = wave.open(p, "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
        w.writeframes(b"".join(struct.pack("<hh", v, v) for i in range(n)
            for v in [int(a * min(1, i / 240, (n - i) / 240) * math.sin(2 * math.pi * 1000 * i / 48000))]))
    wav("/tmp/pro1x-silence.wav", 48000, 0); wav("/tmp/pro1x-beep.wav", 4800, 8000)'
    scp /tmp/pro1x-silence.wav /tmp/pro1x-beep.wav root@172.16.42.1:/tmp/

0. **Before the flash, on the old kernel** (the reference): steps 2 and 3,
   to hear the pop as it is.
1. **After the flash:** `uname -r` is 7.1.0-pro1x.9; `cat /proc/asound/cards`
   shows the card id (`Pro1X` is assumed below, in `hw:Pro1X,0`);
   `dmesg | grep -i aw87339` shows `AW87339 found` twice and no
   `enable sequence failed`;
   `grep . /sys/module/snd_soc_aw87339/parameters/*` shows 40 and Y.
2. **PulseAudio's timeout down to 3 s, until its next start** (nothing
   written). PulseAudio is the session user's (`user`, socket in
   `/run/user/<uid>/pulse`), so from root:

       pa() { su user -c "XDG_RUNTIME_DIR=/run/user/\$(id -u) $*"; }
       pa pactl unload-module module-suspend-on-idle
       pa pactl load-module module-suspend-on-idle timeout=3

3. **Listen,** loudspeaker (no headphones), media volume as usual:
   - `pa paplay /tmp/pro1x-silence.wav`, five times, at least 6 s apart
     (3 s timeout plus margin): a click when it starts (power-up) or about
     3 s after it ends (power-down)? With digital silence anything heard
     is power sequencing.
   - `pa paplay /tmp/pro1x-beep.wav` after 6 s of silence, then again within
     3 s (the card still open): do both beeps start the same? A click or a
     shorter beep only after idle, with the silence file clean, is the
     output stage coming up in the middle of the sound.
   - Without PulseAudio, the kernel alone (as root, while PulseAudio's sink
     is suspended): `aplay -q -D hw:Pro1X,0 -t raw -f S16_LE -r 48000 -c 2 -d 1 /dev/zero`
     is one power-up and one power-down of digital silence.
4. **Which part does it** (each setting for the next power-up; back to
   the defaults afterwards), `P=/sys/module/snd_soc_aw87339/parameters`:
   - `echo 0 > $P/on_delay_ms`: the order change alone. The silence file
     stays clean but the beep after idle clicks or is cut: both halves are
     needed. Both clean: the order was the cause, the wait only costs
     latency (keep it anyway unless it is noticeable).
   - Still a click at the start with the defaults: `echo 80 > $P/on_delay_ms`
     (a longer turn-on than the datasheet's typical one), then
     `echo N > $P/off_gpio` (the amplifier is not reset between streams).
   - A click left at the start with all of these comes from the
     amplifier's own turn-on; one at the end, from its shutdown or from the
     codec after it (the order below says which).
5. **The order, without ears:**
   - DAPM's own log: `echo 1 > "/sys/kernel/debug/asoc/F(x)tec Pro1X/dapm_pop_time"`,
     one sound after idle, `dmesg | grep 'pop test'`, then `echo 0` again.
     On power-up `Spk0 SPK PA`/`Spk1 SPK PA` are queued after
     `HPHL PGA`/`HPHR PGA` (on pro1x.7 and before, before them); on
     power-down before them.
   - The trace events with times:

         T=/sys/kernel/tracing; [ -e $T/events ] || mount -t tracefs nodev $T
         for e in dapm_start dapm_done dapm_widget_event_start dapm_widget_event_done; do
             echo 1 > $T/events/asoc/snd_soc_$e/enable; done
         echo > $T/trace; sleep 5; pa paplay /tmp/pro1x-beep.wav; sleep 5
         grep -E 'SPK PA|HPH. PGA|dapm_start|dapm_done' $T/trace
         echo 0 > $T/events/asoc/enable

     Power-up (`snd_soc_dapm_start` with `event=1`, a stream start):
     `HPHL PGA` `val=2` (POST_PMU) done before `Spk0 SPK PA` `val=2`, and
     the run's `snd_soc_dapm_done` about 40 ms after the second
     `SPK PA` (the `Settle` widgets have no trace events of their own).
     Power-down (`event=2`): `SPK PA` `val=4` (PRE_PMD) before
     `HPHL PGA` `val=4`.
   - The widget states: `grep . "/sys/kernel/debug/asoc/F(x)tec Pro1X/"*aw87339*/dapm/*`
     (`SPK PA: On` while a sound plays, `Off` 3 s after it).
6. **Pass:** no click in five tries of the silence file and the beep
   starting the same after idle as with the card open. Then the
   PulseAudio switch below; with `tools/pro1x-powerlog`, the `pcm` column
   and the +100 mA phase after a sound then last about 3 s.

## The PulseAudio switch (by the kernel since 2026-10-30)

`rootfs-overlay/etc/pulse/default.pa.d/50-pro1x.pa` chooses by itself: 3 s
when `/sys/module/snd_soc_aw87339/parameters/on_delay_ms` exists (a kernel
with patch 0026, pro1x.8 and later), 300 s otherwise. So the reference
listening on the old kernel (step 0) still has 300 s, and step 2's pactl
lines are only needed for a PulseAudio started before the file was there.
It goes to the phone with the other power files:

    PHONE="ssh root@172.16.42.1" bash scripts/71-power-userspace.sh

(Leste and the MeeGo copy; Kali and Arch get WirePlumber's 3 s), and
PulseAudio's next start (a reboot) uses it. If the test fails (a pop with
the new kernel): `touch /etc/pulse/pro1x-suspend-300` in Leste and in the
MeeGo copy, and the next start is back at 300 s; `71-power-userspace.sh
restore` puts the old file back.

## The headset jack (patch 0027, 7.1.0-pro1x.9; UCM patch 0002; untested)

The 3.5 mm jack is the WCD9370's: its MBHC (multi-button headset control)
sees a plug by the jack's L-detect switch, tells a headphone from a headset
by the voltage on the microphone line with MIC BIAS2 on, and a headset's
buttons by comparators (`freebsd/audio/README.md`, "The headset jack", has
the details; FreeBSD drives the same MBHC from user space).

**What Linux had already.** The sound card (`sm8250.c`, `fxtec,pro1x-sndcard`)
registers the jack for every back end through `qcom_snd_wcd_jack_setup()`:

- the input device "F(x)tec Pro1X Headset Jack" (`SW_HEADPHONE_INSERT`,
  `SW_MICROPHONE_INSERT`, `SW_LINEOUT_INSERT`, `SW_JACK_PHYSICAL_INSERT`;
  buttons 0-3 as `KEY_MEDIA`, `KEY_VOICECOMMAND`, `KEY_VOLUMEUP`,
  `KEY_VOLUMEDOWN`);
- the card controls "Headphone Jack" and "Mic Jack" (interface CARD, so
  `amixer cget` needs `iface=CARD,`).

On the `WCD Capture` link (`TX_CODEC_DMA_TX_3`) it hands the jack to
wcd937x, whose `set_jack` starts the MBHC. The codec's interrupts come in
band: the TX device raises a SoundWire alert on swr0 (SPI 296, or its wake
interrupt SPI 79, `swr_wake_irq`, while the bus' clock is stopped). The
jack type needs nothing either: the vendor device tree has
`qcom,msm-mbhc-hphl-swh` and `gnd-swh` = 1, normally open, which is the
binding's default (no `qcom,*-jack-type-normally-closed`). There is no
US/Euro switch, in the vendor's tree neither.

**What was missing:**

1. **The button thresholds** (patch 0027,
   `device/kernel/patches/0027-arm64-dts-qcom-sm6115-pro1x-headset-jack-mbhc-thresholds.patch`).
   Without `qcom,mbhc-buttons-vthreshold-microvolt`,
   `wcd_dt_parse_mbhc_data()` logs "missing
   qcom,mbhc-buttons-vthreshold-microvolt entry" and sets each threshold to
   500000, which `wcd937x_mbhc_program_btn_thr()` takes as millivolts: the
   6-bit fields of `ANA_MBHC_BTN0-7` (0x301a-0x3021, bits 7:2, mV * 2 / 25)
   got `(500000 * 2 / 25) & 0x3f` = 0, all eight thresholds 0 mV. Now the
   vendor's 75, 150, 237 and 500 mV (`bengal.c`, `def_wcd_mbhc_cal`), and
   the headset and headphone thresholds (1.7 V, 50 mV, the driver's defaults)
   written out as every upstream WCD93xx board has them. The device tree
   of pro1x.9 has them (`dtc -I dtb -O dts out-pro1x.9/sm6115-fxtec-pro1x.dtb`,
   in the `qcom,wcd9370-codec` node; the only difference to pro1x.8's).
2. **The UCM profile followed no jack** (`patches/nixos-audio/0002-ucm-headset-jack.patch`
   to the NixOS port's `device/audio/ucm2/Pro1X`). PulseAudio gave the ports
   no availability, the loudspeaker (priority 200) always won over the
   headphones (100), and there was no headset microphone. Now:
   - `Headphones` has `JackControl "Headphone Jack"` and priority 300, in
     both verbs: above `Speaker` (HiFi) and `Earpiece` (Voice Call), but
     "not available" without a plug.
   - The new `Headset` device (HiFi) records a headset's microphone, with
     `JackControl "Mic Jack"` and priority 200, above the built-in `Mic`.
     The path is AMIC2 on the WCD9370's ADC2 (`ADC2 MUX` INP2, `ADC2 Volume`
     12), over SoundWire into the TX macro (`TX SMIC MUX0` SWR_MIC4, the
     SM6115's mux names; `TX DEC0 MUX` SWR_MIC, `TX_DEC0 Volume` 84, 0 dB),
     then `TX_CODEC_DMA_TX_3`: the vendor's `amic2` path. It is recorded
     through MultiMedia2 (`hw:Pro1X,1`) like the built-in microphone, so
     PulseAudio has one source with the ports Mic and Headset; a front end
     has one back end, so the device moves MultiMedia2's route to
     `TX_CODEC_DMA_TX_3` and back to `VA_CODEC_DMA_TX_0`.
   - No `JackHWMute`: nothing mutes the loudspeakers in hardware; the
     device sequences switch the amplifiers (`Spk0/Spk1 OUT Switch`).
   - No `ConflictingDevice`: with none, PulseAudio 16 combines no devices
     into combination ports (`ucm_check_conformance`); a list naming only
     some would.

How PulseAudio (16.1 on Leste) uses it: a port with a `JackControl` is
"available" or "not available" by its control, the others are "availability
unknown". `module-switch-on-port-available` switches to a port that becomes
available and away from one that stops being available; at its start a
sink takes its highest-priority port that is not unavailable. PipeWire
(Arch) does the same through its ALSA card profiles and WirePlumber.

**Not covered:**

- Calls keep the built-in microphone, a headset too (the headphones do take
  the call's sound). The DSP's voice session (q6voice's CVP) takes its tx
  port when it is created; a microphone switched during a call would end
  the call's uplink. FreeBSD's daemon restarts the session for that; on
  Linux it would need `pro1x-voice-call` to do so.
- A recording that is running while the headset comes or goes keeps its
  microphone until it starts again (the DSP's session stays on its port).
- A plug that the MBHC reports as line out (both channels above 5 kOhm, an
  amplifier's input, or a cable without load: `SND_JACK_LINEOUT`) sets
  neither control, so the sound stays on the loudspeaker. A headset wired
  the other way round (OMTP; ground and microphone swapped) is "unsupported":
  without a US/Euro switch nothing can be done for it.
- The jack's DAPM pins (the card's "Headphone Jack" and "Mic Jack" widgets)
  stay unrouted on purpose: the AW87339s sit behind the WCD9370's HPH
  outputs, and a headphone widget behind HPHL/HPHR would end their
  endpoint status, so the loudspeaker path would depend on the jack.

**Where the profile comes from.** New Leste images: `45-pro1x-hardware.sh`
installs the NixOS port's files and applies the patch; Arch, Kali and
LuneOS (`*/20-pro1x-hardware.sh`) copy Leste's profile when they are made.
The systems on the phone now, all at once (Leste, the MeeGo copy,
`kali.img`, `luneos.img`, `arch.img`), from the build host:

    PHONE="ssh root@172.16.42.1" bash scripts/64-ucm-install.sh

Each system's profile is saved once as `/var/backups/pro1x-ucm/Pro1X`;
`... 64-ucm-install.sh restore` puts it back. Nothing restarts the sound
server; the next start of PulseAudio or PipeWire reads the profile (a
reboot does it). On the PC, `sh tests/ucm-pro1x/check.sh parse` makes
the profile from the NixOS port's files and the patch and has alsa-lib
parse it (PASS on 2026-10-30 with alsa-lib 1.2.16; Leste has 1.2.8).

### Test on the phone (2026-10-31, with 7.1.0-pro1x.9)

The kernel side first, with the old profile (PulseAudio does not move the
sound yet), then the profile. Root shell in Leste; `pa()` as in the
loudspeaker test; `evtest` from Leste's archive (`apt install evtest`).
Headphones without a microphone and a 4-pole headset (CTIA) with a
button, ideally three (play, volume up, volume down).

1. **Nothing plugged in**, after the flash:
   - `uname -r` is 7.1.0-pro1x.9; `dmesg | grep -i -E 'mbhc|wcd937x'` has no
     "missing qcom,mbhc-buttons-vthreshold-microvolt entry" (pro1x.8 and
     before log it) and no "mbhc initialization failed".
   - `grep -B1 -A4 'Headset Jack' /proc/bus/input/devices`:
     `N: Name="F(x)tec Pro1X Headset Jack"`, `H: Handlers=kbd eventN`.
   - `amixer -c Pro1X cget iface=CARD,name='Headphone Jack'` and `... 'Mic Jack'`:
     `: values=off` both. **If "on" with nothing in**, the switch is normally
     closed after all: stop here, do not install the profile (PulseAudio
     would take the headphones), and note it (the device tree would need
     `qcom,hphl-jack-type-normally-closed`).
   - The thresholds the MBHC wrote:
     `for r in /sys/kernel/debug/regmap/*/registers; do grep -q '^301a:' $r && grep -E '^30(1[a-f]|2[01]):' $r; done`:
     bits 7:2 are 6, 12, 18, 40, 40, 40, 40, 40, that is
     `301a: 18`, `301b: 30`, `301c: 48`, `301d: a0`, `301e: a0`, ...
     (pro1x.8: 00 everywhere; 0x301f/0x3020 may differ in bits 1:0 after an
     impedance measurement).
2. **The kernel's events.** In a second shell `evtest /dev/input/eventN`
   (N from step 1). Each step from idle (no sound for 30 s, the card and
   SoundWire suspended: the wake interrupt's path) and once while
   `pa paplay` plays something long; `grep -E 'swr|wcd' /proc/interrupts`
   before and after shows which interrupts counted:
   - **Headphones in:** `SW_JACK_PHYSICAL_INSERT 1` and
     `SW_HEADPHONE_INSERT 1` within about a second; "Headphone Jack" on,
     "Mic Jack" off. **Out:** both 0, both controls off.
   - **Headset in:** `SW_HEADPHONE_INSERT 1` and `SW_MICROPHONE_INSERT 1`
     (the microphone can follow up to 3 s later, the MBHC's corrections);
     both controls on.
   - **Its buttons:** a short press gives `KEY_MEDIA 1` and `0` on release;
     held for more than 0.4 s, the `1` comes after 0.4 s. Volume up and down:
     `KEY_VOLUMEUP`, `KEY_VOLUMEDOWN` (an Android-type headset: 0, 240 and
     470 Ohm; a 135 Ohm button is `KEY_VOICECOMMAND`). All buttons as
     `KEY_MEDIA`, or none: the thresholds (step 1).
   - **Out:** all 0.
   - **A boot with headphones in** (not known: the MBHC is started with
     the plug already there, its first switch interrupt has to come from
     that state): after the boot "Headphone Jack" is on without touching
     the plug.
   - Nothing at all, the controls never change: the codec's interrupt does
     not arrive (see the counts). Only while playing: the wake path from
     clock stop (SPI 79). The `jack` scenario of
     `freebsd/audio/test/leste-audio-trace.sh` records what Linux does for
     it (`sh /tmp/leste-audio-trace.sh jack /host/audio-trace-jack.txt`).
3. **The profile** (once 1 and 2 pass), from the build host:

       PHONE="ssh root@172.16.42.1" bash scripts/64-ucm-install.sh
       scp tests/ucm-pro1x/check.sh root@172.16.42.1:/tmp/ucm-check.sh

   On the phone `sh /tmp/ucm-check.sh controls`: PASS (every control and
   enumerated value the profile sets exists on the card), then a reboot.
4. **PulseAudio**, `pa pactl list modules short | grep switch-on-port`
   lists `module-switch-on-port-available`. Then
   `pa pactl list sinks | grep -E 'Name:|Active Port|\[Out\]'` and the same
   for `sources` (`\[In\]`):
   - Nothing in: sink ports `[Out] Speaker` (priority 200, availability
     unknown), `[Out] Headphones` (300, not available), `[Out] Earpiece`
     (50); `Active Port: [Out] Speaker`. Source ports `[In] Mic` (100,
     availability unknown), `[In] Headset` (200, not available);
     `Active Port: [In] Mic`. Sound from the loudspeaker, the built-in
     microphone records as before (`pa parecord --channels=1 /tmp/m.wav`,
     a few seconds, then `pa paplay /tmp/m.wav`).
   - Headphones in, while something plays: `Active Port: [Out] Headphones`
     (available) within a second, the sound in the headphones only
     (`amixer -c Pro1X cget name='Spk0 OUT Switch'`: off). The source stays
     `[In] Mic`. Out: back to `[Out] Speaker` and the loudspeaker.
   - Headset in: the same for the sink, and the source on `[In] Headset`
     (available); `pa parecord --channels=1 /tmp/h.wav` while speaking into
     the headset, then play it back: the headset's microphone. Out: the
     source back on `[In] Mic`.
   - A reboot with the headset in: both ports chosen at PulseAudio's start.
   - `pa pactl list cards`: the profiles `HiFi` and `Voice Call`, each with
     its ports' availability.
   - With a SIM: a call with headphones in has its sound in the headphones
     (the microphone stays the built-in one, see above).
   - Arch (PipeWire): the same with `pactl` (pipewire-pulse) or
     `wpctl status`.
5. **Back:** `PHONE=... bash scripts/64-ucm-install.sh restore` and a
   reboot gives the old profile; the kernel as in "The way back" of
   `docs/kernel-update.md`.

## Music with a deep buffer (patches 0034/0035, pro1x-music-sink; 2026-10-31, untested on the phone)

Music and podcasts kept the CPU busy for nothing: while anything plays, the
CPU wakes 50 times a second. Now music goes to a second sink whose
periods last 640 ms, so the CPU sleeps most of the time; everything else
stays as it was.

**Why it was 50 a second.** q6asm-dai marks its PCMs `SNDRV_PCM_INFO_BATCH`
(the position moves a whole period at a time, when the DSP returns a
buffer), and PulseAudio 16 turns timer-based scheduling off for every
batch device except USB ones (`alsa-util.c`: "Disabling tsched mode since
BATCH flag is set"). So `tsched_buffer_size` does nothing on this card:
PulseAudio runs in interrupt mode with fixed latency, one wake-up per
period, and module-alsa-card opens the UCM sink with the daemon's
defaults, 4 x 25 ms, which the driver's 480-frame step turns into
5 x 20 ms (100 ms). Each period is one ASM write to the DSP and one
WRITE_DONE back over GLINK, and PulseAudio and the player refill it, for a
notification as for an hour of music. One sink with long periods would
delay every sound and every volume change by its whole buffer: q6asm-dai
also has `SNDRV_PCM_INFO_NO_REWINDS`, so nothing written can be taken back.

**What the DSP path offers** (sound/soc/qcom/qdsp6, 7.1):

- Playback periods of 128 bytes to 64 KiB, 2 to 8 of them, in steps of 480
  frames: at most 16320 frames (340 ms) at 48 kHz stereo S16, 2.7 s of
  buffer. Capture: periods of at most 4 KiB. The vendor kernel's driver for
  this ADSP family (`msm-pcm-q6-v2.h`, msm-4.19 "bengal") allows 122880
  bytes per period.
- A front end (MultiMedia1-8) is one ASM session; the ADSP mixes all the
  sessions routed to a port (q6routing: one COPP per port, each session
  in its matrix map). MultiMedia2 (the microphones) and MultiMedia3 (the
  FM receiver) are taken by capture, and a front end's playback and
  capture share its session (`q6asm_audio_client_alloc` returns the one
  that is there), so a second playback stream needs a fourth front end.
- There is no "deep buffer" front end in mainline, nor a DSP-side volume
  (q6asm opens its streams with the null post-processing topology): the
  volume stays PulseAudio's, in software, before the buffer.
- Compress offload: q6asm-dai has it (`is-compress-dai` on a front end;
  MP3, FLAC, WMA, ALAC, APE; fragments of 8-128 KiB, up to 64). The
  ADSP firmware (`adsp.mbn`, QX1050) has decoder modules for FLAC, ALAC,
  APE, Vorbis, WMA, WMA Pro and HE-AAC, and the MP3 format id in 11
  places but no MP3 module of its own (it may be built into the core;
  only the phone can tell). No player on Leste, Kali or Arch writes to a
  compress device (tinycompress's `cplay` plays MP3 files, nothing else
  of the desktop does), so this was not pursued.

**The kernel** (test build `7.1.0-pro1x.11-audio` in `$WORK/out-pro1x.11-audio`,
patches 0001-0035, built 2026-10-31, not flashed):

- `0034-arm64-dts-qcom-sm6115-pro1x-deep-buffer-front-end-multimedia4.patch`:
  MultiMedia4 as a playback-only front end (`dai@3`, `direction` RX) and
  its DAI link, the last one: the card numbers its PCMs by link order,
  back ends included, so the others keep their numbers and MultiMedia4 is
  `hw:Pro1X,8` (`/proc/asound/pcm`: `00-08: MultiMedia4 ...`). Like
  MultiMedia1 it opens only with a route, `RX_CODEC_DMA_RX_0 Audio Mixer
  MultiMedia4`.
- `0035-asoc-qcom-q6asm-dai-playback-periods-up-to-122880-bytes.patch`:
  periods of up to 122880 bytes, the vendor driver's value (30720 frames,
  640 ms, a multiple of 480). The buffer each MultiMedia substream gets
  preallocated grows from 512 to 960 KiB (seven substreams now: about
  6.6 MiB instead of 3 MiB).

**User space** (Leste and the MeeGo copy, PulseAudio 16.1):

- `tools/pro1x-music-sink` (`/usr/local/bin/pro1x-music-sink`, started by
  `Xsession.post/48pro1x-music-sink`), a PulseAudio client of the session:
  - loads `module-alsa-sink device=hw:Pro1X,8 sink_name=pro1x_music
    fragments=2 fragment_size=122880` (2 x 640 ms; on a kernel with 0034
    but not 0035 PulseAudio gets 4 x 320 ms) with
    `device.intended_roles='x-maemo music'`, so that module-intended-roles
    (in Leste's `default.pa`) creates such streams on it: the media player
    (mafw-gst-renderer sets `media.role` x-maemo, `buffer-time` 600 ms)
    and players that say music;
  - gives pro1x_music the default sink's volume and mute at every change:
    the volume keys, the status menu and `pro1x-volume-osd` set the
    default sink's volume, and pro1x_music has no hardware volume either;
  - keeps the output the user chose: a new stream of those roles moves to
    pro1x_music only from the default sink while that is the phone's own
    (the same `alsa.card`), and from pro1x_music to the default sink while
    that is another one (Bluetooth headphones);
  - stopped (SIGTERM), unloads the sink; PulseAudio moves its streams to
    the default sink.
  Without it there is no such sink and everything plays as before; that is
  why the sink is not in `50-pro1x.pa`: music on it without the volume
  follower would not hear the volume keys, and module-intended-roles alone
  would take music away from Bluetooth headphones. Nothing is loaded on a
  kernel without MultiMedia4 (`/proc/asound/Pro1X/pcm8p`), or with
  `/etc/pulse/pro1x-deep-buffer-off`. `-p BYTES` sets another period
  (a multiple of 1920; 61440 is 320 ms), `-v` logs what it does.
- `/etc/udev/rules.d/86-pro1x-music-route.rules` runs
  `/usr/local/sbin/pro1x-music-route` when the card appears: the route
  above, which the UCM profile does not know (as `BootSequence` keeps
  MultiMedia1's). Nothing is powered by it until a stream runs.
- Kali and Arch (PipeWire) are unchanged. PipeWire runs one graph clock
  for all streams and does lower its quantum while only a stream with a
  large `node.latency` runs, but on a batch device its ALSA node keeps a
  headroom of a period, and the period is fixed when the PCM opens; the
  same split (a node on `hw:Pro1X,8` for music, WirePlumber's role-based
  linking) would be its way, not tried. FreeBSD's `pro1x-audiod` is
  unchanged as well.

**What it costs.** Pause, seek and volume changes on the music sink take
effect after what is already in the DSP: up to 1.28 s, 0.64 s on average
(the player's own volume, a role volume of PulseAudio, the same). A track
starts as before (PulseAudio fills the 1.28 s at once). Notifications,
the keyboard and the other sounds stay on the UCM sink with its 20 ms
periods; a call's audio does not go through PulseAudio at all (q6voice).
If the delay is too long: `-p 61440` in the session hook (320 ms periods,
about 3 wake-ups a second).

**Measured on the PC (2026-10-31).** `tests/audio-deep-buffer`: Leste's own
PulseAudio 16.1 and GStreamer in the Leste chroot (qemu) against
`pcm_q6sim`, an ALSA PCM with q6asm-dai's limits (480-frame steps, 2-8
periods, at most 64 KiB or 120 KiB, the position in whole periods, a
period taken only once written completely, no rewinds; PulseAudio sees
it as the phone's: interrupt mode, 5 x 20 ms for the UCM sink), and perf
on the PC for when PulseAudio's and the player's threads ran. Players:
`mafw` as mafw-gst-renderer (pulsesink 600/300 ms, role x-maemo), `qt` as
a QtMultimedia player (pulsesink's defaults 200/10 ms, role music).
20 s each, after 5 s:

    BASE_IMG=<image> CHROOT=<dir> bash tests/audio-deep-buffer/pc-measure.sh

| | player | periods/s MM1 | MM4 | thread switches/s | wake-ups/s | median sleep |
|---|---|---|---|---|---|---|
| before (UCM sink) | mafw | 50.0 | - | 291 | 50.5 | 20 ms |
| before (UCM sink) | qt | 50.0 | - | 401 | 50.2 | 19 ms |
| after (0034+0035) | mafw | 0 | 1.6 | 179 | 2.2 | 432 ms |
| after (0034+0035) | qt | 0 | 1.6 | 168 | 2.2 | 457 ms |
| after, 0034 alone | mafw | 0 | 3.2 | 173 | 3.8 | 314 ms |
| after, 0034 alone | qt | 0 | 3.2 | 170 | 4.0 | 312 ms |

"Wake-ups" counts runs of those threads more than 2 ms after the previous
one: how often the CPU leaves idle for the sound. The thread switches
fall less, because the decoder still hands its buffers (1024 frames) one
by one, but they come in a burst once a period. In each "after" run the
stream was on pro1x_music and the UCM sink stayed suspended, and
pro1x_music followed the default sink's volume (37 %). With another
default sink (a null sink as a Bluetooth headset, `target.sh start
after-bt mafw`) the stream played there; with pro1x-music-sink stopped the
sink was gone and the stream back on the UCM sink.

### Test on the phone (with 7.1.0-pro1x.11-audio)

From the build host, `pa()` as in the loudspeaker test. The current needs
the battery (`pro1x-powerlog` suspends the charger's input itself).

1. **Kernel and files:**

       PHONE="ssh root@172.16.42.1" OUT=$WORK/out-pro1x.11-audio WORK=$WORK \
           bash scripts/65-kernel-update.sh prepare
       ... 65-kernel-update.sh flash            (with the phone's owner there)
       PHONE="ssh root@172.16.42.1" bash scripts/72-audio-deep-buffer.sh

   then a reboot (a cold start: after a kexec the ADSP keeps the first
   kernel's state).
2. **Kernel side:** `uname -r` is 7.1.0-pro1x.11-audio; `grep MultiMedia4
   /proc/asound/pcm` shows `00-08: MultiMedia4 ... : playback 1`;
   `amixer -c Pro1X cget name='RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia4'`
   is on (the udev rule); `dmesg | grep -i -E 'q6asm|multimedia'` has no
   errors. By hand, without PulseAudio:
   `aplay -D hw:Pro1X,8 --period-size=30720 --buffer-size=61440 /tmp/pro1x-beep.wav`
   (the beep of the loudspeaker test; aplay pads the last period with
   silence) plays while PulseAudio's sinks are suspended, and `aplay
   --dump-hw-params -D hw:Pro1X,8 /tmp/pro1x-beep.wav` ends the
   `PERIOD_BYTES` range at 122880 (65536 without 0035).
3. **The sink:** `pa pactl list short sinks` lists `pro1x_music` (and the
   UCM sink); `pa pactl list sinks | grep -A3 pro1x_music` has
   `device.intended_roles = "x-maemo music"`. Without it:
   `pa /usr/local/bin/pro1x-music-sink -v` in a shell says why (Ctrl-C ends
   it; it unloads only a sink it loaded itself).
4. **Music on it:** an MP3 in the media player; `pa pactl list sink-inputs |
   grep -E 'Sink:|media.role'`: `media.role = "x-maemo"` on pro1x_music's
   index. Listen: no gap at the start, at the end of a track, between
   tracks; a notification (an SMS, `pa paplay /tmp/pro1x-beep.wav`) during
   the music comes at once; the volume keys change the music within about
   a second; pause stops it within about a second; the volume bar shows
   the volume as before. Headphones in and out: the music follows (the UCM
   sink's port switches the codec path both front ends share).
5. **Before and after, display off,** the same MP3 looping in the media
   player at the same volume, 10 minutes each, from the phone's shell
   (as root):
   - before: `kill $(pidof pro1x-music-sink)` (the sink goes, the music
     moves to the UCM sink), then
     `pro1x-powerlog music-ucm 10 10 &` and halfway `pro1x-wakeups 60`;
   - after: `su user -c "XDG_RUNTIME_DIR=/run/user/$(id -u user) /usr/local/bin/pro1x-music-sink -v > /tmp/music-sink.log 2>&1 &"`
     (module-intended-roles moves the playing stream to the new sink;
     `pa pactl list sink-inputs | grep Sink:` to check), then
     `pro1x-powerlog music-deep 10 10 &` and halfway `pro1x-wakeups 60`.

   | 2026-10-31, display off, MP3 | before (UCM sink) | after (pro1x_music) |
   |---|---|---|
   | mean current (powerlog, last line) | | |
   | irq/s (powerlog mean) | | |
   | ctxt/s | | |
   | idle % | | |
   | CPU idle exits/s (pro1x-wakeups) | | |
   | the ADSP's glink interrupts/s (pro1x-wakeups, interrupts) | | |
   | pulseaudio and player wake-ups/s (pro1x-wakeups, tasks) | | |
6. **Back:** `touch /etc/pulse/pro1x-deep-buffer-off` (the next session
   loads no sink) or `PHONE=... bash scripts/72-audio-deep-buffer.sh
   restore`; the kernel as in "The way back" of `docs/kernel-update.md`.

Not done here: gPodder (`gpodder-leste`) plays through QtMultimedia
without a role, so it stays on the UCM sink; `audioRole:
MediaPlayer.MusicRole` in its `GPodderPlayback.qml`, or
`PULSE_PROP='media.role=music'` in its launcher, would put it on
pro1x_music (that package's repository). Video in the media player (also
x-maemo) plays on pro1x_music too; its A/V sync rests on PulseAudio's
latency, which moves in whole periods there (not tried).
