#!/usr/bin/env python3
"""lift_image.py EXTRACTED_DIR OUT.BIN -- Air Combat 22's program image FOR THE TRANSLATOR (Ghidra + tools/gen/lift68k.py).

MAME ROM_LOAD32_BYTE (airco22b, Japan ACS1 Ver.B): acs1verb.4 / .3 / .2 / .1 are byte 0 / 1 / 2 / 3 of every long, 1 MB each,
4 MB in all, no banks.
"""
import sys

def build(d):
    lanes = [open(f'{d}/acs1verb.{n}', 'rb').read() for n in (4, 3, 2, 1)]
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
