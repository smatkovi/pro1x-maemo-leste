# Übergabe: Maemo Leste auf dem F(x)tec Pro1X — Stand 24.10.2026, 17:15

Zum Mitlesen im Browser: <https://claude.ai/artifact/S8E6wTczpWF4kZie3P8wXd>

Diese Übergabe ist für eine Session **auf archbang** (192.168.1.21, User
`sebastian`). Dort liegen Buildbaum, Artefakte, Backups und `fastboot`-Zugang.
Das Telefon selbst ist kein verlässlicher Arbeitsplatz mehr: die bisherigen
Sessions liefen *auf* Sailfish und starben mit jedem Bootversuch.

## Stand in einem Satz

Alles ist gebaut und liegt bereit, **Slot B bootet aber nicht** — zwei
Versuche, beide mit vollständigem Verbrauch der sechs ABL-Retries und
automatischem Rückfall auf Sailfish. Sailfish selbst ist unbeschädigt.

## Aktueller Zustand (gemessen, nicht vermutet)

Telefon läuft Sailfish OS 5.0.0.78 aus **Slot A**, erreichbar unter
`defaultuser@192.168.1.15` (SSH-Key von archbang ist hinterlegt, `sudo` ohne
Passwort).

GPT-Slot-Attribute (Qualcomm-Bitlage: Priority 48-49, Active 50, Retry 51-53,
Successful 54, Unbootable 55):

| Partition | hi16 | Prio | Active | Tries | Successful | Unbootable |
|---|---|---|---|---|---|---|
| `boot_a` | 0x007f | 3 | ja | 7 | ja | nein |
| `dtbo_a` | 0x007f | 3 | ja | 7 | ja | nein |
| `vbmeta_a` | 0x107f | 3 | ja | 7 | ja | nein |
| `boot_b` | 0x0082 | 2 | nein | **0** | **nein** | **ja** |
| `dtbo_b` | 0x0040 | 0 | nein | 0 | ja | nein |
| `vbmeta_b` | 0x1040 | 0 | nein | 0 | ja | nein |

Auf dem Telefon:

| Pfad | Größe | Inhalt |
|---|---|---|
| `/userdata/maemo-leste.img` | 24 GiB (3,5 GB belegt) | Leste-Rootfs, ext4 clean, 6291456 Blöcke |
| `/userdata/boot-leste.img` | 96 MiB | Boot-Image mit AVB-Footer, identisch zu `boot_b` |
| `/userdata/dualboot-backup/` | 243,7 MB | `boot_a/b`, `dtbo_a/b`, `vbmeta_a/b`, `misc`, `devinfo`, `frp`, `keystore`, GPT aller 6 LUNs |
| `/userdata/60-flash-slot-b.sh` | — | Flash-Skript (verlangt Argument `flash`) |
| `~defaultuser/reboot-bootloader.py` | — | Reboot in den Bootloader per `reboot(2)`-Syscall |

Auf archbang unter
`/run/media/sebastian/2e638a7f-26db-4e89-9446-81d688464798/pro1x-leste/`:

| Pfad | Inhalt |
|---|---|
| `repo/` | Git-Repo, identisch mit <https://github.com/smatkovi/pro1x-maemo-leste> (öffentlich) |
| `out/Image.gz` | Linux 7.1 + 14 Pro1X-Patches, 16,1 MiB |
| `out/initrd.img-7.1.0+` | 11,2 MiB, zstd, mit Loop-Root-Hooks |
| `out/sm6115-fxtec-pro1x.dtb` | 80 KiB |
| `out/boot-leste.img` | 96 MiB, Header v2 + AVB-Footer |
| `out/maemo-leste.img` | 24 GiB, **gleiche Geometrie** wie die Kopie auf dem Telefon (rsync-Delta möglich) |
| `out/modules/` | Module 7.1.0+ |
| `build/linux/` | Kernelbaum, Patches angewendet |
| `build/nixos-pro1x/` | Referenz-Port (Patches, Config, UCM, U-Boot-Rezept) |
| `backup-slot-a/` | Spiegel der Gerätebackups, `SHA256SUMS.rel` verifiziert |
| `switch-and-connect.log`, `switch2.log`, `ramboot.log` | Protokolle der drei Versuche |

## Was zweimal versucht wurde

**Versuch 1, 16:00:51** — `boot_b` mit mkbootimg-Image ohne AVB-Strukturen,
`dtbo_b` genullt, `fastboot --set-active=b`, Reboot. Kein USB-Gadget innerhalb
von 5 min. Danach Sailfish aus Slot A, `boot_b` auf Tries 0 / Unbootable.

**Versuch 2, 17:01:33** — identisch, aber mit AVB-Hash-Footer
(`avbtool add_hash_footer --algorithm NONE --partition_name boot
--partition_size 100663296`), nachgebaut exakt nach dem funktionierenden
`boot_a`. Ergebnis gleich: kein Gadget, Rückfall, `boot_b` wieder unbootable.
Laut Nutzer kam das Gerät „nicht über fastboot hinaus".

**Zwischenversuch** — `fastboot boot out/boot-leste.img` (RAM-Boot, wäre
risikofrei gewesen): Image wird übertragen (98304 KB OKAY), dann
`Booting FAILED (remote: 'unknown command')`. **Der Pro1X-ABL kennt
`fastboot boot` nicht.** Testen heißt auf diesem Gerät zwangsläufig flashen.

## Was dadurch ausgeschlossen ist

* **Firmware von Slot B** — alle `*_b`-Partitionen sind bit-identisch zu Slot A
  (`xbl`, `xbl_config`, `rpm`, `tz`, `hyp`, `abl`, `modem`, `dsp`, `keymaster`,
  `devcfg`, `qupfw`, `imagefv`, `uefisecapp`, `featenabler`, `bluetooth`).
* **Kernelformat** — `boot_a` und unser Image beginnen beide mit gzip (`1f8b`);
  im dekomprimierten Image steht Magic `ARM\x64`, `text_offset = 0x0`,
  EFI-Stub vorhanden. Genau das, was der ABL laut NixOS-Port erwartet.
* **Initramfs-Kompression** — zstd, und der Kernel hat `CONFIG_RD_ZSTD=y`.
* **AVB-Struktur** — ab Versuch 2 vorhanden und verifiziert: `AVBf` bei
  Partitionsende − 64, `AVB0` (512 B) bei Offset 28692480, Algorithm NONE,
  sha256-Hash-Deskriptor für Partition `boot`. Strukturgleich zu `boot_a`.
* **Boot-Image-Offsets** — aus `boot_a` ausgelesen und unabhängig bestätigt
  durch `lib/bootimg.nix` des NixOS-Ports: base 0x0, kernel 0x8000,
  ramdisk 0x1000000, second 0xf00000, tags 0x100, dtb 0x1f00000, Pagesize 4096,
  Header v2.
* **Größe** — 96 MiB Image in eine 96-MiB-Partition, Payload 27,4 MiB.

## Offene Hypothesen, nach Priorität

**H1 — Slot B bootet auf diesem Gerät grundsätzlich nicht.** Dafür spricht die
Notiz im NixOS-Port (`lib/artifacts.nix`): *„First install: U-Boot into boot_a
(slot b does not boot on this phone)"* — jemand mit funktionierendem Port hat
dasselbe erlebt. Und zwei unserer Versuche sind damit konsistent.
*Gegenargument:* der Nutzer erinnert sich, Sailfish schon einmal aus Slot B
gestartet zu haben, damals mit toter Tastatur. Dazu passt der DTBO-Befund
(unten), aber ein Beweis, dass Slot B je gebootet hat, liegt nicht vor.

**H2 — unser Boot-Image bootet generell nicht**, unabhängig vom Slot. Kandidaten:
das Loop-Root-Initramfs (`root=/dev/loop0` wird erst durch unser
`local-top`-Skript erzeugt), die übernommene NixOS-Kernelconfig, oder die
Nullung des DTBO.

**H1 und H2 sind mit einem einzigen Experiment zu trennen** — siehe unten.

**H3 — vbmeta.** Der NixOS-Port flasht ein eigenes vbmeta mit abgeschalteter
Verifikation (`scripts/mkvbmeta.py` in `build/nixos-pro1x/`). Unsere
`vbmeta_a`/`vbmeta_b` sind unverändert Werkszustand (signiert, Flags 0x0).
Sailfish bootet damit, unser Image vielleicht nicht.

## Der entscheidende nächste Test

Das Leste-Image in den **aktiven Slot A** flashen — den Slot, von dem
nachweislich gebootet wird. Bootet es dort, ist das Image gut und H1 bestätigt.
Bootet es auch dort nicht, ist H2 dran und der Slot war nie das Problem.

Wichtig: `dtbo_a` muss für diesen Test mit geleert werden, weil der ABL sonst
das Sailfish-Overlay auf unseren Mainline-DTB legt. Beides wird danach aus dem
Backup zurückgeschrieben.

```bash
cd /run/media/sebastian/2e638a7f-26db-4e89-9446-81d688464798/pro1x-leste

# 0. Backups verifizieren, BEVOR irgendwas geschrieben wird
( cd backup-slot-a && sha256sum -c SHA256SUMS.rel )

# 1. Telefon in den Bootloader (läuft Sailfish, Slot A)
ssh defaultuser@192.168.1.15 'sudo python3 ~/reboot-bootloader.py'

# 2. warten, bis fastboot da ist
until fastboot devices | grep -q .; do sleep 2; done

# 3. Leste in Slot A, DTBO leeren
fastboot flash boot_a out/boot-leste.img
fastboot erase dtbo_a
fastboot --set-active=a
fastboot reboot

# 4. auf Lebenszeichen warten: USB-Ethernet (enx*/usb*) und ttyACM0
#    screen /dev/ttyACM0 115200     -> Kernel-Log und Login-Prompt
#    ssh root@172.16.42.1           -> Passwort pro1x-leste
```

**Rückweg, immer und in jedem Fall** (Gerät im Bootloader; notfalls Power-Taste
10-15 s, dann Tastenkombination bzw. erneut `reboot-bootloader.py` aus
Sailfish):

```bash
fastboot flash boot_a backup-slot-a/boot_a.img    # 96 MiB, mit Footer+vbmeta
fastboot flash dtbo_a backup-slot-a/dtbo_a.img    # 24 MiB, enthält aw9523b@5b
fastboot --set-active=a
fastboot reboot
```

Kommt das Gerät gar nicht mehr in den Bootloader, bleibt EDL — dafür liegen
die GPTs aller sechs LUNs im Backup, aber das ist die letzte Stufe.

## Wenn H1 bestätigt wird: U-Boot statt Slot-Wechsel

Dann gibt es Dualboot nicht über zwei Slots, sondern über **ein** Boot-Image
mit Menü. `build/nixos-pro1x/packages/uboot/` und `lib/artifacts.nix` zeigen
das Rezept; dort ist auch der Fallstrick dokumentiert:

> U-Boot's arm64 Image header has text_offset = -0x80000000; the stock ABL
> rejects that (straight back to fastboot). Linux has 0 there.

Also: U-Boot bauen, die 8 Byte bei Offset 8 nullen, gzippen, DTB anhängen, per
mkbootimg mit denselben Offsets paketieren, nach `boot_a`. Danach bootet U-Boot
beide Kernel als Dateien von `userdata`:

* Eintrag 1 — Sailfish: Kernel und Ramdisk aus `backup-slot-a/boot_a.img`
  extrahieren (Offsets im Header, `repo/scripts/lib/bootimg-params.py` liest
  sie aus), Cmdline aus `backup-slot-a/cmdline-slot-a.txt`
* Eintrag 2 — Leste: `out/Image.gz`, `out/initrd.img-7.1.0+`,
  `out/sm6115-fxtec-pro1x.dtb`, Cmdline wie unten

## Technische Details, die man sonst neu erarbeiten müsste

**Kernel.** Der Branch `qx1050-7.1` von `sm6115-mainline/linux` ist *identisch
mit Upstream v7.1* (Commit `8cd9520d35a6`) — der Gerätesupport steckt
vollständig in den 14 Patches des NixOS-Ports und muss in **dieser Reihenfolge**
angewendet werden: touch, egpio, dsi, panelgap, dpugap, audio, q6voice, camera,
keyboard, leds, power, haptics, bluetooth, ipa. Gebaut mit `LLVM=1` (clang 23,
kein aarch64-GCC auf dem Host nötig), Config ist
`build/nixos-pro1x/device/kernel/config.aarch64`. Modulversion `7.1.0+`.

**Loop-Root.** Es gibt keine freie Partition für ein zweites Rootfs (`userdata`
ist die letzte Partition der LUN, danach 40 Sektoren), und modernes
initramfs-tools kennt `loop=` nicht mehr. Daher liegt Leste als Image-Datei auf
`userdata`, und drei Skripte in `repo/initramfs/` erledigen den Rest:
`local-top/pro1x-loop-root` (Backing-FS über UUID suchen, auf `/host` mounten,
Image als `/dev/loop0` einhängen), `init-bottom/pro1x-move-host`,
`hooks/pro1x-loop` (`losetup` ins Initramfs). Verifiziert: die Skripte,
`losetup`, `blkid`, `mountpoint` und busybox sind im Initramfs enthalten.

**Cmdline des Boot-Images:**

```
root=/dev/loop0 rootfstype=ext4 rootwait rw
pro1x.backing=UUID=cffbd472-3eed-4cb3-b73e-d8088ac7fcdd
pro1x.image=/maemo-leste.img
loglevel=7 ignore_loglevel earlycon=msm_geni_serial,0x4a90000
console=ttyMSM0,115200n8 console=ttyGS0,115200 console=tty1
```

`tty1` steht zuletzt und ist damit `/dev/console` (Ausgabe bleibt auf dem
Panel). UFS-Gerätenamen unterscheiden sich zwischen Halium-4.19 und Mainline,
darum wird das Backing-FS über die UUID gesucht, nie über `/dev/sdaN`.

**Zugang in Leste.** root-Passwort `pro1x-leste` (ändern!), archbangs Public Key
in `/root/.ssh/authorized_keys`, `sshd` startet automatisch (`S03ssh`).
`usb0` statisch auf **172.16.42.1/24**, Gegenstelle am PC 172.16.42.2/24, kein
DHCP. `getty` auf `ttyGS0` (inittab-Tag `T2`) und `ttyMSM0` (`T0`) — WLAN ist
**nicht** vorkonfiguriert, Maemo verwaltet `wlan0` über seinen eigenen Stack.
Der g_cdc-Gadget ist eingebaut (`CONFIG_USB_CDC_COMPOSITE=y`) und bindet von
allein; er liefert `usb0` und `ttyACM0` am PC.

**DTBO.** `dtbo_a` (von Sailfish geflasht) hat **1** Eintrag und enthält
`aw9523b@5b` (Tastaturmatrix), `idea_keys`, `:kbd_backlight`, Goodix-Touch.
`dtbo_b` ist Werkszustand mit **37** Einträgen, und Eintrag 0 hat keinen dieser
Knoten; das Sailfish-Overlay ist unter den 37 überhaupt nicht enthalten (kein
SHA256-Treffer). Das erklärt die tote Tastatur, die der Nutzer von einem
früheren Slot-B-Start erinnert. Für Leste ist das irrelevant: der Mainline-DTB
liegt im Boot-Image und enthält die Hardware selbst — geprüft:
`fxtec,pro1x-aw9523-keypad` mit vollständiger 64-Tasten-`linux,keymap`,
`qcom,wcn3990-wifi`, `qcom,wcn3950-bt`, `awinic,aw86224`,
`fxtec,pro1x-charger-icl`, `qcom,sm6115-mpss-pas`/`-adsp-pas` mit den
Firmwarepfaden `qcom/sm6115/Fxtec/QX1050/*.mbn`, `sony,imx582`,
`samsung,s5k4h7`, `dongwoon,dw9800`, `qcom,qcm2290-camss`.

**Firmware-Blobs** kommen aus `sm6115-mainline/firmware-fxtec-qx1050` und sind
im Rootfs-Image unter `/lib/firmware` installiert.

**Fallstricke, die Zeit gekostet haben.** UFS hat **4-KiB-Sektoren** — GPT-Dumps
mit `bs=512` schneiden die Partitionseinträge ab. Sailfish hat **kein
`truncate`** (busybox), Imagegrößen per `python3` setzen. Das auf Arch
registrierte binfmt zeigt auf das **dynamisch** gelinkte `qemu-aarch64`, dessen
Host-Bibliotheken im Chroot fehlen; `40-make-rootfs.sh` registriert für die
Dauer des Builds `qemu-aarch64-static` und stellt den Zustand danach wieder her.
Der Erst-Transfer des 24-GiB-Images per rsync dauert Stunden (volle apparent
size, 1,5 MB/s) — stattdessen `resize2fs -M`, zstd-Stream, auf dem Gerät wieder
aufziehen; **danach** ist rsync mit `--inplace` das richtige Werkzeug, die
Geometrie beider Seiten ist dafür schon angeglichen.

## Skripte im Repo

| Datei | Wo | Zweck |
|---|---|---|
| `00-slot-ab-check.sh` | Gerät, root | A/B-Paare vergleichen (lesend) |
| `10-backup-slot-a.sh` | Gerät, root | Boot/dtbo/vbmeta/GPT sichern (lesend) |
| `20-fetch-sources.sh` | Build-Host | Kernel-Branch, Leste-Image holen |
| `30-build-kernel.sh` | Build-Host | Patches anwenden, Kernel + Module bauen |
| `40-make-rootfs.sh` | Build-Host, sudo | Rootfs-Image + Initramfs bauen |
| `50-build-bootimg.sh` | Build-Host | Boot-Image v2 packen **inkl. AVB-Footer** |
| `55-deploy-rootfs.sh` | Gerät, root | Erstdeployment des Rootfs-Images |
| `56-update-rootfs.sh` | Gerät, root | Updates per rsync-Delta |
| `60-flash-slot-b.sh` | Gerät, root | schreibend: Boot-Image in den inaktiven Slot |
| `70-switch-slot.sh` | PC | Slot per fastboot umschalten |
| `80-switch-and-connect.sh` | PC, losgelöst | umschalten, booten, USB-Link, SSH-Test |
| `81-ramboot-and-connect.sh` | PC | RAM-Boot-Test — **auf diesem Gerät nutzlos**, ABL kennt `fastboot boot` nicht |
| `lib/bootimg-params.py` | beide | Android-Boot-Header v0-v2 auslesen |
| `lib/reboot-bootloader.py` | beide, root | Reboot in den Bootloader |

## Quellen

* <https://github.com/sm6115-mainline/linux> — Kernel, Branch `qx1050-7.1`
* <https://github.com/sm6115-mainline/firmware-fxtec-qx1050> — Firmware
* <https://github.com/nyymi609/fxtec-pro1x-nixos> — Gerätepatches, Config,
  U-Boot-Rezept, Hardware-Status
* <https://maedevu.maemo.org/images/arm64-generic/> — Leste-Images
* <https://leste.maemo.org/Leste_FAQ> — Leste nutzt kein Halium
* <https://gist.github.com/Kabouik/94e9e60d84c0eeae9d48e18c11ed419e> — Dualboot Pro1
