#!/usr/bin/env python3
"""make_prog.py -- Downhill Bikers' program images from the ROM files (no MAME needed). Namco System 23, MAME's downhillu set (US, DH3 Ver. A,
the one MAME runs; its parent downhill = World DH2 Ver. A).
dh3vera.ic2 and .ic1 are byte-interleaved, .ic2 first (MAME ROM_LOAD16_BYTE into "user1", 4 MB at 0x1FC00000), as Time Crisis 2's boot ROM.
It holds the NMON boot monitor and, at 0x20000, the game as a MIPS ELF; the layout is read from the ELF. Each chip is checked by size and CRC.
  python -I tools/make_prog.py ROMDIR [OUTDIR]  ->  boot.bin, nmon.bin, prog.bin (text at 0x80000000), data.bin, sub.bin (H8/3002),
                                                    io.bin (the ASCA-3 I/O board's H8/3334), prog_entry.txt"""
import sys, os, struct, zlib
d = sys.argv[1]; o = sys.argv[2] if len(sys.argv) > 2 else '.'
def rd(n, crc, size):
    f = os.path.join(d, n)
    if not os.path.isfile(f): sys.exit('%s missing in %s' % (n, d))
    x = open(f, 'rb').read()
    if len(x) != size or zlib.crc32(x) & 0xFFFFFFFF != crc: sys.exit('%s is not the right chip (size or CRC)' % n)
    return x
wr = lambda n, b: open(os.path.join(o, n), 'wb').write(b)
a, b = rd('dh3vera.ic2', 0x5D9952E9, 0x200000), rd('dh3vera.ic1', 0x64A236F3, 0x200000)
boot = bytearray(len(a) * 2); boot[0::2] = a; boot[1::2] = b
wr('boot.bin', boot)
wr('nmon.bin', boot[:0x20000])
E = 0x20000
assert boot[E:E + 4] == b'\x7fELF', 'no ELF at boot.bin 0x20000'
entry, phoff = struct.unpack_from('>II', boot, E + 0x18)
phnum, = struct.unpack_from('>H', boot, E + 0x2C)
load = [struct.unpack_from('>8I', boot, E + phoff + 32 * k) for k in range(phnum)]
load = [p for p in load if p[0] == 1]                       # PT_LOAD (the first header is MIPS REGINFO)
text, data = load[0], load[1]
assert text[2] == 0x80000000, 'text segment not at 0x80000000'
wr('prog.bin', boot[E + text[1]:E + text[1] + text[4]])
wr('data.bin', boot[E + data[1]:E + data[1] + data[4]])
s = bytearray(rd('dh3vera.ic3', 0x98F9FC8B, 0x80000)); s[0::2], s[1::2] = s[1::2], s[0::2]
wr('sub.bin', s)
wr('io.bin', rd('asc3_io-c.ic14', 0x2F272A7B, 0x20000))   # namco_asca3.zip (also inside downhill.zip in a non-merged set)
open(os.path.join(o, 'prog_entry.txt'), 'w').write('%08X\n' % entry)
print('ELF entry %08X' % entry)
for p in load:
    print('  PT_LOAD vaddr %08X  boot.bin 0x%06X  filesz 0x%06X  memsz 0x%06X  %s' % (p[2], E + p[1], p[4], p[5], 'RX' if p[6] & 1 else 'RW'))
