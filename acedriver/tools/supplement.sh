#!/bin/bash
# Ace Driver: THE SUPPLEMENT -- code the shipped lifted program (gen/rr_lifted.c) does not have, because the game only ever reaches it
# through its copy of the program in work RAM (the other cars' tasks in a linked race; rr_game_t ram_code_*). A fresh Ghidra project of
# the program with our seeds (supp_seeds.txt: the entry points found by running linked races), then tools/gen/lift68k.py in SUPPLEMENT
# mode: only the functions gen/rr_lifted.c lacks, named XL_..., looked up by ads_supp_jump() (src/rr_main.c's trap hook calls it).
#
#   N22TOOLS=/c/n22tools acedriver/tools/supplement.sh        (after acedriver/tools/setup_roms.py made acedriver_main.bin)
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# the tools folder: ghidra_12.1.4_PUBLIC, jdk-21.0.12.1+1 and the Ghidra project folder work/ (e.g. N22TOOLS=/c/n22tools)
T="${N22TOOLS:?set N22TOOLS to the tools folder (ghidra_12.1.4_PUBLIC, jdk-21.0.12.1+1, work/)}"
TW="$(cygpath -w "$T")"
WORK=$T/work
GH=$T/ghidra_12.1.4_PUBLIC
export JAVA_HOME="$TW\jdk-21.0.12.1+1"
export PATH="$T/jdk-21.0.12.1+1/bin:$PATH"
mkdir -p "$HERE/gen_supp"
cp "$HERE/acedriver_main.bin" "$WORK/"
cp "$ROOT/tools/gen/ghidra/"*.java "$WORK/scripts/"
touch "$HERE/supp_seeds.txt"; cp "$HERE/supp_seeds.txt" "$WORK/ad_seeds.txt"
rm -rf "$WORK/acedriver.gpr" "$WORK/acedriver.rep"
( cd "$WORK" && "$GH/support/analyzeHeadless.bat" "$TW\work" acedriver -import "$TW\work\acedriver_main.bin" \
    -loader BinaryLoader -processor 68000:BE:32:MC68020 -scriptPath "$TW\work\scripts" -preScript SeedVectors.java \
    -postScript SeedTables.java "$TW\work\ad_seeds.txt" \
    -postScript DumpPcode.java "$TW\work\acedriver.pcode" > ad_import.log 2>&1 )
grep -E "SeedTables:|DumpPcode:" "$WORK/ad_import.log" | sed 's/^INFO *//; s/(GhidraScript).*//'
cp "$WORK/acedriver.pcode" "$HERE/acedriver.pcode"
# every instruction address the shipped program has (its rr_ia_ table)
python -I - "$HERE/gen/rr_lifted.c" "$HERE/gen_supp/known.txt" <<'EOF'
import re, sys
s = open(sys.argv[1], encoding='utf-8').read()
a = s.index('static const uint32_t rr_ia_[] = {'); b = s.index('};', a)
open(sys.argv[2], 'w').write('\n'.join('%X' % int(x, 16) for x in re.findall(r'0x([0-9A-F]+)U', s[a:b])))
EOF
rm -f "$HERE"/gen_supp/ads_lifted_*.c
( cd "$ROOT" && LIFT_SUPPLEMENT="$HERE/gen_supp/known.txt" LIFT_SUPP_NAME=ads_supp_jump \
    python -I tools/gen/lift68k.py acedriver/acedriver.pcode acedriver/gen_supp ads 4 acedriver/acedriver_main.bin | grep -v "untranslated\|CALLOTHER" )
if [ -f "$HERE/build/build.ninja" ]; then ( cd "$HERE" && ninja -C build ad 2>&1 | grep -E 'error|Linking' ); fi   # (no build/ yet on a first import: configure it with cmake, then ninja; MINGW64's own ninja, wherever MSYS2 is installed)
