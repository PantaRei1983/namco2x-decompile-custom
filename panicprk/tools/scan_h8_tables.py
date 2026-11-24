#!/usr/bin/env python3
"""scan_h8_tables.py BIN SIZE PCS -- add to an H8 seed file (cov/*.pcs) the code addresses held in 16-bit jump tables: runs of 2+ words,
each an even address in the ROM right after a return (rts 5470 / rte 5670) or zero padding -- the translator's static walk does not
follow table jumps (Rapid River's sub-CPU 0x6D88, I/O board 0x0A86)."""
import sys, struct
d = open(sys.argv[1], 'rb').read()[:int(sys.argv[2], 0)]
def ok(w): return 0x100 <= w < len(d) and w % 2 == 0 and d[w - 2:w] in (b'\x54\x70', b'\x56\x70', b'\x00\x00')
seeds = set(int(x, 16) for x in open(sys.argv[3]).read().split())
k = 0
while k < len(d) - 1:
    n = 0
    while k + 2 * n < len(d) - 1 and ok(struct.unpack_from('>H', d, k + 2 * n)[0]): n += 1
    if n >= 2: seeds.update(struct.unpack_from('>H', d, k + 2 * i)[0] for i in range(n)); k += 2 * n
    else: k += 2
open(sys.argv[3], 'w').write(''.join('%06X\n' % x for x in sorted(seeds)))
print(len(seeds), 'seeds ->', sys.argv[3])
