#!/bin/bash
# Build Alpine Racer (MAME `alpinerd`, `alpinerc` or `alpinerjc`: the program is translated from the set you have) on the shared System 22 runtime.
#
#   ./build.sh /path/to/alpinerd.zip     first time: unpack the ROMs (and alpiner_main.bin)
#   ./build.sh                           later: just rebuild
#
# The game program is translated from YOUR ROM with Ghidra (tools/import.sh, once): gen/al_lifted_*.c is not in the repository.
#   export N22TOOLS=/e/n2xtools          (the folder with ghidra_12.1.4_PUBLIC, jdk-21.0.12.1+1 and work/)
#   alpiner/tools/import.sh             (~1-4 minutes; then this script builds)
# The master DSP and the sound MCU run interpreted (src/al_interp.c, src/al_snd_interp.c): nothing else to generate.
set -e
ARGS=(); for a in "$@"; do ARGS+=("$(realpath -m -- "$a")"); done; set -- "${ARGS[@]}"
cd "$(dirname "$0")"
fail() { printf '\n*** %s\n\n' "$*" >&2; exit 1; }
for t in gcc cmake pkg-config python3; do command -v "$t" >/dev/null 2>&1 || fail "'$t' is not installed. Run ../install-deps.sh first."; done
pkg-config --exists sdl2 || fail "SDL2 development files are missing. Run ../install-deps.sh first."
PY=python3; command -v python3 >/dev/null 2>&1 || PY=python
if [ $# -gt 0 ]; then $PY tools/setup_roms.py "$@" || exit 1
elif ! $PY tools/setup_roms.py --check; then
    $PY tools/setup_roms.py || fail "No ROMs yet. Run:   ./build.sh /path/to/alpinerd.zip   (or alpinerc.zip / alpinerjc.zip, with alpinerd.zip for a split clone set)"
fi
ls gen/al_lifted_*.c >/dev/null 2>&1 || fail "The program is not translated yet. Run:   export N22TOOLS=<tools folder>;  tools/import.sh   (then ./build.sh again)"
GEN=(); command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
[ -f build/CMakeCache.txt ] || cmake -S . -B build "${GEN[@]}" -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target al -j"${JOBS:-$(nproc)}"
printf '\nDone! Start the game from the top folder with:   ./launch.sh al\n'
printf '  5 = insert coin   Enter = decision / start   Z / X = select   Left / Right (or A / D) = swing the steps   Esc = menu\n\n'
