#!/bin/bash
# Build Final Furlong (System 23 / GORgON host in ../s23) on Linux -- or Windows (MSYS2 MINGW64).
#
#   ./build.sh /path/to/finfurl.zip /path/to/namco_asca1.zip     first time: the ROMs into roms/ (and extracted/)
#   ./build.sh                                           later: just rebuild
#
# The game program is translated from YOUR ROM with Ghidra: gen/finfurl_lifted_*.c is not in the repository. With N22TOOLS set (the folder
# with ghidra_12.1.4_PUBLIC -- plus jdk-21.0.12.1+1 on Windows; Linux uses the system's Java 21 -- and an empty work/), this script runs it
# (tools/regen.sh, ~3-5 minutes) the first time. The two H8 programs (sub-CPU, I/O board) are translated at build time from the ROM.
# Needs gcc, cmake, pkg-config, python3 and the SDL2 / zlib / OpenGL development files (../install-deps.sh). JOBS=n limits the parallel
# compiles (the translated program is large: each part needs about 1 GB of gcc).
set -e
ARGS=(); for a in "$@"; do ARGS+=("$(realpath -m -- "$a")"); done; set -- "${ARGS[@]}"
cd "$(dirname "$0")"
fail() { printf '\n*** %s\n\n' "$*" >&2; exit 1; }
for t in gcc cmake pkg-config; do command -v "$t" >/dev/null 2>&1 || fail "'$t' is not installed. Run ../install-deps.sh first."; done
command -v python3 >/dev/null 2>&1 || fail "'python3' is not installed. Run ../install-deps.sh first."
pkg-config --exists sdl2 || fail "SDL2 development files are missing. Run ../install-deps.sh first."
mkdir -p extracted/roms roms
# The zips go into roms/: the game unpacks its chips from there on its first run, matching them by size and CRC (MAME has renamed some of them
# between versions). Their files also go into extracted/roms/, for the program images the translator and the build read (tools/make_prog.py).
for z in "$@"; do
    if [ -d "$z" ]; then cp -n "$z"/* extracted/roms/ 2>/dev/null || true; cp -n "$z"/*.zip roms/ 2>/dev/null || true
    else python3 -I -c 'import sys, zipfile, os
z = zipfile.ZipFile(sys.argv[1])
for i in z.infolist():
    n = os.path.basename(i.filename)
    if n: open(os.path.join("extracted", "roms", n), "wb").write(z.read(i))' "$z" || fail "cannot read $z"
         cp -f "$z" roms/
    fi
done
[ -f extracted/roms/ff2vera.ic2 ] || fail "No ROMs yet. Run:   ./build.sh /path/to/finfurl.zip /path/to/namco_asca1.zip"
python3 -I tools/make_prog.py extracted/roms extracted >/dev/null
if ! ls gen/finfurl_lifted_*.c >/dev/null 2>&1; then
    [ -n "${N22TOOLS:-}" ] || fail "The program is not translated yet. Set N22TOOLS to your Ghidra tools folder and run ./build.sh again
    (or:  export N22TOOLS=<tools folder>;  tools/regen.sh)"
    bash tools/regen.sh extracted/roms
fi
GEN=(); command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
[ -f build/CMakeCache.txt ] || cmake -S . -B build "${GEN[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target finfurl -j"${JOBS:-$(nproc)}"
printf '\nDone! Start the game from the top folder with:   ./launch.sh finfurl\n\n'
