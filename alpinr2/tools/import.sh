#!/bin/bash
# Alpine Racer 2: a FRESH Ghidra project from the chips (run once, and again whenever tools/lift_image.py changes), then lifted C + build.
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
python -I "$HERE/tools/lift_image.py" "$HERE/extracted" "$HERE/alpinr2_main.bin"
cp "$HERE/alpinr2_main.bin" "$WORK/"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/a2_seeds.txt"
rm -rf "$WORK/alpinr2.gpr" "$WORK/alpinr2.rep"
( cd "$WORK" && "$GH/support/analyzeHeadless.bat" "$TW\work" alpinr2 -import "$TW\work\alpinr2_main.bin" \
    -loader BinaryLoader -processor 68000:BE:32:MC68020 -scriptPath "$TW\work\scripts" -preScript SeedVectors.java \
    -postScript SeedTables.java "$TW\work\a2_seeds.txt" \
    -postScript DumpPcode.java "$TW\work\alpinr2.pcode" > a2_import.log 2>&1 )
grep -E "DumpPcode:" "$WORK/a2_import.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/alpinr2.pcode" "$HERE/alpinr2.pcode"
rm -f "$HERE"/gen/a2_lifted_*.c
( cd "$ROOT" && python -I tools/gen/lift68k.py alpinr2/alpinr2.pcode alpinr2/gen a2 8 alpinr2/alpinr2_main.bin | grep -v "untranslated\|CALLOTHER" )
if [ -f "$HERE/build/build.ninja" ]; then ( cd "$HERE" && ninja -C build a2 2>&1 | grep -E 'error|Linking' ); fi   # (no build/ yet on a first import: configure it with cmake, then ninja; MINGW64's own ninja, wherever MSYS2 is installed)
