#!/usr/bin/env python3
"""Where FreeBSD stopped (tools/devday, runbook 12.3): find its kernel in a DDR dump of the Pro1X
(qdl ramdump DDRCS0_0.BIN, physical 0x40000000), check it against
kernel.bin, read boot variables by symbol, symbolize the boot stack.

    fbsd-forensic.py DDRCS0_0.BIN kernel.bin kernel.full
"""
import mmap, struct, subprocess, sys

dump_path, kbin_path, kfull = sys.argv[1:4]
DUMP_PA = 0x40000000
VA0 = 0xffff000000000000

kbin = open(kbin_path, 'rb').read()
f = open(dump_path, 'rb')
d = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
print(f'dump {len(d) >> 20} MiB at PA 0x{DUMP_PA:x}')

# the kernel: a 256-byte run of text, then the image start 2 MiB aligned
probe = kbin[0x1000:0x1100]
loads = []
pos = d.find(probe)
while pos >= 0:
    start = pos - 0x1000
    loads.append(start)
    pos = d.find(probe, pos + 1)
for s in loads:
    print(f'kernel image candidate at PA 0x{DUMP_PA + s:x} (2 MiB aligned: {(DUMP_PA + s) % (2 << 20) == 0})')
if not loads:
    sys.exit('kernel not found in this dump')
base = [s for s in loads if (DUMP_PA + s) % (2 << 20) == 0] or loads
base = base[0]
LOAD_PA = DUMP_PA + base
print(f'using load PA 0x{LOAD_PA:x}')

def pa_of(va):
    return LOAD_PA + (va - VA0)

def rd(va, n):
    o = pa_of(va) - DUMP_PA
    return d[o:o + n]

def u64(va):
    return struct.unpack('<Q', rd(va, 8))[0]

def u32(va):
    return struct.unpack('<I', rd(va, 4))[0]

# symbols
syms = {}
for line in subprocess.run(['nm', '-S', kfull], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 4:
        syms[p[3]] = (int(p[0], 16), int(p[1], 16))
    elif len(p) == 3:
        syms[p[2]] = (int(p[0], 16), 0)

# text and rodata against the file (data changes by running)
def diff_ranges(lo, hi, label):
    img = d[base + lo:base + hi]
    ref = kbin[lo:hi]
    runs, i, n = [], 0, len(ref)
    step = 4096
    for off in range(0, n, step):
        if img[off:off + step] != ref[off:off + step]:
            runs.append(lo + off)
    print(f'{label} 0x{lo:x}-0x{hi:x}: {len(runs)} of {(hi - lo + step - 1) // step} pages differ')
    for r in runs[:12]:
        a, b = img[r - lo:r - lo + step], ref[r - lo:r - lo + step]
        nb = sum(1 for x, y in zip(a, b) if x != y)
        print(f'   page 0x{r:x} (PA 0x{LOAD_PA + r:x}): {nb} bytes differ')
    return runs

diff_ranges(0x800, 0x8704e0 + 0x800, 'text')
diff_ranges(0x880000, 0x880000 + 0x145e4c8, 'rodata')

print('\nvariables:')
for name, kind in [('boothowto', 'u32'), ('boot_el', 'u32'), ('early_boot', 'u32'),
                   ('ramcons_buf', 'u64'), ('ramcons_pos', 'u64'), ('msgbufp', 'u64'),
                   ('msgbufmapped', 'u32'), ('physmem', 'u64'), ('dmap_phys_base', 'u64'),
                   ('virtual_avail', 'u64'), ('kern_envp', 'u64'), ('fdtp', 'u64'),
                   ('cpu_desc', 'u64'), ('bootstack', 'u64')]:
    if name not in syms:
        print(f'  {name}: no symbol')
        continue
    va = syms[name][0]
    v = u32(va) if kind == 'u32' else u64(va)
    print(f'  {name:15s} @0x{va:x} = 0x{v:x}')

def cstr(name, n):
    va = syms[name][0]
    b = rd(va, n)
    return b.split(b'\0')[0].decode(errors='replace')

print('  linux_command_line =', repr(cstr('linux_command_line', 0x201)))
env = rd(syms['static_kenv'][0], 0x1000)
print('  static_kenv =', [e.decode(errors='replace') for e in env.split(b'\0') if e][:40])

print('\ncn_devtab:')
cd = rd(syms['cn_devtab'][0], 0x40)
print('  ', ' '.join(f'{x:016x}' for x in struct.unpack('<8Q', cd)))

# the boot stack: kernel text addresses, deepest first
text_lo, text_hi = VA0 + 0x800, VA0 + 0x800 + 0x8704e0
lo, hi = syms['initstack'][0], syms['initstack_end'][0]
stk = rd(lo, hi - lo)
hits = []
for off in range(0, len(stk), 8):
    v = struct.unpack_from('<Q', stk, off)[0]
    if text_lo <= v < text_hi:
        hits.append((lo + off, v))
used = next((off for off in range(0, len(stk), 8) if struct.unpack_from('<Q', stk, off)[0]), None)
print(f'\ninitstack 0x{lo:x}-0x{hi:x}: first nonzero word at +0x{used:x}' if used is not None else '\ninitstack: all zero')
if hits:
    out = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', kfull] + [f'0x{v:x}' for _, v in hits],
                         capture_output=True, text=True).stdout.splitlines()
    # with -i an address can yield several lines; print pairs plainly
    j = 0
    for slot, v in hits:
        fn = out[j] if j < len(out) else '?'
        loc = out[j + 1] if j + 1 < len(out) else '?'
        j += 2
        print(f'  [0x{slot:x}] 0x{v:x}  {fn}  {loc.split("/sys/")[-1]}')
