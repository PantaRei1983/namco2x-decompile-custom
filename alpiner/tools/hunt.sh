#!/bin/bash
# Alpine Racer: run until the program reaches code that is not translated yet, add that address to seeds_<set>.txt, regenerate, again.
#   alpiner/tools/hunt.sh [rounds] [frames] [extra al.exe arguments ...]
# Stops when a run gets through all its frames without a trap, or the same address traps twice.
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROUNDS=${1:-20}; FRAMES=${2:-12000}; shift 2 2>/dev/null
cd "$HERE"
last=
SEEDS="seeds_$(tr -d '\r\n' < extracted/SET).txt"
BIN=./build/al; [ -x ./build/al.exe ] && BIN=./build/al.exe          # Windows or Linux
for ((r = 1; r <= ROUNDS; r++)); do
    rm -f build/alpiner.log
    "$BIN" "$HERE/extracted" --frames "$FRAMES" --autoplay "$@" > run.out 2>&1
    LOG=run.out; [ -f build/alpiner.log ] && LOG=build/alpiner.log      # Windows writes the log beside the .exe, Linux to the terminal
    t=$(grep -a -m1 '^\[TRAP\]' "$LOG" | sed -n 's/.*-> 0x\([0-9A-Fa-f]*\):.*/\1/p')
    if [ -z "$t" ]; then echo "round $r: no trap in $FRAMES frames"; grep -a "^\[AL\] frame" "$LOG" | tail -1; exit 0; fi
    echo "round $r: $(grep -a -m1 '^\[TRAP\]' "$LOG")"
    if [ "$t" = "$last" ]; then echo "same address again -- stopping"; exit 1; fi
    last=$t
    echo "$t" >> "$SEEDS"
    bash tools/regen.sh > regen.out 2>&1 || { echo "regen failed"; tail -5 regen.out; exit 1; }
    grep -E "in all|functions," regen.out | tail -2
done
