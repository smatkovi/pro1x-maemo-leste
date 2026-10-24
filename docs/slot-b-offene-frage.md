# Offene Frage: bootet Slot B auf dem Pro1X überhaupt?

Der ganze Dualboot-Plan hängt daran, dass der stock-ABL des Pro1X den
inaktiven Slot booten kann, wenn man ihn aktiv schaltet. Dazu gibt es
widersprüchliche Hinweise.

## Hinweis gegen Slot B

Im NixOS-Port (`lib/artifacts.nix`) steht im Flash-Skript wörtlich:

> First install: U-Boot into boot_a (**slot b does not boot on this phone**),
> vbmeta, rootfs.

Jemand, der das Gerät nachweislich zum Laufen gebracht hat, hat Slot B also
als nicht bootfähig erlebt. Warum, steht dort nicht — es kann an ihrem
U-Boot-Image liegen (U-Boot als Android-„Kernel" mit manipuliertem
`text_offset`) oder an einer ABL-Eigenheit des Geräts.

## Hinweise für Slot B

Alles, was am Gerät messbar ist, spricht nicht gegen Slot B:

* Sämtliche `*_b`-Firmwarepartitionen sind befüllt und **bit-identisch** zu
  Slot A: `xbl_b`, `xbl_config_b`, `rpm_b`, `tz_b`, `hyp_b`, `abl_b`,
  `modem_b`, `dsp_b`, `keymaster_b`, `devcfg_b`, `qupfw_b`, `imagefv_b`,
  `uefisecapp_b`, `featenabler_b`, `bluetooth_b`.
* `boot_b` enthält aktuell dasselbe hybris-boot wie `boot_a` (identischer
  SHA256) — Slot B ist also mit einem bootfähigen Image bestückt.
* Die GPT-Slot-Attribute von `boot_b` sind gesund:
  Priority 3, Retry 6, `SUCCESSFUL` gesetzt, `UNBOOTABLE` **nicht** gesetzt —
  nur `ACTIVE` steht auf A:

  | Partition | attr (hi16) | ACTIVE | TRIES | SUCCESSFUL | UNBOOTABLE |
  |---|---|---|---|---|---|
  | `boot_a` | 0x0077 | ja | 6 | ja | nein |
  | `boot_b` | 0x0073 | nein | 6 | ja | nein |

  (Bitlage nach Qualcomm `gpt-utils`: Priority 48-49, Active 50,
  Retry 51-53, Successful 54, Unbootable 55.)
* `vbmeta_a` und `vbmeta_b` sind identisch und unverändert Werkszustand
  (signiert, Flags `0x0`, also *keine* abgeschaltete Verifikation) — Sailfish
  bootet trotzdem, weil der Bootloader entsperrt ist (`verifiedbootstate=orange`).
  Die AVB-Begründung des NixOS-Ports („ABL refuses unsigned images") greift in
  diesem Zustand also nicht zwingend.

Die Slot-B-Partitionen außer `boot_b` haben allerdings Priority 0 / Retry 0
(`dtbo_b`, `vbmeta_b`, `modem_b` = `0x0040`/`0x1040`). Genau das räumt
`fastboot --set-active=b` auf, indem es Priority, Retry und Active für den
ganzen Slot setzt.

## Risikoloser Test, bevor irgendwas geflasht wird

Weil `boot_b` **schon jetzt** das identische Sailfish-Boot-Image enthält,
lässt sich die Frage ohne jeden Schreibvorgang klären:

1. Gerät per USB an den Build-Host, `reboot bootloader`
2. `fastboot --set-active=b`
3. `fastboot reboot`

Ergebnis:

* **Sailfish startet** → Slot-Umschaltung funktioniert, der Dualboot-Plan
  trägt. Danach Leste-Boot-Image nach `boot_b` und wieder umschalten.
* **Gerät bootet nicht / fällt auf Slot A zurück** → die NixOS-Notiz gilt auch
  für uns. Rückweg jederzeit per `fastboot --set-active=a`.

Fehlschlag kostet einen Bootversuch, kein Brick: ABL zählt bei Misserfolg die
Retries herunter und fällt auf Slot A zurück, Backups von `boot_a`/`dtbo_a`
liegen auf Gerät und Build-Host.

## Fallback, falls Slot B wirklich nicht bootet

Dann gibt es Dualboot nicht über zwei Slots, sondern nur über **ein**
Boot-Image, das beim Start auswählt:

* U-Boot (wie im NixOS-Port) nach `boot_a`, mit zwei Einträgen im Boot-Menü:
  einmal der Mainline-Kernel für Leste, einmal der Halium-4.19-Kernel samt
  hybris-Initramfs für Sailfish. Beide Kernel liegen dann als Dateien auf
  `userdata`, nicht in Boot-Partitionen. Das ist der aufwändigere, aber
  belastbarere Weg — und er liefert ein echtes Bootmenü statt eines
  Slot-Schalters, der ohne `bootctl` nur am PC umlegbar ist.
* Nachteil: Sailfish wird dann aus U-Boot statt direkt vom ABL gestartet;
  dessen hybris-boot-Initramfs erwartet das nicht und muss entsprechend
  gefüttert werden (Kernel + Ramdisk getrennt, passende Cmdline).
