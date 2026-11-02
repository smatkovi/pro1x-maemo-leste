# ABL for a Pro1X with failing RAM above 4 GiB

On 2026-11-02 one Pro1X stopped booting any slot. ABL (the Android
bootloader, edk2's LinuxLoader in `abl_a`/`abl_b`) went to fastboot.
Every attempt failed the same way:

- `fastboot continue` answers `Failed to load image from partition:
  Device Error`.
- `fastboot download` hangs.

In EDL, F(x)tec's own firehose programmer reads the whole UFS without
errors, and every image matches its backup. A fresh DDR training (the `ddr`
partition cleared) changed nothing. FreeBSD had already shown the cause the
day before: stores above 4 GiB stall, which is why FreeBSD runs with
`hw.physmem=2500M` (docs/device-day-2026-10-31.md).

ABL allocates its boot image buffer from the top of memory, which is above
4 GiB on this 8 GiB phone. This ABL keeps that RAM out of use. The patch is
in `patches/0001-...`:

- `LinuxLoaderEntry` first takes every free page above 4 GiB
  (`AllocatePages` at fixed addresses; the pages are never touched). Every
  later allocation, from the image buffers to the thread stacks, comes
  from below.
- `AddMemMap` gives the kernel's device tree only the RAM below 4 GiB, so
  Linux (Leste, Sailfish) and FreeBSD started from it do not use the rest.
  The phone then has about 3 GiB.

It is built from CodeLinaro's `LA.UM.9.15.r1-05900-KAMORTA.QSSI12.0` with
the stock image's Android 11 flags (bengal, AVB 2, dynamic partitions,
virtual A/B). The one difference is `USER_BUILD_VARIANT=0`, which gives
`fastboot boot` and the oem commands. The stock ABL is F(x)tec's build of
7 July 2022 (v2.1.2) and also signed with Qualcomm's test keys.

## Build

    WORK=/path/to/work QTESTSIGN=/path/to/qtestsign.py bash abl/build.sh

The result is `$WORK/abl/out/abl-test-signed.mbn`, which carries the hash
segment XBL checks. It needs Docker and about 2 GB of space for the
container.

## Flash (EDL)

Nothing here goes through fastboot: its `download` hangs on this phone.

1. Get into EDL: hold power, volume up and volume down while it starts.
   Let go when the logo flashes and the screen goes dark. The PC then shows
   `05c6:9008`.
2. Write the new ABL and reset. The firehose is
   `prog_firehose_ddr.elf` from F(x)tec's stock package
   `Release_SM6115_S701_gms_V2.1.2_20220707-1501_user.tar.gz`; qdl is 2.8.
   Write only the slot that boots, slot b (the boot menu), so that `abl_a`
   stays stock:

       qdl -s ufs prog_firehose_ddr.elf write 4/abl_b abl-test-signed.mbn

3. To go back, write the saved stock image the same way:

       qdl -s ufs prog_firehose_ddr.elf write 4/abl_b abl_b-stock.bin

Leave EDL by holding power for more than 10 s.
