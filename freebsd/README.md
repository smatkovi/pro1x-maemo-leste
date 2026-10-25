# FreeBSD on the Pro1X

A real FreeBSD kernel (15.1) on the F(x)tec Pro1X: its own kernel, not
Linux's. First milestone: the kernel boots on the screen, the root
filesystem is in RAM, a shell runs. FreeBSD has no drivers for most of the
phone (storage, keyboard, touchscreen, Wi-Fi, modem, battery); this is a
bring-up project.

## How it starts

FreeBSD's arm64 kernel carries the Linux arm64 Image header
(`LINUX_BOOT_ABI`, in GENERIC), so anything that starts a Linux kernel
can start it and hand it the device tree. Two ways, neither touches a
partition:

- **From the boot menu** ("FreeBSD"): the Linux initramfs loads
  `/host/freebsd/kernel.bin` with `kexec -s` (the device tree it booted
  with goes along) and jumps into it. From there on only FreeBSD runs.
- **Without Linux**: `fastboot boot out/freebsd-boot.img` from the PC; the
  phone's bootloader starts the kernel from RAM, once (an Android boot
  image like Leste's: gzip kernel with the device tree appended).

The console is `vt_simplefb` on the framebuffer the bootloader leaves
running (the device tree's `simple-framebuffer` under `/chosen`,
1080x2160). Before it takes over, a green stripe at the top of the screen
shows that the kernel was entered at all
(`patches/0001-arm64-pro1x-proof-of-life.patch`).

FreeBSD has no network here either, so it also leaves traces where Linux
can read them after the next boot: the spare part of the bootloader's
splash memory behind the framebuffer, which nobody else writes and which
survives the reboot. The kernel puts `PRO1X-FBSD:ENTERED` at 0x5ca00000 on
entry; `rc` writes its report with the full `dmesg` to 0x5ca01000 and
reboots after 30 s. In Linux:
`dd if=/dev/mem bs=4096 skip=$((0x5ca00000/4096)) count=16 | strings`.

## Files

| File | |
| --- | --- |
| `build.sh` | builds everything on a Linux PC (clang, lld, libarchive): sources, cross toolchain, root filesystem, kernel, boot image |
| `PRO1X` | kernel config: GENERIC plus the root filesystem image and the static environment |
| `pro1x.env` | static kernel environment: one CPU (PSCI under Qualcomm's hypervisor is untested), verbose boot |
| `mfsroot/etc/rc` | shows what the kernel found and leaves it in the splash memory, then reboots into the boot menu after 30 s (no keyboard yet) |
| `patches/0001-*` | proof of life: the green stripe and the entry marker |
| `patches/0002-*` | building on an Arch Linux host: glibc 2.43 (two bootstrap fixes) and a clang that reports more than one "version" line |

The root filesystem is the release's `/rescue` (one static binary for sh,
init, mount, ifconfig...) from `base.txz`, built into the kernel
(`MFS_IMAGE`).

## Known limits and risks

- Nothing appears before the console starts; the green stripe tells
  "entered" from "never reached".
- Linux could switch the display off on its way out of kexec; then the
  `fastboot boot` way shows whether FreeBSD itself is fine.
- The bootloader appends its own arguments to the command line, which
  FreeBSD then ignores; settings go into `pro1x.env`.
- No input: `rc` only reports, and the system reboots after 30 s.
