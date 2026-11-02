# Put the stock ABL's MBN v6 metadata (SW_ID 0x1c = ABL, ...) into a
# qtestsign'ed ABL: qtestsign leaves the 120 bytes zero, and XBL checks
# the software ID even with secure boot off.
#   python3 abl/setmeta.py SIGNED.mbn STOCK-ABL.elf OUT.mbn
import struct, sys


def hashseg(d):
    phoff = struct.unpack_from('<I', d, 0x1c)[0]
    phnum = struct.unpack_from('<H', d, 0x2c)[0]
    for i in range(phnum):
        t, off, va, pa, fsz, msz, fl, al = struct.unpack_from('<8I', d, phoff + 32 * i)
        if t == 0 and (fl & 0x07000000) == 0x02000000:
            return off
    raise SystemExit('no hash segment')


signed = bytearray(open(sys.argv[1], 'rb').read())
ref = open(sys.argv[2], 'rb').read()
so, ro = hashseg(signed), hashseg(ref)
sh = struct.unpack_from('<12I', signed, so)
rh = struct.unpack_from('<12I', ref, ro)
assert sh[1] == rh[1] == 6 and sh[11] == rh[11] == 0x78, (sh, rh)
meta = ref[ro + 48:ro + 48 + 0x78]
assert struct.unpack_from('<I', meta, 8)[0] == 0x1c, 'reference is not an ABL'
signed[so + 48:so + 48 + 0x78] = meta
open(sys.argv[3], 'wb').write(signed)
print('metadata copied, SW_ID 0x1c')
