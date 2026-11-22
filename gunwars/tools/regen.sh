#!/bin/bash
# Gunmen Wars: import (a fresh Ghidra project, ~3 min), translate (tools/gen/liftmips.py) and build. Run it the first time (build.sh does, when
# N22TOOLS is set) and again after adding code addresses to seeds.txt (one hex address a line: a trap's "jump to an address that is no
# known instruction").   export N22TOOLS=<tools folder>;  gunwars/tools/regen.sh
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/tools/env.sh"
bash "$HERE/tools/import.sh" "${1:-$HERE/extracted/roms}"
mkdir -p "$HERE/gen"; rm -f "$HERE"/gen/gunwars_lifted_*.c
( cd "$ROOT" && $PY -I tools/gen/liftmips.py gunwars/gunwars.pcode gunwars/gen gunwars 16 )
NINJA_OR_MAKE   # (no build/ yet on a first import: build.sh configures it)
