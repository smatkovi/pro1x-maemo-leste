#!/usr/bin/env python3
# Make the Pro1X's LUN 4 GPT consistent for one slot: the slot whose _x
# partitions carry the real type GUIDs (the other has Qualcomm's
# "inactive" GUID 77036cd4-...) gets the active attribute bits, the other
# loses them. ABL refuses to boot when the active bits and the type GUIDs
# disagree ("Device Error"), which pro1x-boot's attribute-only switch
# caused on 2026-11-02.
#
#   gptfix.py PRIMARY.bin BACKUP.bin OUT-PRIMARY.bin OUT-BACKUP.bin
#
# PRIMARY: LBA 0-5 (MBR, header, entry blocks), BACKUP: the last 5 LBAs of
# the LUN (entry blocks, backup header). 4096-byte blocks.
import struct, sys, uuid, zlib

SS = 4096
INACTIVE = uuid.UUID('77036cd4-03d5-42bb-8ed1-37e5a88baa34')


def entries(blob, off, n, esz):
    return [bytearray(blob[off + i * esz:off + (i + 1) * esz]) for i in range(n)]


def name(e):
    return e[56:128].decode('utf-16le').rstrip('\0')


def tguid(e):
    return uuid.UUID(bytes_le=bytes(e[:16]))


prim = bytearray(open(sys.argv[1], 'rb').read())
back = bytearray(open(sys.argv[2], 'rb').read())
assert len(prim) == 6 * SS and len(back) == 5 * SS
hdr = prim[SS:SS + 92]
assert hdr[:8] == b'EFI PART'
cur, alt, first, last = struct.unpack_from('<QQQQ', hdr, 24)
ent_lba, nent, esz = struct.unpack_from('<QII', hdr, 72)
AS = nent * esz  # the entry array the CRC covers
assert (cur, ent_lba, esz) == (1, 2, 128) and AS <= 4 * SS
bhdr = back[4 * SS:4 * SS + 92]
assert bhdr[:8] == b'EFI PART'
assert struct.unpack_from('<QQ', bhdr, 24) == (alt, 1)
bent = struct.unpack_from('<Q', bhdr, 72)[0]
assert alt - 4 <= bent < alt, bent
BO = (bent - (alt - 4)) * SS  # backup entries' offset in BACKUP
# both entry arrays must agree before anything is changed
assert prim[2 * SS:2 * SS + AS] == back[BO:BO + AS], 'primary and backup entries differ'
ents = entries(prim, 2 * SS, nent, esz)
assert zlib.crc32(prim[2 * SS:2 * SS + AS]) == struct.unpack_from('<I', hdr, 88)[0]
assert zlib.crc32(back[BO:BO + AS]) == struct.unpack_from('<I', bhdr, 88)[0]

byname = {name(e): e for e in ents if e[:16] != b'\0' * 16}
pairs = sorted(n[:-2] for n in byname if n.endswith('_a') and n[:-2] + '_b' in byname)
# which slot holds the real type GUIDs, by majority: featenabler and
# uefisecapp are the other way round from the factory (2026-10-24 too) and
# stay untouched
side = {}
for p in pairs:
    ra = tguid(byname[p + '_a']) != INACTIVE
    rb = tguid(byname[p + '_b']) != INACTIVE
    if ra != rb:
        side[p] = 'a' if ra else 'b'
na = sum(1 for v in side.values() if v == 'a')
nb = len(side) - na
assert min(na, nb) <= 2 and max(na, nb) >= 15, f'no clear slot: a {na}, b {nb}'
slot = 'a' if na > nb else 'b'
other = 'b' if slot == 'a' else 'a'
odd = sorted(p for p, v in side.items() if v != slot)
pairs = [p for p in pairs if side.get(p) == slot]
assert 'boot' in pairs and 'abl' in pairs
print(f'{len(pairs)} A/B pairs with the real type GUIDs on slot {slot}: '
      f'making {slot} active; left alone: {", ".join(odd) or "none"}')

for p in pairs:
    for s, on in ((slot, True), (other, False)):
        e = byname[p + '_' + s]
        a = struct.unpack_from('<Q', e, 48)[0]
        a = a | (1 << 50) if on else a & ~(1 << 50)
        if p == 'boot':
            a &= ~(0xff << 48)
            if on:
                # priority 3, active, 7 tries, successful
                a |= (3 << 48) | (1 << 50) | (7 << 51) | (1 << 54)
            else:
                # priority 2, kept bootable and successful
                a |= (2 << 48) | (7 << 51) | (1 << 54)
        struct.pack_into('<Q', e, 48, a)

arr = b''.join(ents)
crc = zlib.crc32(arr)
for blob, hoff in ((prim, SS), (back, 4 * SS)):
    off = 2 * SS if blob is prim else BO
    blob[off:off + AS] = arr
    struct.pack_into('<I', blob, hoff + 88, crc)
    struct.pack_into('<I', blob, hoff + 16, 0)
    hsz = struct.unpack_from('<I', blob, hoff + 12)[0]
    struct.pack_into('<I', blob, hoff + 16, zlib.crc32(blob[hoff:hoff + hsz]))
open(sys.argv[3], 'wb').write(prim)
open(sys.argv[4], 'wb').write(back)
for p in ('boot', 'abl', 'tz', 'xbl_config' if 'xbl_config' in pairs else 'dtbo'):
    for s in 'ab':
        e = byname[p + '_' + s]
        a = struct.unpack_from('<Q', e, 48)[0] >> 48
        print(f'  {p}_{s:1s} {str(tguid(e))[:8]} attr {a:#06x} active {a >> 2 & 1}')
