#!/bin/bash
# Cyber Cycles: Ghidra (tables + seeds) -> p-code -> lifted C -> build. Run from anywhere:  cybrcycc/tools/regen.sh
# Needs the Ghidra project made by the first import (tools/import.sh) in $WORK.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/tools/env.sh"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/cc_seeds.txt"
( cd "$WORK" && "$HEADLESS" "$WW" cybrcycc -process cybrcycc_main.bin -noanalysis \
    -scriptPath "$WW${SEP}scripts" \
    -postScript SeedTables.java "$WW${SEP}cc_seeds.txt" \
    -postScript DumpPcode.java "$WW${SEP}cybrcycc.pcode" > cc_regen.log 2>&1 ) || { echo "Ghidra failed: see $WORK/cc_regen.log"; tail -5 "$WORK/cc_regen.log"; exit 1; }
grep -E "SeedTables:|DumpPcode:" "$WORK/cc_regen.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/cybrcycc.pcode" "$HERE/cybrcycc.pcode"
rm -f "$HERE"/gen/cc_lifted_*.c
( cd "$ROOT" && $PY -I tools/gen/lift68k.py cybrcycc/cybrcycc.pcode cybrcycc/gen cc 8 cybrcycc/cybrcycc_main.bin | grep -v "untranslated\|CALLOTHER" )
NINJA_OR_MAKE
