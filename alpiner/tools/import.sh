#!/bin/bash
# Alpine Racer: a FRESH Ghidra project of alpiner_main.bin (made by tools/setup_roms.py, which alpiner/build.sh runs), then the lifted C
# (gen/al_lifted_*.c) and, when build/ exists, the build. The seeds are the set's own: seeds_<set>.txt (extracted/SET names the set).
#   export N22TOOLS=/e/n2xtools        (Windows/MSYS2; on Linux any folder with ghidra_12.1.4_PUBLIC and work/)
#   alpiner/tools/import.sh
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/tools/env.sh"
[ -f "$HERE/alpiner_main.bin" ] && [ -f "$HERE/extracted/SET" ] || { echo "alpiner_main.bin missing: run  alpiner/build.sh /path/to/alpinerd.zip  first"; exit 1; }
SET=$(tr -d '\r\n' < "$HERE/extracted/SET"); SEEDS="$HERE/seeds_$SET.txt"; touch "$SEEDS"
echo "Alpine Racer: translating the $SET program"
cp "$HERE/alpiner_main.bin" "$WORK/"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
cp "$SEEDS" "$WORK/al_seeds.txt"
rm -rf "$WORK/alpiner.gpr" "$WORK/alpiner.rep"
( cd "$WORK" && "$HEADLESS" "$WW" alpiner -import "$WW${SEP}alpiner_main.bin" \
    -loader BinaryLoader -processor 68000:BE:32:MC68020 -scriptPath "$WW${SEP}scripts" -preScript SeedVectors.java \
    -postScript SeedTables.java "$WW${SEP}al_seeds.txt" \
    -postScript DumpPcode.java "$WW${SEP}alpiner.pcode" > al_import.log 2>&1 ) || { echo "Ghidra failed: see $WORK/al_import.log"; tail -5 "$WORK/al_import.log"; exit 1; }
grep -E "SeedTables:|DumpPcode:" "$WORK/al_import.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/alpiner.pcode" "$HERE/alpiner.pcode"
mkdir -p "$HERE/gen"; rm -f "$HERE"/gen/al_lifted_*.c
( cd "$ROOT" && $PY -I tools/gen/lift68k.py alpiner/alpiner.pcode alpiner/gen al 8 alpiner/alpiner_main.bin | grep -v "untranslated\|CALLOTHER" )
NINJA_OR_MAKE
