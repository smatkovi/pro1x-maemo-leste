# FM radio on the Pro1X (Leste)

The Pro1X's Bluetooth chip, the WCN3950, has an FM receiver. Android drives
it from user space: the FM HAL writes Qualcomm's FM HCI commands to the
Bluetooth UART as H4 packets of their own type (0x11 commands, 0x14 events),
and the chip sends the audio digitally over SLIMbus to the DSP
(`SLIMBUS_8_TX`). Mainline Linux has none of this. The research behind the
plan, with sources: `~/ps/pro1x-fm-research.md` (not in this repository).

| Stage | What | State |
|---|---|---|
| 0 | Bluetooth comes up | patch 0013, **works** (7.1.0-pro1x.4) |
| 1 | FM commands and events over the Bluetooth UART | patch 0014, **works** |
| 2 | V4L2 radio device `/dev/radio0` (tune, seek, signal, RDS) | patches 0015, 0017, **tune and seek work** (no antenna yet) |
| 3 | Audio: SLIMbus, WCN3950 codec, q6afe `SLIMBUS_8_TX` | patches 0018-0021 since 7.1.0-pro1x.6 (in pro1x.9 for the 2026-10-31 flash), built, not booted |
| 4 | Hildon app (`apps/pro1x-fm`), shell tests (`tools/pro1x-fm`) | written, compile-checked |

## Stage 0: why Bluetooth failed since 7.1.0-pro1x.1

Every boot of pro1x.1, .2 and .3 shows:

    QCA Downloading qca/cmnv12.bin
    Frame reassembly failed (-84)
    command 0x0c03 tx timeout
    QCA Reset failed (-110)

The kernel log shows it began with pro1x.1 (`grep` of `/var/log/kern.log`):
every boot of `7.1.0+` before it completed setup. Patch 0008 lowered the
UART from 3.2 to 3 Mbaud. btqca writes the host's rate into the NVM's HCI
transport tag (17), at byte 1 for the WCN3991 and later and at byte 2 for
older chips. The WCN3950 counts as older, but its `cmnv*.bin` has the
WCN3991 layout of that tag:

    cmnv12.bin, cmnv13*.bin, crnv32*.bin:  15 11 22 00 2e 00
    crnv21.bin, apnv1*.bin:                82 01 11 08 04 0a 28 0x 15

So the rate byte stayed `0x11` (3.2 Mbaud). After the download, `HCI_RESET`
makes the chip apply its NVM and switch to 3.2 Mbaud while the host stays at
3 Mbaud. From then on everything arrives garbled. At 3.2 Mbaud the stale
byte happened to match. Patch 0013 writes the rate into byte 1 for the
WCN3950 too, which keeps 0008's 3 Mbaud.

If the patch is wrong, the fallback is `max-speed = <3200000>` in the DT.

The retries afterwards ("Reading QCA version information failed") all fail
for a second reason. The chip shares its supply rails with Wi-Fi, so
"power off" does not reset it: it stays at 3.2 Mbaud while the host starts
again at 115200. A `qcom,wcn3950-pmu` power sequencer (as on the RB2, with
`swctrl-gpios = <&tlmm 87>`) would address that. It is not done.

## Stages 1 and 2: the control path

- **Patch 0014 (hci_qca):**
  - accepts 0x14 events and hands them to an attached FM driver; they never
    reach the Bluetooth core;
  - sends 0x11 commands through the same queue as HCI frames, including
    in-band sleep;
  - populates the `fm` child node of the Bluetooth node;
  - `qca_fm_get()` brings hci0 up (`hci_dev_open()`, now exported) when it
    is down, and `qca_fm_put()` takes it down again.
- **Patch 0015 (radio-qca-fm):** a V4L2 radio device with these parts:
  - tuner range 87.5–108 MHz on a 100 kHz grid, stereo/mono;
  - signal from the chip's RSSI (-120 dBm is 0 %, -80 dBm is 100 %);
  - hardware seek;
  - mute and de-emphasis (50/75 µs) controls;
  - RDS PTY, station name and radio text as the standard read-only RDS
    controls, from the chip's own RDS decoding (events 0x09/0x0a).

  The device tree gets `bluetooth { fm { compatible = "qcom,wcn3950-fm"; }; }`.
  The module loads by its OF alias but sends nothing until `/dev/radio0` is
  opened.
- **Power:** the receiver is on from the first open to the last close. Each
  open with Bluetooth down costs one firmware download, about 1 s.
- **Command set:** taken from the GPL `radio-iris` and the BSD-licensed
  helium FM HAL (LineageOS `android_vendor_qcom_opensource_fm-commonsys`). The
  proprietary QTI Bluetooth HAL was used only to confirm the wire format.

  Enable sequence: `4C01` RX on, `4C04` config, `4C06` stereo, `4C16` event
  mask 0x07, `4C13` RDS processing (RT|PS|PS simple), `4C05` mute, `5401`
  tune (kHz, then a tune-status event 0x01), `FC0E` SLIMbus port on.

## Tests on the phone

**7.1.0-pro1x.4 (2026-10-29), without headphones:**
- Bluetooth: `QCA setup on UART is completed`, hci0 UP RUNNING.
- `pro1x-fm-test 99.9`: the receiver is on after 0.1 s, and RSSI and the
  RDS sync flag come back.
- Seek: with no station above the threshold, the chip goes round the band
  in about 7 s and comes back to where it started (Search Complete, then a
  tune status). The driver reported that as success, so `scan` ended after
  one line. With patch 0017 it returns `ENODATA` ("no station found"); a
  rebuilt module was tried on the phone. The chip's own seek threshold is
  at its lowest (0 of 0-3).
- The module parameter `debug` (`/sys/module/radio_qca_fm/parameters/debug`)
  logs every FM command and event, because Leste has no dynamic debug.

## Stage 3: the audio path (patches 0018-0021, 7.1.0-pro1x.6)

Downstream (msm-4.19 bengal), the receiver's audio goes this way:
WCN3950 slave ports TX1/TX2 (`btfm_slim`) → LPASS SLIMbus NGD → AFE port
`SLIMBUS_8_TX` (0x4011, 48 kHz, S16LE, 2 channels) → a "SLIMBUS_8
Hostless" capture. The mainline version:

- **0018, q6dsp:** the AFE ports `SLIMBUS_7_RX/TX` and `SLIMBUS_8_RX/TX`
  (IDs 149-152, AFE 0x400e-0x4011), with DAIs, DAPM and routing mixers.
  The SLIMbus DAIs now tell TX from RX by their stream, not by the
  parity of their ID.
- **0019, the codec `wcn399x`** (`qcom,wcn3950-slim-audio`):
  - FM ports 1 and 2 on channels 160/161, with the register writes of
    downstream `btfm_slim`;
  - a platform device that looks the chip's SLIMbus devices up only when a
    stream starts, so the sound card never waits for SLIMbus (the NGD
    registers its bus only once the ADSP has brought it up);
  - the MR !99 draft did not compile, so the driver is new.
- **0020, sm8250 machine:** SLIMbus back ends copy the codec's channel
  map to the DSP port, as `sdm845.c` does.
- **0021, DT:**
  - NGD `slim-ngd@a5c0000` (`v1.5.0`; v1.5 and v2.1 have the same register
    layout in mainline, NGD 1 at +0x1000 as downstream) and its BAM at
    `0xa584000`, without an IOMMU stream, as downstream;
  - the WCN3950 `ifd@0,0`/`codec@1,0` (`slim217,220`);
  - the "FM Capture" link: SLIMBUS_8_TX → q6routing.
- **User space:** the app sets the mixer `MultiMedia3 Mixer SLIMBUS_8_TX`.
  It opens that front end (`hw:Pro1X,2`, which the UCM profile does not
  use) as the PulseAudio source `fm_radio` and loops it back to the
  default sink. The radio driver already sends `FC0E 01`, which puts the
  receiver's audio on its SLIMbus ports.

What can still go wrong, in the order the test will show it:
1. The ADSP has no SLIMbus QMI service, or the NGD does not come up:
   `qcom,slim-ngd` errors in dmesg; no `/sys/bus/slimbus/devices`.
2. SMMU: the BAM's DMA is blocked. The kernel has
   `CONFIG_ARM_SMMU_DISABLE_BYPASS_BY_DEFAULT=y`, so it blocks unmatched
   streams and logs their ID: "Blocked unknown Stream ID 0x...". That ID
   goes into `iommus` of `slimbam` and the NGD, and the next kernel is
   built with it.
3. The WCN3950 does not report present (no `217:220:...` device after
   the radio is on). The chip may need SLIMbus clocking or more than
   `FC0E` first.
4. The stream starts but stays silent: the channel numbers or the port
   registers.

Not done: Bluetooth SCO/A2DP over SLIMBUS_7, the NGD's missing
`disable_stream` (an upstream patch exists; it only matters when the rate
changes), and the DSP's own loopback (hostless), which would save power.

## Test plan for 7.1.0-pro1x.9 (2026-10-31, with the owner)

Written for pro1x.6; the flash on 2026-10-31 is pro1x.9, which contains it.
Kernel: `$WORK/out-pro1x.9` = 0001-0027: pro1x.5 (0001-0017), the audio
path 0018-0021, pro1x.7's power patches 0022-0025, the loudspeaker
change 0026 and the headset jack's thresholds 0027 (`docs/audio.md`). It is the same prepare and flash as the
notify_rpm test in `docs/power-management.md`: once. `PHONE` is the usual
root ssh to Leste.

0. **Without a flash:** run `qrtr-lookup` on the phone. The NGD needs the
   ADSP's SLIMbus QMI service 769 (0x301), version 1. If the ADSP does
   not list it, pro1x.9 cannot give FM sound either.

1. **Prepare.** Writes files on the phone, no partition:

       PHONE="..." WORK=$WORK OUT=$WORK/out-pro1x.9 bash scripts/65-kernel-update.sh prepare

2. **Flash**, with the owner there:

       PHONE="..." WORK=$WORK OUT=$WORK/out-pro1x.9 bash scripts/65-kernel-update.sh flash

   - This saves the current boot_b as `/host/boot_b-before-7.1.0-pro1x.9.img`.
   - Slot b is then on trial for 3 starts. The owner reboots.
3. **Check that nothing else broke:**
   - `uname -r`;
   - `cat /proc/asound/cards; lsmod | grep -E 'wcn399x|slimbus'`: the card
     now needs the `snd-soc-wcn399x` component. If the card is missing,
     run `modprobe snd-soc-wcn399x` first, before rolling back;
   - `aplay -l` and a sound on the speaker;
   - the microphone;
   - `dmesg | grep -i -E 'smmu|fault'`.
4. **FM sound,** with headphones plugged in (the cable is the antenna):
   1. Copy the test over:

          scp tools/pro1x-fm/pro1x-fm-audio-test root@172.16.42.1:/usr/local/bin/

   2. Run it:

          pro1x-fm-audio-test 99.9 10

   3. **Pass:** `/sys/bus/slimbus/devices` lists the WCN3950, `arecord`
      runs, and the recording is "sound", not "silent".
   4. Listen with `paplay /tmp/fm.wav`, then try the app.
5. **Rollback:**
   - From Leste:

         TARGET=b IMG=/host/boot_b-before-7.1.0-pro1x.9.img VBMETA= sh scripts/60-flash-slot-b.sh flash

   - From fastboot: flash
     `$WORK/out-pro1x.9/boot_b-before-7.1.0-pro1x.9.img` into boot_b.
   - If the new kernel does not boot at all, the trial falls back to slot a
     (Sailfish OS) after 3 failed starts.

## FreeBSD

Written, built and tested on the PC, not yet on the phone:
`freebsd/fm/README.md`.

- **Bluetooth UART:** `qcom_geni_uart.ko`, the GENI UART as a uart(4)
  class (`/dev/cuau0`), with the serial engine's clock voted and its pins
  muxed itself, since FreeBSD has no clock or pin control driver here.
- **The chip and FM control:** `pro1x-fmd` does in user space what
  hci_qca/btqca (with patch 0013) and radio-qca-fm do here: power pulses,
  download, in-band sleep, then the FM commands; the app and
  `pro1x-fmctl` talk to it over a Unix socket instead of `/dev/radio0`.
- **Audio:** `pro1x-fmaudio` programs Android's `play-fm` path from user
  space: the SLIMbus NGD as the ADSP's satellite over its message
  registers (no BAM DMA), the WCN3950's FM ports and channels 160/161,
  then over `/dev/apr` the AFE's SLIMBUS_8_TX, RX_CODEC_DMA_RX_0, the AFE
  loopback between them and the LPASS clock votes; `qcom_lpass.ko` maps
  the LPASS registers for it. Host tested, device runs `fm-slim` and
  `fm-afe` prepared. The output behind RX_CODEC_DMA_RX_0 (RX macro,
  SoundWire, WCD9370) is not written yet: nothing to hear.
- **App:** `apps/pro1x-fm` builds for FreeBSD's Hildon (`pro1x-fm` part)
  and switches the sound path with pro1x-fmd's `audio on|off`.
