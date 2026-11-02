# pro1x-gptfix: repair a Pro1X slot that disagrees with itself

On the Pro1X (SM6115) a boot slot is three things, and ABL boots only when
they agree. The check is `ValidateSlotGuids` in ABL's
`PartitionTableUpdate.c`; on a mismatch it answers `Failed to load image
from partition: Device Error` and stays in fastboot.

1. **The attribute bits** of `boot_a`/`boot_b` (and the active bit of every
   other `_a`/`_b` partition): bit 48-49 priority, 50 active, 51-53 tries,
   54 successful, 55 unbootable.
2. **The type GUIDs of the `_a`/`_b` pairs.** The active slot carries the
   real type GUIDs, the other one Qualcomm's "inactive" GUID
   `77036cd4-03d5-42bb-8ed1-37e5a88baa34`. XBL loads `abl`, `tz`, `hyp` and
   the rest by these GUIDs, not by the attribute bits. `featenabler` and
   `uefisecapp` are the other way round from the factory and stay so.
3. **The UFS boot LUN** (`bBootLunEn`: 1 boots `xbl_a`, 2 boots `xbl_b`).
   Linux shows it in `/sys/devices/platform/soc@0/4804000.ufshc/attributes/boot_lun_enabled`.

`fastboot set_active` changes all three: it swaps the type GUIDs of every
pair, sets the bits and switches the LUN. On 2026-11-02 the boot menu's
Sailfish entry ran `pro1x-boot slot a`, which changed only the bits of
`boot_a`/`boot_b` with sgdisk. From then on no slot booted. Every later
`set_active` swapped GUIDs and bits together, so the two stayed out of step.
`set_active` to the slot that is already active swaps nothing; it only puts
the LUN right.

## Repair (as done on 2026-11-02)

The tool takes the slot that holds the real type GUIDs and gives it the
active bits (`boot_x`: priority 3, 7 tries, successful). The other slot
loses them. Both GPT copies get new CRCs. Nothing else changes.

1. In EDL (`abl/README.md`), read the primary GPT (LBA 0-5) and the backup
   GPT (the LUN's last 5 LBAs) of LUN 4:

       qdl -s ufs -R prog_firehose_ddr.elf read 4/0+6 prim.bin \
           read 4/1048571+5 back.bin

2. Fix both copies:

       python3 tools/gptfix/pro1x-gptfix.py prim.bin back.bin \
           prim-fixed.bin back-fixed.bin

3. Write them back, read them back, then reset:

       qdl -s ufs -R prog_firehose_ddr.elf write 4/0+6 prim-fixed.bin \
           write 4/1048571+5 back-fixed.bin
       qdl -s ufs prog_firehose_ddr.elf reset

4. If ABL is still in fastboot (`Device Error`), the LUN is wrong. Run
   `fastboot set_active <the same slot>`, then `fastboot reboot`.

Since `abl/patches/0002` the Pro1X ABL does this repair by itself on every
start (the GUIDs and the LUN follow the bits). The tool remains for the
stock ABL and for a phone that cannot run ABL at all.

Never use `fastboot flash partition:4` for this. Before it writes the GPT,
ABL erases the whole LUN (`WriteGpt` calls `ErasePartition` on the entire
device).
