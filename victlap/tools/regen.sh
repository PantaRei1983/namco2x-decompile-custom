#!/bin/bash
# Victory Lap: Ghidra (tables + seeds) -> p-code -> lifted C -> build, re-reading the project tools/import.sh made. Run from anywhere.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/tools/env.sh"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/vl_seeds.txt"
( cd "$WORK" && "$HEADLESS" "$WW" victlap -process victlap_main.bin -noanalysis \
    -scriptPath "$WW${SEP}scripts" \
    -postScript SeedTables.java "$WW${SEP}vl_seeds.txt" \
    -postScript DumpPcode.java "$WW${SEP}victlap.pcode" > vl_regen.log 2>&1 ) || { echo "Ghidra failed: see $WORK/vl_regen.log"; tail -5 "$WORK/vl_regen.log"; exit 1; }
grep -E "SeedTables:|DumpPcode:" "$WORK/vl_regen.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/victlap.pcode" "$HERE/victlap.pcode"
rm -f "$HERE"/gen/rr_lifted_*.c
( cd "$ROOT" && $PY -I tools/gen/lift68k.py victlap/victlap.pcode victlap/gen rr 8 victlap/victlap_main.bin | grep -v "untranslated\|CALLOTHER" )
NINJA_OR_MAKE
