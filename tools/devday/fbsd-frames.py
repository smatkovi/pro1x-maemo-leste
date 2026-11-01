#!/usr/bin/env python3
"""The trapframes on FreeBSD's boot stack in a DDR dump (DDRCS0_0.BIN, PA
0x40000000), outermost first: fault address, ESR, ELR and LR symbolized with
kernel.full. LOAD_PA: where kexec put the kernel (fbsd-ddr.py prints it).

    fbsd-frames.py DDRCS0_0.BIN kernel.full
"""
import mmap, struct, subprocess, sys
dump, kfull = sys.argv[1:3]
DUMP_PA, VA0 = 0x40000000, 0xffff000000000000
LOAD_PA = int(__import__("os").environ.get("LOAD_PA", "0x48000000"), 0)
f = open(dump, 'rb'); d = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
lo, hi = 0xffff000001e03000, 0xffff000001e07000
o = LOAD_PA + (lo - VA0) - DUMP_PA
stk = d[o:o + (hi - lo)]
w = lambda off: struct.unpack_from('<Q', stk, off)[0]
tlo, thi = VA0 + 0x800, VA0 + 0x870ce0
frames = []
for off in range(0, len(stk) - 36 * 8, 8):
    sp, lr, elr, spsr, esr, far = (w(off + 8 * i) for i in range(6))
    if tlo <= elr < thi and (spsr & 0xf) == 0x5 and (esr >> 26) in (0x20, 0x21, 0x24, 0x25):
        frames.append((lo + off, sp, lr, elr, spsr, esr, far, [w(off + 48 + 8 * i) for i in range(30)]))
print(f'{len(frames)} trapframes')
def sym(a):
    r = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', kfull, f'0x{a:x}'], capture_output=True, text=True).stdout.split('\n')
    return f'{r[0]} {r[1].split("/sys/")[-1]}'
for fr in sorted(frames, key=lambda x: -x[0])[:4] + sorted(frames, key=lambda x: x[0])[:1]:
    at, sp, lr, elr, spsr, esr, far, x = fr
    print(f'\nframe @0x{at:x}: sp 0x{sp:x} spsr 0x{spsr:x} esr 0x{esr:x} (EC 0x{esr>>26:x} ISS 0x{esr & 0x1ffffff:x}) far 0x{far:x}')
    print(f'  elr 0x{elr:x} {sym(elr)}')
    print(f'  lr  0x{lr:x} {sym(lr) if tlo <= lr < thi else ""}')
    print('  x0-x7 ' + ' '.join(f'{v:x}' for v in x[:8]))
