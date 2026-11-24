#!/bin/bash
# Panic Park: a FRESH Ghidra project from the game program (tools/make_prog.py's prog.bin at 0x80000000), then its raw p-code
# (panicprk.pcode) for the MIPS translator. Arg: the ROM folder (MAME's panicprk files, unzipped; default extracted/ as build.sh fills it).
# Windows (MSYS2) or Linux:  export N22TOOLS=<the folder with ghidra_12.1.4_PUBLIC (+ jdk-21.0.12.1+1 on Windows) and work/>
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
. "$HERE/tools/env.sh"
ROMS="${1:-$HERE/extracted/roms}"
[ -f "$ROMS/pnp2vera.ic2" ] || ROMS="$HERE/extracted"      # (loose chips straight in extracted/ work too)
mkdir -p "$HERE/extracted"
$PY -I "$HERE/tools/make_prog.py" "$ROMS" "$HERE/extracted"
$PY -I "$HERE/tools/scan_seeds.py" "$HERE/extracted" "$HERE/extracted/seeds_static.txt"
cp "$HERE/extracted/prog.bin" "$WORK/panicprk_prog.bin"
cp "$HERE/extracted/nmon.bin" "$WORK/panicprk_nmon.bin"
cp "$HERE/../tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; { cat "$HERE/extracted/prog_entry.txt"; cat "$HERE/extracted/seeds_static.txt"; cat "$HERE/seeds.txt"; } > "$WORK/pp_seeds.txt"
rm -rf "$WORK/panicprk.gpr" "$WORK/panicprk.rep"
( cd "$WORK" && "$HEADLESS" "$WW" panicprk -import "$WW${SEP}panicprk_prog.bin" \
    -loader BinaryLoader -loader-baseAddr 0x80000000 -processor MIPS:BE:64:64-32addr -scriptPath "$WW${SEP}scripts" \
    -preScript SeedMips.java "$WW${SEP}pp_seeds.txt" "block:BFC00000:$WW${SEP}panicprk_nmon.bin" \
    -postScript DumpPcode.java "$WW${SEP}panicprk.pcode" > pp_import.log 2>&1 ) || { echo "Ghidra failed: see $WORK/pp_import.log"; tail -5 "$WORK/pp_import.log"; exit 1; }
grep -E "SeedMips:|DumpPcode:" "$WORK/pp_import.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/panicprk.pcode" "$HERE/panicprk.pcode"
