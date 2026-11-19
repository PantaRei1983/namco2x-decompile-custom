#!/bin/bash
# Alpine Racer 2: Ghidra (tables + seeds) -> p-code -> lifted C -> build. Run from anywhere:  alpinr2/tools/regen.sh
# Needs the Ghidra project made by the first import (see docs in CMakeLists.txt) in $WORK.
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
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/a2_seeds.txt"
( cd "$WORK" && "$GH/support/analyzeHeadless.bat" "$TW\work" alpinr2 -process alpinr2_main.bin -noanalysis \
    -scriptPath "$TW\work\scripts" \
    -postScript SeedTables.java "$TW\work\a2_seeds.txt" \
    -postScript DumpPcode.java "$TW\work\alpinr2.pcode" > a2_regen.log 2>&1 )
grep -E "SeedTables:|DumpPcode:" "$WORK/a2_regen.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/alpinr2.pcode" "$HERE/alpinr2.pcode"
( cd "$ROOT" && python -I tools/gen/lift68k.py alpinr2/alpinr2.pcode alpinr2/gen a2 8 alpinr2/alpinr2_main.bin | grep -v "untranslated\|CALLOTHER" )
if [ -f "$HERE/build/build.ninja" ]; then ( cd "$HERE" && ninja -C build a2 2>&1 | grep -E 'error|Linking' ); fi   # (no build/ yet on a first import: configure it with cmake, then ninja; MINGW64's own ninja, wherever MSYS2 is installed)
