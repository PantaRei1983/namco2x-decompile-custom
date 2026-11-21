#!/bin/bash
# Aqua Jet: Ghidra (tables + seeds) -> p-code -> lifted C -> build. Run from anywhere:  aquajet/tools/regen.sh
# Needs the Ghidra project made by the first import (tools/import.sh) in $WORK.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/tools/env.sh"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/aj_seeds.txt"
( cd "$WORK" && "$HEADLESS" "$WW" aquajet -process aquajet_main.bin -noanalysis \
    -scriptPath "$WW${SEP}scripts" \
    -postScript SeedTables.java "$WW${SEP}aj_seeds.txt" \
    -postScript DumpPcode.java "$WW${SEP}aquajet.pcode" > aj_regen.log 2>&1 ) || { echo "Ghidra failed: see $WORK/aj_regen.log"; tail -5 "$WORK/aj_regen.log"; exit 1; }
grep -E "SeedTables:|DumpPcode:" "$WORK/aj_regen.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/aquajet.pcode" "$HERE/aquajet.pcode"
rm -f "$HERE"/gen/aj_lifted_*.c
( cd "$ROOT" && $PY -I tools/gen/lift68k.py aquajet/aquajet.pcode aquajet/gen aj 8 aquajet/aquajet_main.bin | grep -v "untranslated\|CALLOTHER" )
NINJA_OR_MAKE
