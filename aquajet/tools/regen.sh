#!/bin/bash
# Aqua Jet: Ghidra (tables + seeds) -> p-code -> lifted C -> build. Run from anywhere:  aquajet/tools/regen.sh
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
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/aj_seeds.txt"
( cd "$WORK" && "$GH/support/analyzeHeadless.bat" "$TW\work" aquajet -process aquajet_main.bin -noanalysis \
    -scriptPath "$TW\work\scripts" \
    -postScript SeedTables.java "$TW\work\aj_seeds.txt" \
    -postScript DumpPcode.java "$TW\work\aquajet.pcode" > aj_regen.log 2>&1 )
grep -E "SeedTables:|DumpPcode:" "$WORK/aj_regen.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/aquajet.pcode" "$HERE/aquajet.pcode"
( cd "$ROOT" && python -I tools/gen/lift68k.py aquajet/aquajet.pcode aquajet/gen aj 8 aquajet/aquajet_main.bin | grep -v "untranslated\|CALLOTHER" )
if [ -f "$HERE/build/build.ninja" ]; then ( cd "$HERE" && ninja -C build aj 2>&1 | grep -E 'error|Linking' ); fi   # (no build/ yet on a first import: configure it with cmake, then ninja; MINGW64's own ninja, wherever MSYS2 is installed)
