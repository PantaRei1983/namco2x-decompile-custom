#!/bin/bash
# Victory Lap: a FRESH Ghidra project of victlap_main.bin (made by ../acedriver/tools/setup_roms.py --game victlap, which victlap/build.sh
# runs), then the lifted C (gen/rr_lifted_*.c) and, when build/ exists, the build. Later rounds (new seeds) use tools/regen.sh.
#   export N22TOOLS=/e/n2xtools        (Windows/MSYS2; on Linux any folder with ghidra_12.1.4_PUBLIC and work/)
#   victlap/tools/import.sh
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/tools/env.sh"
[ -f "$HERE/victlap_main.bin" ] || { echo "victlap_main.bin missing: run  victlap/build.sh /path/to/victlap.zip /path/to/namcoc74.zip  first"; exit 1; }
cp "$HERE/victlap_main.bin" "$WORK/"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/vl_seeds.txt"
rm -rf "$WORK/victlap.gpr" "$WORK/victlap.rep"
( cd "$WORK" && "$HEADLESS" "$WW" victlap -import "$WW${SEP}victlap_main.bin" \
    -loader BinaryLoader -processor 68000:BE:32:MC68020 -scriptPath "$WW${SEP}scripts" -preScript SeedVectors.java \
    -postScript SeedTables.java "$WW${SEP}vl_seeds.txt" \
    -postScript DumpPcode.java "$WW${SEP}victlap.pcode" > vl_import.log 2>&1 ) || { echo "Ghidra failed: see $WORK/vl_import.log"; tail -5 "$WORK/vl_import.log"; exit 1; }
grep -E "SeedTables:|DumpPcode:" "$WORK/vl_import.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/victlap.pcode" "$HERE/victlap.pcode"
mkdir -p "$HERE/gen"; rm -f "$HERE"/gen/rr_lifted_*.c
( cd "$ROOT" && $PY -I tools/gen/lift68k.py victlap/victlap.pcode victlap/gen rr 8 victlap/victlap_main.bin | grep -v "untranslated\|CALLOTHER" )
NINJA_OR_MAKE
