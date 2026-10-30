# PC test: oFono's IPA data path against a simulated modem

Checks `patches/fixes/ofono.patch` (Leste's oFono 2.18 with the mainline
IPA data path, [docs/leste-mobile-data.md](../../docs/leste-mobile-data.md))
without the phone: the patched `ofonod` enables a simulated modem and makes
its rmnet links on a dummy `rmnet_ipa0`.

| File | Role |
|---|---|
| `run.sh` | the driver: gets an ofonod (the release's arm64 packages in a chroot of Leste's image, or `OFONOD=` built on the PC), loads `qrtr`, `rmnet`, `dummy`, runs the cases, checks them |
| `run-one.sh` | one case, as root in its own mount namespace: dummy `rmnet_ipa0`, the IPA's sysfs files, a private D-Bus, the fake modem, ofonod, then the modem powered over D-Bus |
| `fakemodem.py` | the modem on the PC's QRTR: DMS, UIM, WDS, NAS, WDA, DPM; answers Get Operating Mode, DPM Open Port, WDA Get/Set Data Format as the scenario says, logs every request with its TLVs |
| `sysfs-shim.c` | `LD_PRELOAD` for ofonod: `.../rmnet_ipa0/device/*` (endpoint ids, offload names of the IPA v4.2) from a directory, since a dummy device has none |
| `bus.conf` | the private bus's configuration |

Run (build host, sudo; about 5 minutes with the chroot):

    BASE_IMG=... CHROOT=... DELETE=1 bash tests/ofono-ipa/run.sh
    OFONOD=/path/to/ofonod bash tests/ofono-ipa/run.sh v5 silent

The first form downloads the release's `ofono_*.deb` (`RELEASE=`, checked
against its `SHA256SUMS`; or `DEBS=dir`), installs it in
`scripts/leste-chroot.sh`'s chroot and runs it there under qemu; `DELETE=1`
removes the chroot afterwards. `KEEP=1` keeps the results
(`/tmp/ofono-ipa-test.*/<case>/`: `fake.log`, `ofonod.log`, `props.txt`,
`rmnet.txt`, ...); a failed run keeps them anyway.

The cases and what each checks:

| Case | The modem | Checked |
|---|---|---|
| `v5` | takes QMAPv5 | udevng's mainline IPA path; DPM Open Port with TLV 0x11 `01 04000000 01000000 01000000 09000000` (one port: embedded, interface 1, modem RX endpoint 1, TX 9); WDA set QMAPv5; modem powered; `rmnet0..3` with flags 0xd |
| `qmap` | reports QMAP whatever is set | WDA set 9, 8, 5; QMAP taken; links 0xd |
| `refuse` | refuses QMAPv5 and QMAPv4 | WDA set 9, 8, 5; QMAP taken; links 0xd |
| `missing` | wants the endpoint info TLV | Get again with TLV 0x10, Set with TLV 0x17 (`04000000 01000000`) |
| `nodpm` | has no DPM | no DPM message; data still set up |
| `nowda` | refuses every WDA format | `no mobile data: ...`; modem powered anyway (calls, SMS); no links |
| `silent` | never answers DPM | after 20 s `no mobile data: no answer from the modem`; modem powered; no links |
| `stale` | as `v5`, with `rmnet_data0..3` made before oFono starts (what `pro1x-rmnet` did) | oFono deletes all four; then its own `rmnet0..3` |
| `override` | as `v5`, ofonod with `OFONO_IPA_WDA=qmap OFONO_IPA_DPM=0 OFONO_IPA_RMNET_FLAGS=0x1` | no DPM; only QMAP; links with flags 0x1 |

Not covered: what the real modem firmware answers, the data contexts
(`BIND_MUX_DATA_PORT`, `START_NETWORK`), icd2, any traffic. That is the
device test in the doc.

The cases run in the PC's initial network namespace, because the PC kernel
creates QRTR sockets nowhere else: the dummy `rmnet_ipa0` (and the rmnet
links oFono puts on it) exist on the PC for a few seconds per case and are
deleted after it. `run.sh` refuses to start while the PC has a
`rmnet_ipa0` or any rmnet link (oFono deletes every rmnet link it finds when
it starts), and udevng also looks at the PC's own devices: no USB modem
plugged in meanwhile. The PC's system bus and `/var/lib/ofono` stay out of
it (private bus, tmpfs in the case's mount namespace). Modules it loaded
are unloaded at the end.
