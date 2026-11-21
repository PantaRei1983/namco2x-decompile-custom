#!/usr/bin/env python3
"""scan_seeds.py EXTRACTED OUT -- code addresses Ghidra's analysis does not reach by itself, found in the images (tools/make_prog.py):
  - the boot monitor (nmon.bin at 0xBFC00000): every jal target inside it, and every aligned word that points into it;
  - the game (prog.bin at 0x80000000): every aligned word of its DATA segment (data.bin) that points into the text -- function tables.
Over-seeding is safe on MIPS (fixed 4-byte aligned instructions: a wrong seed cannot shift real code); unrun code costs only size."""
import sys, os, struct
d, out = sys.argv[1], sys.argv[2]
nmon = open(os.path.join(d, 'nmon.bin'), 'rb').read()
prog = open(os.path.join(d, 'prog.bin'), 'rb').read()
data = open(os.path.join(d, 'data.bin'), 'rb').read()
NB, NE = 0xBFC00000, 0xBFC00000 + len(nmon)
PB, PE = 0x80000000, 0x80000000 + len(prog)
seeds = set()
for k in range(0, len(nmon) - 3, 4):
    w, = struct.unpack_from('>I', nmon, k)
    if w >> 26 == 3:                                   # jal
        t = ((NB + k + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        if NB <= t < NE: seeds.add(t)
    for base in (0xBFC00000, 0x9FC00000):              # a pointer into the monitor (KSEG1, or its cached KSEG0 mirror)
        if base <= w < base + len(nmon) and not w & 3: seeds.add(w - base + NB)
for k in range(0, len(data) - 3, 4):
    w, = struct.unpack_from('>I', data, k)
    if PB <= w < PE and not w & 3: seeds.add(w)
# pointers inside the text segment itself (its read-only data: switch and handler tables) that point at a FUNCTION START -- a
# stack-frame prologue (addiu sp,sp,-n) or the word after a return (jr ra; delay slot) -- so data pointers are not seeded as code
def word(a): return struct.unpack_from('>I', prog, a - PB)[0] if PB <= a < PE - 3 else 0
for k in range(0, len(prog) - 3, 4):
    w, = struct.unpack_from('>I', prog, k)
    if PB <= w < PE and not w & 3 and w not in seeds:
        op = word(w)
        if (op >> 16) == 0x27BD and (op & 0x8000) or word(w - 8) == 0x03E00008: seeds.add(w)
# RAPID RIVER: also every address right after a return (jr ra + its delay slot) in the monitor and the text -- the next function's
# start, reached only through computed jumps (the monitor's exception dispatch: 0xBFC0043C -> 0xBFC007D4). Over-seeding is safe (above).
for img, base in ((nmon, NB), (prog, PB)):
    for k in range(0, len(img) - 11, 4):
        if struct.unpack_from('>I', img, k)[0] == 0x03E00008: seeds.add(base + k + 8)
with open(out, 'w') as f:
    for s in sorted(seeds): f.write('%08X\n' % s)
print('scan_seeds: %d monitor + %d game seeds -> %s' % (sum(1 for s in seeds if s >= NB), sum(1 for s in seeds if s < NB), out))
