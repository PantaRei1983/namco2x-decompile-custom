#!/bin/bash
# Air Combat 22: a FRESH Ghidra project from the chips (run once, and again whenever tools/lift_image.py changes), then lifted C + build.
# Later rounds (new seeds) use tools/regen.sh, which re-reads the existing project.
#   export N22TOOLS=/e/n2xtools        (Windows/MSYS2; on Linux any folder with ghidra_12.1.4_PUBLIC and work/)
#   airco22/tools/import.sh
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/tools/env.sh"
$PY -I "$HERE/tools/lift_image.py" "$HERE/extracted" "$HERE/airco22_main.bin"
cp "$HERE/airco22_main.bin" "$WORK/"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/ac_seeds.txt"
rm -rf "$WORK/airco22.gpr" "$WORK/airco22.rep"
( cd "$WORK" && "$HEADLESS" "$WW" airco22 -import "$WW${SEP}airco22_main.bin" \
    -loader BinaryLoader -processor 68000:BE:32:MC68020 -scriptPath "$WW${SEP}scripts" -preScript SeedVectors.java \
    -postScript SeedTables.java "$WW${SEP}ac_seeds.txt" \
    -postScript DumpPcode.java "$WW${SEP}airco22.pcode" > ac_import.log 2>&1 ) || { echo "Ghidra failed: see $WORK/ac_import.log"; tail -5 "$WORK/ac_import.log"; exit 1; }
grep -E "DumpPcode:" "$WORK/ac_import.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/airco22.pcode" "$HERE/airco22.pcode"
mkdir -p "$HERE/gen"; rm -f "$HERE"/gen/ac_lifted_*.c
( cd "$ROOT" && $PY -I tools/gen/lift68k.py airco22/airco22.pcode airco22/gen ac 8 airco22/airco22_main.bin | grep -v "untranslated\|CALLOTHER" )
NINJA_OR_MAKE   # (no build/ yet on a first import: configure it with cmake, then ninja / make)
