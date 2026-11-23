#!/bin/bash
# Gunmen Wars: a FRESH Ghidra project from the game program (tools/make_prog.py's prog.bin at 0x80000000), then its raw p-code
# (gunwars.pcode) for the MIPS translator. Arg: the ROM folder (MAME's gunwars files, unzipped; default extracted/ as build.sh fills it).
# Windows (MSYS2) or Linux:  export N22TOOLS=<the folder with ghidra_12.1.4_PUBLIC (+ jdk-21.0.12.1+1 on Windows) and work/>
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
. "$HERE/tools/env.sh"
ROMS="${1:-$HERE/extracted/roms}"
ls "$ROMS"/gm[12]ver[ab].ic2 >/dev/null 2>&1 || ROMS="$HERE/extracted"      # (loose chips straight in extracted/ work too)
mkdir -p "$HERE/extracted"
$PY -I "$HERE/tools/make_prog.py" "$ROMS" "$HERE/extracted"
$PY -I "$HERE/tools/scan_seeds.py" "$HERE/extracted" "$HERE/extracted/seeds_static.txt"
cp "$HERE/extracted/prog.bin" "$WORK/gunwars_prog.bin"
cp "$HERE/extracted/nmon.bin" "$WORK/gunwars_nmon.bin"
cp "$HERE/../tools/gen/ghidra/"*.java "$WORK/scripts/"
# seeds.txt: a bare address is Ver. B's (gunwarsj); "gunwars 800321B0" / "gunwarsa 80032130" the same place in the other sets' programs
SET="$(cat "$HERE/extracted/SET" 2>/dev/null || echo gunwarsj)"; echo "Gunmen Wars set: $SET"
touch "$HERE/seeds.txt"; { cat "$HERE/extracted/prog_entry.txt"; cat "$HERE/extracted/seeds_static.txt"
  tr -d '\r' < "$HERE/seeds.txt" | awk -v s="$SET" 'NF == 1 && s == "gunwarsj" { print $1 } NF == 2 && $1 == s { print $2 }'; } > "$WORK/gw_seeds.txt"
rm -rf "$WORK/gunwars.gpr" "$WORK/gunwars.rep"
( cd "$WORK" && "$HEADLESS" "$WW" gunwars -import "$WW${SEP}gunwars_prog.bin" \
    -loader BinaryLoader -loader-baseAddr 0x80000000 -processor MIPS:BE:64:64-32addr -scriptPath "$WW${SEP}scripts" \
    -preScript SeedMips.java "$WW${SEP}gw_seeds.txt" "block:BFC00000:$WW${SEP}gunwars_nmon.bin" \
    -postScript DumpPcode.java "$WW${SEP}gunwars.pcode" > gw_import.log 2>&1 ) || { echo "Ghidra failed: see $WORK/gw_import.log"; tail -5 "$WORK/gw_import.log"; exit 1; }
grep -E "SeedMips:|DumpPcode:" "$WORK/gw_import.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/gunwars.pcode" "$HERE/gunwars.pcode"
