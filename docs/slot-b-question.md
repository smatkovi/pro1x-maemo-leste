# Slot B: boots, but the DTBO there is wrong (settled)

The question was whether the stock ABL of the Pro1X boots the inactive slot.
It is settled: **yes, slot B boots.** The note in the NixOS port
(`lib/artifacts.nix`: "slot b does not boot on this phone") applies to its
U-Boot image, not in general.

## What actually happens on slot B

Sailfish was once started from slot B on this device. It booted, but **the
keyboard was dead**. The cause is the DTBO, not the boot image:

| | `dtbo_a` (Sailfish flash) | `dtbo_b` (factory state) |
|---|---|---|
| Entries | 1 | 37 |
| Page size | 2048 | 4096 |
| Total size | 122,640 B | 3,935,828 B |
| `aw9523b@5b` (keyboard matrix) | **present** | missing in entry 0 |
| `idea_keys` (alt/shift/ctrl/home/arrows) | **present** | missing |
| `:kbd_backlight` | **present** | missing |

The overlay flashed by Sailfish is **not among** the 37 factory entries of
slot B (no SHA256 match against `dtboA-00`). `boot_a` and `boot_b`, on the
other hand, are bit-identical. In other words: slot B ran the same hybris
boot image, but the bootloader slipped it a factory overlay without the
AW9523B node -- and without that node the Halium kernel does not find the
keyboard matrix.

The kernel cmdline of slot A confirms this: `androidboot.dtbo_idx=0`.
Slot A has only this one (correct) entry; on slot B, index 0 is a completely
different overlay.

## Consequence for Maemo Leste: no problem

The mainline kernel brings its device tree **inside the boot image**
(header v2, `--dtb`), not via an overlay. The built `sm6115-fxtec-pro1x.dtb`
contains the complete hardware description, checked:

* `fxtec,pro1x-aw9523-keypad` with the complete 64-key `linux,keymap`,
  plus `kbd-backlight` and `kbd-capslock`
* `qcom,wcn3990-wifi`, `qcom,wcn3950-bt`
* `awinic,aw86224` (vibration), `fxtec,pro1x-charger-icl`, `qcom,pm6125`
* `qcom,sm6115-mpss-pas` and `-adsp-pas` with the firmware paths
  `qcom/sm6115/Fxtec/QX1050/*.mbn`
* `sony,imx582`, `samsung,s5k4h7`, `dongwoon,dw9800`, `qcom,qcm2290-camss`
* Goodix touch, hall switch, panel

That is why `dtbo_b` is **zeroed** when flashing (`ERASE_DTBO=1` in
`scripts/60-flash-slot-b.sh`): this way the bootloader does not lay a foreign
overlay over our DTB. The NixOS port takes the same route ("flashing erases
dtbo").

(Later finding, see [HANDOFF.md](HANDOFF.md): with a zeroed dtbo the ABL
ignores the DTB section of a v2 header and only accepts a DTB appended to the
kernel, so the boot image is now built as header v0 with the DTB appended.)

## If Sailfish should go onto slot B later

Then `dtbo_a` has to be copied to `dtbo_b` -- without the right overlay the
keyboard stays dead there. That is exactly what must not happen the other way
round in our setup, so not both at once: either `dtbo_b` zeroed for Leste, or
`dtbo_b` = `dtbo_a` for Sailfish.

## Way back

`boot_a`, `dtbo_a`, `vbmeta_a` and the GPTs of all six LUNs are backed up in
`/userdata/dualboot-backup` and mirrored on the build host. Switching in
either direction with `fastboot --set-active=a|b`
(`scripts/70-switch-slot.sh`). If a boot attempt fails, the ABL counts down
the retries and falls back to slot A.
