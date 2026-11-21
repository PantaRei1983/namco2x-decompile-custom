#!/usr/bin/env python3
"""lift_image.py EXTRACTED_DIR OUT.BIN -- Aqua Jet's program image FOR THE TRANSLATOR (Ghidra + tools/gen/lift68k.py).

MAME ROM_LOAD32_BYTE (aquajet, AJ2 Ver.B): aj2verb.4 / .3 / .2 / .1 are byte 0 / 1 / 2 / 3 of every long: 4 x 1 MB = the 4 MB image.
Older MAME sets name the same chips aj2ver-b.4 / .3 / .2 / .1 (same CRCs): either name is taken.
"""
import os, sys

def chip(d, n):
    for name in (f'aj2verb.{n}', f'aj2ver-b.{n}'):
        if os.path.exists(f'{d}/{name}'):
            return open(f'{d}/{name}', 'rb').read()
    sys.exit(f'{d}: neither aj2verb.{n} nor aj2ver-b.{n} is there')

def build(d):
    lanes = [chip(d, n) for n in (4, 3, 2, 1)]
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
