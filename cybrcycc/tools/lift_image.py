#!/usr/bin/env python3
"""lift_image.py EXTRACTED_DIR OUT.BIN -- Cyber Cycles' program image FOR THE TRANSLATOR (Ghidra + tools/gen/lift68k.py).

The game's own copy protection: at 0x5764 it decodes 32 words stored at 0x578C (each word: xor the keycus id 0x0387, add the loop
count, invert, rotate right 7) into work RAM at 0xE10682 and jumps there; 0x5524 jumps to 0xE10686, four bytes in (past the block's
register push -- its pop then takes the caller's D4-D7/A5 into D0-D3/A0). The decoded block reads keycus word 7 around a delay and
writes keycus word 5 -- position-independent code (absolute keycus address, relative branches only).

Code in RAM is invisible to the ROM translation, so this image has the block DECODED IN PLACE at 0x578C and both jumps aimed there
(0xE10682 -> 0x578C, 0xE10686 -> 0x5790). Only the translation reads this image: the game runs on the genuine chips (src/cc_game.c
loads them), so its decoder still reads the scrambled words and still fills RAM, exactly as on the board.
"""
import sys, struct

def build(d):
    lanes = [open(f'{d}/cb2verc.{n}', 'rb').read() for n in (4, 3, 2, 1)]
    img = bytearray(4 * len(lanes[0]))
    for i, lane in enumerate(lanes):
        img[i::4] = lane
    return img

def main():
    src, out = sys.argv[1], sys.argv[2]
    img = build(src)
    SRC, RAM, N = 0x578C, 0xE10682, 32
    dec = bytearray()
    for i, cnt in enumerate(range(N - 1, -1, -1)):
        w = struct.unpack_from('>H', img, SRC + 2 * i)[0]
        w = (~((w ^ 0x0387) + cnt)) & 0xFFFF
        dec += struct.pack('>H', ((w >> 7) | (w << 9)) & 0xFFFF)
    assert dec[:4] == bytes.fromhex('48e7f080') and dec[-2:] == bytes.fromhex('4e75'), 'unexpected decoded block'
    img[SRC:SRC + 2 * N] = dec
    # every way in: jmp / jsr (xxx).l (absolute) and bra.l / bsr.l (32-bit displacement) aimed at the block's start or +4
    # (0x5786, 0x5528 jmp; 0x722, 0x7A0, 0xEDB8 bsr.l). An even address and an exact target: a stray match in data is all but impossible
    ways = []
    for at in range(0, 0x400000 - 6, 2):
        op = img[at:at + 2]
        if op in (b'\x4e\xf9', b'\x4e\xb9'):
            t = struct.unpack_from('>I', img, at + 2)[0]
            if t in (RAM, RAM + 4):
                struct.pack_into('>I', img, at + 2, SRC + (t - RAM)); ways.append(at)
        elif op in (b'\x60\xff', b'\x61\xff'):
            t = (at + 2 + struct.unpack_from('>i', img, at + 2)[0]) & 0xFFFFFFFF
            if t in (RAM, RAM + 4):
                struct.pack_into('>i', img, at + 2, SRC + (t - RAM) - (at + 2)); ways.append(at)
    for need in (0x5786, 0x5528, 0x722, 0x7A0, 0xEDB8):
        assert need in ways, f'expected a jump into the block at {need:#x}'
    open(out, 'wb').write(img)
    print(f'{out}: decoded block at {SRC:#x}; jumps into it redirected at ' + ', '.join(f'{a:#x}' for a in ways))

if __name__ == '__main__':
    main()
