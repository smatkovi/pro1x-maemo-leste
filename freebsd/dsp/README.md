# The Qualcomm DSPs under FreeBSD (sensors, Wi-Fi, modem, audio)

On the Pro1X's SM6115 the radios, the audio and the sensors are not
devices of the application processor: they are services of the SoC's
other processors (the modem's Hexagon DSP, the audio DSP "ADSP", the
compute DSP "CDSP"), reached through shared memory. Linux has the whole
stack for that (Linaro and Qualcomm, about ten years of mainline work);
FreeBSD has none of it. This directory builds it for FreeBSD, as kernel
modules and ports of the Linux userspace daemons, modelled on Linux's
drivers (their files keep Linux's GPL-2.0 licence).

Nothing here has run on the phone yet. QEMU emulates none of this
hardware: what can be checked off the phone is that it builds, and the
protocol code (GLINK, QRTR, QMI) by host tests against itself.

## Layers

| Layer | Linux | Here | For |
| --- | --- | --- | --- |
| Secure monitor calls: load and start signed firmware (PAS) | `qcom_scm` | `qcom_scm.c` | every DSP |
| Hardware mutex (TCSR) | `qcom_hwspinlock` | `qcom_hwlock.c` | SMEM |
| Shared memory items (SMEM) | `qcom_smem` | `qcom_smem.c` | all below |
| Bit states between processors (SMP2P) | `qcom_smp2p`, `smem_state` | `qcom_smp2p.c` | start, stop, crash |
| Remote processors (PAS) | `qcom_q6v5_pas`, `mdt_loader` | `qcom_pas.c` + `pro1x-dsp` (userspace loader) | ADSP, modem, CDSP |
| Message channels (GLINK over SMEM) | `qcom_glink_native`, `qcom_glink_smem` | `qcom_glink.c` | QRTR, APR, FastRPC |
| IPC router (QRTR) and its name service | `net/qrtr` | `qrtr.c` (socket family) | QMI services |
| QMI (requests in TLV) | `qmi_interface`, `qmi_encdec` | LinuxKPI `qmi.c` | ath10k |
| Modem file system memory | `rmtfs_mem` | `qcom_rmtfs.c` | modem |
| Remote procedure calls to the DSPs | `fastrpc` | `qcom_fastrpc.c` | sensors |
| Wi-Fi (WCN3950 through the modem) | `ath10k_snoc` | FreeBSD's ath10k (LinuxKPI) + SNOC attachment | Wi-Fi |
| Audio services on the ADSP (APR, q6afe, q6asm), LPASS, SoundWire, WCD937x | ASoC drivers | not yet | audio |

Userspace, from Linux (`apps.sh` parts for FreeBSD): `qrtr` (tools and
library), `rmtfs`, `tqftpserv`, `pd-mapper`, `hexagonrpcd`, `libssc`.

## Order

1. The substrate up to a running ADSP: SCM, mutex, SMEM, SMP2P, PAS,
   the loader. Test: the ADSP reports "ready" over SMP2P.
2. GLINK and QRTR: the ADSP's services appear in the name service
   (`qrtr-lookup`).
3. Sensors: FastRPC, hexagonrpcd, libssc (step counter, accelerometer).
4. Modem: rmtfs, tqftpserv, pd-mapper, the modem started; its QMI
   services answer. Data (IPA) and calls are not planned (no SIM in the
   test phone; Leste cannot test calls either).
5. Wi-Fi: ath10k over SNOC, once the modem runs (the WLAN firmware is
   part of it).
6. Audio: last, not promised.

## Firmware

The DSPs' firmware (`qcom/sm6115/Fxtec/QX1050/*.mbn`, signed by the phone's
maker) is not in this repository or its releases. `rootfs.sh` copies it from
the local Leste image (`out/maemo-leste.img`, `/lib/firmware`) into the root
image. A loader in userspace (`pro1x-dsp`) reads it and hands the segments
to the kernel: the modem's firmware alone is 51 MB, too big to be a kernel
module.

## Tests on the phone

FreeBSD on the Pro1X has no USB and no network, so every test is an
unattended run (`pro1x.test=dsp` in `/host/freebsd/cmdline`): the rc
script loads the modules in order, writes what happened to the console and
reboots; the watchdog reboots on a hang. Leste reads the RAM console after
the next boot (`fbsdtest.sh`). One layer per run:

| Run | Does | Expected |
| --- | --- | --- |
| dsp1 | `kldload qcom_dsp`: SCM, mutex, SMEM, SMP2P attach | SMEM partitions listed, SMP2P entries of the ADSP found |
| dsp2 | SMMU probe: the ADSP's FastRPC context bank set to bypass | context bank reads back as bypass |
| dsp3 | `pro1x-dsp start adsp` | PAS init, mem setup, auth and reset OK; "ready" over SMP2P |
| dsp4 | GLINK on the ADSP's edge, QRTR | the ADSP's channels open; its services in `qrtr-lookup` |
| dsp5 | FastRPC, hexagonrpcd, libssc (`pro1x.dsp=sensors`) | the sensor registry served, QRTR service 400, accelerometer values, a step count |
| dsp6 | rmtfs, tqftpserv, pd-mapper, `pro1x-dsp start modem` | modem ready, its QMI services listed |
| dsp7 | ath10k | `wlan0` scans |

## Risks

- **SMMU:** FreeBSD has no driver for the SoC's ARM SMMU (MMU-500).
  FastRPC's buffers and the Wi-Fi chip's memory sit behind its context
  banks. The display works with its context bank switched to bypass; if
  Qualcomm's hypervisor does not allow that for the DSPs' context banks,
  sensors and Wi-Fi are blocked in this approach (run dsp2 tells).
- **RPM votes:** Linux votes the DSPs' power domains (CX, MX) and clocks at
  the RPM. FreeBSD has no RPM driver and inherits whatever Linux left
  voted at kexec. If a DSP does not come up, an SMD-RPM driver is next.
- **No network to debug:** every test round is a reboot into FreeBSD and
  back.
