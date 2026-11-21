#!/bin/bash
# Air Combat 22: Ghidra (tables + seeds) -> p-code -> lifted C -> build. Run from anywhere:  airco22/tools/regen.sh
# Needs the Ghidra project made by the first import (tools/import.sh) in $WORK.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/tools/env.sh"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/ac_seeds.txt"
( cd "$WORK" && "$HEADLESS" "$WW" airco22 -process airco22_main.bin -noanalysis \
    -scriptPath "$WW${SEP}scripts" \
    -postScript SeedTables.java "$WW${SEP}ac_seeds.txt" \
    -postScript DumpPcode.java "$WW${SEP}airco22.pcode" > ac_regen.log 2>&1 ) || { echo "Ghidra failed: see $WORK/ac_regen.log"; tail -5 "$WORK/ac_regen.log"; exit 1; }
grep -E "SeedTables:|DumpPcode:" "$WORK/ac_regen.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/airco22.pcode" "$HERE/airco22.pcode"
rm -f "$HERE"/gen/ac_lifted_*.c
( cd "$ROOT" && $PY -I tools/gen/lift68k.py airco22/airco22.pcode airco22/gen ac 8 airco22/airco22_main.bin | grep -v "untranslated\|CALLOTHER" )
NINJA_OR_MAKE
