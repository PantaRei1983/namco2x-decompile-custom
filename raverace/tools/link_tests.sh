#!/bin/bash
# Link-play tests for Rave Racer: 4 headless copies on this PC, linked over 127.0.0.1 (docs/LINK_NOTES.md).
#
#   tools/link_tests.sh [race|gather|missing|bad|skew|crash|start|all]
#
# Every copy plays tools/link_race.rep (coin, start, choose linked play, drive). Results: the [LINK] lines of each
# copy's log, and "down N" = frames the GAME itself saw its link down after it first came up (0 is the goal).
set -u
cd "$(dirname "$0")/.."
ROMS="$PWD/extracted"
REP="$PWD/tools/link_race.rep"
W="$PWD/soak/link4"
mkdir -p "$W"
cp build/rr.exe "$W/"
cd "$W"

run() {   # run CAB FRAMES [extra args...] in the background, pid in PID_<cab>
    local cab=$1 frames=$2; shift 2
    ./rr.exe "$ROMS" --frames "$frames" --replay "$REP" --link "$cab" --link-cabinets 4 --link-port 26220 "$@" 2>/dev/null &
    eval "PID_$cab=$!"
}
summary() {
    for c in 1 2 3 4; do
        [ -f raveracer_cab$c.log ] || continue
        echo "  cabinet $c: $(grep '\[LINK\] frame [0-9]*  down' raveracer_cab$c.log | tail -1 | sed -E 's/.*frame ([0-9]+)  down ([0-9]+).*game IDs ([0-9: ]+)  chip.*lost ([0-9]+).*\| game: cabinet ([0-9]), hears ([0-9A-F]+) \(([0-9]+) in the ring\), link ([A-Za-z]+).*/frame \1, game ID \5, hears the others: \7 (mask \6), link \8, game saw link down for \2 frames, net lost \4/')"
        grep -h "lost the link\|WARNING" raveracer_cab$c.log | head -3 | sed 's/^/      /'
    done
}

t_race() {
    echo "== 4-CAR RACE: all four switched on together, each plays the race script, 100 s"
    rm -f *.log
    for c in 1 2 3 4; do run $c 6000; done; wait
    for c in 1 2 3 4; do grep -h "linked (\|NOT linked" raveracer_cab$c.log | sed "s/^/  cabinet $c: /"; done
    summary
}
t_bad() {
    echo "== BAD NETWORK: +5 ms, up to 15 ms jitter, 3% of frames lost, 100 s"
    rm -f *.log
    for c in 1 2 3 4; do run $c 6000 --link-test 5:15:3; done; wait
    summary
}
t_skew() {
    echo "== CLOCK DRIFT: cabinet 2 +0.2%, cabinet 4 -0.2%, 2 minutes"
    rm -f *.log
    run 1 7200; run 2 7200 --link-test 0:0:0:2000; run 3 7200; run 4 7200 --link-test 0:0:0:-2000; wait
    summary
}
t_crash() {
    echo "== CRASH: cabinet 3 killed at 50 s (mid-race), restarted at 65 s: it must stay out, the others carry on"
    rm -f *.log
    for c in 1 2 3 4; do run $c 6000; done
    sleep 50; kill -9 "$PID_3" 2>/dev/null; echo "  (cabinet 3 killed -- a crash: no goodbye)"
    sleep 15; mv raveracer_cab3.log raveracer_cab3_before.log; run 3 1800; echo "  (cabinet 3 restarted)"
    wait
    summary
    grep -h "NOT linked\|linked (" raveracer_cab3.log | sed 's/^/  restarted cabinet 3: /'
}
t_gather() {
    echo "== STAGGERED POWER-ON: cabinets 1+2 at once, 3 after 5 s, 4 after 10 s: all start together"
    rm -f *.log
    run 1 1800; run 2 1800; sleep 5; run 3 1800; sleep 5; run 4 1800; wait
    for c in 1 2 3 4; do grep -h "linked (" raveracer_cab$c.log | sed "s/^/  cabinet $c: /"; done
    summary
}
t_missing() {
    echo "== ONE MISSING, THEN LATE: 3 cabinets, 10 s wait; cabinet 4 switched on 20 s later must stay out"
    rm -f *.log
    for c in 1 2 3; do run $c 3600 --link-wait 10; done
    sleep 30; run 4 1200 --link-wait 10; wait
    for c in 1 2 3 4; do grep -h "linked (\|NOT linked" raveracer_cab$c.log | sed "s/^/  cabinet $c: /"; done
    summary
}

case "${1:-all}" in
    race) t_race ;; bad) t_bad ;; skew) t_skew ;; crash) t_crash ;; gather) t_gather ;; missing) t_missing ;;
    start) t_gather; t_missing; t_crash ;;
    all) t_race; t_gather; t_missing; t_bad; t_skew; t_crash ;;
esac
