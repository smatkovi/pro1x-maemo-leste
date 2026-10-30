# Mobile data on Kali (Pro1X)

Kali gets the modem for mobile data only (no calls, no SMS setup). The
path: the modem (MPSS remoteproc, started by `rmtfs -s`) talks QMI over
QRTR; its packets go through the IPA, whose driver makes `rmnet_ipa0`;
ModemManager's `qcom-soc` plugin takes the QRTR node as control port and
`rmnet_ipa0` as data port, creates an rmnet link on it per connection
(`qmapmux0.0`), and NetworkManager puts the IP configuration on that link.

State 2026-10-30: checked from the sources, not on the phone (no SIM so
far). Test: [below](#device-test-2026-10-31).

## The chain, and where each part was checked

**Device tree** (`build/linux`, `arch/arm64/boot/dts/qcom/sm6115-fxtec-pro1x.dts`,
the IPA node at about line 957): from the NixOS port's
`device/kernel/patches/pro1x-ipa.patch` (github nyymi609/fxtec-pro1x-nixos,
579356e), unchanged by the Leste patches since (`git diff 019a5d9bd9 HEAD`
has no hunk in it). Compared with the stock Bengal device tree (the DTB of
Sailfish's downstream kernel in `$WORK/backup-slot-a/boot_a.img`, at offset
0x17ea000, model "Qualcomm Technologies, Inc. Bengal SoC"): IPA v4.2
(`qcom,ipa-hw-ver = <16>`), interrupts SPI 257/259, SMMU stream ids 0x140
(AP) and 0x142 (microcontroller), the modem tables in IMEM at 0x0c123000
(0x2000, stock `qcom,additional-mapping`), the three bus paths IPA->EBI,
IPA->OCIMEM, CPU->IPA_CFG, GSI firmware `ipa_fws` with PAS id 15: all as
in the mainline node. Differs: the firmware's staging region, stock
0x55600000, mainline `sm6115.dtsi` 0x56100000 (mainline moved every region
after the longer ADSP region). TrustZone takes that layout for the GPU's
zap shader at 0x56115000 (the GPU works), and the IPA probe went through
on Leste with 7.1.0-pro1x.2: `ipa 5840000.ipa: IPA driver removed` at a
shutdown in `~/ps/pro1x-power/ramoops-2026-10-29-0925-pro1x.2-ordered.txt`
is only printed for a bound driver, and with `qcom,gsi-loader = "self"` the
probe fails unless TrustZone authenticated `ipa_fws.mbn`.

**Driver** (`drivers/net/ipa`, Linux 7.1): `qcom,qcm2290-ipa` falls back
to `qcom,sc7180-ipa` = `ipa_data_v4_2`. Loader "self": the probe loads
`qcom/sm6115/Fxtec/QX1050/ipa_fws.mbn` into the `memory-region` and has
TrustZone authenticate it, without the modem (`ipa_firmware_load()`), then
sets the IPA up. IMEM comes from `sram` (`ipa_mem_init()`), the
interconnects by name (`memory`, `imem`, `config` in `ipa_data-v4.2.c`),
the core clock is `RPM_SMD_IPA_CLK` (in the SM6115 RPM clock table). The
modem's start is seen through the SSR notifier (`ipa_modem.c`); after the
QMI handshake with the modem (`ipa_qmi.c`) `ipa_modem_start()` makes
`rmnet_ipa0`. So: driver before the modem, network interface after it.
For ModemManager the driver exports `device/feature/rx_offload` and
`tx_offload` ("MAPv4" below IPA 4.5) and `device/modem/{rx,tx}_endpoint_id`
(`ipa_sysfs.c`).

**Kernel config** (`out-pro1x.8/config-7.1.0-pro1x.8`): `QCOM_IPA=m`
(`ipa.ko`), `RMNET=m` (alias `rtnl-link-rmnet`, loaded when a link is
added), `QRTR=y`, `QRTR_SMD=y`, `QCOM_PD_MAPPER=y` (no pd-mapper daemon),
`QCOM_RMTFS_MEM=m`, `QCOM_Q6V5_PAS=m`, `IPV6=y`. Kali gets the modules of
the running kernel from `/host/pro1x-modules/<version>`
(`initramfs/init-bottom/pro1x-shared-modules`) and the firmware in
`/lib/firmware/updates` (step 20, Leste's copy of
github sm6115-mainline/firmware-fxtec-qx1050). The initramfs loads no
modem modules (`MODULES=list`), so `ipa.ko` probes in Kali, where the
firmware is.

**Userspace** (kali-rolling index of 2026-10-30, sources from
http.kali.org): ModemManager 1.24.2 (depends on libqrtr-glib0, i.e. built
with QRTR; `meson_options.txt` `qrtr` default true), libqmi 1.38.0,
libqrtr-glib 1.4.0, NetworkManager 1.58.1, rmtfs 1.3, tqftpserv 1.2,
qrtr-tools 1.2, mobile-broadband-provider-info 20251101. In ModemManager:

- `src/plugins/qcom-soc/77-mm-qcom-soc.rules`: a net device with
  `DRIVERS=="ipa"` gets `ID_MM_PHYSDEV_UID=qcom-soc`, the QRTR node has the
  same uid (`mm-kernel-device-qrtr.h`): one modem, ports `qrtr<node>`
  (QMI) and `rmnet_ipa0` (net).
- `src/mm-qrtr-bus-watcher.c`: a QRTR node counts as a modem only if WDS,
  DMS and NAS appear within 4 s of its first service.
- `src/mm-port-qmi.c`, for the net driver "ipa": always rmnet multiplexing,
  a DPM "open port" with the endpoint ids from sysfs, and link flags from
  `rx_offload`/`tx_offload` (`ingress-map-cksumv4`, `egress-map-cksumv4`
  for MAPv4). The link is named `qmapmux<modem>.<n>`
  (`mm-bearer-qmi.c`, libqmi `qmi-net-port-manager-rmnet.c`).
- A port that turns up after the modem object exists makes ModemManager
  build the modem again (`additional_port()` in `mm-base-manager.c`), so a
  late `rmnet_ipa0` is no problem.
- It does not choose the SIM application itself (it uses the primary
  GW provisioning session the modem has): see the failure list.

tqftpserv 1.2 looks in `/lib/firmware/updates` as well (`translate.c`),
where Kali has the firmware. The NixOS port runs this same combination
(rmtfs, tqftpserv, ModemManager; `modules/modem.nix`) on the same kernel
and device tree and lists mobile data as working.

## What Kali has for it

- Packages (step 20): `network-manager modemmanager rmtfs tqftpserv
  qrtr-tools`, now also `mobile-broadband-provider-info` (the APN database
  for NetworkManager's `gsm.auto-config`) and `libqmi-utils` (`qmicli`),
  which before only came as Recommends.
- `rmtfs.service.d/pro1x.conf`: Debian's unit has
  `ConditionPathExists=/dev/qcom_rmtfs_mem1`, which systemd checks before
  `ExecStartPre`; a start before udev loaded `rmtfs_mem` was skipped. The
  drop-in clears the condition, orders rmtfs after tqftpserv, loads
  `rmtfs_mem` and `ipa` (the IPA before the modem starts), waits up to
  120 s for the device node and the modem's remoteproc
  (`/usr/lib/pro1x/pro1x-modem-wait remoteproc`; `rmtfs -s` exits when
  the remoteproc is not there yet) and has no start limit.
- `ModemManager.service.d/pro1x.conf`: after rmtfs and tqftpserv, and only
  once WDS, DMS and NAS are on QRTR (`pro1x-modem-wait qmi`, at most 90 s;
  the 4 s window above).
- Nothing creates `rmnet_data*` links on Kali (that is Leste's oFono
  setup); ModemManager makes its own.
- `kali/25-mobile-data.sh` brings these into the image made on 10-25.

No kernel or device tree change is needed for this as far as the sources
go.

## Device test (2026-10-31)

Needs the user with the SIM. Mobile data costs: the steps below use a few
MB. Never write the PIN into a file, the repo or a chat.

### 0. Before Kali

- The kernel of the day is flashed and its trial confirmed in Leste:
  `pro1x-boot list` there shows `start confirmed` (or no trial). The next
  boot then ends the trial in the initramfs, so Kali uses no try of the
  bootloader.
- The SIM is in the slot the phone calls SIM 1 (dual SIM: "SIM
  application" under the failure pictures).
- PC: the phone's USB address after every re-enumeration:
  `nmcli dev modify enp19s0u1u1 +ipv4.addresses 172.16.42.2/24`.

### 1. Into Kali, with the new files

From the PC, while Leste runs (repo checked out at HEAD; the tar goes
through a file, piping `git archive` into ssh failed before):

    git archive -o /tmp/kali.tar HEAD kali
    scp /tmp/kali.tar root@172.16.42.1:/root/
    ssh root@172.16.42.1

and in Leste:

    mkdir -p /root/pro1x-kali && tar -C /root/pro1x-kali -xf /root/kali.tar
    mkdir -p /mnt/kali && { mountpoint -q /mnt/kali || mount -o loop /host/kali.img /mnt/kali; }
    MNT=/mnt/kali bash /root/pro1x-kali/kali/25-mobile-data.sh
    umount /mnt/kali
    pro1x-boot next kali && /sbin/reboot

If 25 reports missing packages, install them in Kali once it has a network
(Wi-Fi): `sudo apt install mobile-broadband-provider-info libqmi-utils`;
without them only `gsm.auto-config` and `qmicli` are missing.

Kali comes up in about a minute: `ssh -o HostKeyAlias=pro1x-kali kali@172.16.42.1`
(password `kali`; the alias keeps Kali's host key apart from Leste's on
the same address). Everything below runs there.

### 2. Kernel side

    uname -r                                     # 7.1.0-pro1x.8 (or the kernel flashed)
    findmnt /usr/lib/modules/$(uname -r)         # bound from /host/pro1x-modules/...
    lsmod | grep -E '^(ipa|qcom_q6v5_pas|rmtfs_mem|rmnet) '
    sudo dmesg | grep -E 'ipa|remoteproc|rmtfs'
    systemctl status rmtfs tqftpserv --no-pager
    ip -br link show rmnet_ipa0
    cat /sys/class/net/rmnet_ipa0/device/version \
        /sys/class/net/rmnet_ipa0/device/feature/rx_offload \
        /sys/class/net/rmnet_ipa0/device/modem/rx_endpoint_id \
        /sys/class/net/rmnet_ipa0/device/modem/tx_endpoint_id

Expected, in this order (messages from the driver sources; `dmesg` has them
although the console's loglevel hides them): `ipa 5840000.ipa: IPA driver
setup completed successfully`, `ipa 5840000.ipa: IPA driver initialized`,
then `remoteproc remoteprocN: powering up modem`, `... Booting fw image
qcom/sm6115/Fxtec/QX1050/modem.mbn, size 51579892`, `ipa 5840000.ipa:
received modem starting event`, `ipa 5840000.ipa: received modem running
event`, `remote processor modem is now up`. `rmnet_ipa0` exists some
seconds later (no message; state DOWN is fine, ModemManager brings it up);
version `4.2`, `MAPv4`, two endpoint numbers.

### 3. QRTR and ModemManager

    qrtr-lookup | grep -E 'Wireless Data|Device Management|Network Access|IPA'
    mmcli -L
    mmcli -m any

Expected: services 1, 2, 3 and 49 on one node (column `Node`, usually 0);
one modem in `mmcli -L`; in `mmcli -m any` plugin `qcom-soc`, drivers
`ipa, qrtr`, primary port `qrtr0`, ports `qrtr0 (qmi), rmnet_ipa0 (net)`,
an IMEI under `equipment id`, and a state: `locked` (SIM PIN),
`disabled`/`enabled`/`registered`, or `failed` with a reason.

### 4. SIM PIN

Only if the state is `locked` (`mmcli -m any` also shows `unlock retries`:
three wrong PINs lock the SIM until the PUK). Best: let NetworkManager ask
when the connection comes up, step 5 (`nmcli --ask` prompts on the
terminal and hands the PIN to ModemManager over D-Bus; in Xfce nm-applet
asks instead). Or by hand with ModemManager, from a root shell, because
sudo logs every command line it runs, the PIN included, to the journal:

    sudo -i
    read -rs PIN && mmcli -i any --pin="$PIN"; unset PIN; exit

(`read -s` keeps it off the screen and out of the shell history). A PIN
given as `gsm.pin` to `nmcli` would be stored in
`/etc/NetworkManager/system-connections/mobile.nmconnection`; with
`gsm.pin-flags 2` (not saved) it never is.

### 5. Connection

    sudo nmcli radio wwan on
    sudo nmcli connection add type gsm ifname '*' con-name mobile \
        gsm.auto-config yes gsm.pin-flags 2 connection.autoconnect no
    sudo nmcli --ask connection up mobile

`gsm.auto-config yes` takes APN, user and password from
mobile-broadband-provider-info for the network the modem registers to.
If that finds nothing or the wrong one, set the APN:
`sudo nmcli connection modify mobile gsm.auto-config no gsm.apn <apn>`.
Austrian entries in the database (20251101), for orientation, the
provider's own data wins:

| Provider | MCC/MNC | APN |
|---|---|---|
| A1 | 232 01 | `a1.net` |
| Magenta (T-Mobile) | 232 03 | `gprsinternet` |
| Drei | 232 05, 232 10 | `drei.at` |
| HoT | 232 07 | `webaut` |
| spusu | 232 17 | `web` |
| yesss! | 232 12 | `web.yesss.at` |

`connection.autoconnect no` keeps the SIM off when the test is over; set it
to `yes` once it works.

### 6. Data

    nmcli device                                   # the modem device (qrtr0), gsm, connected
    b=$(mmcli -m any | grep -o '/org/freedesktop/ModemManager1/Bearer/[0-9]*' | tail -1)
    mmcli -b "$b"                                 # interface qmapmux0.0, connected yes, IPv4 (IPv6) static
    ip -br addr show qmapmux0.0
    sudo nmcli radio wifi off                      # default route and DNS from the modem only
    ip route; ip -6 route; cat /etc/resolv.conf
    ping -c 3 1.1.1.1
    ping -c 3 debian.org                           # DNS
    ping -6 -c 3 2606:4700:4700::1111              # only with an IPv6 address
    curl -sS -o /dev/null -w '%{http_code} %{speed_download} B/s\n' \
        https://deb.debian.org/debian/dists/sid/Release
    ip -s link show qmapmux0.0                     # RX/TX counters grow
    sudo nmcli radio wifi on

The USB link (usb0) is not NetworkManager's and stays up throughout.

### 7. Logs, and back

    sudo mmcli -G DEBUG                            # after the SIM is unlocked, before a connection attempt
    sudo nmcli connection down mobile; sudo nmcli --ask connection up mobile
    d=/host/kali-mobile-$(date +%F-%H%M); sudo mkdir -p $d
    sudo dmesg | sudo tee $d/dmesg.txt > /dev/null
    sudo journalctl -b -u ModemManager -u NetworkManager -u rmtfs -u tqftpserv --no-pager | sudo tee $d/journal.txt > /dev/null
    b=$(mmcli -m any | grep -o '/org/freedesktop/ModemManager1/Bearer/[0-9]*' | tail -1)
    { mmcli -m any; mmcli -b "$b"; qrtr-lookup; nmcli device; ip -d addr; } 2>&1 | sudo tee $d/state.txt > /dev/null
    sudo mmcli -G INFO

In the debug log: `Opening DPM port with TX ID: ... and RX ID: ...`,
`Creating RMNET link with flags: ingress-map-cksumv4, egress-map-cksumv4`,
the data format (`aggregation protocol ul/dl`).

Back: `sudo nmcli connection down mobile`, then `sudo pro1x-boot next leste
&& sudo reboot` (Leste is also the menu default).

## Failure pictures

| What you see | Where it breaks | Next |
|---|---|---|
| no `ipa` in `lsmod`, no ipa lines | module of another kernel: `findmnt` in step 2 empty, `/host/pro1x-modules/$(uname -r)` missing | `scripts/65-kernel-update.sh prepare` puts them there |
| `error -2 requesting "qcom/sm6115/Fxtec/QX1050/ipa_fws.mbn"` | firmware not in Kali | `ls /lib/firmware/updates/qcom/sm6115/Fxtec/QX1050/`; copy from Leste's `/lib/firmware/qcom/sm6115/Fxtec/QX1050/` |
| `error ... loading` / `error ... authenticating "...ipa_fws.mbn"` | TrustZone refused the GSI firmware (region or image) | kernel/DT question (staging region 0x56100000 vs stock 0x55600000): send the dmesg |
| `no IOMMU domain found`, GSI `timed out`, `bad state` | SMMU or GSI | dmesg to the kernel side |
| no `modem is now up`, rmtfs restarting | modem start: `systemctl status rmtfs`, `journalctl -u rmtfs`, `cat /sys/class/remoteproc/*/{name,state}` | Wi-Fi is gone too in this case; compare with Leste |
| modem up, no `rmnet_ipa0`, no service 49 in `qrtr-lookup` | the modem never finished its IPA side | `dmesg \| grep -i 'ipa\|qmi'` (`error ... init driver request`), send it |
| `mmcli -L`: no modems | ModemManager missed the QRTR node | `journalctl -u ModemManager -b \| grep -i qrtr` ("doesn't have required services"); `sudo systemctl restart ModemManager` |
| modem `failed`, reason `sim-missing`, though a SIM is in | no SIM application selected | below |
| `locked` | SIM PIN | step 4 |
| `registered` not reached (`searching`, `denied`) | radio / network | `mmcli -m any --3gpp-scan` (1-2 min), signal under `mmcli -m any` |
| `nmcli connection up` fails | APN, IP type | debug log (step 7); set `gsm.apn`; IPv4 only: `sudo nmcli connection modify mobile ipv6.method disabled` |
| connected, address on `qmapmux0.0`, but no ping, RX stays 0 | data path (QMAP, checksum flags, DPM) | debug log for the flags/DPM lines; `ip -s link show rmnet_ipa0`; dmesg; send all three |
| ping by address works, by name not | DNS | `nmcli -f IP4.DNS,IP6.DNS connection show mobile`, `/etc/resolv.conf` |

SIM application (seen with dual-SIM Qualcomm modems; postmarketOS and
Debian's qcom-phone-utils select it at boot for that reason):

    node=$(qrtr-lookup 1 | awk 'NR == 2 { print $4 }')
    sudo qmicli -p -d qrtr://$node --uim-get-card-status

If a slot says `Card state: 'present'` while `Primary GW: session doesn't
exist`, select its USIM application (slot number from `Slot [N]`, the AID
is the line under `Application ID:` of the `usim (2)` application):

    sudo qmicli -p -d qrtr://$node --uim-change-provisioning-session='slot=N,activate=yes,session-type=primary-gw-provisioning,aid=A0:00:...'
    sudo systemctl restart ModemManager

That lasts until the modem restarts. If it is needed every time, it
becomes a boot step (ExecStartPre of ModemManager after the QMI wait). A
SIM in the second slot: `mmcli -m any` lists `sim slot paths`;
`sudo mmcli -m any --set-primary-sim-slot=2`.

## Leste and Arch

**Arch** has the same stack: Arch Linux ARM's ModemManager 1.24.2 with
libqrtr-glib and libqmi, NetworkManager, mobile-broadband-provider-info
(a dependency of modemmanager), rmtfs/tqftpserv from Leste, and Kali's
units with these drop-ins (`arch/20` copies `kali/rootfs-overlay/etc/systemd/system`
and now `pro1x-modem-wait`). Mobile data goes the same way once arch/20
ran again or the two drop-ins and the helper are copied into arch.img;
Phosh shows the modem through ModemManager. Arch has no `qrtr-lookup`, so
the ModemManager wait is a no-op there (the 4 s window stays a risk:
`systemctl restart ModemManager` when `mmcli -L` is empty).

**Leste** uses oFono 2.18 (`qrtrsoc`) and `pro1x-rmnet`, which creates
`rmnet_data0..3` on `rmnet_ipa0` with `ip link add ... type rmnet mux_id N`
(the README: oFono sees the modem; data never tried). Hypothesis from the
sources, to check before relying on it: IPA 4.2 has checksum offload on
both modem endpoints (`ipa_data-v4.2.c`), so the IPA expects a MAPv4
checksum header after the QMAP header on uplink packets and appends a
checksum trailer to downlink ones (`ipa_qmap_header_size()`,
`ipa_endpoint.c`); rmnet handles both only with the link flags
`ingress-map-cksumv4`/`egress-map-cksumv4` (`rmnet_map_data.c`), which
iproute2 cannot set (only `mux_id`) and `pro1x-rmnet` does not. Without
them every uplink packet loses 4 bytes and downlink aggregates are split
wrongly. Whether oFono's qrtrsoc also opens the DPM port and sets the WDA
data format is not checked. So Leste would need the links created with
those flags (libqmi's `qmicli --link-add=...,flags=...`, or a few lines of
netlink), plus a test of oFono's data context; or ModemManager +
NetworkManager as on Kali, which Leste's connectivity stack (icd2) does not
use. Not changed now.
