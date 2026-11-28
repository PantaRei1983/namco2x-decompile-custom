#!/usr/bin/env python3
"""scan_switches.py EXTRACTED OUT -- the game's SWITCH jump tables Ghidra's analysis may miss (Downhill Bikers 0x8004B73C: seven cases at
0x8017D6A0, none disassembled -- "jump to an address that is no known instruction" at frame 617). The compiler's shape:
    sltiu rC, rI, N          (N cases; or no bound check)
    beq   rC, zero, default
    sll   rT, rI, 2
    lui   rB, HI
    addu  rB, rB, rT
    lw    rX, LO(rB)
    jr    rX
One line per table: the jr's address, then every case address (hex). tools/gen/ghidra/FixSwitches.java adds them to the jr as computed-jump
targets and grows the function over them, so the cases stay INSIDE the function (a jump there is a local goto in the translated C) -- a seed
would make each case a function of its own and split loops across the switch."""
import sys, os, struct
d, out = sys.argv[1], sys.argv[2]
prog = open(os.path.join(d, 'prog.bin'), 'rb').read()
PB = 0x80000000
W = [struct.unpack_from('>I', prog, 4 * i)[0] for i in range(len(prog) // 4)]
def sx(v): return (v ^ 0x8000) - 0x8000
tables = []
for i, w in enumerate(W):
    if w & 0xFC1FFFFF != 0x00000008: continue            # jr rs
    rx = (w >> 21) & 31
    if rx in (26, 27, 31): continue                       # returns / exception returns
    lw = next((j for j in range(i - 1, max(i - 6, 0), -1) if W[j] >> 26 == 0x23 and (W[j] >> 16) & 31 == rx), None)
    if lw is None: continue
    rb, lo = (W[lw] >> 21) & 31, sx(W[lw] & 0xFFFF)
    lui = next((j for j in range(lw - 1, max(lw - 6, 0), -1) if W[j] >> 26 == 0x0F and (W[j] >> 16) & 31 == rb), None)
    if lui is None: continue
    tab = ((W[lui] & 0xFFFF) << 16) + lo & 0xFFFFFFFF
    n = None
    for j in range(lui - 1, max(lui - 12, 0), -1):        # the bound check: sltiu rC, rI, N
        if W[j] >> 26 == 0x0B: n = W[j] & 0xFFFF; break
    if not (PB <= tab < PB + len(prog)): continue
    cases = []
    for k in range(n if n else 512):
        o = tab - PB + 4 * k
        if o + 4 > len(prog): break
        t, = struct.unpack_from('>I', prog, o)
        jr = PB + 4 * i
        if t & 3 or not (jr - 0x20000 <= t < jr + 0x20000): break   # a case lies near its switch
        cases.append(t)
    if cases: tables.append((PB + 4 * i, cases))
with open(out, 'w') as f:
    for jr, cases in tables: f.write('%08X %s\n' % (jr, ' '.join('%08X' % c for c in cases)))
print('scan_switches: %d tables, %d cases -> %s' % (len(tables), sum(len(c) for _, c in tables), out))
