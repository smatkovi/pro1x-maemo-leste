# Slot B: bootet, aber das DTBO stimmt dort nicht (geklärt)

Die Frage war, ob der stock-ABL des Pro1X den inaktiven Slot bootet. Sie ist
geklärt: **ja, Slot B bootet.** Der Hinweis im NixOS-Port
(`lib/artifacts.nix`: "slot b does not boot on this phone") gilt für dessen
U-Boot-Image, nicht allgemein.

## Was auf Slot B tatsächlich passiert

Sailfish wurde auf diesem Gerät schon einmal aus Slot B gestartet. Es bootete,
aber **die Tastatur war tot**. Die Ursache liegt im DTBO, nicht im Boot-Image:

| | `dtbo_a` (Sailfish-Flash) | `dtbo_b` (Werkszustand) |
|---|---|---|
| Einträge | 1 | 37 |
| Pagesize | 2048 | 4096 |
| Gesamtgröße | 122.640 B | 3.935.828 B |
| `aw9523b@5b` (Tastaturmatrix) | **vorhanden** | fehlt in Eintrag 0 |
| `idea_keys` (alt/shift/ctrl/home/Pfeile) | **vorhanden** | fehlt |
| `:kbd_backlight` | **vorhanden** | fehlt |

Das von Sailfish geflashte Overlay ist unter den 37 Werks-Einträgen von Slot B
**nicht enthalten** (kein SHA256-Treffer gegen `dtboA-00`). `boot_a` und
`boot_b` sind dagegen bit-identisch. Heißt: aus Slot B lief dasselbe
hybris-boot, bekam aber ein Werks-Overlay ohne den AW9523B-Knoten vom
Bootloader untergeschoben — und ohne diesen Knoten findet der Halium-Kernel
die Tastaturmatrix nicht.

Bestätigend dazu die Kernel-Cmdline von Slot A: `androidboot.dtbo_idx=0`.
Auf Slot A gibt es nur diesen einen (richtigen) Eintrag, auf Slot B ist Index 0
ein ganz anderes Overlay.

## Konsequenz für Maemo Leste: kein Problem

Der Mainline-Kernel bringt seinen Device Tree **im Boot-Image** mit
(Header v2, `--dtb`), nicht über ein Overlay. Im gebauten
`sm6115-fxtec-pro1x.dtb` steckt die Hardware komplett drin, nachgeprüft:

* `fxtec,pro1x-aw9523-keypad` samt vollständiger 64-Tasten-`linux,keymap`,
  dazu `kbd-backlight` und `kbd-capslock`
* `qcom,wcn3990-wifi`, `qcom,wcn3950-bt`
* `awinic,aw86224` (Vibration), `fxtec,pro1x-charger-icl`, `qcom,pm6125`
* `qcom,sm6115-mpss-pas` und `-adsp-pas` mit den Firmwarepfaden
  `qcom/sm6115/Fxtec/QX1050/*.mbn`
* `sony,imx582`, `samsung,s5k4h7`, `dongwoon,dw9800`, `qcom,qcm2290-camss`
* Goodix-Touch, Hall-Switch, Panel

Deshalb wird `dtbo_b` beim Flashen **genullt** (`ERASE_DTBO=1` in
`scripts/60-flash-slot-b.sh`): so legt der Bootloader kein fremdes Overlay auf
unseren DTB. Denselben Weg geht der NixOS-Port ("flashing erases dtbo").

## Falls Sailfish später auf Slot B soll

Dann muss `dtbo_a` nach `dtbo_b` kopiert werden — ohne das richtige Overlay
bleibt die Tastatur dort tot. Das ist genau das, was in unserem Setup
umgekehrt nicht passieren darf, also nicht beides gleichzeitig: entweder
`dtbo_b` genullt für Leste, oder `dtbo_b` = `dtbo_a` für Sailfish.

## Rückweg

`boot_a`, `dtbo_a`, `vbmeta_a` und die GPTs aller sechs LUNs liegen gesichert
in `/userdata/dualboot-backup` und gespiegelt auf dem Build-Host. Umschalten
in beide Richtungen per `fastboot --set-active=a|b`
(`scripts/70-switch-slot.sh`). Scheitert ein Bootversuch, zählt ABL die
Retries herunter und fällt auf Slot A zurück.
