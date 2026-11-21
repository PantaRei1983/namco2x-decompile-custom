#!/bin/bash
# Ridge Racer: a FRESH Ghidra project of ridgerac_main.bin (made by ../acedriver/tools/setup_roms.py --game ridgerac), then the lifted
# C (gen/rr_lifted_*.c) and, when build/ exists, the build. Later rounds (new seeds) use tools/regen.sh, which re-reads the project.
#   export N22TOOLS=/e/n2xtools        (Windows/MSYS2; on Linux any folder with ghidra_12.1.4_PUBLIC and work/)
#   ridgerac/tools/import.sh
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/tools/env.sh"
[ -f "$HERE/ridgerac_main.bin" ] || { echo "ridgerac_main.bin missing: run  python3 acedriver/tools/setup_roms.py --game ridgerac ridgerac.zip namcoc74.zip"; exit 1; }
cp "$HERE/ridgerac_main.bin" "$WORK/"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/rg_seeds.txt"
rm -rf "$WORK/ridgerac.gpr" "$WORK/ridgerac.rep"
( cd "$WORK" && "$HEADLESS" "$WW" ridgerac -import "$WW${SEP}ridgerac_main.bin" \
    -loader BinaryLoader -processor 68000:BE:32:MC68020 -scriptPath "$WW${SEP}scripts" -preScript SeedVectors.java \
    -postScript SeedTables.java "$WW${SEP}rg_seeds.txt" \
    -postScript DumpPcode.java "$WW${SEP}ridgerac.pcode" > rg_import.log 2>&1 )
grep -E "SeedTables:|DumpPcode:" "$WORK/rg_import.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/ridgerac.pcode" "$HERE/ridgerac.pcode"
mkdir -p "$HERE/gen"; rm -f "$HERE"/gen/rr_lifted_*.c
( cd "$ROOT" && $PY -I tools/gen/lift68k.py ridgerac/ridgerac.pcode ridgerac/gen rr 8 ridgerac/ridgerac_main.bin | grep -v "untranslated\|CALLOTHER" )
NINJA_OR_MAKE
