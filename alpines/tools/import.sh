#!/bin/bash
# Alpine Surfer: a FRESH Ghidra project from the chips (run once, and again whenever tools/lift_image.py changes), then lifted C + build.
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
python -I "$HERE/tools/lift_image.py" "$HERE/extracted" "$HERE/alpines_main.bin"
cp "$HERE/alpines_main.bin" "$WORK/"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/af_seeds.txt"
rm -rf "$WORK/alpines.gpr" "$WORK/alpines.rep"
( cd "$WORK" && "$GH/support/analyzeHeadless.bat" "$TW\work" alpines -import "$TW\work\alpines_main.bin" \
    -loader BinaryLoader -processor 68000:BE:32:MC68020 -scriptPath "$TW\work\scripts" -preScript SeedVectors.java \
    -postScript SeedTables.java "$TW\work\af_seeds.txt" \
    -postScript DumpPcode.java "$TW\work\alpines.pcode" > af_import.log 2>&1 )
grep -E "DumpPcode:" "$WORK/af_import.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/alpines.pcode" "$HERE/alpines.pcode"
rm -f "$HERE"/gen/af_lifted_*.c
( cd "$ROOT" && python -I tools/gen/lift68k.py alpines/alpines.pcode alpines/gen af 8 alpines/alpines_main.bin | grep -v "untranslated\|CALLOTHER" )
if [ -f "$HERE/build/build.ninja" ]; then ( cd "$HERE" && ninja -C build af 2>&1 | grep -E 'error|Linking' ); fi   # (no build/ yet on a first import: configure it with cmake, then ninja; MINGW64's own ninja, wherever MSYS2 is installed)
