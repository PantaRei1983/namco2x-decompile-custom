#!/bin/bash
# Final Furlong: after adding code addresses to seeds.txt (one hex address a line: a trap's "jump to an address that is no known
# instruction"), import again (a fresh Ghidra project, ~3 min), translate (tools/gen/liftmips.py) and build.
#   N22TOOLS=/d/Games/Emulation/n22tools tools/regen.sh
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
bash "$HERE/tools/import.sh" "$HERE/extracted"
rm -f "$HERE"/gen/finfurl_lifted_*.c
( cd "$ROOT" && python -I tools/gen/liftmips.py finfurl/finfurl.pcode finfurl/gen finfurl 16 )
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash.exe -lc "cd '$HERE' && ninja -C build finfurl 2>&1 | grep -E 'error|FAILED|Linking'"
