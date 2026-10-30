# Mobile data under FreeBSD (the IPA)

The Pro1X's modem does not hand its packets to the application processor
over QMI: it has a data path of its own, the SM6115's IPA (IP Accelerator,
version 4.2) with its GSI (generic software interface). The modem and the
AP each own IPA endpoints; GSI channels (rings of transfer elements in the
AP's memory) carry QMAP frames between the AP's endpoints and the IPA,
which passes them to and from the modem's. On Linux that is
`drivers/net/ipa` (the IPA and GSI, the network device `rmnet_ipa0`),
`rmnet` (QMAP multiplexing and the MAPv4 checksum offload) and, for the
sessions, ModemManager or Leste's patched oFono (`docs/leste-mobile-data.md`,
`docs/kali-mobile-data.md`). FreeBSD has none of it. This directory has
the kernel side; the sessions are in pro1x-modemd (`freebsd/modem`).

Nothing here has run on the phone (unreachable until 2026-10-31), and
the modem start it builds on (dsp6, `freebsd/dsp/README.md`) has not run
either.

| Stage | What | Here | State |
| --- | --- | --- | --- |
| 1 | IPA probe: RPM votes, SMMU, the registers (ipa_config), the GSI firmware through TrustZone (PAS 15), GSI setup, endpoints, the immediate commands (ipa_setup) | `qcom_ipa.ko` (`sys/arm64/qualcomm/ipa`), `/dev/qcom_pas/ipa` for `pro1x-dsp` | built for arm64 (all symbols resolve against the kernel, qcom_dsp, qrtr); host tested against a model of the hardware; device run prepared |
| 2 | the handshake with the modem: the IPA's QMI service (0x31) both ways | `ipa_qmi.c` on freebsd/dsp's kernel QMI | built; host tested on freebsd/dsp's QMI and QRTR with the test as the modem |
| 3 | the data path: QMAP deaggregation, MAPv4 checksum offload both ways, one interface per mux id | `qmap.c`, `qmap0..` (ipa_fbsd.c) | built; host tested (formats against Linux's `if_rmnet.h`, packets, the rings) |
| 4 | sessions: DPM, WDA, WDS; oFono's ConnectionManager and ConnectionContext; the addresses, routes, DNS | `freebsd/modem/pro1x-modemd/data.c`, `pro1x-data-ifup`, `pro1x-data` | built for FreeBSD (libqmi 1.34); host tested against a simulated modem; device run prepared |

## Stage 1-3: qcom_ipa

A port of Linux 7.1's `drivers/net/ipa` for this one version, its names
kept where a function does what Linux's does (GPL-2.0, as Linux's):

| File | Linux | What |
| --- | --- | --- |
| `ipa_regs.h` | `reg/ipa_reg-v4.2.c`, `reg/gsi_reg-v4.0.c` (gsi_reg.c's table for v4.2), `ipa_reg.h`, `gsi_reg.h`, `gsi_trans.c`, `ipa_cmd.c` | registers, fields, ring elements, command payloads |
| `gsi.c` | `gsi.c`, `gsi_trans.c` | channels and event rings: ALLOCATE, START, STOP, RESET, DE_ALLOC with their interrupts; GENERIC commands for the modem's channels (on v4.2 the AP allocates them, Linux's workaround), flow control; ring programming; the interrupt; transactions (a FIFO per channel, an event per transaction, BEI on TREs toward the IPA as Linux) |
| `ipa_hw.c` | `data/ipa_data-v4.2.c`, `ipa_main.c`, `ipa_endpoint.c`, `ipa_cmd.c`, `ipa_mem.c`, `ipa_table.c`, `ipa_resource.c`, `ipa_interrupt.c`, `ipa_uc.c`, `ipa_modem.c` | the configuration; `ipa_config` (BCR, TX, CLKON, COMP, QSB, the aggregation timer, no hashing, idle indication, SHARED_MEM_SIZE and the canaries, interrupts, uC, FLAVOR_0, resource groups); `ipa_setup` (GSI, the AP endpoints programmed, the command endpoint, header memory and processing contexts zeroed, route and filter tables, the exception endpoint with its status, the default route, the QMI side); receive buffers; transmit; the modem's start, stop and crash path |
| `ipa_qmi.c`, `ipa_qmi_msg.[ch]` | `ipa_qmi.c`, `ipa_qmi_msg.[ch]` (the tables unchanged) | the handshake (below) |
| `qmap.c` | `rmnet_map_data.c`, `rmnet_handlers.c` | QMAP and MAPv4 on byte buffers |
| `ipa_fbsd.c` | `ipa_main.c`'s probe, `ipa_power.c`, `ipa_smp2p.c`, `ipa_sysfs.c`, `ipa_modem.c`'s netdev | FreeBSD: newbus, resources, votes, SMMU, SMEM, SMP2P, the firmware's device, DMA, tasks, the interfaces, sysctls |

The core (`gsi.c`, `ipa_hw.c`, `qmap.c`) uses its system only through
`ipa_os.h` and the `ipa_os_*` functions, so the host tests run it as it is.

**The order on the phone** is Linux's: the driver before the modem (Linux
loads `ipa.ko` before the modem's remoteproc starts), the network interface
after the modem's handshake.

1. `kldload qcom_ipa` (needs qcom_dsp and qrtr): attaches to the node
   `ipa@5840000` (`qcom,qcm2290-ipa`, `qcom,sc7180-ipa`), maps `ipa-reg`,
   `ipa-shared`, `gsi`, takes the interrupts `ipa` (SPI 257) and `gsi`
   (SPI 259), finds the SMP2P bits (`ipa-clock-query`, `ipa-setup-ready`
   of `ipa-modem-to-ap`; the states `ipa-clock-enabled-valid`,
   `ipa-clock-enabled` of `ipa-ap-to-modem`) and the firmware's region
   (`pil_ipa_fw_mem`, 0x56100000, 64 KiB); allocates rings, buffers,
   tables. No register access yet.
2. `pro1x-dsp start ipa .../ipa_fws.mbn` (the loader of freebsd/dsp; the
   image is relocatable, 0x5000 bytes): the first ioctl powers the IPA up,
   with a console line before each step: the RPM votes Linux's clock and
   interconnect drivers make (the IPA core clock, `clk-smd-rpm`'s resource
   "ipa" (0x617069) id 0, key "KHz", 100000, active and sleep set, as
   `ipa_data-v4.2.c`'s 100 MHz; the master bandwidth of `qxm_ipa` (RPM id
   59, `icc-rpm.c`: key "bw", its summed average 80000 kB/s = 80000000 B/s,
   both sets)), the SMMU streams of `iommus` (0x140, 0x142) without
   translation (`qcom_smmu_bypass`), SMEM item 497 for the modem
   (`ipa_smem_init`), then `ipa_config`. TrustZone takes the metadata
   (PAS 15), the segments go into the region, `auth_and_reset` runs the
   GSI firmware; `GSI_STATUS` enabled, then `ipa_setup`.
3. The modem starts (dsp6). It asks over SMP2P whether the IPA is powered
   (answered: enabled, valid), then the QMI handshake (stage 2), then
   `qmap0` appears.

Where FreeBSD differs from Linux, and why:

- **No runtime PM.** The IPA stays powered once voted; detach refuses
  then. `ipa_uc_power`'s proxy vote is not needed.
- **SMMU.** Linux gives the IPA an IOMMU domain and maps IMEM
  (`modem-tables@c123000`) and SMEM item 497 identity into it; with
  translation off, DMA addresses are physical and both are reachable as
  they are. All DMA memory is below 4 GiB (the GSI's doorbells and events
  carry the low half of addresses).
- **Not cache-coherent** (no `dma-coherent` in the node): rings, command
  payloads and tables uncached, buffers synced by bus_dma.
- **Interconnects.** Only the IPA master's bandwidth is voted; Linux's
  icc-rpm also sets the NoC bus clocks from all its users' votes, which
  FreeBSD does not have (they stay as the boot menu's Linux left them).
- **BEI.** TREs toward the IPA have BEI (no interrupt on completion), as
  on Linux; their completions come with the moderation timer's interrupt.
  The command waits also look at the event ring themselves, and transmit
  polls its channel when the ring fills, so a missing interrupt costs time
  (`stats`: polled) rather than a hang.
- **The modem's start and crash.** Linux hears them from the remoteproc
  (SSR notifier). Here the first start needs nothing (the SMP2P state is
  fresh), and the modem going away is its QRTR node's BYE or its IPA
  service's DEL_SERVER; then what Linux does before a shutdown
  (`ipa_modem_crashed`: flow control, HOLB, table reset, the exception
  reset with a pipeline clear, the modem's memory zeroed) and the
  handshake again on the next start.
- **Receive buffers are copied out** per packet into mbufs (rmnet copies
  each deaggregated packet too); 128 buffers of 8 KiB per RX endpoint
  (`hw.qcom_ipa.rx_buffers`), posted as Linux posts them (8128 bytes, the
  aggregation's byte limit 6 KiB as Linux computes it).
- **Interfaces.** Linux has `rmnet_ipa0` (raw QMAP) and rmnet links on it;
  here the driver demultiplexes itself: `qmap0` is mux id 1, `qmapN` mux
  id N + 1 (`hw.qcom_ipa.muxes`, default 1). Raw IP, point to point,
  MTU 1500 at most (Linux's `IPA_MTU`); BPF as DLT_RAW.
- **Checksum offload** (`hw.qcom_ipa.csum`, IFCAP_HWCSUM, both families):
  uplink TCP/UDP checksums are the IPA's (the MAPv4 header: start, offset,
  UDP flag; the field complemented as rmnet does, from the pseudo-header
  sum the stack leaves there); IPv6 with extension headers is checksummed
  in software (the IPA wants TCP/UDP right after the header). Downlink: the
  trailer checked as rmnet does; a good one marks the packet checked
  (IPv4: its header too, which the check includes).

Sysctls (`dev.qcom_ipa.0`): `state` (off, powered, configured, setup,
running), `power` (1: votes and `ipa_config` only, for the device run),
`regs` (FLAVOR_0, SHARED_MEM_SIZE, ROUTE, the interrupt registers,
GSI_STATUS, HW_PARAM_2, the channels' states), `stats` (GSI errors,
command timeouts, polled completions, SMP2P queries, per channel and
endpoint counts, QMAP counts), `qmi` (the handshake), `mem_offset`, and
as Linux's sysfs: `version` (4.2), `feature.rx_offload`,
`feature.tx_offload` (MAPv4), `modem.rx_endpoint_id` (9),
`modem.tx_endpoint_id` (1). Tunables (`hw.qcom_ipa.*`): `rpm_clock`,
`rpm_bw`, `smmu` (all 1), `muxes`, `rx_buffers`, `csum`, `dump_rx` (log
the start of so many receive buffers).

## Stage 2: the handshake

As Linux's `ipa_qmi.c`, on freebsd/dsp's kernel QMI (`qmi_core.c`, Linux's
API):

- the AP serves the IPA service 0x31 instance 1; the modem sends
  `INDICATION_REGISTER` (0x20) and `DRIVER_INIT_COMPLETE` (0x35);
- the AP looks up the modem's (0x31 instance 2) and sends it `INIT_DRIVER`
  (0x21): platform MSM Android; the modem's header table 0x4a8-0x5e7,
  routes 0x3a0/0x428 (indices 0-7), filters 0x290/0x318, its memory
  0xbf0+0x140c, processing contexts 0x5f0-0x7ef, quota statistics
  0xa50/0x60, the AP endpoint for its control packets (9), `skip_uc_load`
  after the first boot (all plus SHARED_MEM_SIZE's base);
- with the response and both requests, `INIT_COMPLETE` (0x22) to the
  modem's server, then `ipa_modem_start`: the endpoints AP_MODEM_TX (1)
  and AP_MODEM_RX (9) enabled, buffers posted, `qmap0` created. Linux
  enables the endpoints when `rmnet_ipa0` goes up; here at once.

`INIT_DRIVER` waits for its answer (a minute) on the driver's own thread,
not on the QMI handles' one that delivers it.

Two differences of freebsd/dsp's `qmi_core.c` from Linux's
`qmi_interface.c`, found by the tests and handled here (left to the DSP
stack's owner): `qmi_send_*` return the length sent and `qmi_txn_wait` the
decoded length (Linux: 0), so only negative values are errors; and
`ops.bye` gets the sender of the name service's relayed BYE (the local
node), where Linux passes the BYE's `client.node`, so the modem's departure
is also taken from the client handle's DEL_SERVER of its IPA service.

## Stage 4: sessions (pro1x-modemd)

`freebsd/modem/pro1x-modemd/data.c`, with small hooks in `main.c`
(options), `ofono.c` (`Interfaces`, the registration) and `modemd.h`.
Every QMI message is libqmi's; the order is ModemManager 1.24's qcom-soc
support as Leste's patched oFono documents it (`patches/fixes/ofono.patch`,
`docs/leste-mobile-data.md`; ModemManager's source is not on this
machine):

1. **DPM Open Port** (if the modem has DPM): hardware data port EMBEDDED,
   interface 1, the modem's RX endpoint = the IPA's TX one
   (`modem.tx_endpoint_id`, 1), its TX endpoint = the IPA's RX one (9).
2. **WDA**: Get Data Format (again with the endpoint info EMBEDDED/1 when
   the modem answers MISSING_ARGUMENT), then Set Data Format: raw IP,
   QMAPv5 both ways, 32 datagrams / 32768 bytes downlink, read back; if
   refused or not taken, QMAPv4, then QMAP (`-W` takes one only). The
   IPA's own side is fixed by the driver (MAPv4 checksums on v4.2); which
   format the modem's side takes is the device run's question.
3. **A session** per IP family on its own WDS client: Bind Mux Data Port
   (EMBEDDED, 1, the context's mux id, client type tethered), Set IP Family,
   Start Network (APN, user, password, CHAP/PAP/none, the family), Get
   Current Settings (address, netmask, gateway, DNS, MTU). Raw IP over QMAP
   has no DHCP: oFono's `Method` "static".
4. **The addresses**: the hook (`-I`, `/usr/local/libexec/pro1x-data-ifup
   up|down qmap0` with `IPV4_*`, `IPV6_*`, `MTU` in the environment):
   `ifconfig qmap0 inet ADDRESS GATEWAY`, IPv6 with its prefix, the
   default route only when there is none (Wi-Fi stays first), the name
   servers through resolvconf (`/etc/resolv.conf` points to
   `/var/run/resolv.conf`). FreeBSD's Hildon has no icd2 (only its
   headers), so nothing else would put them on.
5. **The end**: Packet Service Status (the network ended it), Stop
   Network (`Active` false, `Powered` false, `DeactivateAll`, SIGTERM).

oFono's API (2.18, `doc/connman-api.txt`): `ConnectionManager` on the
modem (`/qrtr_0`): `GetProperties`, `SetProperty` (`Powered`,
`RoamingAllowed`), `GetContexts`, `AddContext`, `RemoveContext`,
`DeactivateAll`; `Attached` (registered or roaming: NAS's registration,
not the PS attach state), `Bearer` (lte, umts, gprs, none), `Suspended`;
`PropertyChanged`, `ContextAdded`, `ContextRemoved`. `ConnectionContext`
(`/qrtr_0/context1`, up to 4; context N on mux id N, interface
`qmap(N-1)`): `Active`, `AccessPointName`, `Type`, `AuthenticationMethod`,
`Username`, `Password`, `Protocol` (ip, ipv6, dual), `Name`, `Settings`,
`IPv6.Settings`. `Modem.Interfaces` lists ConnectionManager once the data
port is set up.

The user's file `/dev/pro1x-host/freebsd/data.conf` (`-c`; written from
Leste by the user, never by anything here; nothing is written back):

    apn=a1.net
    username=ppp@a1plus.at
    password=ppp
    auth=chap
    protocol=ip
    roaming=0
    autoconnect=0

(The APN table of `docs/leste-mobile-data.md` has the Austrian operators'.)
The password is never logged. `pro1x-data` is the shell's way:
`pro1x-data status|up|down`, `pro1x-data apn APN [USER PASSWORD [AUTH]]`,
`protocol`, `roaming`, `power`.

Mobile data needs the IPA behind the modem's QRTR node: over a QMUX file
(the modem tests' simulated modem) it is off unless `-X` is given, so
`freebsd/modem/test/run.sh` runs as before.

At boot (both off until the device runs pass): `pro1x_ipa_enable="YES"`
(`rootfs/usr/local/etc/rc.d/pro1x_ipa`: qcom_ipa and its firmware, before
`pro1x_modem`), then `pro1x_modemd_enable="YES"` as for calls.

## Tests on the PC

    make -C freebsd/data/test           # C, ASan and UBSan
    sh freebsd/data/test/data_run.sh    # pro1x-modemd's sessions

- `test_regs`: every offset, stride and field mask of `ipa_regs.h`
  (`regs.def`, 212 entries) against Linux's own tables, compiled from the
  Linux tree (`LINUX=`, default `../build/linux`).
- `test_qmap`: the formats against Linux's `if_rmnet.h` (sizes, offsets,
  flags), the GSI's and the commands' layouts against Linux's definitions;
  packets built with a checksum routine of the test's own: TCP and UDP over
  IPv4 and IPv6 aggregated with padding and a MAP command, UDP without
  checksum, ICMP, a fragment, a payload changed on the way (both families),
  a bad IPv4 header, a trailer without its valid flag, an empty and a
  truncated frame; the uplink headers and the complemented field.
- `test_gsi`: `gsi.c` and `ipa_hw.c` unchanged against a model of the IPA
  and the GSI (registers, the read-only state fields, channel, event ring
  and GENERIC commands with their interrupts, TREs taken at the doorbell,
  immediate commands decoded and applied, the pipeline clear's tagged
  packet in the exception endpoint, receive buffers filled, events,
  BEI with a late interrupt, an interrupt thread). Checked with values
  worked out by hand from Linux's code for the v4.2 data: every register
  `ipa_config` and `ipa_setup` write (endpoints 1, 6, 8, 9: CFG, HDR,
  HDR_EXT, METADATA_MASK, MODE, AGGR, SEQ, STATUS; ROUTE; resources;
  canaries), the channels' and event rings' programming, the ten
  immediate commands of the setup with their payloads, the table memory;
  then 1000 downlink buffers (3000 packets, the rings wrap), 3000 uplink
  frames of one to three segments each completed once, the crash path
  (flow control, HOLB, table reset, exception reset with its pipeline
  clear, zeroing), a restart, the teardown (all deallocated, the modem's
  channels halted).
- `test_qmi`: `ipa_qmi.c` on freebsd/dsp's `qmi_core.c` and `qrtr_core.c`;
  the test is the modem (node 5) with Linux's `ipa_qmi_msg.c` tables
  (compiled from the Linux tree, renamed): the INIT_DRIVER request's
  values and its bytes against Linux's encoding, INDICATION_REGISTER, the
  response, DRIVER_INIT_COMPLETE, INIT_COMPLETE to the modem's server, the
  start; BYE; a second boot (`skip_uc_load`, no new indication needed).
- `data_run.sh`: pro1x-modemd (host build, ASan, UBSan) against
  `fakedata.py`, the modem tests' `fakemodem.py` with WDS, WDA and DPM
  (from libqmi's data files), on a private bus, checked by `test_data.py`:
  the DPM port's endpoints, QMAPv5 refused then QMAPv4, the endpoint info
  after MISSING_ARGUMENT, ConnectionManager's properties, the context from
  data.conf, a dual-stack session (two WDS clients bound to mux 1, Start
  Network per family with APN, user, password, CHAP), its Settings and
  IPv6.Settings, the hook's environment, the network ending it, a wrong
  APN (the error with the call end reasons), IPv4 only, no change while
  active, Stop Network, AddContext and RemoveContext; the password in no
  log; no sanitizer report, no libqmi decode warning.

What they cannot show: whether the phone's IPA takes these values (the
model is written from the same reading of Linux), whether TrustZone takes
the image into this region, the SMMU's and the RPM's answers, the real
modem's handshake and its QMAP format, interrupts on the GIC.

## Device runs (prepared)

FreeBSD on the phone has no network of its own yet (that is this), so as
the other runs: a reboot into FreeBSD with `pro1x.test=data
pro1x.data=STEP` in `/host/freebsd/cmdline`; rc.local runs
`/usr/local/libexec/pro1x-datatest`, which writes to the console (the RAM
console, read by Leste after the next boot, `fbsdtest.sh`) and reboots.
`qcom_ipa.ko` comes with `freebsd/build.sh` (MODULES) or
`freebsd/data/build.sh` (`$WORK/data-out/modules`); modules in
`/host/freebsd/modules` are found before the root image's, so a new
module needs no new root image (it must match `qcom_pas_ioctl.h` of the
qcom_dsp it runs with: rebuild them together). The script, rc.local's
branch and the CLI are in the root image (`rootfs/`, `modem/extra/`).

| Run | cmdline | Expected on the RAM console |
| --- | --- | --- |
| ipa | `pro1x.test=data pro1x.data=ipa` | `qcom_ipa0: <Qualcomm IPA v4.2>`, `GSI firmware: PAS id 15 supported`, `SMP2P query found, states found`; `power: RPM votes`, `RPM: IPA core clock 100000 kHz: voted (0)`, `RPM: IPA master bandwidth 80000000 B/s: voted`, `power: SMMU streams`, `qcom_smmu: qcom_ipa: stream 0x140 ...` and `0x142`, `SMEM item 497: ...`, `power: the first register reads`, `shared memory: offset 0x0, size 0x2000`, `endpoints: N TX from 0, M RX from K (FLAVOR_0 ...)`, `IPA v4.2 configured`; `ipa_fws.mbn, 5 segments, 20 KB, relocatable`, `PAS 15: authenticating the GSI firmware`, `GSI status 0x1 after N ms`, `QMI: IPA service 0x31.1 served`, `IPA driver setup completed successfully`; `pro1x-dsp status ipa`: running, ready; `regs` with the four channels started/allocated; `stats` without GSI errors or command timeouts |
| modem | `... pro1x.data=modem` | ipa's lines, dsp6's, `the modem asked for the IPA's clock: enabled`, `QMI: the modem's IPA service at 0:N`, `QMI: init driver request`, `QMI: the modem registered for the indication`, `QMI: init driver response: result 0`, `QMI: the modem's driver init is complete`, `microcontroller running`, `QMI: init complete indication sent`, `modem data path up (endpoints 1 TX, 9 RX)`, `qmap0: QMAP mux id 1, MAPv4 checksum offload`; `after N s: qmap0: flags=...`; qrtr-lookup with services 1 (WDS), 26 (WDA), 47 (DPM), 49 (IPA) |
| wds | `... pro1x.data=wds` (with the user's `sim-pin`, and `data.conf` for a session) | modem's lines, then pro1x-modemd's: registration, `data: DPM Open Port: modem RX endpoint 1, TX endpoint 9`, `data: DPM port open`, `data: WDA data format now: ...`, `data: data port ready: raw IP, qmapv5|qmapv4|qmap ...`, `ConnectionManager there`; with data.conf: `data: context 1 IPv4: session up`, its address, gateway, DNS, MTU, `pro1x-data-ifup up qmap0: done`, `ifconfig qmap0` with the address, a default route on qmap0, resolv.conf's name servers, `ping 1.1.1.1` and `deb.debian.org` answering, `fetch` a Release file, qmap0's byte counts, `stats` with QMAP packets and checksums ok, then `inactive (on request)` |

Without `data.conf` no session is started: mobile data costs (a run uses
well under 1 MB). Risks, in the order they would show:

- **The IPA's clock and bus**: a hang or reset at the first register read
  means the vote did not reach or the NoC is closed: `hw.qcom_ipa.rpm_clock`
  and `rpm_bw` (both votes are logged with the RPM's answer), the bus
  clocks Linux's icc-rpm sets (not done here).
- **SMMU**: the hypervisor may refuse translation off for the IPA's
  streams as for the DSPs' (freebsd/dsp's risk); the IPA's DMA then fails
  (GSI errors, no completions).
- **TrustZone**: the image in the mainline region 0x56100000 (the stock
  DT has 0x55600000; Linux 7.1 probed with the mainline one on Leste,
  `docs/kali-mobile-data.md`); `PAS auth and reset: N` says no.
- **Interrupts**: the GSI's level interrupt; without it every command
  waits its 50 ms (`command timeouts`) and completions come from polling.
- **The modem's side**: the handshake (`qmi`), the QMAP format WDA takes,
  whether the modem wants the DPM port; the SIM and registration as for
  calls (`freebsd/modem/README.md`).
- **Checksum offload**: if the uplink's checksums come out wrong, TCP
  stalls while ping works: `hw.qcom_ipa.csum=0` (all in software) tells.
