#!/bin/bash
# the H8 programs (sub-CPU, I/O board): run, and each "UNTRANSLATED code at X" the log reports is added to cov/h8*_seeds.pcs, rebuild, again.
#   tools/h8hunt.sh [ROUNDS] [INSTRUCTIONS]
HERE="$(cd "$(dirname "$0")/.." && pwd)"; N=${1:-8}; INS=${2:-2000000000}
for r in $(seq 1 $N); do
  ( cd "$HERE" && cmake -B build >/dev/null && ninja -C build gunwars 2>&1 | grep -E 'error|FAILED' )   # (in the MSYS2 MINGW64 shell this runs in: its ninja, wherever MSYS2 is installed)
  ( cd "$HERE/build" && S23_STOP_ON_TRAP=1 timeout 400 ./gunwars.exe reset $INS >/dev/null 2>&1 )
  L="$HERE/build/tc2.log"; new=0
  for w in sub io; do
    tag=$([ $w = sub ] && echo "sub-CPU" || echo "I/O board")
    a=$(grep -oE "\[H8\] $tag: UNTRANSLATED code at [0-9A-F]+" "$L" | grep -oE '[0-9A-F]+$' | head -1)
    if [ -n "$a" ] && ! grep -qx "$a" "$HERE/cov/h8${w}_seeds.pcs"; then echo "$a" >> "$HERE/cov/h8${w}_seeds.pcs"; echo "round $r: $tag $a"; new=1; fi
  done
  grep -E "^\[TC2\] [0-9]+ instructions|\[TRAP\]" "$L" | tail -2
  [ $new = 0 ] && break
done
