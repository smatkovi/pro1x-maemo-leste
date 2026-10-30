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
