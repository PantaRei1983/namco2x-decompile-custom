#!/bin/bash
# Alpine Surfer: a FRESH Ghidra project from the chips (run once, and again whenever tools/lift_image.py changes), then lifted C + build.
# Later rounds (new seeds) use tools/regen.sh, which re-reads the existing project.
#   export N22TOOLS=/e/n2xtools        (Windows/MSYS2; on Linux any folder with ghidra_12.1.4_PUBLIC and work/)
#   alpines/tools/import.sh
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/tools/env.sh"
$PY -I "$HERE/tools/lift_image.py" "$HERE/extracted" "$HERE/alpines_main.bin"
cp "$HERE/alpines_main.bin" "$WORK/"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/af_seeds.txt"
rm -rf "$WORK/alpines.gpr" "$WORK/alpines.rep"
( cd "$WORK" && "$HEADLESS" "$WW" alpines -import "$WW${SEP}alpines_main.bin" \
    -loader BinaryLoader -processor 68000:BE:32:MC68020 -scriptPath "$WW${SEP}scripts" -preScript SeedVectors.java \
    -postScript SeedTables.java "$WW${SEP}af_seeds.txt" \
    -postScript DumpPcode.java "$WW${SEP}alpines.pcode" > af_import.log 2>&1 ) || { echo "Ghidra failed: see $WORK/af_import.log"; tail -5 "$WORK/af_import.log"; exit 1; }
grep -E "DumpPcode:" "$WORK/af_import.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/alpines.pcode" "$HERE/alpines.pcode"
mkdir -p "$HERE/gen"; rm -f "$HERE"/gen/af_lifted_*.c
( cd "$ROOT" && $PY -I tools/gen/lift68k.py alpines/alpines.pcode alpines/gen af 8 alpines/alpines_main.bin | grep -v "untranslated\|CALLOTHER" )
NINJA_OR_MAKE   # (no build/ yet on a first import: configure it with cmake, then ninja / make)
