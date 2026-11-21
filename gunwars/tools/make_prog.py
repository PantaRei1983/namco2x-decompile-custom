#!/usr/bin/env python3
"""make_prog.py -- Gunmen Wars' program images from the ROM files (no MAME needed).
gm1verb.ic2 and .ic1 are byte-interleaved, .ic2 first (MAME ROM_LOAD16_BYTE into "user1", 4 MB at 0x1FC00000), as Time Crisis 2's boot ROM.
It holds the NMON boot monitor ("NMON 1.0.5 P for SYSTEM23 P1") and, at 0x20000, the game as a MIPS ELF; the layout is read from the ELF.
  python -I tools/make_prog.py ROMDIR [OUTDIR]  ->  boot.bin (4 MB), nmon.bin (its first 128 KB), prog.bin (text, load at 0x80000000), data.bin (initialised data),
                                                    sub.bin (H8/3002, word-swapped back), prog_entry.txt"""
import sys, os, struct
d = sys.argv[1]; o = sys.argv[2] if len(sys.argv) > 2 else '.'
rd = lambda n: open(os.path.join(d, n), 'rb').read()
wr = lambda n, b: open(os.path.join(o, n), 'wb').write(b)
a, b = rd('gm1verb.ic2'), rd('gm1verb.ic1')
boot = bytearray(len(a) * 2); boot[0::2] = a; boot[1::2] = b
wr('boot.bin', boot)
wr('nmon.bin', boot[:0x20000])                                # the boot monitor alone (0xBFC00000..), for the translator
E = 0x20000
assert boot[E:E + 4] == b'\x7fELF', 'no ELF at boot.bin 0x20000'
entry, phoff = struct.unpack_from('>II', boot, E + 0x18)
phnum, = struct.unpack_from('>H', boot, E + 0x2C)
load = [struct.unpack_from('>8I', boot, E + phoff + 32 * k) for k in range(phnum)]
load = [p for p in load if p[0] == 1]                       # PT_LOAD: (type, off, vaddr, paddr, filesz, memsz, flags, align)
text, data = load[0], load[1]
assert text[2] == 0x80000000, 'text segment not at 0x80000000'
wr('prog.bin', boot[E + text[1]:E + text[1] + text[4]])
wr('data.bin', boot[E + data[1]:E + data[1] + data[4]])
s = bytearray(rd('gm1vera.ic3')); s[0::2], s[1::2] = s[1::2], s[0::2]
wr('sub.bin', s)
if os.path.exists(os.path.join(d, 'asc5_io-a.ic14')): wr('io.bin', rd('asc5_io-a.ic14'))   # the ASCA-5 I/O board (H8/3334)
open(os.path.join(o, 'prog_entry.txt'), 'w').write('%08X\n' % entry)
print('ELF entry %08X' % entry)
for p in load:
    print('  PT_LOAD vaddr %08X  boot.bin 0x%06X  filesz 0x%06X  memsz 0x%06X  %s' % (p[2], E + p[1], p[4], p[5], 'RX' if p[6] & 1 else 'RW'))
