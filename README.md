# Maemo Leste als zweiter Booteintrag auf dem F(x)tec Pro1X

Ziel: Sailfish OS bleibt unangetastet auf Slot A, Maemo Leste kommt als
zweites System auf Slot B.

Stand: **alles gebaut, nichts geflasht.** Kernel, Rootfs-Image und Boot-Image
sind fertig; der erste Schreibvorgang an einer Boot-Partition steht noch aus
und haengt an einer offenen Frage ([docs/slot-b-offene-frage.md](docs/slot-b-offene-frage.md)).

## Ausgangslage

| | |
|---|---|
| Geraet | F(x)tec Pro1X, `halium-qx1050`, SoC SM6115 (bengal) |
| Installiertes OS | Sailfish OS 5.0.0.78, Kernel 4.19.157-perf-halium, Android-HAL im LXC-Container |
| Rootfs SFOS | Verzeichnis `/userdata/halium-rootfs` auf `/dev/sda13` (225 GiB, ~129 GB frei) |
| `/home` | `home.img` ueber dm-crypt (`home_encrypted`), 76 GiB, nur ~9 GB frei |
| Bootloader | entsperrt (`androidboot.vbmeta.device_state=unlocked`, `verifiedbootstate=orange`) |
| Slots | A/B vollstaendig vorhanden, aktiv ist `_a` |
| `bootctl` | auf diesem Port **nicht** installiert -> Slot-Wechsel nur per fastboot vom PC |
| UFS | sechs LUNs (sda..sdf), **4-KiB-Sektoren** |

Vollstaendige Partitionsliste: [`device/partition-map.txt`](device/partition-map.txt).
`userdata` (sda13) ist die letzte Partition der LUN; zwischen ihrem Ende und dem
Plattenende liegen nur 40 Sektoren. Eine eigene Partition fuer Leste waere also
nur durch Schrumpfen von `userdata` moeglich -- und das nur offline. Deshalb
liegt Leste als ext4-Image-Datei auf `userdata`, neben `halium-rootfs`.

## Diagnose-Ergebnisse

### Slot B ist technisch intakt

`scripts/00-slot-ab-check.sh` vergleicht jedes `*_a`/`*_b`-Paar (Inhalt + Hash):

* `xbl`, `xbl_config`, `rpm`, `tz`, `hyp`, `abl`, `bluetooth`, `modem`, `dsp`,
  `keymaster`, `devcfg`, `qupfw`, `vbmeta`, `imagefv`, `uefisecapp`,
  `featenabler`, `multiimgoem`: **befuellt und bit-identisch zu Slot A**
* `mdtp`, `mdtpsecapp`, `cmnlib`, `cmnlib64`: in **beiden** Slots leer (ab Werk)
* `boot_a` == `boot_b`: in Slot B liegt aktuell dasselbe SFOS-hybris-boot
* `dtbo`: unterscheidet sich -- `dtbo_a` hat 1 Eintrag (vom SFOS-Flash),
  `dtbo_b` 37 (Werkszustand)

GPT-Slot-Attribute von `boot_b`: Priority 3, Retry 6, `SUCCESSFUL` gesetzt,
`UNBOOTABLE` nicht gesetzt -- nur `ACTIVE` steht auf A. `vbmeta_a`/`vbmeta_b`
sind unveraendert Werkszustand (signiert, Flags `0x0`); Sailfish bootet
trotzdem, weil der Bootloader entsperrt ist.

**Aber:** der NixOS-Port notiert "slot b does not boot on this phone". Diese
Widersprueche und der risikolose Test dazu stehen in
[docs/slot-b-offene-frage.md](docs/slot-b-offene-frage.md).

### Boot-Image-Parameter

Aus dem laufenden `boot_a` ausgelesen (`scripts/lib/bootimg-params.py`),
abgelegt in [`device/boot_a-params.env`](device/boot_a-params.env):
Header v2, Pagesize 4096, base 0x0, kernel 0x8000, ramdisk 0x1000000,
tags 0x100, dtb 0x1f00000. Boot-Partition 96 MiB, dtbo 24 MiB.
Dieselben Offsets verwendet auch der NixOS-Port (`lib/bootimg.nix`) --
unabhaengige Gegenprobe.

## Warum Mainline und nicht Halium

Maemo Leste nutzt **kein** Halium/libhybris, sondern ist ein Devuan-arm64-Port
auf Mainline-Kernel; einen offiziellen Porting-Guide gibt es nicht. Den
vorhandenen 4.19-Halium-Kernel weiterzuverwenden hiesse, libhybris und droid-hal
nach Devuan zu portieren -- viel Arbeit und ausserhalb dessen, was Leste traegt.

Mainline auf dem Pro1X ist dagegen weit. Wichtig: der Branch `qx1050-7.1` von
`sm6115-mainline/linux` ist **identisch mit Upstream v7.1**
(Commit `8cd9520d35a6`); der komplette Gerätesupport steckt in den 14 Patches
des NixOS-Ports, die in dessen Reihenfolge angewendet werden muessen.
Dokumentiert funktionierend: Display, Adreno 610, Touch, Hardware-Tastatur
inkl. Slider, WLAN, BT, Audio, Mikrofone, Telefonie/SMS/Daten, beide Kameras,
Vibration, Laden, Suspend. Offen/ungetestet: GPS, SD-Karte, NFC, Video-Codecs.

## Was gebaut wurde

| Artefakt | Groesse | Inhalt |
|---|---|---|
| `out/Image.gz` | 16,1 MiB | Linux 7.1 + 14 Pro1X-Patches, Config des NixOS-Ports, clang/LLVM gebaut |
| `out/sm6115-fxtec-pro1x.dtb` | 80 KiB | passender Device Tree |
| `out/modules/` | 82 MiB | Kernelmodule 7.1.0+ |
| `out/initrd.img-7.1.0+` | 11,2 MiB | initramfs-tools mit Loop-Root-Hooks |
| `out/maemo-leste.img` | 24 GiB (3,2 GiB belegt) | Leste-Rootfs (Devuan 5 daedalus, arm64-generic `20260426`) + Kernel + Module + Firmware |
| `out/boot-leste.img` | 27 MiB | Android-Boot-Image v2, passt in die 96-MiB-Partition |

Firmware-Blobs kommen aus `sm6115-mainline/firmware-fxtec-qx1050`.

## Boot-Weg

Es ist keine freie Partition fuer ein zweites Rootfs vorhanden, und modernes
initramfs-tools kennt `root=...` mit `loop=` nicht mehr. Deshalb drei kleine
Skripte in [`initramfs/`](initramfs/):

* `local-top/pro1x-loop-root` -- liest `pro1x.backing=` und `pro1x.image=` aus
  der Cmdline, wartet auf das UFS-Device, mountet es auf `/host` und haengt das
  Rootfs-Image als `/dev/loop0` ein. Danach findet initramfs-tools sein
  `root=/dev/loop0` wie gewohnt.
* `init-bottom/pro1x-move-host` -- nimmt den Backing-Mount als `/host` ins neue
  Root mit, damit das Loop-Image nicht an einem verwaisten Mount haengt (und man
  von Leste aus an das Sailfish-Rootfs kommt).
* `hooks/pro1x-loop` -- kopiert `losetup` ins Initramfs.

Cmdline des Boot-Images:

```
root=/dev/loop0 rootfstype=ext4 rootwait rw
pro1x.backing=UUID=cffbd472-3eed-4cb3-b73e-d8088ac7fcdd
pro1x.image=/maemo-leste.img
loglevel=7 ignore_loglevel earlycon=msm_geni_serial,0x4a90000
console=ttyMSM0,115200n8 console=tty1
```

Die Geraetenamen (`/dev/sdaN`) unterscheiden sich zwischen Halium-4.19 und
Mainline, darum wird das Backing-Dateisystem konsequent ueber seine UUID
gesucht, nie ueber einen Pfad.

## Zugang beim ersten Boot

* root-Passwort wird beim Imagebau gesetzt (`ROOT_PW`, Standard `pro1x-leste`)
  -- **nach dem ersten Login aendern**
* der Public Key des Build-Hosts landet in `/root/.ssh/authorized_keys`
* `agetty` auf `ttyMSM0` (115200) fuer die Bringup-Phase, zusaetzlich zur
  Konsole auf dem Panel

## Vorgehen

- [x] Geraet inventarisieren, Bootloader-Status, Slot-Belegung pruefen
- [x] Backups: `boot_a/b`, `dtbo_a/b`, `vbmeta_a/b`, `misc`, `devinfo`, `frp`,
      `keystore`, GPT (primaer + Backup) aller sechs LUNs -- auf dem Geraet und
      auf dem Build-Host, Pruefsummen beidseitig verifiziert
- [x] Boot-Image-Parameter aus `boot_a` extrahieren
- [x] Mainline-Kernel 7.1 + 14 Gerätepatches bauen (clang/LLVM, kein Cross-GCC)
- [x] Leste-Rootfs-Image mit Kernel, Modulen, Firmware und Loop-Root-Initramfs
- [x] Boot-Image bauen und gegen die `boot_a`-Parameter verifizieren
- [ ] Rootfs-Image aufs Geraet uebertragen (additiv, laeuft)
- [ ] **Slot-B-Test ohne Schreibvorgang** (siehe docs/slot-b-offene-frage.md)
- [ ] `boot_b` flashen, `dtbo_b` nullen, umschalten
- [ ] Audio (UCM2-Profile des NixOS-Ports), Sensoren (hexagonfs), Kamera
- [ ] Telefonie: Leste erwartet ofono, Mainline liefert QMI/qrtr

## Risiken

* Slot-Wechsel braucht einen PC mit fastboot (kein `bootctl` auf dem Geraet).
* `/home` hat nur ~9 GB frei; das Leste-Image gehoert nach `/userdata`.
* Ein kaputtes `boot_b` kostet einen Bootversuch, kein Brick -- ABL zaehlt die
  Retries herunter und faellt auf Slot A zurueck, `boot_a` ist gesichert.

## Scripts

| Datei | Wo | Zweck |
|---|---|---|
| `scripts/00-slot-ab-check.sh` | Geraet, root | A/B-Paare vergleichen (rein lesend) |
| `scripts/10-backup-slot-a.sh` | Geraet, root | Boot/dtbo/vbmeta/GPT sichern (rein lesend) |
| `scripts/20-fetch-sources.sh` | Build-Host | Kernel-Branch klonen, Leste-Image holen |
| `scripts/30-build-kernel.sh` | Build-Host | Patches anwenden, Kernel + Module bauen |
| `scripts/40-make-rootfs.sh` | Build-Host, sudo | Rootfs-Image bauen, Initramfs erzeugen |
| `scripts/50-build-bootimg.sh` | Build-Host | Android-Boot-Image v2 packen |
| `scripts/55-deploy-rootfs.sh` | Geraet, root | Image uebertragen und aufziehen (additiv) |
| `scripts/60-flash-slot-b.sh` | Geraet, root | **schreibend**: Boot-Image in den inaktiven Slot |
| `scripts/70-switch-slot.sh` | PC | Slot per fastboot umschalten |
| `scripts/lib/bootimg-params.py` | beide | Android-Boot-Header v0-v2 auslesen |

### Hinweis zum Build-Host (Arch)

Es ist kein aarch64-GCC nötig, der Kernel baut mit `LLVM=1` (clang + lld).
Fuer das Chroot ins arm64-Rootfs reicht das auf Arch registrierte binfmt
**nicht**: es zeigt auf das dynamisch gelinkte `qemu-aarch64`, dessen
Host-Bibliotheken im Chroot fehlen (Kindprozesse scheitern mit "required file
not found"). `40-make-rootfs.sh` registriert darum fuer die Dauer des Builds
`qemu-aarch64-static` und stellt den Ausgangszustand danach wieder her.

## Quellen

* <https://leste.maemo.org/Leste_FAQ> -- Leste nutzt kein Halium
* <https://maedevu.maemo.org/images/arm64-generic/> -- arm64-generic-Images
* <https://github.com/sm6115-mainline/linux> -- Kernel, Branch `qx1050-7.1`
* <https://github.com/sm6115-mainline/firmware-fxtec-qx1050> -- Firmware
* <https://github.com/nyymi609/fxtec-pro1x-nixos> -- Gerätepatches, Config, UCM, Hardware-Status
* <https://gitlab.com/postmarketOS/pmaports/-/merge_requests/5350> -- pmOS qx1050
* <https://gist.github.com/Kabouik/94e9e60d84c0eeae9d48e18c11ed419e> -- Dualboot Pro1
