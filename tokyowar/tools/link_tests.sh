#!/bin/bash
# Link-play fault tests: 4 headless copies of Tokyo Wars on this PC, linked over 127.0.0.1 (docs/LINK_NOTES.md).
#
#   tools/link_tests.sh [gather|missing|bad|skew|crash|start|all]
#
# Each copy plays the game's --autoplay script. Results: the [LINK] lines of each copy's log, and "down N" = frames
# the GAME itself saw its link down after it first came up (0 is the goal).
set -u
cd "$(dirname "$0")/.."
ROMS="$PWD/extracted"
W="$PWD/soak/linktest"
mkdir -p "$W"
cp build/tw.exe "$W/"
cd "$W"

run() {   # run CAB FRAMES [extra args...] in the background, pid in PID_<cab>
    local cab=$1 frames=$2; shift 2
    ./tw.exe "$ROMS" --frames "$frames" --autoplay --link "$cab" "$@" &
    eval "PID_$cab=$!"
}
summary() {
    for c in 1 2 3 4; do
        [ -f tokyowar_cab$c.log ] || continue
        echo "  cabinet $c: $(grep '\[LINK\] frame [0-9]*  down' tokyowar_cab$c.log | tail -2 | head -1 | sed -E 's/.*frame ([0-9]+)  down ([0-9]+).*lost ([0-9]+).*\| game: cabinet [0-9], hears ([0-9A-F]+), ring of ([0-9]+), link ([A-Za-z]+).*/frame \1, game saw link down for \2 frames, net lost \3, ring of \5, link \6/')"
        grep -h "lost the link" tokyowar_cab$c.log | head -3 | sed 's/^/      /'
    done
}

t_bad() {
    echo "== BAD NETWORK: +5 ms, up to 15 ms jitter, 3% of frames lost, 40 s"
    rm -f *.log
    for c in 1 2 3 4; do run $c 2400 --link-test 5:15:3; done; wait
    summary
}
t_skew() {
    echo "== CLOCK DRIFT: cabinet 2 +0.2%, cabinet 4 -0.2%, 2 minutes"
    rm -f *.log
    run 1 7200; run 2 7200 --link-test 0:0:0:2000; run 3 7200; run 4 7200 --link-test 0:0:0:-2000; wait
    summary
}
t_crash() {
    echo "== CRASH: cabinet 3 killed at 25 s, restarted at 40 s: it must stay out, the others carry on"
    rm -f *.log
    for c in 1 2 3 4; do run $c 5400; done
    sleep 25; kill -9 "$PID_3" 2>/dev/null; echo "  (cabinet 3 killed -- a crash: no goodbye)"
    sleep 15; mv tokyowar_cab3.log tokyowar_cab3_before.log; run 3 1800; echo "  (cabinet 3 restarted)"
    wait
    summary
    grep -h "NOT linked\|linked (" tokyowar_cab3.log | sed 's/^/  restarted cabinet 3: /'
}
t_gather() {
    echo "== STAGGERED POWER-ON: cabinets 1+2 at once, 3 after 5 s, 4 after 10 s: all start together"
    rm -f *.log
    run 1 1800; run 2 1800; sleep 5; run 3 1800; sleep 5; run 4 1800; wait
    for c in 1 2 3 4; do grep -h "linked (" tokyowar_cab$c.log | sed "s/^/  cabinet $c: /"; done
    summary
}
t_missing() {
    echo "== ONE MISSING, THEN LATE: 3 cabinets, 10 s wait; cabinet 4 switched on 20 s later must stay out"
    rm -f *.log
    for c in 1 2 3; do run $c 3600 --link-wait 10; done
    sleep 30; run 4 1200 --link-wait 10; wait
    for c in 1 2 3 4; do grep -h "linked (\|NOT linked" tokyowar_cab$c.log | sed "s/^/  cabinet $c: /"; done
    summary
}

case "${1:-all}" in
    bad) t_bad ;; skew) t_skew ;; crash) t_crash ;; gather) t_gather ;; missing) t_missing ;;
    start) t_gather; t_missing; t_crash ;;
    all) t_gather; t_missing; t_bad; t_skew; t_crash ;;
esac
