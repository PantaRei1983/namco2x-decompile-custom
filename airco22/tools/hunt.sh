#!/bin/bash
# Air Combat 22: run until the program reaches code that is not translated yet, add that address to seeds.txt, regenerate, again.
#   airco22/tools/hunt.sh [rounds] [frames] [extra ac.exe arguments ...]
# Stops when a run gets through all its frames without a trap, or the same address traps twice.
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROUNDS=${1:-20}; FRAMES=${2:-12000}; shift 2 2>/dev/null
cd "$HERE"
last=
BIN=./build/ac; [ -x ./build/ac.exe ] && BIN=./build/ac.exe          # Windows or Linux
for ((r = 1; r <= ROUNDS; r++)); do
    rm -f build/airco22.log
    "$BIN" "$HERE/extracted" --frames "$FRAMES" --autoplay "$@" > run.out 2>&1
    LOG=run.out; [ -f build/airco22.log ] && LOG=build/airco22.log      # Windows writes the log beside the .exe, Linux to the terminal
    t=$(grep -a -m1 '^\[TRAP\]' "$LOG" | sed -n 's/.*-> 0x\([0-9A-Fa-f]*\):.*/\1/p')
    if [ -z "$t" ]; then echo "round $r: no trap in $FRAMES frames"; grep -a "^\[AC\] frame" "$LOG" | tail -1; exit 0; fi
    echo "round $r: $(grep -a -m1 '^\[TRAP\]' "$LOG")"
    if [ "$t" = "$last" ]; then echo "same address again -- stopping"; exit 1; fi
    last=$t
    echo "$t" >> seeds.txt
    bash tools/regen.sh > regen.out 2>&1 || { echo "regen failed"; tail -5 regen.out; exit 1; }
    grep -E "in all|functions," regen.out | tail -2
done
