#!/bin/bash
# Cyber Cycles: a FRESH Ghidra project from the chips (run once, and again whenever tools/lift_image.py changes), then lifted C + build.
# Later rounds (new seeds) use tools/regen.sh, which re-reads the existing project.
#   export N22TOOLS=/e/n2xtools        (Windows/MSYS2; on Linux any folder with ghidra_12.1.4_PUBLIC and work/)
#   cybrcycc/tools/import.sh
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/tools/env.sh"
$PY -I "$HERE/tools/lift_image.py" "$HERE/extracted" "$HERE/cybrcycc_main.bin"
cp "$HERE/cybrcycc_main.bin" "$WORK/"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/seeds.txt"; cp "$HERE/seeds.txt" "$WORK/cc_seeds.txt"
rm -rf "$WORK/cybrcycc.gpr" "$WORK/cybrcycc.rep"
( cd "$WORK" && "$HEADLESS" "$WW" cybrcycc -import "$WW${SEP}cybrcycc_main.bin" \
    -loader BinaryLoader -processor 68000:BE:32:MC68020 -scriptPath "$WW${SEP}scripts" -preScript SeedVectors.java \
    -postScript SeedTables.java "$WW${SEP}cc_seeds.txt" \
    -postScript DumpPcode.java "$WW${SEP}cybrcycc.pcode" > cc_import.log 2>&1 ) || { echo "Ghidra failed: see $WORK/cc_import.log"; tail -5 "$WORK/cc_import.log"; exit 1; }
grep -E "DumpPcode:" "$WORK/cc_import.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/cybrcycc.pcode" "$HERE/cybrcycc.pcode"
mkdir -p "$HERE/gen"; rm -f "$HERE"/gen/cc_lifted_*.c
( cd "$ROOT" && $PY -I tools/gen/lift68k.py cybrcycc/cybrcycc.pcode cybrcycc/gen cc 8 cybrcycc/cybrcycc_main.bin | grep -v "untranslated\|CALLOTHER" )
NINJA_OR_MAKE   # (no build/ yet on a first import: configure it with cmake, then ninja / make)
