# FM radio on the Pro1X (Leste)

The Pro1X's Bluetooth chip, the WCN3950, has an FM receiver. Android drives
it from user space: the FM HAL writes Qualcomm's FM HCI commands to the
Bluetooth UART as H4 packets of their own type (0x11 commands, 0x14 events),
and the chip sends the audio digitally over SLIMbus to the DSP
(`SLIMBUS_8_TX`). Mainline Linux has none of this. The research behind the
plan, with sources: `~/ps/pro1x-fm-research.md` (not in this repository).

| Stage | What | State |
|---|---|---|
| 0 | Bluetooth comes up | **fixed in patch 0013, not yet booted** |
| 1 | FM commands and events over the Bluetooth UART | patch 0014, built, untested |
| 2 | V4L2 radio device `/dev/radio0` (tune, seek, signal, RDS) | patch 0015, built, untested |
| 3 | Audio: SLIMbus, WCN3950 codec, q6afe `SLIMBUS_8_TX` | not written (plan below) |
| 4 | Hildon app (`apps/pro1x-fm`) and a shell test (`tools/pro1x-fm`) | written, compile-checked |

Kernel `7.1.0-pro1x.4` (0013-0015, `CONFIG_RADIO_QCA_FM=m`) is built in
`$WORK/out-pro1x.4` and not on the phone.

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

## Test plan (needs the owner's OK and presence for the flash)

`PHONE` is the usual root ssh to Leste (`ssh -o UserKnownHostsFile=... root@172.16.42.1`).

1. **Prepare.** This writes files on the phone but no partition: the
   modules to `/host/pro1x-modules/7.1.0-pro1x.4`, the initramfs in Leste,
   and `/host/boot_b-7.1.0-pro1x.4.img`.

       PHONE="..." WORK=$WORK OUT=$WORK/out-pro1x.4 bash scripts/65-kernel-update.sh prepare

2. **Flash and reboot**, with the owner at the phone:

       PHONE="..." WORK=$WORK OUT=$WORK/out-pro1x.4 bash scripts/65-kernel-update.sh flash

   - This saves the current boot_b (7.1.0-pro1x.3) as
     `/host/boot_b-before-7.1.0-pro1x.4.img`, with a copy in
     `$WORK/out-pro1x.4`.
   - It puts slot b on trial (3 starts). It does not reboot; the owner does.
3. **Bluetooth (stage 0):** check that `uname -r` is `7.1.0-pro1x.4`, then:

       dmesg | grep -E 'QCA setup|reassembly|Reset failed|radio-qca-fm'

   - **Pass:** `QCA setup on UART is completed`, no `-84`, and
     `FM receiver as /dev/radio0`.
   - Then `hciconfig hci0 up; hciconfig hci0` shows UP RUNNING with BD
     address 02:50:31:E8:00:03. After that, `hciconfig hci0 down`.
   - The bluetooth service is still out of the runlevel, so hci0 goes down
     by itself 2 s after setup. That is normal.
4. **FM control (stages 1/2):**
   - Plug in headphones; the cable is the antenna.
   - Enable the debug output:

         echo 'module radio_qca_fm +p' > /sys/kernel/debug/dynamic_debug/control

   - Run the test tool:

         pro1x-fm-test -t 15 99.9     # a strong local station (Ö3 Vienna 99.9, FM4 103.8)
         pro1x-fm-test scan

   - **Pass:** signal well above 0 %, `stereo`, after a few seconds `RDS`
     and the station name (`OE3`, ...). `dmesg` shows RSSI lines.
   - **Failure to look for:** `command 0x4c01: -110` means the chip does not
     answer FM commands. The likely cause is that the FM core needs a
     power-on or vendor command first. Keep the dmesg for that case.
   - `v4l2-ctl -d /dev/radio0 --set-freq=99.9 --get-tuner --list-ctrls` is
     the second opinion.
5. **App:** build and install the app with the `pro1x-fm` lines of
   `scripts/49-apps.sh` (in Leste, as root). It then appears as
   "FM Radio"/"UKW-Radio" in the menu. Without stage 3 it tunes and shows the
   station, but plays no sound.
6. **Rollback:**
   - From Leste:

         TARGET=b IMG=/host/boot_b-before-7.1.0-pro1x.4.img VBMETA= sh scripts/60-flash-slot-b.sh flash

   - From fastboot:

         fastboot flash boot_b $WORK/out-pro1x.4/boot_b-before-7.1.0-pro1x.4.img

   - If the new kernel does not boot at all, the trial falls back to slot a
     (Sailfish OS) after 3 failed starts.

## Stage 3: the audio path (to do)

The receiver's audio exists only as SLIMbus data. Downstream (msm-4.19
bengal), the path is: WCN3950 slave ports TX1/TX2 (`btfm_slim`) → LPASS
SLIMbus NGD → AFE port `SLIMBUS_8_TX` (0x4011, 48 kHz, S16LE, 2 channels) →
a "SLIMBUS_8 Hostless" capture. On mainline this needs:

1. **DT:**
   - the LPASS SLIMbus NGD `slim@a5c0000` with the BAM at `0xa584000` and
     IRQs SPI 283 (controller) and 284 (BAM);
   - `qcom,slim-ngd-v1.5.0` or `v2.1.0` (unknown, try both);
   - children `ifd@0,0` and `codec@1,0` with compatible `slim217,220`;
   - the IOMMU stream ID, if one is needed (unknown).

   The NGD talks to the ADSP's SLIMbus QMI service.
2. **Codec:** a WCN399x SLIMbus codec. Start from Dylan Van Assche's
   `sound/soc/codecs/wcn3990.c` (sdm845-mainline MR !99, GPL-2.0+). It
   already has the FM TX ports. The WCN3950 has the same elemental address
   (`slim217,220`).
3. **q6dsp:** the ports `SLIMBUS_7_RX/TX` and `SLIMBUS_8_RX/TX` (AFE
   0x400e–0x4011) need to be added in these places:
   - `dt-bindings/sound/qcom,q6dsp-lpass-ports.h` (new IDs after 148);
   - `q6afe.c` (port map and SLIMbus config);
   - `q6dsp-lpass-ports.c` (DAI entries);
   - `q6routing.c` (as capture sources).
4. **Machine driver** (`sm8250.c`, compatible `fxtec,pro1x-sndcard`):
   - SLIMbus backends need the codec's channel map copied to the CPU DAI, as
     `sdm845.c` does;
   - a DAI link `SLIMBUS_8_TX` ↔ the codec's FM TX DAI ↔ q6routing.
5. **User space:** a UCM capture device for it. PulseAudio then has a
   source, and the app plays it through `module-loopback` (it looks for a
   source with "fm" in its name). The DSP's own loopback (hostless) would
   save power, but it is not in mainline q6.
6. After `4C01`, the chip needs `FC0E 01` (sent by the driver) to put audio
   on its SLIMbus port.

Main risks:
- the NGD version and the ADSP firmware's SLIMbus service on SM6115;
- the clock and power setup for SLIMbus in LPASS.

BT SCO would use the same SLIMbus (`SLIMBUS_7`, 8 kHz).

## FreeBSD (notes only, nothing written)

- **Bluetooth:** FreeBSD on the Pro1X has no Bluetooth yet. The BT UART, the
  WCN3950 firmware download (btqca) and the in-band sleep would come first,
  for example as a user-space daemon on the UART (`ng_h4`, or a daemon of
  its own).
- **FM control:** easiest in user space there. The commands of
  `radio-qca-fm.c` go over the same UART as 0x11/0x14 packets, from that
  daemon. There is no V4L2, so the FreeBSD app would talk to the daemon,
  and a Hildon app on FreeBSD would need that backend instead of
  `/dev/radio0`.
- **Audio:** this is the big part. FreeBSD needs these, none of which it has:
  - a SLIMbus NGD controller driver (QMI to the ADSP over QRTR);
  - the WCN codec's SLIMbus port setup;
  - AFE `SLIMBUS_8_TX` in its Q6 audio stack.
