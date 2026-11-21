#!/bin/bash
# Ridge Racer: Ghidra (tables + seeds) -> p-code -> lifted C -> build, re-reading the project tools/import.sh made. Run from anywhere.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/tools/env.sh"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/rg_seeds.txt"
( cd "$WORK" && "$HEADLESS" "$WW" ridgerac -process ridgerac_main.bin -noanalysis \
    -scriptPath "$WW${SEP}scripts" \
    -postScript SeedTables.java "$WW${SEP}rg_seeds.txt" \
    -postScript DumpPcode.java "$WW${SEP}ridgerac.pcode" > rg_regen.log 2>&1 )
grep -E "SeedTables:|DumpPcode:" "$WORK/rg_regen.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/ridgerac.pcode" "$HERE/ridgerac.pcode"
rm -f "$HERE"/gen/rr_lifted_*.c
( cd "$ROOT" && $PY -I tools/gen/lift68k.py ridgerac/ridgerac.pcode ridgerac/gen rr 8 ridgerac/ridgerac_main.bin | grep -v "untranslated\|CALLOTHER" )
NINJA_OR_MAKE
