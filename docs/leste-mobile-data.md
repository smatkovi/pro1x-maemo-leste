# Mobile data on Maemo Leste (Pro1X)

Leste keeps oFono: sphone's calls, Conversations' SMS and the call audio
switching (`pro1x-voice-call`) go through it. Mobile data goes through it
too: oFono's GPRS/LTE context on an rmnet link over the IPA, and icd2's
GPRS connection (`libicd-network-ofono`) on top, as Leste's Internet
connection dialog offers it.

State 2026-10-30: built (release `leste-mobile-data-2026.10.30`), tried on
the PC against a simulated modem (below); not on the phone yet, no SIM
until 2026-10-31. The kernel needs no change (IPA v4.2 node and driver:
[kali-mobile-data.md](kali-mobile-data.md)).

## What was broken

Three things, all from the sources (Leste daedalus' `ofono
1:2.18.0-1+4m7.1` from its archive, sha256 as in its Sources index; Linux
7.1 in `build/linux`):

1. **oFono deleted the links it was given.** `pro1x-rmnet` made
   `rmnet_data0..3` on `rmnet_ipa0` before oFono, because oFono's
   `qrtrsoc` modem (`plugins/udevng.c` `setup_qrtrsoc()`) wants at least
   three of them as "premux" interfaces (links an Android kernel makes).
   But oFono 2.18's rmnet module (`src/rmnet.c` `rmnet_init()`, upstream
   commit "rmnet: Delete stale RMnet links at startup", 2024-11-18) deletes
   every rmnet link it finds when `ofonod` starts, a moment after udevng
   has registered `/qrtrsoc_0` with those names: the data contexts pointed
   at interfaces that no longer existed. Seen on the PC with oFono's
   `rmnet.c` in a network namespace (four `rmnet_data*` links on a dummy
   parent: all four deleted). Calls and SMS do not use them.
2. **The links had no checksum offload.** The IPA v4.2 endpoints facing
   the modem have checksum offload and QMAP on, with aggregation on the
   receive side (`drivers/net/ipa/data/ipa_data-v4.2.c`,
   `IPA_ENDPOINT_AP_MODEM_TX/RX`), so uplink packets need a MAPv4 checksum
   header and downlink ones come with a checksum trailer; rmnet does both
   only with `RMNET_FLAGS_INGRESS_MAP_CKSUMV4`/`EGRESS_MAP_CKSUMV4`
   (`rmnet_map_data.c`). `ip link add ... type rmnet mux_id N` (iproute2
   6.1 in daedalus knows only `mux_id`) gives just deaggregation
   (`rmnet_newlink()`'s default).
3. **Nobody set up the modem's data port.** In the premux mode oFono's
   `qrtrqmi` driver neither opens the DPM port nor sets the WDA data format
   (`plugins/qrtrqmi.c`: only the non-premux path, written for PCIe/MHI
   modems, does WDA, with a PCIe endpoint and QMAPv5). ModemManager does
   both for IPA modems (`src/mm-port-qmi.c`, ModemManager 1.24.2), and the
   NixOS port, which uses ModemManager on this kernel and device tree,
   lists mobile data as working.

## The fix

`patches/fixes/ofono.patch`, built by `scripts/49-mobile-data.sh` into
`ofono 1:2.18.0-1+4m7.1+pro1x1` (held). It does what ModemManager's
`qcom-soc` support does, inside oFono:

| Step | oFono with the patch | Source of the details |
|---|---|---|
| detect | `rmnet_ipa0` of the mainline driver (it has `device/modem/rx_endpoint_id`) makes a `qrtrqmi` modem without premux interfaces; leftover `rmnet_data*` are ignored (and deleted as before) | `drivers/net/ipa/ipa_sysfs.c` |
| DPM | "Open Port" (service 0x2f, message 0x20, TLV 0x11): endpoint type EMBEDDED (4), interface 1, modem RX endpoint = sysfs `tx_endpoint_id`, modem TX endpoint = sysfs `rx_endpoint_id` (on IPA v4.2: 1 and 9) | ModemManager `dpm_open_port()`, libqmi 1.38 `data/qmi-service-dpm.json` |
| WDA | "Set Data Format": raw IP, QMAPv5, read back with "Get Data Format"; else QMAPv4, else QMAP; 32 datagrams / 32768 bytes downlink; the endpoint info TLV only if the modem asks for it (MISSING_ARGUMENT) | ModemManager `data_format_combinations[]`, `sync_wda_data_format()` |
| rmnet | `rmnet0..3` (mux id 1..4, MTU 1400) on `rmnet_ipa0` with `INGRESS_DEAGGREGATION` plus the checksum offload sysfs names (`device/feature/{rx,tx}_offload`: "MAPv4" below IPA v4.5): data format 0xd | ModemManager `get_rmnet_device_add_link_flags()`, `ipa_sysfs.c` |
| contexts | unchanged: one gprs-context per link, `BIND_MUX_DATA_PORT` with EMBEDDED/1 and the mux id | oFono `drivers/qmimodem/gprs-context.c` |

If any of it fails, or the modem does not answer within 20 s, oFono logs
`no mobile data: ...` and powers the modem anyway, without data contexts:
calls and SMS do not depend on the data path. For experiments on the
phone without a rebuild, three variables in ofonod's environment change
the steps (exported in `/etc/default/ofono`, which the init script reads):
`OFONO_IPA_WDA=qmapv5|qmapv4|qmap` (only that protocol), `OFONO_IPA_DPM=0`
(no DPM), `OFONO_IPA_RMNET_FLAGS=<n>` (the links' data format).

The modem keeps its D-Bus path `/qrtrsoc_0` (from udevng's driver name),
and nothing changes for voice calls and SMS.

`pro1x-rmnet` is gone; `pro1x-modem-wait` (OpenRC, before `ofono`) holds
oFono back until `rmnet_ipa0` and the modem's DMS, UIM, WDS and NAS
services are there (at most 60 s, then a few seconds for WDA and DPM),
because `qrtrqmi` looks the services up once, when it enables the modem.
`tools/pro1x-rmnet-flags` prints the rmnet links' data format as the
kernel has it (Leste's `ip -d link` shows only the mux id).

Tried on the PC (2026-10-30): the patched oFono, built natively and as
the release's arm64 package (qemu, in a chroot of Leste's image), against
a fake modem on the PC's QRTR (Python, answering DMS, DPM and WDA) with a
dummy `rmnet_ipa0` and the IPA's sysfs files supplied by an `LD_PRELOAD`
shim: the DPM and WDA messages carry the bytes above; QMAPv5 taken,
stepping down to QMAP when the modem refuses or reports another protocol,
the endpoint info after MISSING_ARGUMENT, no DPM service, no answer
(timeout), WDA refusing everything (no data, modem powered), the three
variables; `rmnet0..3` came up with flags 0xd. What only the phone can
show: whether the real modem takes this, and the data context itself.

## The user interface (all in Leste already)

- **SIM PIN**: Leste's PIN dialog (connui-cellular's PIN query, "PIN
  code"). Nowhere else; never in a file or on a command line (see step 4
  below for a fallback without the dialog).
- **The connection**: the first time icd2 looks for networks (the Internet
  connection dialog: status menu, "Internet connection"), `libicd-network-ofono`
  makes a GPRS connection for the SIM (by its IMSI), named after the
  operator (the SIM's SPN, else mobile-broadband-provider-info), with APN,
  user name and password of oFono's internet context. oFono fills that
  context from its own provisioning database (`provision.db`, oFono's
  `data/provision.json`) by the SIM's MCC/MNC: often wrongly, see the
  table below.
- **APN settings**: Settings, "Internet connections", "Connections", the
  SIM's connection, "Edit": "Access point name", user name, password
  (`connui-iapsettings-gprs`). Data roaming and the network mode: Settings,
  "Phone" (`connui-cellular-settings`).
- **Addresses**: icd2 hands oFono's IPv4 settings (address, netmask,
  gateway, DNS) to `libicd-network-ipv4` (`ip address add`, default route,
  `resolvconf`, dnsmasq). icd2 does no IPv6: a dual-stack context's IPv6
  address is not put on the link by icd2 (the kernel may still configure
  one from the network's router advertisement; step 9).

**Check the APN.** oFono's own provisioning gets most Austrian SIMs wrong
(looked up in the `provision.db` built from Leste's oFono 2.18 source with
its `provisiondb.c`): for one MCC/MNC the database keeps only the last
entry without an SPN, and oFono gives up on an entry with two APNs of one
type ("Duplicate detected", "Provisioning failed" in the syslog) and then
makes an "Internet" context with no APN. What oFono sets, and the APN of
mobile-broadband-provider-info 20230416 (Leste's; the operator's own data
wins):

| Operator | MCC/MNC | oFono 2.18 sets | APN to use (user/password) |
|---|---|---|---|
| A1 | 232 01 | `mdata.com` (ABroadband's) | `a1.net` (`ppp@a1plus.at`/`ppp`) |
| Magenta (T-Mobile) | 232 03 | `volmobil` (VOLmobil's) | `gprsinternet` (`t-mobile`/`tm`) |
| Drei | 232 05, 232 10 | nothing (duplicate MMS entries) | `drei.at` |
| HoT | 232 07 | `web` (tele.ring's) | `webaut` |
| tele.ring | 232 07 | `web` | `web` (`web@telering.at`/`web`) |
| Lycamobile | 232 08 | `data.lycamobile.at` | the same (`lmat`/`plus`) |
| Bob | 232 11 | nothing (duplicate internet entries) | `bob.at` (`data@bob.at`/`ppp`) |
| yesss! | 232 12 | `web.yesss.at` | the same |
| spusu | 232 17 | nothing (no entry) | `web` (the Kali doc's newer database) |

So the APN goes in by hand once: in the connection's "Edit" dialog (icd2
then sets it on oFono's context when it connects), or for the shell test
with `set-context-property` (step 6). On LTE the network may accept the
wrong one; that proves nothing about the next network.

## Install (on the phone, needs Wi-Fi)

From the PC, with the repository at the commit of the release:

    gh release download leste-mobile-data-2026.10.30 -R smatkovi/pro1x-maemo-leste \
        -p 'ofono_*' -p 'ofono-scripts_*' -D /tmp/leste-md
    git archive -o /tmp/leste-md.tar HEAD scripts/49-mobile-data.sh \
        rootfs-overlay/etc/init.d/pro1x-modem-wait tools/pro1x-rmnet-flags
    ssh root@172.16.42.1 mkdir -p /root/mobile-data /root/pro1x-md
    scp /tmp/leste-md/*.deb root@172.16.42.1:/root/mobile-data/
    scp /tmp/leste-md.tar root@172.16.42.1:/root/

(the tar goes through a file: piping `git archive` into ssh failed
before). In Leste, as root:

    tar -C /root/pro1x-md -xf /root/leste-md.tar
    DEBS=/root/mobile-data bash /root/pro1x-md/scripts/49-mobile-data.sh
    /sbin/reboot

The script first keeps a copy of the installed ofono
(`/root/ofono-orig/`, `apt-get download`), installs the two packages
without restarting oFono, holds them, replaces `pro1x-rmnet` by
`pro1x-modem-wait` in the default runlevel and installs
`pro1x-rmnet-flags` in `/usr/local/bin`. The new oFono runs after the
reboot. The MeeGo copy (`meego-leste.img`) is a separate root: the same
three commands in its boot, if it should have data too.

## Device test (2026-10-31)

Needs the user and the SIM. Mobile data costs money: the steps below use a
few MB. Order: what works without the SIM, then calls and SMS (so a
regression is seen before the data steps), then data.

### 0. Before

- SIM in (phone off while inserting it).
- PC: the phone's USB address after every re-enumeration:
  `nmcli dev modify enp19s0u1u1 +ipv4.addresses 172.16.42.2/24`.
- Install as above, reboot, `ssh root@172.16.42.1`.

### 1. Kernel and boot service

    dmesg | grep -E 'ipa 5840000|remoteproc.*modem|modem is now up'
    ip -br link | grep rmnet
    cat /sys/class/net/rmnet_ipa0/device/version \
        /sys/class/net/rmnet_ipa0/device/feature/rx_offload \
        /sys/class/net/rmnet_ipa0/device/feature/tx_offload \
        /sys/class/net/rmnet_ipa0/device/modem/rx_endpoint_id \
        /sys/class/net/rmnet_ipa0/device/modem/tx_endpoint_id
    rc-status default | grep -E 'pro1x-modem-wait|ofono|rmnet'
    grep pro1x-modem-wait /var/log/syslog   # empty if rsyslog was not up yet: the boot console has it

Expected: `IPA driver setup completed successfully`, `received modem
running event`, `remote processor modem is now up`; `rmnet_ipa0` plus
`rmnet0@rmnet_ipa0 .. rmnet3@rmnet_ipa0` (no `rmnet_data*`); `4.2`,
`MAPv4`, `MAPv4`, two endpoint numbers (9 and 1 per the driver's data);
`pro1x-modem-wait` started (`modem up after N s`), no `pro1x-rmnet`.

### 2. QRTR and oFono's data path

    for s in 2 11 1 3 26 47 9 5; do qrtr-lookup $s | tail -n +2; done
    grep ofonod /var/log/syslog | grep -E '/qrtrsoc_0:|no mobile data'
    pro1x-rmnet-flags

Expected: one line each for DMS (2), UIM (11), WDS (1), NAS (3), WDA (26),
DPM (47), voice (9), WMS (5). In the syslog: `DPM open port: modem RX
endpoint 1, TX endpoint 9`, `DPM port open`, `WDA data format: raw IP,
aggregation protocol N` (9, 8 or 5: which one the modem took is worth
noting), `rmnet links rmnet0..rmnet3 on rmnet_ipa0, data format 0xd`.
`pro1x-rmnet-flags`: four links, parent `rmnet_ipa0`, mux id 1-4, `flags
0xd INGRESS_DEAGGREGATION INGRESS_MAP_CKSUMV4 EGRESS_MAP_CKSUMV4`.

### 3. The modem in oFono

    S=/usr/share/ofono/scripts
    $S/list-modems | grep -E '^\[|Powered|Online|Interfaces|PinRequired|Present|Status|Name|Technology'

Expected: `[ /qrtrsoc_0 ]`, `Powered = 1`; with the SIM unlocked and the
radio on, `Online = 1`, `org.ofono.ConnectionManager` among the
interfaces, `Status = registered`. Before the PIN: `PinRequired = pin`.
If it stays `Online = 0` after the PIN: flight mode off (status menu),
else `$S/online-modem` (`libicd-network-ofono` brings the modem online
only while it provisions a new SIM).

### 4. SIM PIN

Leste's PIN dialog asks for it on the screen: type it there. Three wrong
PINs lock the SIM until the PUK (`list-modems` shows `Retries`). If no
dialog comes, from the root shell, so the PIN is neither on a command line
nor in the shell history:

    python3 -c 'import dbus, getpass; dbus.Interface(dbus.SystemBus().get_object("org.ofono", "/qrtrsoc_0"), "org.ofono.SimManager").EnterPin("pin", getpass.getpass("PIN: "))'

(not oFono's `enter-pin` script: it takes the PIN as an argument).

### 5. Calls and SMS (no regression)

- A call out and one in with sphone (the voice stays two-way: earpiece,
  microphone; `pro1x-voice-call` as before).
- An SMS out and one in (Conversations).

If one of them broke after the install and not before: the way back
(below), and the syslog of the attempt.

### 6. Data through oFono directly

    $S/list-contexts
    grep -A1 -E '"Attached"' <(dbus-send --system --print-reply --dest=org.ofono /qrtrsoc_0 org.ofono.ConnectionManager.GetProperties)

Expected: `Attached` true, at least one context of `Type = internet`;
its `AccessPointName` is likely wrong or empty (the APN table above). On
LTE one may already be `Active = 1` (the default bearer). Set the APN with
the context inactive: `$S/set-context-property 0 AccessPointName <apn>`
(0 = the first context; also `Username`, `Password`). Then:

    $S/activate-context 1              # 1 = the first context
    $S/list-contexts                   # Active = 1, Settings: Interface = rmnetN, Address, Gateway, DomainNameServers
    ip -br addr show rmnet0            # oFono brings the link up, the address is icd2's job

This proves the data context; the addresses come in step 7. Without icd2
(only if step 7 fails), with the values `list-contexts` printed:
`ip addr add <Address>/<Netmask> dev rmnetN`, `ip route add default via
<Gateway> dev rmnetN`, then step 8; and `$S/deactivate-context` (or
`deactivate-all`) at the end.

### 7. Data through icd2, as the user does it

`$S/deactivate-all` first if step 6 left a context active. Status menu,
"Internet connection", the SIM's connection (the operator's name); if
Wi-Fi stays connected, disconnect it there or turn WLAN off, so the
default route is the cellular one (step 8). The USB link to the PC is not
icd2's and stays. From the shell instead, the IAP's id and icd2's
osso-ic interface (`icd2-osso-ic-dev`'s `osso-ic-dbus.h`):

    for d in $(gconftool-2 --all-dirs /system/osso/connectivity/IAP); do
      [ "$(gconftool-2 -g "$d/type")" = GPRS ] && echo "${d##*/}"; done
    dbus-send --system --type=method_call --print-reply --dest=com.nokia.icd \
        /com/nokia/icd com.nokia.icd.connect string:<iap-id> uint32:0
    dbus-send --system --type=method_call --print-reply --dest=com.nokia.icd \
        /com/nokia/icd com.nokia.icd.get_ipinfo

(if root lists no IAPs, run the loop as the desktop user, whose GConf the
settings may be in). The IAP's GConf entry also holds the SIM's IMSI: keep
it out of public reports.

### 8. Traffic (Wi-Fi off)

    ip -br addr show rmnet0; ip route; cat /etc/resolv.conf
    cat /run/resolvconf/interface/rmnet0.udhcpc
    ping -c 3 1.1.1.1
    ping -c 3 debian.org                              # DNS
    wget -O /dev/null https://deb.debian.org/debian/dists/stable/Release
    ip -s link show rmnet0; ip -s link show rmnet_ipa0   # RX/TX grow on both
    pro1x-rmnet-flags rmnet0

### 9. IPv6 (optional)

    $S/set-context-property 0 Protocol dual      # with the context inactive
    # connect again (step 7), then:
    $S/list-contexts | grep -A3 IPv6
    ip -6 addr show dev rmnet0 scope global; ip -6 route
    ping -6 -c 3 2606:4700:4700::1111

icd2 sets no IPv6; an address here comes from the kernel (router
advertisement on the raw-IP link). `Protocol ip` puts it back.

### 10. Logs, if something fails

    d=/root/mobile-data-log-$(date +%F-%H%M); mkdir -p $d
    dmesg > $d/dmesg.txt; cp /var/log/syslog $d/
    { $S/list-modems; $S/list-contexts; pro1x-rmnet-flags; ip -d addr; ip route;
      for s in 2 11 1 3 26 47; do qrtr-lookup $s; done; } > $d/state.txt 2>&1

oFono with its full debug output (calls and SMS pause meanwhile):

    /etc/init.d/ofono stop
    OFONO_QMI_DEBUG=1 /usr/sbin/ofonod -n -d '*qrtrqmi*:*rmnet*:*udevng*:*gprs*' 2>&1 | tee $d/ofonod.txt
    # repeat the failing step in a second shell; Ctrl-C, then:
    /etc/init.d/ofono start

To try another data format: `export OFONO_IPA_WDA=qmap` (or `qmapv4`,
`OFONO_IPA_DPM=0`, `OFONO_IPA_RMNET_FLAGS=0x1`) in front of the `ofonod`
line above, or in `/etc/default/ofono` and `/etc/init.d/ofono restart`.

## Failure pictures

| What you see | Where it breaks | Next |
|---|---|---|
| no `rmnet_ipa0`, no `modem is now up` | IPA or modem start | the table in [kali-mobile-data.md](kali-mobile-data.md#failure-pictures); Wi-Fi is gone too then |
| `modem not complete after 60 s` (boot) | QMI services late or missing | `qrtr-lookup`; oFono started anyway: `/etc/init.d/ofono restart` once they are there |
| `list-modems` empty | udevng took no modem | `ls /sys/class/net/rmnet_ipa0/device/modem/` (the patch's detection), syslog lines of ofonod; still the old ofono? `dpkg -l ofono` |
| modem there, `Powered = 0` | enable failed (QMI lookup) | syslog; `/etc/init.d/ofono restart` |
| `no mobile data: the modem has no WDA service` | modem firmware | send the logs (step 10) |
| `DPM open port failed (N), going on` | DPM refused | data may still work; if not, note N, try `OFONO_IPA_DPM=0` |
| `no mobile data: the modem took none of QMAPv5, QMAPv4 and QMAP` | WDA | debug log (step 10): the modem's answers |
| `no mobile data: no answer from the modem` | QMI timeout (20 s) | debug log; restart ofono |
| `no mobile data: the rmnet links were not made` | rmnet | `modprobe rmnet`, `dmesg | tail` |
| no `ConnectionManager` interface | SIM locked or not read | PIN (step 4); `list-modems` under `SimManager` |
| `Attached = 0` | not registered for data, roaming, no data on the plan | `list-modems` (`Status`, `Technology`); roaming: Settings, "Phone" |
| `activate-context` fails | APN, user/password, IP type | `set-context-property` (step 6), `Protocol ip`; WDS errors in the debug log |
| active, but icd2 shows an error or `rmnet0` has no address | icd2 / `libicd-network-ipv4` | `grep -E 'icd2|ipv4' /var/log/syslog`; the manual addresses in step 6 |
| address set, `ping 1.1.1.1` gets nothing, `rmnet0` RX stays 0 | the data path (WDA format, checksums, DPM) | `pro1x-rmnet-flags` (0xd?), `ip -s link show rmnet_ipa0`; then one at a time `OFONO_IPA_WDA=qmap`, `OFONO_IPA_DPM=0`; logs |
| address on `rmnet0`, no `default` in `ip route` | the gateway oFono got is outside the netmask: `libicd-network-ipv4`'s `ip route add default via` failed (it reports connected anyway) | `ip route add default dev rmnet0`; note `Gateway`/`Netmask` from `list-contexts` |
| ping by address works, by name not | DNS | `/etc/resolv.conf`, `/run/resolvconf/interface/`, `list-contexts` `DomainNameServers` |
| small packets pass, downloads stall | MTU | `ip link set rmnet0 mtu 1280` |
| after a modem crash: a second modem `/qrtrsoc_1`, data dead | `rmnet_ipa0` came back with a new index; udevng never removes an embedded modem | `/etc/init.d/ofono restart` or reboot |

## The way back

    apt-mark unhold ofono ofono-scripts
    apt-get install -y --allow-downgrades /root/ofono-orig/ofono_*.deb
    # (no copy there: apt-get update && apt-get install -y --allow-downgrades ofono=1:2.18.0-1+4m7.1)
    apt-get remove -y ofono-scripts
    rc-update del pro1x-modem-wait default

and `pro1x-rmnet` from the repository (it was removed by the commit that
added this file), from the PC:

    git show "$(git log -1 --format=%H -- docs/leste-mobile-data.md --diff-filter=A)^:rootfs-overlay/etc/init.d/pro1x-rmnet" > /tmp/pro1x-rmnet
    scp /tmp/pro1x-rmnet root@172.16.42.1:/etc/init.d/pro1x-rmnet

then in Leste `chmod 755 /etc/init.d/pro1x-rmnet && rc-update add
pro1x-rmnet default && /sbin/reboot`. The stock oFono without
`rmnet_data*` links has no modem at all (`qrtrsoc` wants three), so
`pro1x-rmnet` has to come back with it for calls.

## Risks

- Not run on the phone: the fake modem checks the messages and oFono's
  steps, not what the real modem firmware answers.
- QMAPv5 is tried first, as ModemManager does; if the modem takes it and
  the data path still fails, `OFONO_IPA_WDA=qmap` is the first thing to
  try.
- After a modem restart (crash, SSR) data needs an oFono restart.
- icd2 configures IPv4 only.
