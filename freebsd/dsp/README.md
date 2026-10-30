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
each other), the protocol code (GLINK, QRTR, QMI, the RPM's requests) by
host tests, `make -C freebsd/dsp/test`, and the whole start of the ADSP
and the modem, the drivers' own files against simulated hardware and
remote processors: `make -C freebsd/dsp/test sim` (both under ASan and
UBSan; "The simulation" below).

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
| Audio services on the ADSP (q6afe, q6asm, q6adm, q6voice), LPASS, SoundWire, WCD937x | ASoC drivers | `../audio`: `snd_pro1x.ko` (sound(4)), `pro1x-audiod` (every APR packet, the codec's sequences); the SLIMbus NGD in `../fm` | audio |

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
   services answer. Calls, SMS and GPS on top: `../modem/README.md`;
   mobile data (the IPA): `../data/README.md`.
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
unattended run (`pro1x.test=dsp pro1x.dsp=RUN` in `/host/freebsd/cmdline`):
`rootfs/usr/local/libexec/pro1x-dsptest` loads the modules in order,
writes what happened to the console and reboots; the watchdog reboots on
a hang. Leste reads the RAM console after the next boot (`fbsdtest.sh`).
One layer per run, each on the ones below it.

Building and putting a run in place (`build-dsp.sh`: under
`$WORK/.kbuild-lock`, into `$WORK/dsp-test`, never `out/`):

    WORK=$WORK bash freebsd/dsp/build-dsp.sh        # modules, tools, cmdlines
    # to userdata, next to the kernel (Leste: /host/freebsd)
    cp $WORK/dsp-test/modules/*.ko   /host/freebsd/modules/
    cp $WORK/dsp-test/dsp-tools/*    /host/freebsd/dsp-tools/
    cp $WORK/dsp-test/cmdline.dsp-adsp /host/freebsd/cmdline

The modules are built against the kernel in `$WORK/obj`, which the script
copies along as `$WORK/dsp-test/kernel.bin`; when it says that kernel
differs from `out/kernel.bin` (other drivers' newer sources), that one goes
to `/host/freebsd/kernel.bin` with the modules. `pro1x-dsptest` runs a newer copy of itself from
`freebsd/dsp-tools` (as the camera test does; the root image's copy must
be this one or later for that), and `pro1x-dsp` from there first.

Every check prints a line `PASS dsp-RUN: ...` or `FAIL dsp-RUN: ...`;
every wait is bounded by what the watchdog leaves
(`hint.qcom_wdt.0.lifetime`, counted from the boot, less 25 s for the
summary and `rc.local`'s reboot) and prints how long it took; the state is
dumped after each layer (`hw.qcom_smp2p`: the SMP2P entries and their
values; `hw.qcom_glink`: the FIFOs, the negotiation, the channels with
their ids and intents; `net.qrtr.nodes`, `net.qrtr.servers`); the last line
is the run's summary,
`=== dsp-RUN: PASS|FAIL (N passed, M failed; first failure: ...)`.
The cmdline files set the watchdog's lifetime per run.

| Run | Does | Expected (lines from the simulation's console) |
| --- | --- | --- |
| dsp1 `load` (120 s) | `kldload qcom_dsp qrtr`: SCM, SMEM, SMP2P, PAS, rmtfs's memory | `qcom_dsp: secure monitor answers (0)`; `qcom_smem: 0x46000000, 2048 KB, 512 items` and a partition with hosts 1, 2, 5; three `qcom_smp2pN: pid 0 <-> 1/2/5`; `qcom_pasN: adsp: PAS id 1 supported (0)` (modem 4, cdsp 18); `qcom_rmtfs: 0x89b01000+0x200000 shared with VMIDs 15,43: 0`; `hw.qcom_socinfo` `soc_id 417` |
| dsp3, dsp4 `adsp` (180 s) | pd-mapper first (service 64 listed), `pro1x-dsp start adsp`, its GLINK edge and QRTR, stopped again | `adsp: authenticated, out of reset; waiting 5000 ms for ready`, `adsp: ready after N ms`, `handover after N ms`, `glink edge lpass up`, `qrtr_glink: lpass up`, the ADSP's services at node 5 in `net.qrtr.servers`, `hw.qcom_glink`: `lpass ... remote sent VERSION`, `IPCRTR lcid 1 rcid N up held`; at the stop `adsp: stop-ack after N ms` |
| dsp5 `sensors` (300 s) | the ADSP left running: FastRPC, hexagonrpcd, libssc | `/dev/fastrpc-adsp`, QRTR service 400, accelerometer values, a step count |
| dsp6 `modem` (300 s) | tqftpserv, pd-mapper, rmtfs until services 4096, 64, 14 are listed (node 1), then `pro1x-dsp start modem` (without the ADSP) | `modem: ready after N ms`, `qrtr_glink: mpss up`, DMS (2) and the others at node 0 (in the simulation 2, 3, 5, 9, 11, 69), `qmicli --dms-get-revision` answers |
| dsp6v `modemcx` (300 s) | dsp6 with Linux's proxy vote for CX (`hw.qcom_pas.proxy_votes=1`); after dsp7a passed | also `modem: proxy vote CX (rwcx 0) on, level 416: done (0 0 0)` |
| dsp6a `adopt` (300 s) | FreeBSD kexec'd from a running Leste (`dsp/tools/pro1x-kexec-freebsd.sh`): the modem taken over, not started ("The handover") | `remote running: our values kept`; `qcom_rmtfs: again, from VMIDs 3,15,43 (shared before?): 0`; `modem: adopted, running (SMP2P bits 0x6)`; `glink edge mpss: IPCRTR: Linux's id N (QRTR data, to node 0), the remote's M (QRTR data, from node 0)`; `qrtr_glink: mpss up (Linux's channel adopted)`; the modem's services listed again, DMS answers |
| dsp7a `rpm` (120 s) | the RPM's edge, the Wi-Fi's rails voted on and off (`pro1x.dsp=rpm`, needs nothing else) | `qcom_rpm: message RAM 0x45f0000 (28 KiB), doorbell bit 0, FIFOs ...; channel N (M of Linux's requests seen)`, then `l8 (ldoa 8) on at 400000 uV: done` and the same for l16 (1704000), l17 (1152000), l23 (3200000), then `off: done`; `hw.qcom_rpm.refused` and `timeouts` 0 |
| dsp7b `wifi` (420 s) | dsp6, then ath10k | QRTR service 69 (WLFW) listed; `qcom_smmu: ath10k0: stream 0x1a0 ...`; ath10k's QMI log (MSA info, capabilities, board data, firmware ready); the rails on (as dsp7a); `ath10k0` in `net.wlan.devices` within 60 s; `wlan0` created; `ifconfig wlan0 scan` lists the networks around |
| dsp7c `wpa` (480 s) | dsp7b, then the network of `/host/freebsd/wpa_supplicant.conf` | `status: associated`, a DHCP lease on `wlan0`, the gateway answers `ping` (keys in the firmware: `compat.linuxkpi.80211.hw_crypto=1`, which the test and `rc.d/pro1x_wifi` set) |

Where it stops tells the layer: no `secure monitor answers` (SCM),
`not initialised by the boot loader` (SMEM), `PAS init image: N`,
`PAS mem setup: N`, `PAS auth and reset: N` (TrustZone refused the image
or its place), `no ready (35) after 5000 ms, SMP2P bits 0x0` (the DSP never
ran: power, clocks; next: dsp6v, or the handover), `fatal error: ...`
(its reason from SMEM), `remote never sent VERSION` (GLINK: its FIFOs or
interrupt), IPCRTR up but no services (QRTR's HELLO).

The order for the first runs on 2026-10-31: dsp1, dsp3/4; then dsp6 (the
modem without the ADSP). If the ADSP or the modem gives no ready, dsp6a
tells whether the layers above PAS work against the real firmware.

## The simulation

`make -C freebsd/dsp/test sim` (`test/sim`) runs the start of the ADSP and
the modem on the PC: qcom_dsp.ko's and qrtr.ko's own files (SCM, SMEM,
the IPC doorbells, SMP2P, PAS, GLINK over SMEM, rmtfs, QRTR with its name
service and its GLINK endpoints) and the loader `pro1x-dsp`, unchanged but
for two `#ifdef QCOM_DSP_SIM` hooks (the `smc` instruction, the doorbell
write), against:

- **the kernel** (`simkern.h`, `simkern.c`): FreeBSD's interfaces the
  drivers use on POSIX threads (mutexes that fail on recursion, condition
  variables, sleeps, a thread per task queue and per interrupt line as
  FreeBSD's ithreads, newbus over the device tree's buses, modules in
  kldload's order, character devices, sbuf, sysctl);
- **the device tree** (`fdt.c`): the deployed `sm6115-fxtec-pro1x.dtb` when
  found (`DTB=`), else `pro1x-dsp.dts`, the nodes the stack reads, as
  decompiled from it;
- **the hardware** (`hw.c`): SMEM as the boot loader lays it out (Linux's
  `smem.c` structures: version 12, a table of partitions at the end, a
  global partition and one per pair with the apps processor), the TCSR
  mutex, the APCS doorbells, the DSPs' carveouts, rmtfs's region;
  TrustZone's calls as Linux's `qcom_scm` makes them (the argument
  registers, the buffer for more than four): `INIT_IMAGE` checks the
  metadata, `MEM_SETUP` the place, `AUTH_AND_RESET` every loaded segment
  against the image's hash segment (and the zeroed tails) before the
  remote starts, `MP_ASSIGN` the region's current owners; a call is once
  interrupted (resumed with its session) and once busy, as Qualcomm's
  monitor may do;
- **the firmware images** (`simfw.c`): signed ELF images laid out as
  Qualcomm's (headers, hash segment, relocatable segments), one `.mbn`
  and one split `.mdt` with `.bNN` files;
- **the remote processors** (`remote.c`), written apart from FreeBSD's code
  so that the two do not share mistakes, with Linux 7.1 as the other end's
  reference: their SMP2P item (ready, handover, stop acknowledged, fatal
  with a reason in SMEM, RESTART_DONE toggled at a restart), GLINK
  (VERSION, opens answered at once with OPEN_ACK and an OPEN, five 1 KiB
  intents, intent requests granted only with the intent, RX_DONE with
  reuse), QRTR (HELLO as soon as it has one of our intents, its servers
  after node 1's HELLO, RESUME_TX), the modem's DMS answering "get
  revision", and the modem's need for rmtfs: without rmtfs announced
  within 2 s of the HELLO it fails, as a modem without its file systems;
- **the daemons** (`sim.c`): rmtfs (answering OPEN), tqftpserv, pd-mapper
  and qmicli as ports on the router, as their sockets are on the phone.

Runs (`sim [-q] [-d DTB] RUN`, each a process): `dsp` (dsp1; the ADSP
started with pd-mapper first, its GLINK edge, QRTR, services, a user
registered late, stopped, started again, crashed; the modem started
without rmtfs and failing, then in the right order with the proxy vote,
its services, DMS), `ready-early` (the ADSP's SMP2P item appears with
"ready" already set), `no-irq` (and it never interrupts), `adopt` (the
modem as Linux leaves it at a kexec: both SMP2P items, the GLINK FIFOs'
past with IPCRTR's opens, intents, QRTR traffic, another channel's data,
one packet Linux never read; rmtfs's region already shared). Every check
is a `PASS`/`FAIL` line; the run's last line its summary.

What the simulation does not tell: the real firmware's timing and
behaviour where Linux's code does not say (e.g. how a DSP answers a
second HELLO), TrustZone's real checks, the RPM, power and clocks, the
SMMU, interrupts as the GIC delivers them, and the TCSR mutex as hardware
(here plain memory: the remote's allocations do not race FreeBSD's).

## The handover: starting the DSPs, or taking them over

Two ways to running DSPs under FreeBSD:

1. **FreeBSD starts them** (the default): the boot menu kexecs FreeBSD
   from its initramfs, where Linux has started none of them
   (`qcom_q6v5_pas`, `rmtfs_mem` and `qcom_glink_smem` are modules there).
   FreeBSD makes the TrustZone calls, loads the firmware, shares rmtfs's
   region, and every layer starts from the beginning: SMP2P, GLINK's
   VERSION and opens, QRTR's HELLO. This is the path the firmware knows
   (a cold boot) and the one Linux takes. Its risks: TrustZone's PAS calls
   from FreeBSD, and power: Linux votes the modem's CX at the RPM for its
   start (`hw.qcom_pas.proxy_votes`, run dsp6v) and its xo clock (on while
   this processor runs).
2. **FreeBSD takes them over** (`pro1x-dsp adopt NAME`, run dsp6a): Leste
   has started them (its rmtfs, tqftpserv, pd-mapper serving), and
   `dsp/tools/pro1x-kexec-freebsd.sh` kexecs FreeBSD from the running
   Leste, as KolibriOS's `pro1x-kexec.sh -m` does (`qcom_q6v5_pas` has no
   shutdown hook: the DSPs run on). Nothing goes to TrustZone, the votes
   stay as Linux left them, the SIM stays unlocked. FreeBSD: SMP2P keeps
   the values of our entries (`remote running: our values kept`: the
   modem reads the "ipa" bits Linux set), rmtfs's sharing is made again
   from the owners it has, PAS takes the processor as running if its ready
   bit is set (and no fatal or stop acknowledgement), GLINK takes the edge
   as it is (the FIFO indices kept, no VERSION) and IPCRTR with Linux's
   channel ids, found in what the FIFOs hold behind their heads (Linux's
   in the newest QRTR packet from node 1 in the transmit FIFO, the
   DSP's in the newest one to node 1 in the receive FIFO, else their
   OPEN commands); our intents get ids from 0x10000, data into Linux's
   intents is taken through a bounce buffer and answered with RX_DONE (so
   the DSP retires them); our name service says HELLO on it, as a node
   that started again. `rc.d/pro1x_modem` adopts a modem it finds
   running.

Which is the safer first step? Neither is safer as a whole: they fail in
different places. Taking over avoids what only the device can show for
the start (TrustZone's PAS calls from FreeBSD, the power votes), but it
rests on assumptions no code here can check: that a DSP answers a second
HELLO from node 1 by telling its servers again (KolibriOS relies on the
same; Linux's name service does that, the firmware's is not known), that
the newest QRTR packets in the FIFOs are IPCRTR's, that the modem lives
with rmtfs going away and coming back (Leste's `rmtfs -r` kept the modem's
EFS writes in its memory, which the kexec loses; FreeBSD's rmtfs serves the
partitions as they are on disk: the modem may find its EFS older than it
left it; KolibriOS has the same gap), and that the Leste side works: the
kexec from a running system must rebind UFS (the controller of Leste's
own root) with an identity mapping before the jump, which the boot menu
only does from its initramfs. Other channels Linux had open (the ADSP's
APR and FastRPC, the modem's DATA and voice channels) are not adopted: a
user opening one sends an OPEN the DSP may not expect. The Wi-Fi does not
come back this way (ath10k waits for a firmware ready the running
firmware does not send again). So the start stays the default and the
first device runs are its; the takeover is implemented as the option that
tests GLINK, QRTR and QMI against the real firmware without the start,
should the start fail. A boot menu (initramfs) variant, Linux starting the
DSPs there before the kexec, would need the remoteproc modules, the
firmware and rmtfs, tqftpserv and pd-mapper in the initramfs and the same
adoption on FreeBSD's side, for nothing gained over Leste: not proposed.

KolibriOS's adoption has not run on the device either (its
`docs/pro1x-modem.md`, the device test needs the phone).

## Checked against Linux 7.1

Layer by layer against Linux 7.1 (`drivers/firmware/qcom/qcom_scm*.c`,
`drivers/soc/qcom/smem.c`, `smp2p.c`, `rmtfs_mem.c`, `mdt_loader.c`,
`drivers/remoteproc/qcom_q6v5*.c`, `qcom_common.c`,
`drivers/rpmsg/qcom_glink_native.c`, `qcom_glink_smem.c`, `net/qrtr`) and
the Pro1X's device tree. As Linux: the SCM calls (function ids, argument
types, the buffer for more than four arguments, interrupted and busy
calls), PAS's order (init image, memory setup, segments, auth and reset,
5 s for ready, shutdown on a timeout; the stop over SMP2P, 5 s for the
acknowledgement, then shutdown), the metadata (headers and hash segment),
relocation, SMEM's partitions and items, the TCSR mutex, SMP2P's items and
negotiation, GLINK's commands, version negotiation, intents and their
reuse, the FIFO layout and reserves, QRTR's headers, flow control and name
service. What differed, and is now as Linux:

- **SMP2P** reported no bits for an entry the remote creates after our
  attach that it already had set (Linux's `qcom_smp2p_notify_in` starts
  such an entry from 0): a DSP that set "ready" before its interrupt was
  handled was never seen ready and shut down after the timeout. PAS also
  reads the bits while it waits. The simulation's `ready-early` and
  `no-irq` runs fail 8 checks with the old code.
- **QRTR's HELLO**: the endpoint was registered after the channel's
  intents went out, so the DSP's HELLO, sent as soon as it has one, could
  be dropped, and with it every server (Linux registers the endpoint in
  `qcom_smd_qrtr_probe`, before `qcom_glink_announce_create`).
- **pd-mapper before every DSP** (Linux's pdm subdevice is prepared before
  the start), **the modem after rmtfs, tqftpserv, pd-mapper** are listed
  (`rmtfs -s` starts it once its service is registered): the test and
  `rc.d/pro1x_modem` wait for the services.
- **The modem's proxy vote** for CX (`sc8180x_mpss_resource`'s
  `proxy_pd_names`; the SM6115's ADSP and CDSP have none): sent as Linux's
  rpmpd sends it when `hw.qcom_pas.proxy_votes=1`.
- **A watchdog while stopping** is the stop (`q6v5_wdog_interrupt`).
- **rmtfs**: `qcom,use-guard-pages`.
- **The loader**: all segments from `.bNN` files if any lies beyond the
  file (`qcom_mdt_bins_are_split`).
- **A BYE for every node** behind a QRTR endpoint that goes.
- **A GLINK user registering late** is told of channels the DSP opened
  before (Linux's driver core binds late drivers).
- **QRTR types** the name service's notices by their command (Linux types
  by the sender's port).

Found besides: `sysctl` handlers drained into userspace with a mutex held
once their output passed 256 bytes (`net.qrtr.servers` with the modem's
services); the name service held its lists' lock while it called other
ports, so a port sending to it from there recursed on the mutex; the old
test script called `pro1x-dsp` and `qmicli` from `/usr/local`, which rc's
PATH lacks.

Still different from Linux: no SSCTL shutdown over QMI before the SMP2P
stop (`sysmon`), no subsystem-restart notifications to other DSPs and to
ath10k (`qcom_glink_ssr`, `ssr` subdevice), no PIL info in IMEM
(`qcom_pil_info_store`, for the boot loader's crash dumps), the xo clock
not voted (on while this processor runs), the proxy vote off by default
and CX kept at its level after the handover (Linux lowers it to what its
other consumers ask, which FreeBSD does not know), a full transmit FIFO
waited for in the receive path where Linux defers RX_DONE to a work item.

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
- **RPM votes:** Linux votes the modem's CX at the RPM for its start
  (its only proxy domain on the SM6115; the ADSP has none) and keeps the
  xo clock on while this processor runs. FreeBSD inherits what Linux left
  voted at the kexec, regulators are voted by `qcom_rpm.c`, and the CX
  vote is sent with `hw.qcom_pas.proxy_votes=1` (run dsp6v), off by
  default until dsp7a showed the RPM's channel works.
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
