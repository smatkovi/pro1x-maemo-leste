# Maemo Leste als zweiter Booteintrag auf dem F(x)tec Pro1X

Ziel: Sailfish OS bleibt unangetastet auf Slot A, Maemo Leste kommt als
zweites System auf Slot B. Umgeschaltet wird per `fastboot --set-active`.

Stand: Vorarbeiten abgeschlossen (Diagnose + Backups), Build noch nicht gestartet.

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

Vollstaendige Partitionsliste: [`device/partition-map.txt`](device/partition-map.txt).
`userdata` (sda13) ist die letzte Partition der LUN; zwischen ihrem Ende und dem
Plattenende liegen nur 40 Sektoren. Eine eigene Partition fuer Leste waere also
nur durch Schrumpfen von `userdata` moeglich -- und das nur offline. Deshalb:
Leste-Rootfs als ext4-Image-Datei neben `halium-rootfs`.

## Wichtigstes Ergebnis der Diagnose: Slot B ist sicher

`scripts/00-slot-ab-check.sh` vergleicht jedes `*_a`/`*_b`-Paar (Inhalt + Hash):

* `xbl`, `xbl_config`, `rpm`, `tz`, `hyp`, `abl`, `bluetooth`, `modem`, `dsp`,
  `keymaster`, `devcfg`, `qupfw`, `vbmeta`, `imagefv`, `uefisecapp`,
  `featenabler`, `multiimgoem`: **befuellt und bit-identisch zu Slot A**
* `mdtp`, `mdtpsecapp`, `cmnlib`, `cmnlib64`: in **beiden** Slots leer (so ab Werk)
* `boot_a` == `boot_b`: in Slot B liegt aktuell dasselbe SFOS-hybris-boot
* `dtbo`: unterscheidet sich -- `dtbo_a` hat 1 Eintrag (vom SFOS-Flash),
  `dtbo_b` hat 37 (Werkszustand)

Heisst: Slot B kann gebootet werden, ohne dass Firmware fehlt, und in `boot_b`
geht durch Ueberschreiben nichts verloren. Fuer einen Mainline-Kernel muss
`dtbo_b` vermutlich geleert werden, damit der Bootloader keine fremden Overlays
auf den im Boot-Image mitgelieferten DTB anwendet (der NixOS-Port loescht dtbo).

## Boot-Image-Parameter

Aus dem laufenden `boot_a` ausgelesen (`scripts/lib/bootimg-params.py`),
abgelegt in [`device/boot_a-params.env`](device/boot_a-params.env):
Header v2, Pagesize 4096, base 0x0, kernel 0x8000, ramdisk 0x1000000,
tags 0x100, dtb 0x1f00000. Boot-Partition 96 MiB, dtbo 24 MiB.

## Warum Mainline und nicht Halium

Maemo Leste nutzt **kein** Halium/libhybris, sondern ist ein Devuan-arm64-Port
auf Mainline-Kernel; einen offiziellen Porting-Guide gibt es nicht. Den
vorhandenen 4.19-Halium-Kernel weiterzuverwenden hiesse, libhybris und droid-hal
nach Devuan zu portieren -- viel Arbeit und ausserhalb dessen, was Leste traegt.

Mainline auf dem Pro1X ist dagegen weit: `sm6115-mainline/linux`, Branch
`qx1050-7.1` (Stand August 2026). Der NixOS-Port fuer dasselbe Geraet
dokumentiert als funktionierend: Display, Adreno 610, Touch, Hardware-Tastatur
inkl. Slider, WLAN, BT, Audio, Mikrofone, Telefonie/SMS/Daten, beide Kameras,
Vibration, Laden, Suspend, FDE. Offen/ungetestet: GPS, SD-Karte, NFC,
Video-Codecs, Headset-Erkennung.

Leste-Seite: Rootfs aus den aktiv gebauten `arm64-generic`-Images
(`20260426-daedalus`), Kernel bringen wir selbst mit.

## Vorgehen

- [x] Geraet inventarisieren, Bootloader-Status, Slot-Belegung pruefen
- [x] Backups: `boot_a/b`, `dtbo_a/b`, `vbmeta_a/b`, `misc`, `devinfo`, `frp`,
      `keystore`, GPT (primaer+Backup) aller sechs LUNs -- auf dem Geraet und
      auf den Build-Host gespiegelt, Pruefsummen beidseitig verifiziert
- [x] Boot-Image-Parameter aus `boot_a` extrahieren
- [ ] Mainline-Kernel `qx1050-7.1` bauen (clang/LLVM, kein Cross-GCC noetig)
- [ ] Leste-Rootfs aus dem arm64-generic-Image nach `/userdata/maemo-leste.img`
- [ ] Initramfs: `userdata` ueber `by-partlabel` mounten, Image per Loop,
      `switch_root`. Achtung: Blockgeraete-Namen unterscheiden sich zwischen
      4.19-Halium und Mainline -- nie `/dev/sdaN` hart verdrahten
- [ ] Boot-Image bauen, `dtbo_b` leeren, `boot_b` flashen
- [ ] Mit `fastboot --set-active=b` testen; Rueckweg immer `--set-active=a`
- [ ] Firmware-Blobs aus dem laufenden SFOS extrahieren, soweit Mainline sie
      nicht selbst mitbringt
- [ ] Telefonie: Leste erwartet ofono, Mainline liefert QMI/qrtr -- zu verdrahten

## Risiken

* Slot-Wechsel braucht einen PC mit fastboot (kein `bootctl` auf dem Geraet).
* `/home` hat nur ~9 GB frei; das Leste-Image gehoert nach `/userdata`, nicht
  nach `/home`.
* Ein kaputtes `boot_b` kostet einen Bootversuch, kein Brick -- A/B faellt auf
  Slot A zurueck, und `boot_a` ist gesichert.

## Scripts

| Datei | Wo | Zweck |
|---|---|---|
| `scripts/00-slot-ab-check.sh` | Geraet, root | A/B-Paare vergleichen (rein lesend) |
| `scripts/10-backup-slot-a.sh` | Geraet, root | Boot/dtbo/vbmeta/GPT sichern (rein lesend) |
| `scripts/20-fetch-sources.sh` | Build-Host | Kernel-Branch klonen, Leste-Image holen |
| `scripts/lib/bootimg-params.py` | beide | Android-Boot-Header v0-v2 auslesen |

## Quellen

* <https://leste.maemo.org/Leste_FAQ> -- Leste nutzt kein Halium
* <https://maedevu.maemo.org/images/arm64-generic/> -- arm64-generic-Images
* <https://github.com/sm6115-mainline/linux> -- Kernel, Branch `qx1050-7.1`
* <https://github.com/nyymi609/fxtec-pro1x-nixos> -- Hardware-Status Mainline
* <https://gitlab.com/postmarketOS/pmaports/-/merge_requests/5350> -- pmOS qx1050
* <https://gist.github.com/Kabouik/94e9e60d84c0eeae9d48e18c11ed419e> -- Dualboot Pro1
