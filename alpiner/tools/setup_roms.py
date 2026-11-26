#!/usr/bin/env python3
"""setup_roms.py [ZIP-or-FOLDER ...] -- Alpine Racer's ROMs into alpiner/extracted/, and alpiner_main.bin for the translator.

Any of MAME's three Alpine Racer sets works; the program is translated from the one you have:
  alpinerd   World, AR2 Ver.D (MAME's parent set)   ar2verd.1 .. .4   ROM_LOAD32_BYTE, 4 x 1 MB
  alpinerc   World, AR2 Ver.C                       ar2verc.1 .. .4   ROM_LOAD32_BYTE, 4 x 1 MB
  alpinerjc  Japan, AR1 Ver.C                       ar1verc.1, .2     ROM_LOAD32_WORD_SWAP, 2 x 2 MB
Graphics, models, sound and the default EEPROM are the same chips in every set (a clone's zip may leave them in alpinerd.zip:
pass both). Every chip is checked by name, size and MAME's CRC32. Writes extracted/SET (the set found) and alpiner_main.bin
(the 4 MB program, big-endian as the 68020 sees it). The DSP's c71.bin is built into the engine: namcoc71.zip is not needed.
  --check   exit 0 if extracted/ already holds a complete set
"""
import os, sys, zipfile, zlib

COMMON = {
    "alpiner_defaults.nv": (0x2000, 0xefbef3d8),
    "ar1ccrh.1d": (0x80000, 0xee7a4803), "ar1ccrl.3d": (0x200000, 0x17387b2c),
    "ar1cg0.8d": (0x200000, 0x93f3a9d9), "ar1cg1.10d": (0x200000, 0x39828c8b), "ar1cg2.12d": (0x200000, 0xf7b058d1),
    "ar1cg3.13d": (0x200000, 0xc28a3d2a), "ar1cg4.14d": (0x200000, 0xabdb161f), "ar1cg5.16d": (0x200000, 0x2381cfea),
    "ar1cg6.18d": (0x200000, 0xca0b6d23), "ar1cg7.19d": (0x200000, 0xffb9f9f9),
    "ar1datab.8k": (0x80000, 0xc26306f8),
    "ar1ptrl0.18k": (0x80000, 0x82405108), "ar1ptrl1.16k": (0x80000, 0x8739b09c), "ar1ptrl2.15k": (0x80000, 0xbda693a9),
    "ar1ptrl3.14k": (0x80000, 0x82797405), "ar1ptrm0.18j": (0x80000, 0x64bd6620), "ar1ptrm1.16j": (0x80000, 0x2232f0a5),
    "ar1ptrm2.15j": (0x80000, 0x8ee14e6f), "ar1ptrm3.14j": (0x80000, 0x1094a970), "ar1ptru0.18f": (0x80000, 0x26d88467),
    "ar1ptru1.16f": (0x80000, 0xc5e2c208), "ar1ptru2.15f": (0x80000, 0x1321ec59), "ar1ptru3.14f": (0x80000, 0x139d7dc1),
    "ar1scg0.12f": (0x200000, 0xe7be830a), "ar1scg1.10f": (0x200000, 0x8f15a686),
    "ar1wavea.2l": (0x200000, 0xdbf64562),
}
# a chip a set may carry under its twin board's name (same dump): MAME's alpinerd lists both
TWINS = {"ar1ccrh.1d": "ar1ccrh.5b", "ar1ccrl.3d": "ar1ccrl.7b", "ar1cg0.8d": "ar1cg0.12b", "ar1cg1.10d": "ar1cg1.13b",
         "ar1cg2.12d": "ar1cg2.14b", "ar1cg3.13d": "ar1cg3.16b", "ar1cg4.14d": "ar1cg4.18b", "ar1cg5.16d": "ar1cg5.19b",
         "ar1cg6.18d": "ar1cg6.18b", "ar1cg7.19d": "ar1cg7.15a", "ar1scg0.12f": "ar1scg0.12l", "ar1scg1.10f": "ar1scg1.10l"}
PROGRAMS = [   # (set, layout, [(chip, size, crc)]) -- preferred first
    ("alpinerd", "byte", [("ar2verd.1", 0x100000, 0xfa3380b9), ("ar2verd.2", 0x100000, 0x76141352),
                          ("ar2verd.3", 0x100000, 0x9beffe6a), ("ar2verd.4", 0x100000, 0x1f3f1134)]),
    ("alpinerc", "byte", [("ar2verc.1", 0x100000, 0x61323842), ("ar2verc.2", 0x100000, 0x43795b2d),
                          ("ar2verc.3", 0x100000, 0xacb3003b), ("ar2verc.4", 0x100000, 0x800acc21)]),
    ("alpinerjc", "word", [("ar1verc.1", 0x200000, 0x8a87dc27), ("ar1verc.2", 0x200000, 0x5f4f5615)]),
]

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(HERE, "extracted")


def sources(args):
    """every file found in the given zips / folders (and roms/ or extracted/ by default): name -> loader"""
    found = {}
    paths = args or [p for p in (os.path.join(HERE, "roms"), os.path.join(HERE, "..", "roms"), DEST) if os.path.isdir(p)]
    for p in paths:
        if os.path.isdir(p):
            for n in os.listdir(p):
                f = os.path.join(p, n)
                if os.path.isfile(f) and n.lower().endswith(".zip"):
                    paths.append(f)
                elif os.path.isfile(f):
                    found.setdefault(n.lower(), (lambda f=f: open(f, "rb").read()))
        elif p.lower().endswith(".zip") and os.path.isfile(p):
            z = zipfile.ZipFile(p)
            for i in z.infolist():
                n = os.path.basename(i.filename).lower()
                if n: found.setdefault(n, (lambda z=z, i=i: z.read(i)))
        else:
            sys.exit("not a zip or folder: %s" % p)
    return found


def good(found, name, size, crc, cache):
    for n in (name, TWINS.get(name)):
        if n and n in found:
            d = cache.get(n) or found[n]()
            cache[n] = d
            if len(d) == size and (zlib.crc32(d) & 0xffffffff) == crc:
                return d
    # not under MAME's name: the same dump under another one (a newer MAME names alpinerc's program ar2ver-c.1 .. .4 for ar2verc.1 ..)
    # -- any file of the right size and CRC
    for n in found:
        d = cache.get(n) or found[n]()
        cache[n] = d
        if len(d) == size and (zlib.crc32(d) & 0xffffffff) == crc:
            return d
    return None


def image(layout, data):
    img = bytearray(0x400000)
    if layout == "byte":            # chip .1 -> byte 3 of every long, .2 -> 2, .3 -> 1, .4 -> 0
        for k, d in enumerate(data):
            img[3 - k::4] = d
    else:                           # ROM_LOAD32_WORD_SWAP: ar1verc.2 the high word, ar1verc.1 the low word, each stored low byte first
        for h, d in enumerate((data[1], data[0])):
            img[h * 2 + 0::4] = d[1::2]
            img[h * 2 + 1::4] = d[0::2]
    return img


def main():
    check = "--check" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--check"]
    if check:
        args = [DEST] if os.path.isdir(DEST) else []
        if not args: sys.exit(1)
    found = sources(args)
    cache, out, bad = {}, {}, []
    for name, (size, crc) in COMMON.items():
        d = good(found, name, size, crc, cache)
        if d is None: bad.append(name)
        else: out[name] = d
    prog = None
    for set_, layout, chips in PROGRAMS:
        ds = [good(found, n, s, c, cache) for n, s, c in chips]
        if all(d is not None for d in ds):
            prog = (set_, layout, chips, ds)
            break
    if check:
        sys.exit(0 if not bad and prog and os.path.isfile(os.path.join(HERE, "alpiner_main.bin")) else 1)
    if bad or not prog:
        if bad: print("missing or wrong (name, size or CRC):", ", ".join(bad))
        if not prog: print("no complete program: need ar2verd.1-4 (alpinerd), ar2verc.1-4 (alpinerc) or ar1verc.1-2 (alpinerjc)")
        sys.exit(1)
    set_, layout, chips, ds = prog
    os.makedirs(DEST, exist_ok=True)
    for name, d in out.items():
        open(os.path.join(DEST, name), "wb").write(d)
    for (name, _, _), d in zip(chips, ds):
        open(os.path.join(DEST, name), "wb").write(d)
    open(os.path.join(DEST, "SET"), "w").write(set_ + "\n")
    img = image(layout, ds)
    open(os.path.join(HERE, "alpiner_main.bin"), "wb").write(img)
    print("Alpine Racer: %s (%d chips, all CRCs good) -> %s; alpiner_main.bin, reset PC 0x%X"
          % (set_, len(out) + len(chips), DEST, int.from_bytes(img[4:8], "big")))


if __name__ == "__main__":
    main()
