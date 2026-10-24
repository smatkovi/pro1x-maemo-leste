#!/usr/bin/env python3
"""Read an Android boot image header (v0-v2).

Usage: bootimg-params.py /dev/disk/by-partlabel/boot_a
Prints the header fields and the matching mkbootimg arguments.
"""
import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else "/dev/disk/by-partlabel/boot_a"
with open(path, "rb") as f:
    d = f.read(2048)
if d[:8] != b"ANDROID!":
    sys.exit(f"{path}: not an Android boot image (magic={d[:8]!r})")

ks, ka, rs, ra, ss, sa, ta, ps, hv, osv = struct.unpack("<10I", d[8:48])
name = d[48:64].rstrip(b"\0").decode()
cmdline = d[64:576].rstrip(b"\0").decode()
extra = d[608:1632].rstrip(b"\0").decode()
rdtbo_sz, = struct.unpack("<I", d[1632:1636])
rdtbo_off, = struct.unpack("<Q", d[1636:1644])
hsz, = struct.unpack("<I", d[1644:1648])
dtbsz, = struct.unpack("<I", d[1648:1652])
dtbaddr, = struct.unpack("<Q", d[1652:1660])
base = ka - 0x8000

print(f"header_version = {hv}")
print(f"page_size      = {ps}")
print(f"header_size    = {hsz}")
print(f"kernel_size    = {ks} ({ks/1048576:.2f} MiB)")
print(f"ramdisk_size   = {rs} ({rs/1048576:.2f} MiB)")
print(f"second_size    = {ss}")
print(f"dtb_size       = {dtbsz} ({dtbsz/1024:.1f} KiB)")
print(f"recovery_dtbo  = size {rdtbo_sz} off {rdtbo_off}")
print(f"name           = {name!r}")
print(f"os_version raw = 0x{osv:08x}")
print(f"\nkernel_addr=0x{ka:08x} ramdisk_addr=0x{ra:08x} "
      f"second_addr=0x{sa:08x} tags_addr=0x{ta:08x} dtb_addr=0x{dtbaddr:x}")
print("\n-- mkbootimg --")
print(f"--base 0x{base:08x} --kernel_offset 0x{ka-base:08x} --ramdisk_offset 0x{ra-base:08x} "
      f"--second_offset 0x{sa-base:08x} --tags_offset 0x{ta-base:08x} "
      f"--dtb_offset 0x{dtbaddr-base:x} --pagesize {ps} --header_version {hv}")
print("\n-- cmdline --")
print(cmdline)
if extra:
    print("-- extra_cmdline --")
    print(extra)
