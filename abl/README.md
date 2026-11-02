# ABL for the Pro1X: slot repair, working downloads, `fastboot boot`

On 2026-11-02 the Pro1X stopped booting any slot. ABL (the Android
bootloader, edk2's LinuxLoader in `abl_a`/`abl_b`) went to fastboot every
time:

- `fastboot continue` answered `Failed to load image from partition:
  Device Error`.
- `fastboot download` hung.

**The Device Error was not a hardware fault.** It was a slot that
disagreed with itself; `tools/gptfix/README.md` has the details. ABL
(`ValidateSlotGuids` in `PartitionTableUpdate.c`) refuses to boot when
these three do not agree:

- the slot's attribute bits;
- the type GUIDs of the `_a`/`_b` partitions;
- the UFS boot LUN.

The boot menu's Sailfish entry had changed only the attribute bits.
`tools/gptfix/pro1x-gptfix.py` repaired the GPT through EDL, and the phone
booted again. In EDL, F(x)tec's firehose read the whole UFS without errors,
and every image matched its backup.

This ABL came out of the search, and it stays:

- **It repairs a slot that disagrees with itself** (`patches/0002`). In
  `FindBootableSlot`, when the active slot holds the inactive type GUIDs,
  it swaps them to that slot with set_active's own `SwitchPtnSlots`, which
  also sets the boot LUN. When only the LUN disagrees, it sets the LUN.
  `pro1x-boot slot a|b`, which changes only the bits, is safe with this
  ABL: the boot menu's Sailfish entry and `pro1x-boot next leste` from
  Sailfish.
- **`download` works with it.** The stock ABL hung on it. FreeBSD had shown
  on 2026-11-01 that stores above 4 GiB can stall (it runs with
  `hw.physmem=2500M`, docs/device-day-2026-10-31.md).
  `LinuxLoaderEntry` therefore takes every free page above 4 GiB first
  (`AllocatePages` at fixed addresses; the pages are never touched), so
  the loader's own buffers, from the image buffers to the thread stacks,
  come from below. The kernel still gets all 8 GiB: patch 0001 also cut
  the device tree's memory to 4 GiB, and 0002 takes that out again.
  Whether the RAM itself is at fault is open; Linux used all of it for days.
- **It has `fastboot boot`** (`USER_BUILD_VARIANT=0`).

It is built from CodeLinaro's `LA.UM.9.15.r1-05900-KAMORTA.QSSI12.0` with
the stock image's Android 11 flags (bengal, AVB 2, dynamic partitions,
virtual A/B). The one difference is `USER_BUILD_VARIANT=0`, which gives
`fastboot boot` and the oem commands. The stock ABL is F(x)tec's build of
7 July 2022 (v2.1.2) and also signed with Qualcomm's test keys.

## Build

    WORK=/path/to/work QTESTSIGN=/path/to/qtestsign.py \
        STOCK_ABL=abl_a-stock.bin bash abl/build.sh

The result is `$WORK/abl/out/abl-pro1x.mbn`, which carries the hash
segment XBL checks: SHA-384 hashes from qtestsign, plus the stock image's v6
metadata (software ID 0x1c), which qtestsign leaves zero. It needs Docker and about 2 GB of space for the
container.

## Flash (EDL)

Nothing here goes through fastboot: its `download` hangs on this phone.

1. Get into EDL: hold power, volume up and volume down while it starts.
   Let go when the logo flashes and the screen goes dark. The PC then shows
   `05c6:9008`.
2. Write the new ABL and reset. The firehose is
   `prog_firehose_ddr.elf` from F(x)tec's stock package
   `Release_SM6115_S701_gms_V2.1.2_20220707-1501_user.tar.gz`; qdl is 2.8.
   XBL picks the ABL by the type GUID of the active slot, not by the
   attribute bits, so write both:

       qdl -s ufs prog_firehose_ddr.elf write 4/abl_a abl-pro1x.mbn \
           write 4/abl_b abl-pro1x.mbn

3. To go back, write the saved stock images the same way (`abl_a-stock.bin`,
   `abl_b-stock.bin`; both are F(x)tec's 2022-07-07 build).

Leave EDL by holding power for more than 10 s.
