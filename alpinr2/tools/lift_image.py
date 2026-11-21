#!/usr/bin/env python3
"""lift_image.py EXTRACTED_DIR OUT.BIN -- Alpine Racer 2's program image FOR THE TRANSLATOR (Ghidra + tools/gen/lift68k.py).

MAME ROM_LOAD32_BYTE (alpinr2a, ARS2 Ver.A): ars2vera.5 / .4 / .3 / .2 are byte 0 / 1 / 2 / 3 of every long. Each chip is 2 MB with
its second half empty, so the program is the first 1 MB of each: a 4 MB image, the size of the board's program space here.
"""
import sys

def build(d):
    lanes = [open(f'{d}/ars2vera.{n}', 'rb').read()[:0x100000] for n in (5, 4, 3, 2)]
    img = bytearray(4 * len(lanes[0]))
    for i, lane in enumerate(lanes):
        img[i::4] = lane
    return img

def main():
    src, out = sys.argv[1], sys.argv[2]
    img = build(src)
    open(out, 'wb').write(img)
    sp, pc = int.from_bytes(img[0:4], 'big'), int.from_bytes(img[4:8], 'big')
    print(f'{out}: {len(img):#x} bytes, reset SP {sp:#x}, reset PC {pc:#x}')

if __name__ == '__main__':
    main()
