#!/bin/bash
# Alpine Surfer: Ghidra (tables + seeds) -> p-code -> lifted C -> build. Run from anywhere:  alpines/tools/regen.sh
# Needs the Ghidra project made by the first import (tools/import.sh) in $WORK.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/tools/env.sh"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/af_seeds.txt"
( cd "$WORK" && "$HEADLESS" "$WW" alpines -process alpines_main.bin -noanalysis \
    -scriptPath "$WW${SEP}scripts" \
    -postScript SeedTables.java "$WW${SEP}af_seeds.txt" \
    -postScript DumpPcode.java "$WW${SEP}alpines.pcode" > af_regen.log 2>&1 ) || { echo "Ghidra failed: see $WORK/af_regen.log"; tail -5 "$WORK/af_regen.log"; exit 1; }
grep -E "SeedTables:|DumpPcode:" "$WORK/af_regen.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/alpines.pcode" "$HERE/alpines.pcode"
rm -f "$HERE"/gen/af_lifted_*.c
( cd "$ROOT" && $PY -I tools/gen/lift68k.py alpines/alpines.pcode alpines/gen af 8 alpines/alpines_main.bin | grep -v "untranslated\|CALLOTHER" )
NINJA_OR_MAKE
