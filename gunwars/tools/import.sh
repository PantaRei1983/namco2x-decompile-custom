#!/bin/bash
# Gunmen Wars: a FRESH Ghidra project from the game program (tools/make_prog.py's prog.bin at 0x80000000), then its raw p-code
# (gunwars.pcode) for the MIPS translator. Args: the ROM folder (MAME's gunwars files, unzipped).
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
T="${N22TOOLS:?set N22TOOLS to the tools folder (ghidra_12.1.4_PUBLIC, jdk-21.0.12.1+1, work/)}"
TW="$(cygpath -w "$T")"
WORK=$T/work
GH=$T/ghidra_12.1.4_PUBLIC
export JAVA_HOME="$TW\jdk-21.0.12.1+1"
export PATH="$T/jdk-21.0.12.1+1/bin:$PATH"
ROMS="${1:-$HERE/extracted/roms}"
mkdir -p "$HERE/extracted"
python -I "$HERE/tools/make_prog.py" "$ROMS" "$HERE/extracted"
python -I "$HERE/tools/scan_seeds.py" "$HERE/extracted" "$HERE/extracted/seeds_static.txt"
cp "$HERE/extracted/prog.bin" "$WORK/gunwars_prog.bin"
cp "$HERE/extracted/nmon.bin" "$WORK/gunwars_nmon.bin"
cp "$HERE/../tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; { cat "$HERE/extracted/prog_entry.txt"; cat "$HERE/extracted/seeds_static.txt"; cat "$HERE/seeds.txt"; } > "$WORK/gw_seeds.txt"
rm -rf "$WORK/gunwars.gpr" "$WORK/gunwars.rep"
( cd "$WORK" && "$GH/support/analyzeHeadless.bat" "$TW\work" gunwars -import "$TW\work\gunwars_prog.bin" \
    -loader BinaryLoader -loader-baseAddr 0x80000000 -processor MIPS:BE:64:64-32addr -scriptPath "$TW\work\scripts" \
    -preScript SeedMips.java "$TW\work\gw_seeds.txt" "block:BFC00000:$TW\work\gunwars_nmon.bin" \
    -postScript DumpPcode.java "$TW\work\gunwars.pcode" > gw_import.log 2>&1 )
grep -E "SeedMips:|DumpPcode:" "$WORK/gw_import.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/gunwars.pcode" "$HERE/gunwars.pcode"
