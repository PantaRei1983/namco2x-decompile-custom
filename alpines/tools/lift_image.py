#!/usr/bin/env python3
"""lift_image.py EXTRACTED_DIR OUT.BIN -- Alpine Surfer's program image FOR THE TRANSLATOR (Ghidra + tools/gen/lift68k.py).

MAME ROM_LOAD32_BYTE (alpines, AF2 Ver.A): af2ver-a_uu.ic5 / _um.ic4 / _lm.ic3 / _ll.ic2 are byte 0 / 1 / 2 / 3 of every long, 8 MB
in all. 0x000000-0x1FFFFF is fixed; 0x200000-0x3FFFFF is a window onto one of three 2 MB banks (MAME alpines_state::rombank_w, the
register at 0x300000). The program is all in the fixed part (the banks hold data), so the translator gets the first 4 MB: the fixed
part and bank 0 (empty, 0xFF).
"""
import sys

def build(d):
    lanes = [open(f'{d}/af2ver-a_{n}', 'rb').read() for n in ('uu.ic5', 'um.ic4', 'lm.ic3', 'll.ic2')]
    img = bytearray(4 * len(lanes[0]))
    for i, lane in enumerate(lanes):
        img[i::4] = lane
    return img

def main():
    src, out = sys.argv[1], sys.argv[2]
    img = build(src)[:0x400000]
    open(out, 'wb').write(img)
    sp, pc = int.from_bytes(img[0:4], 'big'), int.from_bytes(img[4:8], 'big')
    print(f'{out}: {len(img):#x} bytes, reset SP {sp:#x}, reset PC {pc:#x}')

if __name__ == '__main__':
    main()
