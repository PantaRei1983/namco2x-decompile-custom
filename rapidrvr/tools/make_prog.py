#!/usr/bin/env python3
"""make_prog.py -- Rapid River's program images from the ROM files (no MAME needed). Namco GORgON ("System 22.5").
rd3verc.ic2 and .ic1 are byte-interleaved, .ic2 first (MAME ROM_LOAD16_BYTE into "user1", 4 MB at 0x1FC00000), as Time Crisis 2's boot ROM.
It holds the NMON boot monitor ("NMON 0.8.7 for GORgON P2") and, at 0x20000, the game as a MIPS ELF; the layout is read from the ELF.
  python -I tools/make_prog.py ROMDIR [OUTDIR]  ->  boot.bin, nmon.bin, prog.bin (text at 0x80000000), data.bin, sub.bin (H8/3002), io.bin (ASCA-1), prog_entry.txt"""
import sys, os, struct
d = sys.argv[1]; o = sys.argv[2] if len(sys.argv) > 2 else '.'
rd = lambda n: open(os.path.join(d, n), 'rb').read()
wr = lambda n, b: open(os.path.join(o, n), 'wb').write(b)
a, b = rd('rd3verc.ic2'), rd('rd3verc.ic1')
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
s = bytearray(rd('rd3verc.ic3')); s[0::2], s[1::2] = s[1::2], s[0::2]
wr('sub.bin', s)
if os.path.exists(os.path.join(d, 'asc1_io-a.ic13')): wr('io.bin', rd('asc1_io-a.ic13'))   # the ASCA-1 I/O board (H8/3334)
open(os.path.join(o, 'prog_entry.txt'), 'w').write('%08X\n' % entry)
print('ELF entry %08X' % entry)
for p in load:
    print('  PT_LOAD vaddr %08X  boot.bin 0x%06X  filesz 0x%06X  memsz 0x%06X  %s' % (p[2], E + p[1], p[4], p[5], 'RX' if p[6] & 1 else 'RW'))
