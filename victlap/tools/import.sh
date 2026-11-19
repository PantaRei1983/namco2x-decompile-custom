#!/bin/bash
# Victory Lap: a FRESH Ghidra project of victlap_main.bin (made by ../acedriver/tools/setup_roms.py --game victlap), then lifted C + build.
# Later rounds (new seeds) use tools/regen.sh, which re-reads the existing project.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# the tools folder: ghidra_12.1.4_PUBLIC, jdk-21.0.12.1+1 and the Ghidra project folder work/ (e.g. N22TOOLS=/c/n22tools)
T="${N22TOOLS:?set N22TOOLS to the tools folder (ghidra_12.1.4_PUBLIC, jdk-21.0.12.1+1, work/)}"
TW="$(cygpath -w "$T")"
WORK=$T/work
GH=$T/ghidra_12.1.4_PUBLIC
export JAVA_HOME="$TW\jdk-21.0.12.1+1"
export PATH="$T/jdk-21.0.12.1+1/bin:$PATH"
cp "$HERE/victlap_main.bin" "$WORK/"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/vl_seeds.txt"
rm -rf "$WORK/victlap.gpr" "$WORK/victlap.rep"
( cd "$WORK" && "$GH/support/analyzeHeadless.bat" "$TW\work" victlap -import "$TW\work\victlap_main.bin" \
    -loader BinaryLoader -processor 68000:BE:32:MC68020 -scriptPath "$TW\work\scripts" -preScript SeedVectors.java \
    -postScript SeedTables.java "$TW\work\vl_seeds.txt" \
    -postScript DumpPcode.java "$TW\work\victlap.pcode" > vl_import.log 2>&1 )
grep -E "SeedTables:|DumpPcode:" "$WORK/vl_import.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/victlap.pcode" "$HERE/victlap.pcode"
rm -f "$HERE"/gen/rr_lifted_*.c
( cd "$ROOT" && python -I tools/gen/lift68k.py victlap/victlap.pcode victlap/gen rr 8 victlap/victlap_main.bin | grep -v "untranslated\|CALLOTHER" )
if [ -f "$HERE/build/build.ninja" ]; then ( cd "$HERE" && ninja -C build vl 2>&1 | grep -E 'error|Linking' ); fi   # (MINGW64's own ninja, wherever MSYS2 is installed)
