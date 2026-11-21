#!/bin/bash
# Alpine Racer: Ghidra (tables + seeds) -> p-code -> lifted C -> build, re-reading the project tools/import.sh made. Run from anywhere.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/tools/env.sh"
SET=$(tr -d '\r\n' < "$HERE/extracted/SET"); SEEDS="$HERE/seeds_$SET.txt"; touch "$SEEDS"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
cp "$SEEDS" "$WORK/al_seeds.txt"
( cd "$WORK" && "$HEADLESS" "$WW" alpiner -process alpiner_main.bin -noanalysis \
    -scriptPath "$WW${SEP}scripts" \
    -postScript SeedTables.java "$WW${SEP}al_seeds.txt" \
    -postScript DumpPcode.java "$WW${SEP}alpiner.pcode" > al_regen.log 2>&1 ) || { echo "Ghidra failed: see $WORK/al_regen.log"; tail -5 "$WORK/al_regen.log"; exit 1; }
grep -E "SeedTables:|DumpPcode:" "$WORK/al_regen.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/alpiner.pcode" "$HERE/alpiner.pcode"
rm -f "$HERE"/gen/al_lifted_*.c
( cd "$ROOT" && $PY -I tools/gen/lift68k.py alpiner/alpiner.pcode alpiner/gen al 8 alpiner/alpiner_main.bin | grep -v "untranslated\|CALLOTHER" )
NINJA_OR_MAKE
