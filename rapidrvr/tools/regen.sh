#!/bin/bash
# Rapid River: after adding code addresses to seeds.txt (one hex address a line: a trap's "jump to an address that is no known
# instruction"), import again (a fresh Ghidra project, ~3 min), translate (tools/gen/liftmips.py) and build.
#   N22TOOLS=/d/Games/Emulation/n22tools tools/regen.sh
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
bash "$HERE/tools/import.sh" "$HERE/extracted"
rm -f "$HERE"/gen/rapidrvr_lifted_*.c
( cd "$ROOT" && python -I tools/gen/liftmips.py rapidrvr/rapidrvr.pcode rapidrvr/gen rapidrvr 16 )
if [ -f "$HERE/build/build.ninja" ]; then ( cd "$HERE" && ninja -C build rapidrvr 2>&1 | grep -E 'error|FAILED|Linking' ); fi   # (no build/ yet on a first import: configure it with cmake, then ninja; MINGW64's own ninja, wherever MSYS2 is installed)
