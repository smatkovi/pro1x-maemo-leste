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
hardware: what can be checked off the phone is that it builds (every
module below does, and all their symbols resolve against the kernel and
each other), and the protocol code (GLINK, QRTR, QMI, the RPM's requests)
by host tests: `make -C freebsd/dsp/test` (under ASan and UBSan).

## Layers

| Layer | Linux | Here | For |
| --- | --- | --- | --- |
| Secure monitor calls: load and start signed firmware (PAS) | `qcom_scm` | `qcom_scm.c` | every DSP |
| Shared memory items (SMEM) and their hardware mutex (TCSR) | `qcom_smem`, `qcom_hwspinlock` | `qcom_smem.c` | all below |
| Doorbells to the other processors (APCS IPC) | `qcom_apcs_ipc` | `qcom_ipc.c` | SMP2P, GLINK |
| Bit states between processors (SMP2P) | `qcom_smp2p`, `smem_state` | `qcom_smp2p.c` | start, stop, crash |
| Remote processors (PAS) | `qcom_q6v5_pas`, `mdt_loader` | `qcom_pas.c` + `pro1x-dsp` (userspace loader) | ADSP, modem, CDSP |
| Message channels (GLINK over SMEM) | `qcom_glink_native`, `qcom_glink_smem` | `qcom_glink.c` | QRTR, APR, FastRPC |
| IPC router (QRTR) and its name service | `net/qrtr` | `qrtr_core.c`, `qrtr_sock.c` (socket family 45), `qrtr_glink.c` | QMI services |
| QMI (requests in TLV) | `qmi_interface`, `qmi_encdec` | `qmi_core.c`, `qmi_encdec.c` (in `qrtr`), LinuxKPI's `linux/soc/qcom/qmi.h` | ath10k |
| Votes at the RPM (regulators) over its GLINK edge in the RPM's message RAM | `qcom_glink_rpm`, `smd-rpm`, `qcom_smd-regulator` | `rpm_core.c`, `qcom_rpm.c` (in `qcom_dsp`) | the Wi-Fi's rails |
| Modem file system memory, shared with the modem through TrustZone | `rmtfs_mem`, `qcom_scm_assign_mem` | `qcom_rmtfs.c` | modem |
| Remote procedure calls to the DSPs | `fastrpc` | `qcom_fastrpc.c` | sensors |
| Linux platform drivers on the device tree | the driver core, `of`, `devres` | `lkpi_platform.c` (LinuxKPI's side), `lkpi_platform_bsd.c` (newbus), shim headers in `lkpi/`, `patches/0031` (`dev->of_node`) | ath10k |
| Wi-Fi (WCN3990 core through the modem) | `ath10k_snoc` | module `ath10k_snoc`: FreeBSD's ath10k (LinuxKPI) with Linux's `snoc.c`, `qmi.c`, `qmi_wlfw_v01.c` | Wi-Fi |
| APR, the packets of the ADSP's audio services | `apr` | `qcom_apr.c` (module `qcom_apr`, `/dev/apr`), `tools/pro1x-q6` (q6core; the SLIMbus QMI service) | the FM radio's sound (`../fm/README.md`) |
| Audio services on the ADSP (q6afe, q6asm), SLIMbus, LPASS, SoundWire, WCD937x | ASoC drivers, `qcom-ngd-ctrl` | not yet | audio |

Userspace, from Linux (`apps.sh` parts for FreeBSD, `hildon/patches/<part>`
for what differs): `qrtr` (library and `qrtr-lookup`), `rmtfs`, `tqftpserv`,
`pd-mapper`, `hexagonrpcd`, `libqrtr-glib`, `libqmi`, `libssc`.

The modem's side, as Leste runs it: `rmtfs -r -P` reads the modem's EFS
partitions (`/dev/gpt/modemst1`, `modemst2`, `fsg`, `fsc`) and keeps its
writes in memory, so the IMEI and calibration partitions are never written;
`tqftpserv` serves the modem files from the firmware directory (among them
`wlanmdsp.mbn`, the Wi-Fi's firmware); `pd-mapper` answers which DSP serves
which protection domain (the firmware's `.jsn` files; Linux has it in the
kernel). Each has an rc.d script (`hildon/extra/<part>`); the modem starts
after them (`pro1x-dsp start modem`).

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
| dsp6 | rmtfs, tqftpserv, pd-mapper, `pro1x-dsp start modem` (`pro1x.dsp=modem`, without the ADSP) | the region shared (`qcom_rmtfs: ... 0`), modem ready, its QMI services listed, `qmicli --dms-get-revision` answers |
| dsp7a | the RPM's edge, the Wi-Fi's rails voted on and off (`pro1x.dsp=rpm`, needs nothing else) | `qcom_rpm: message RAM 0x45f0000 (28 KiB), doorbell bit 0, FIFOs ...; channel N (M of Linux's requests seen)`, then `l8 (ldoa 8) on at 400000 uV: done` and the same for l16 (1704000), l17 (1152000), l23 (3200000), then `off: done`; `hw.qcom_rpm.refused` and `timeouts` 0 |
| dsp7b | dsp6, then ath10k (`pro1x.dsp=wifi`) | QRTR service 69 (WLFW) listed; `qcom_smmu: ath10k0: stream 0x1a0 ...`; ath10k's QMI log (MSA info, capabilities, board data, firmware ready); the rails on (as dsp7a); `ath10k0` in `net.wlan.devices` within 60 s; `wlan0` created; `ifconfig wlan0 scan` lists the networks around |
| dsp7c | dsp7b, then the network of `/host/freebsd/wpa_supplicant.conf` (`pro1x.dsp=wpa`) | `status: associated`, a DHCP lease on `wlan0`, the gateway answers `ping` (keys in the firmware: `compat.linuxkpi.80211.hw_crypto=1`, which the test and `rc.d/pro1x_wifi` set) |

## The Wi-Fi (dsp7)

The WCN3990 core's firmware (`wlanmdsp.mbn`) runs on the modem, which
fetches it through `tqftpserv`; the modem then offers WLFW, the firmware's
control service (QMI 69). On the application processor, ath10k does what
Linux's `ath10k_snoc` does: it finds WLFW, registers for its indications,
hands over the MSA memory and the board data (`board-2.bin`, entry for the
device tree's `qcom,calibration-variant`, else the chip's generic one, as
Linux), and once the firmware is ready it powers the rails, sends the copy
engines' layout (`WLAN_CFG`) and switches the firmware on (`WLAN_MODE`);
from there on it is ath10k over its copy engines (HTC, WMI-TLV, HTT) as on
PCI, below LinuxKPI's mac80211 and net80211.

Kernel modules (all built by `build.sh`, `out/modules` with `linker.hints`):
`firmware`, `usb` (LinuxKPI depends on both), `linuxkpi`, `wlan`,
`linuxkpi_wlan`, `athk_common`, `wlan_wep`, `wlan_tkip`, `wlan_ccmp`,
`wlan_gcmp` (the ciphers wpa_supplicant's rc.d script loads), and
`ath10k_snoc` (`sys/modules/ath10k_snoc`): ath10k's core as
`sys/modules/ath10k` builds it, Linux's SNOC attachment and QMI client
instead of PCI, with `CONFIG_OF` (the device tree parts of the core: the
calibration variant, `qcom,coexist-support`, `ext-fem-name`). The kernel
has `device pci` (no host bridge; LinuxKPI depends on it and keeps its DMA
functions there). The pieces in between:

- **Platform driver** (`lkpi_platform*.c`): `module_platform_driver`
  adds a newbus driver on simplebus named after the Linux driver up to its
  first underscore, so the WCN3990's node `wifi@c800000` attaches as
  `ath10k0` (the name net80211 and devd know). The `platform_device` is the
  softc, set up as LinuxKPI sets up a PCI device (kobject, DMA tags of
  `patches/0021`, devres); `dev->of_node` (`patches/0031`) points to a node
  object behind the device tree's phandle. Resources by `reg-names` and
  index; the twelve copy-engine interrupts (SPI 358-369) as numbers from
  `0x40000000`, `IRQF_NO_AUTOEN` and `disable_irq`/`enable_irq` with
  Linux's semantics (arm64's nexus cannot mask one interrupt, so the handler
  is torn down and set up again); `devm_ioremap`, `devm_memremap` (the MSA
  region `memory@51900000`, 1 MiB, as device memory: Linux's arm64
  `ioremap_wt` is `ioremap`).
- **SMMU:** before the probe, the node's `iommus` (`<&apps_smmu 0x1a0
  0x1>`) is switched to translation off (`qcom_smmu.c`), so the chip's DMA
  addresses are physical. No `wifi-firmware` child in the device tree:
  ath10k leaves the MSA to TrustZone (`qcom,msa-fixed-perm`: no SCM call).
- **Regulators:** `vdd-0.8-cx-mx`, `vdd-1.8-xo`, `vdd-1.3-rfa`,
  `vdd-3.3-ch0` are the PM6125's L8, L16, L17, L23 (their phandles in the
  device tree); `vdd-3.3-ch1` is absent, a dummy as on Linux. They go on
  at ath10k's power-up as Linux's `qcom_smd-regulator` does it the first
  time: `swen` 1 and `uv` the node's `regulator-min-microvolt` (Linux's
  regulator core sets a range's minimum before the first enable). The boot
  menu kexecs from the initramfs, where `ath10k_snoc` (a module) never
  probed: nothing has voted for them, so FreeBSD has to. `qcom_rpm.c` sends
  the requests on Linux's channel `rpm_requests`, which stays open on the
  RPM's side across the kexec (no OPEN; its id is found in the requests
  Linux left in the transmit FIFO), and polls for the answer.
  `hw.lkpi_pf.regulators=0` (command line) leaves them alone.
- **QMI:** ath10k connects its handle to the server it found
  (`kernel_connect`) and sends requests without an address; `qmi_connect`
  keeps the peer in the handle. LinuxKPI's stub `kernel_connect` is
  replaced by that in our `linux/soc/qcom/qmi.h`.
- **Firmware files:** LinuxKPI's `request_firmware` reads
  `/boot/firmware/ath10k/WCN3990/hw1.0/firmware-5.bin` and `board-2.bin`;
  `/boot/firmware/ath10k` points to `rootfs.sh`'s copy in
  `/usr/local/share/pro1x/firmware/ath10k`.
- **Small shims:** `of_*` accessors, `MODULE_DEVICE_TABLE(of, ...)`,
  `netif_threaded_enable` (LinuxKPI's NAPI is threaded anyway), `msleep`
  through `net/sock.h` (else FreeBSD's `msleep(9)` macro), ACPICA's two
  functions ath10k links against (`lkpi_noacpi.c`, never called: no ACPI
  handle), SMEM and SCM with Linux's meanings.

Userland: `rc.d/pro1x_modem` (`pro1x-dsp start modem` after `tqftpserv`,
`pd_mapper`, `rmtfs`) and `rc.d/pro1x_wifi` (loads the modules in order,
waits for `ath10k0`, then does what devd does for a card that attached:
`/etc/pccard_ether ath10k0 startchildren`, i.e. `wlans_ath10k0="wlan0"`,
`ifconfig_wlan0="WPA DHCP"`). devd's own attach event comes too early: the
newbus device attaches before ath10k has talked the firmware up and
registered with net80211. All of it is off in `rc.conf` until the device
tests pass. wpa_supplicant reads `/host/freebsd/wpa_supplicant.conf` on
userdata (`/dev/pro1x-host/freebsd/...` under FreeBSD), written from
Linux, never part of an image; resolvconf writes `/var/run/resolv.conf`,
where `/etc/resolv.conf` points (the root is read only).

The keys go into the firmware, as with ath10k on Linux:
`compat.linuxkpi.80211.hw_crypto=1` (LinuxKPI's default is net80211's
ciphers in software, which ath10k's native Wi-Fi mode does not carry),
set by `rc.d/pro1x_wifi` and the test before `linuxkpi_wlan` loads.

Test runs: `pro1x.test=dsp pro1x.dsp=rpm|wifi|wpa` in
`/host/freebsd/cmdline` (wifi and wpa with ath10k's debug for BOOT,
SNOC and QMI, `compat.linuxkpi.debug_mask=0x300020`, unless the command
line sets it; reading `hw.qcom_rpm.edge` brings the RPM's edge up). Modules in `/host/freebsd/modules` (a copy of
`out/modules`, matching `/host/freebsd/kernel.bin`) are found before the
root image's (`kldconfig -i`), so a new module build needs no new root
image.

## Risks

- **SMMU:** FreeBSD has no driver for the SoC's ARM SMMU (MMU-500).
  FastRPC's buffers and the Wi-Fi chip's memory sit behind its context
  banks. The display works with its context bank switched to bypass; if
  Qualcomm's hypervisor does not allow that for the DSPs' context banks,
  sensors and Wi-Fi are blocked in this approach (run dsp2 tells).
- **RPM votes:** Linux votes the DSPs' power domains (CX, MX) and clocks at
  the RPM. FreeBSD inherits whatever Linux left voted at kexec; it votes
  only regulators so far (`qcom_rpm.c`). If a DSP does not come up, its
  power domains are next (resource types `rwcx`, `rwmx` of Linux's
  `rpmpd`, the same requests).
- **The RPM's channel:** the requests go on the channel id Linux used,
  found in what it left in the FIFO (`hw.qcom_rpm.lcid` overrides it). If
  the RPM wants a GLINK handshake after all, dsp7a shows no answers
  (`timeouts`), and an OPEN of `rpm_requests` is next.
- **Wi-Fi without a modem restart:** ath10k expects the firmware's
  `FW_READY` after its board data; a modem started by FreeBSD (dsp6) sends
  it as after a cold boot. A modem that crashes is not passed on to ath10k
  (no SSR notifications yet).
- **No network to debug:** every test round is a reboot into FreeBSD and
  back.
