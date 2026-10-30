# Audio on the Pro1X (Leste)

The playback path: PulseAudio opens `hw:Pro1X,0` (MultiMedia1, q6asm), the
DSP routes it to `RX_CODEC_DMA_RX_0`, the LPASS RX macro sends it over
SoundWire to the WCD9370, whose headphone PA (HPHL/HPHR) drives both the
headphone jack and two Awinic AW87339 class-K amplifiers (I2C 0x59 and
0x5a, enable lines GPIO 85 and 98, `Spk0`/`Spk1` in the mixer) for the
loudspeakers. The UCM profile is the NixOS port's (`Speaker` switches
`Spk0 OUT Switch`/`Spk1 OUT Switch` on, `Headphones` off); PulseAudio runs
at 48 kHz, S16 (`rootfs-overlay/etc/pulse/daemon.conf.d/50-pro1x.conf`).

## The loudspeaker pop (patch 0026, 7.1.0-pro1x.8, untested)

Every power-up of the card popped in the loudspeaker, so
`rootfs-overlay/etc/pulse/default.pa.d/50-pro1x.pa` lets PulseAudio keep
the card open for 300 s after a sound instead of 5 s. That costs about
100 mA and 400 interrupts a second for those 5 minutes and keeps
`pro1x-autosuspend` from suspending (`docs/power-management.md`).

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

## Test on the phone (with 7.1.0-pro1x.8)

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
1. **After the flash:** `uname -r` is 7.1.0-pro1x.8; `cat /proc/asound/cards`
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

## The PulseAudio switch (after the test passes)

`rootfs-overlay/etc/pulse/default.pa.d/50-pro1x.pa` has the 3 s line
prepared as a comment: make it the active line and comment out the
`timeout=300` one, then on the phone

    scp rootfs-overlay/etc/pulse/default.pa.d/50-pro1x.pa root@172.16.42.1:/etc/pulse/default.pa.d/

and PulseAudio's next start (a reboot) uses it. The README's audio row and
the 300 s notes in `docs/power-management.md` go with it.
