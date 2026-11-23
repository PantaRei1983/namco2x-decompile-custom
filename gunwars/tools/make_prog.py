#!/usr/bin/env python3
"""make_prog.py -- Gunmen Wars' program images from the ROM files (no MAME needed). Any of MAME's three sets (include/gw_version.h):
  gunwars   World, GM2 Ver. B (MAME's parent)   gm2verb.ic2 / .ic1
  gunwarsj  Japan, GM1 Ver. B                   gm1verb.ic2 / .ic1   (MAME's "gunwars" before the World set)
  gunwarsa  Japan, GM1 Ver. A                   gm1vera.ic2 / .ic1
The first complete one found is used (in that order; GW_SET=gunwarsj etc. picks one). Each is checked by size and MAME's CRC32.
The .ic2 and .ic1 chips are byte-interleaved, .ic2 first (MAME ROM_LOAD16_BYTE into "user1", 4 MB at 0x1FC00000), as Time Crisis 2's boot ROM.
It holds the NMON boot monitor ("NMON 1.0.5 P for SYSTEM23 P1") and, at 0x20000, the game as a MIPS ELF; the layout is read from the ELF.
  python -I tools/make_prog.py ROMDIR [OUTDIR]  ->  boot.bin (4 MB), nmon.bin (its first 128 KB), prog.bin (text, load at 0x80000000), data.bin (initialised data),
                                                    sub.bin (H8/3002, word-swapped back), prog_entry.txt, SET (the set used), gw_set.h (the same, for the build)"""
import sys, os, struct, zlib
d = sys.argv[1]; o = sys.argv[2] if len(sys.argv) > 2 else '.'
rd = lambda n: open(os.path.join(d, n), 'rb').read()
wr = lambda n, b: open(os.path.join(o, n), 'wb').write(b)
SETS = [   # (set, number in include/gw_version.h, (.ic2 name, CRC), (.ic1 name, CRC))
    ('gunwars',  0, ('gm2verb.ic2', 0xE413B643), ('gm2verb.ic1', 0xEC00BCF9)),
    ('gunwarsj', 1, ('gm1verb.ic2', 0x401F8264), ('gm1verb.ic1', 0xF9FD0F2B)),
    ('gunwarsa', 2, ('gm1vera.ic2', 0xCF61467F), ('gm1vera.ic1', 0xABC9FFE6)),
]
def chip(name, crc):
    f = os.path.join(d, name)
    if not os.path.isfile(f): return None
    x = open(f, 'rb').read()
    return x if len(x) == 0x200000 and zlib.crc32(x) & 0xFFFFFFFF == crc else None
want = os.environ.get('GW_SET', '').strip().lower()
pick = None
for name, num, hi, lo in SETS:
    if want and name != want: continue
    a, b = chip(*hi), chip(*lo)
    if a and b: pick = (name, num); break
if not pick:
    sys.exit('no complete Gunmen Wars program in %s%s: need gm2verb.ic1/.ic2 (gunwars), gm1verb.ic1/.ic2 (gunwarsj) or gm1vera.ic1/.ic2 (gunwarsa)'
             % (d, ' for GW_SET=' + want if want else ''))
open(os.path.join(o, 'SET'), 'w').write(pick[0] + '\n')
open(os.path.join(o, 'gw_set.h'), 'w').write('/* written by tools/make_prog.py: the set the program is translated from (include/gw_version.h) */\n#define GW_SET %d   /* %s */\n' % (pick[1], pick[0]))
print('set %s' % pick[0])
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
