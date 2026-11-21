#!/bin/bash
# Build Ace Driver: Victory Lap (MAME `victlap`, World ADV2 Ver.B) on the shared System 22 runtime.
#
#   ./build.sh /path/to/victlap.zip /path/to/namcoc74.zip     first time: unpack the ROMs (and victlap_main.bin)
#   ./build.sh                                                  later: just rebuild
#
# The game program is translated from YOUR ROM with Ghidra (tools/import.sh, once): gen/rr_lifted_*.c is not in the repository.
#   export N22TOOLS=/e/n2xtools          (the folder with ghidra_12.1.4_PUBLIC, jdk-21.0.12.1+1 and work/)
#   victlap/tools/import.sh             (~1-4 minutes; then this script builds)
# The master DSP and the sound MCU run interpreted (src/vl_interp.c): nothing else to generate.
set -e
ARGS=(); for a in "$@"; do ARGS+=("$(realpath -m -- "$a")"); done; set -- "${ARGS[@]}"
cd "$(dirname "$0")"
fail() { printf '\n*** %s\n\n' "$*" >&2; exit 1; }
for t in gcc cmake pkg-config python3; do command -v "$t" >/dev/null 2>&1 || fail "'$t' is not installed. Run ../install-deps.sh first."; done
pkg-config --exists sdl2 || fail "SDL2 development files are missing. Run ../install-deps.sh first."
PY=python3; command -v python3 >/dev/null 2>&1 || PY=python
if [ $# -gt 0 ]; then $PY ../acedriver/tools/setup_roms.py --game victlap "$@" || exit 1
elif ! $PY ../acedriver/tools/setup_roms.py --game victlap --check; then
    ZIPS=()
    for z in victlap.zip namcoc74.zip; do
        for d in . .. roms ../roms "$HOME/Downloads"; do
            if [ -f "$d/$z" ]; then ZIPS+=("$d/$z"); echo "Found ROMs: $d/$z"; break; fi
        done
    done
    [ ${#ZIPS[@]} -gt 0 ] && $PY ../acedriver/tools/setup_roms.py --game victlap "${ZIPS[@]}" || true
    $PY ../acedriver/tools/setup_roms.py --game victlap --check || fail "No ROMs yet. Run:   ./build.sh /path/to/victlap.zip /path/to/namcoc74.zip"
fi
ls gen/rr_lifted_*.c >/dev/null 2>&1 || fail "The program is not translated yet. Run:   export N22TOOLS=<tools folder>;  tools/import.sh   (then ./build.sh again)"
GEN=(); command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
[ -f build/CMakeCache.txt ] || cmake -S . -B build "${GEN[@]}" -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target vl -j"${JOBS:-$(nproc)}"
printf '\nDone! Start the game from the top folder with:   ./launch.sh vl\n'
printf '  5 = insert coin (2 coins a game)   X / Up = gas   Z / Down = brake   arrows = steer   A / S = shift   Esc = menu\n\n'
