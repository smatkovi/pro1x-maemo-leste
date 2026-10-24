#!/usr/bin/env python3
"""Turn the RELA relocations of prelinked 32-bit ARM ELF files back into REL.

Harmattan was prelinked. ARM uses REL relocations, whose addend lives in
the relocated word itself; prelink overwrites that word with the prelinked
value, so it rewrote such tables as RELA to keep the addends. glibc dropped
prelink support in 2.36 and its ARM loader ignores DT_RELA (ELF_MACHINE_NO_RELA),
so these libraries load without their relocations and crash.

The conversion stores each addend back in its target word, packs the table
as 8-byte REL entries in place and renames the dynamic tags. The result is
an ordinary prelinked-but-REL object, which current glibc relocates correctly
at any load address.

  rela2rel.py [--check] FILE...
  rela2rel.py --list < paths     print the files among paths that have DT_RELA
"""
import struct
import sys

DT_NULL, DT_RELA, DT_RELASZ, DT_RELAENT, DT_REL, DT_RELSZ, DT_RELENT = 0, 7, 8, 9, 17, 18, 19
DT_RELACOUNT, DT_RELCOUNT = 0x6FFFFFF9, 0x6FFFFFFA
SHT_RELA, SHT_REL, SHT_NOBITS, SHT_DYNAMIC = 4, 9, 8, 6
PT_LOAD, PT_DYNAMIC = 1, 2
EM_ARM = 40


class Elf:
    def __init__(self, data):
        self.d = data
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            raise ValueError("not a 32-bit little-endian ELF")
        (self.type, self.machine, _, _, self.phoff, self.shoff, _, _,
         self.phentsize, self.phnum, self.shentsize, self.shnum, _) = \
            struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
        if self.machine != EM_ARM:
            raise ValueError("not ARM")
        self.ph = [struct.unpack_from("<8I", data, self.phoff + i * self.phentsize)
                   for i in range(self.phnum)]

    def off(self, vaddr, size=4):
        """File offset of vaddr, None if it lies in memory-only space (bss)."""
        for p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, _, _ in self.ph:
            if p_type == PT_LOAD and p_vaddr <= vaddr and vaddr + size <= p_vaddr + p_memsz:
                if vaddr + size <= p_vaddr + p_filesz:
                    return p_offset + vaddr - p_vaddr
                return None
        raise ValueError("address 0x%x not mapped" % vaddr)

    def dynamic(self):
        for p_type, p_offset, _, _, p_filesz, _, _, _ in self.ph:
            if p_type == PT_DYNAMIC:
                n = p_filesz // 8
                return p_offset, [struct.unpack_from("<iI", self.d, p_offset + i * 8)
                                  for i in range(n)]
        return None, []


def convert(path, check_only=False):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    try:
        elf = Elf(data)
    except ValueError:
        return "skip"
    dynoff, dyn = elf.dynamic()
    tags = {t: v for t, v in dyn}
    if DT_RELA not in tags:
        return "no-rela"
    if DT_REL in tags:
        raise ValueError("has both DT_REL and DT_RELA")
    if tags.get(DT_RELAENT, 12) != 12:
        raise ValueError("unexpected DT_RELAENT")
    rela_off = elf.off(tags[DT_RELA], tags[DT_RELASZ])
    n = tags[DT_RELASZ] // 12
    entries = [struct.unpack_from("<IIi", data, rela_off + i * 12) for i in range(n)]

    for r_offset, r_info, r_addend in entries:
        o = elf.off(r_offset)
        if o is None:
            if r_addend:
                raise ValueError("addend for bss word 0x%x" % r_offset)
            continue
        struct.pack_into("<i", data, o, r_addend)

    table = b"".join(struct.pack("<II", r_offset, r_info) for r_offset, r_info, _ in entries)
    data[rela_off:rela_off + n * 12] = table + bytes(n * 12 - len(table))

    rename = {DT_RELA: DT_REL, DT_RELASZ: DT_RELSZ, DT_RELAENT: DT_RELENT, DT_RELACOUNT: DT_RELCOUNT}
    for i, (t, v) in enumerate(dyn):
        if t == DT_NULL:
            break
        if t in rename:
            if t == DT_RELASZ:
                v = n * 8
            elif t == DT_RELAENT:
                v = 8
            struct.pack_into("<iI", data, dynoff + i * 8, rename[t], v)

    # Section headers, for tools (readelf, strip, prelink -u ...).
    for i in range(elf.shnum):
        so = elf.shoff + i * elf.shentsize
        sh = list(struct.unpack_from("<10I", data, so))
        if sh[1] == SHT_RELA and sh[3] == tags[DT_RELA]:
            sh[1], sh[5], sh[9] = SHT_REL, n * 8, 8
            struct.pack_into("<10I", data, so, *sh)

    if not check_only:
        with open(path, "r+b") as f:
            f.write(data)
    return "converted %d relocations" % n


def has_rela(path):
    try:
        with open(path, "rb") as f:
            if f.read(4) != b"\x7fELF":
                return False
            f.seek(0)
            elf = Elf(f.read())
    except (OSError, ValueError, struct.error):
        return False
    return DT_RELA in dict(elf.dynamic()[1])


def main():
    args = sys.argv[1:]
    if args == ["--list"]:
        for line in sys.stdin:
            p = line.rstrip("\n")
            if has_rela(p):
                print(p)
        return 0
    check = "--check" in args
    files = [a for a in args if a != "--check"]
    rc = 0
    for p in files:
        try:
            print("%s: %s" % (p, convert(p, check)))
        except (ValueError, struct.error) as e:
            print("%s: ERROR %s" % (p, e))
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
