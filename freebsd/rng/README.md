# FreeBSD on the Pro1X: random numbers

What feeds random(4) on the phone, and what was added: the SoC's PRNG
(`patches/0038`) and the seed Linux's kexec hands over (`patches/0039`).
The same hardware for KolibriOS: `kolibrios-arm`'s `docs/rng.md`.

## Before

- **No hardware source.** FreeBSD 15.1's `qcom_rnd` (`sys/dev/qcom_rnd`)
  knows only `"qcom,prng"`, the whole PRNG of the IPQ4018, and is built
  only for ARMv7 (`sys/arm/qualcomm/std.ipq4018`); it would also write
  the PRNG's configuration, which on the SM6115 belongs to TrustZone.
  The phone's PRNG is `"qcom,prng-ee"` (below). `device armv8_rng` in
  `PRO1X` finds no `RNDR` on the Kryo 260 (Cortex-A73/A53, ARMv8.0) and
  registers nothing.
- **No saved seed.** The kernel starts without loader(8) (kexec or
  fastboot), so there is no `/boot/entropy`; the root is the read-only
  `root.zfs` with `/var` in memory, so `rc.d/random`'s saved entropy is
  gone at every start.
- What is left are timestamps (the generic timer, 19.2 MHz) of device
  attaches, software interrupts, callouts and network frames; no Pro1X
  driver asks for its interrupts to be harvested (`INTR_ENTROPY`).
  Fortuna unblocks `/dev/random` once pool 0 holds 64 bytes of such
  events: a count of bytes, not of entropy. So `getrandom(2)` waits, but
  what it waits for is a second of timer readings.

## The PRNG: `patches/0038`

From Linux 7.1 (`arch/arm64/boot/dts/qcom/sm6115.dtsi`,
`drivers/crypto/qcom-rng.c`, `drivers/clk/qcom/gcc-sm6115.c`,
`clk-branch.c`):

- `rng@1b53000`, `compatible = "qcom,prng-ee"`, 4 KiB: the execution
  environment's window: `DATA_OUT` at 0, `STATUS` at 4 (bit 0: a word is
  there). TrustZone set the PRNG up; its configuration (`LFSR_CFG` 0x100,
  `CONFIG` 0x104) is not ours: Linux's match data for `prng-ee` has
  `skip_init`.
- Its clock `GCC_PRNG_AHB_CLK`: a voted branch (`BRANCH_HALT_VOTED`): bit 13
  of the APCS vote register GCC 0x79004, its CBCR at 0x21004 with hardware
  gating (`hwcg_bit` 1). Linux votes it on around each read; on, it waits
  up to 200 us for CLK_OFF (bit 31) to clear or the NOC FSM (bits 30:28) to
  say on, and checks nothing while the branch is hardware gated; off, 10 us.
- A word whenever STATUS has one, polled every 200 us for at most 10 ms; a
  word of zero fails the read. Linux registers it only with the crypto API
  (`stdrng`, driver `qcom-rng`), not as a hwrng, so it does not feed
  Linux's own pool either. What is inside (Qualcomm calls it a
  pseudo-random number generator, seeded inside the SoC) is not
  documented.

`qcom_rnd` now takes `"qcom,prng-ee"` on arm64 (`device qcom_rnd`,
`files.arm64`) and reads it that way (`sys/dev/qcom_rnd/qcom_rnd_ee.h`):
the GCC found through the node's `clocks` and mapped (FreeBSD has no clock
driver for the SM6115; only `qcom,gcc-sm6115` is known), the vote around
each read, the configuration never touched. A word of zero, no word for
10 ms, a clock that does not come on, or a word as the one before
(FIPS 140-2's continuous test) fails the read; three failed reads in a row
and it gives nothing any more. It is a pure source (`RANDOM_PURE_QUALCOMM`,
"Qualcomm Entropy Adapter"): the harvest thread reads it 10 times a
second until Fortuna is seeded (all pools at once), then
`kern.random.harvest.seeded_hz` times (1 on the phone, `patches/0027`),
8 bytes per pool, so the clock is voted on for a moment once a second.
The attach reads 8 bytes first and does not register a source that gives
none.

`sysctl dev.qcom_rnd.0`: `words`, `zeros`, `timeouts`, `repeats`,
`clock_stuck`, `dead`.

## Kexec's seed: `patches/0039`

Linux's `kexec_file_load` (the boot menu's `kexec -s`) writes 128 bytes of
its own generator into the device tree as `/chosen/rng-seed`, and 8 as
`kaslr-seed` (`of_kexec_alloc_and_setup_fdt`, only once Linux's generator
is seeded, which it is long before the menu); QEMU's virt writes 32 and 8.
FreeBSD read neither. Now `random_harvestq_prime` (SI_SUB_RANDOM) puts
both into Fortuna's pool 0 as `RANDOM_CACHED`, all of it, so that the
first reseed (which uses pool 0 only) takes them whole, and wipes them in
the tree, as Linux does. The console says

```
random: 128 bytes of the device tree's rng-seed, wiped in the tree
random: 8 bytes of the device tree's kaslr-seed, wiped in the tree
```

and `kern.random.harvest.fdt_seed` counts the bytes. Started by `fastboot
boot` instead, the tree is the bootloader's, which may have neither.

## sshd's host keys over USB

`rootfs/usr/local/etc/rc.d/pro1x_usbnet` makes the host keys of its sshd
(in `/var/run/pro1x-sshd` on the read-only root: new ones at every start).
The order was right already: `rc.d/random` runs before `netif` (`BEFORE:
netif`), `pro1x_usbnet` after `NETWORKING`; `ssh-keygen` takes its random
numbers through OpenSSL from `getrandom(2)`, which waits until Fortuna is
seeded. What it was seeded from was the question; with the two sources
above it is the PRNG and Linux's seed. `pro1x_usbnet_hostkeys` now reads
a byte of `/dev/random` before new keys (the wait made explicit) and logs
what random(4) has: `pro1x_usbnet: new host keys; random(4)'s sources:
'Qualcomm Entropy Adapter', the device tree's seed: 136 bytes`.

## Building and testing

`freebsd/rng/build.sh` (`WORK=...`, `$WORK/.kbuild-lock` taken, results in
`$WORK/rng-out`, `out/` not touched):

- `host`: `test/qcom_rnd_ee_test.c`, the read against a model of the
  PRNG's window and its GCC: the vote around each read with the other
  votes kept, hardware gating and the NOC FSM's status counted as on, a
  clock that does not come on (200 us, the PRNG not touched), a word of
  zero, the same word twice (also across reads), no word (10 ms), no clock
  of its own. 11 checks
- `qemu`: `PRO1XQRNG` for QEMU's virt; `test/qemu/run.sh` runs it:
  `seed` (QEMU's rng-seed and kaslr-seed taken and wiped,
  `kern.random.harvest.fdt_seed: 40`, `/dev/random` reads), `noseed`
  (`dtb-randomness=off`), `prng` (a `qcom,prng-ee` and a `qcom,gcc-sm6115`
  node on RAM that gives one word: the attach reads it twice and does not
  register), `clock` (CLK_OFF stuck: refused without reading the PRNG)
- `phone`: `PRO1XRNG`, `freebsd/PRO1X` as it is, no modules: that the
  phone's kernel builds (2026-10-30: yes). The joint rebuild
  (`freebsd/build.sh`) makes the kernel for the phone.

## On the phone (2026-10-31)

After a start from the boot menu:

```
dmesg | grep -E '^random:|^qcom_rnd'
sysctl kern.random.random_sources kern.random.harvest.fdt_seed dev.qcom_rnd.0
```

Expected: `qcom_rnd0: <Qualcomm PRNG> mem 0x1b53000-0x1b53fff on simplebus0`
(or `ofwbus0`), `qcom_rnd0: the execution environment's window, its clock
voted around each read`, the two `random: ... wiped in the tree` lines
(128 and 8 bytes), `kern.random.random_sources: 'Qualcomm Entropy
Adapter'`, `kern.random.harvest.fdt_seed: 136`, and under `dev.qcom_rnd.0`
`words` growing by some 64 a second, `zeros`, `timeouts`, `repeats` and
`clock_stuck` 0, `dead` 0. A minute later again: `words` grown. Then

```
dd if=/dev/random bs=64k count=16 of=/tmp/r && python3 kolibrios-arm/tools/qcom-rng-check.py
```

is not needed (that is the PRNG through Linux, under Leste:
`kolibrios-arm`'s `docs/rng.md`); FreeBSD's output is Fortuna's, not the
PRNG's. What could show instead:

- `qcom_rnd0: ... gives no words (zeros Z, timeouts T, repeats R, clock
  stuck C): not used`: the numbers are the finding (C: the vote or the CBCR
  differ from Linux's description; T or Z: the window does not produce);
  random(4) then has kexec's seed and the timestamps
- `qcom_rnd0: three failed reads in a row ... not used any more`: the same,
  later
- no `random: ... rng-seed` line: FreeBSD was not started by `kexec -s`
- with the USB network (`pro1x.usb=1`): `/var/log/messages` has
  `pro1x_usbnet: new host keys; random(4)'s sources: 'Qualcomm Entropy
  Adapter', the device tree's seed: 136 bytes`
