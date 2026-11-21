#!/usr/bin/env python3
"""liftmips.py PCODE OUTDIR PREFIX [PARTS]

A System 23 game's MIPS R4650 program (and its boot monitor), as Ghidra's RAW p-code (tools/gen/ghidra/DumpPcode.java, language
MIPS:BE:64:64-32addr), to C for the System 23 host's runtime (s23/include/lift_rt.h + s23_mips_rt.h, s23/src/s23_cpu.c): the same
shape as lift68k.py's output, which this reuses. Our own implementation (the original project's MIPS generator is not public).

What MIPS adds to lift68k.py:
  - DELAY SLOTS. Ghidra puts a branch's delay-slot instruction INTO the branch's p-code (so the branch is complete as it stands) and also
    lists it on its own, its text starting with '_'. The listed copy is only reached by a jump to its address; the branch's fall-through
    is its address + 8. A call returns to + 8 too.
  - CP0: mtc0 is Ghidra's setCopReg user-op on a named register -> a register write, then rr_mips_cop0_write (the host's timer and
    interrupt pins); eret -> rr_rte (it picks EPC / ErrorEPC itself).
  - THE FPU: Ghidra's FLOAT_* p-code -> the runtime's FOP2 / FCMP / ... (lift_rt.h: R4650 single precision with its exception flags).
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lift68k as L68
from lift68k import vn

COP0_LO, COP0_HI = 0x2000, 0x2100              # Ghidra's CP0 registers (Count 0x2048, Compare 0x2058, Status 0x2060, Cause 0x2068, EPC 0x2070)

class MipsLifter(L68.Lifter):
    def ret_addr(self, ins):
        return ins.ft if ins.ft is not None else ins.addr + 8

    def special(self, ins):
        mn = ins.text.split()[0] if ins.text else ''
        if mn == 'eret': return ['rr_rte(); return;']
        if mn == 'wait': return ['{ rr_budget = 0; RR_POLL(); }']
        return None

    def wr(self, s, expr):
        c = super().wr(s, expr)
        sp, off, sz = vn(s)
        if sp == 'register' and COP0_LO <= off < COP0_HI:
            c += ' rr_mips_cop0_write(0x%XU, 0x%XU);' % (self.cur_ins, off & ~7)
        return c

    def op(self, ins, k, o):
        name, out, inp = o
        self.cur_ins = ins.addr
        A = ins.addr
        r = self.rd
        osz = vn(out)[2] if out != '-' else 0
        def sz(i): return vn(inp[i])[2]
        fop = {'FLOAT_ADD': '+', 'FLOAT_SUB': '-', 'FLOAT_MULT': '*', 'FLOAT_DIV': '/'}
        fcmp = {'FLOAT_LESS': '<', 'FLOAT_LESSEQUAL': '<=', 'FLOAT_EQUAL': '==', 'FLOAT_NOTEQUAL': '!='}
        if name in fop: return self.wr(out, 'FOP2(%d, %s, %s, %s)' % (osz, r(inp[0]), r(inp[1]), fop[name]))
        if name in fcmp: return self.wr(out, 'FCMP(%d, %s, %s, %s)' % (sz(0), r(inp[0]), r(inp[1]), fcmp[name]))
        if name == 'FLOAT_NEG': return self.wr(out, 'FNEG(%d, %s)' % (osz, r(inp[0])))
        if name == 'FLOAT_ABS': return self.wr(out, 'FABS(%d, %s)' % (osz, r(inp[0])))
        if name == 'FLOAT_SQRT': return self.wr(out, 'FSQRT(%d, %s)' % (osz, r(inp[0])))
        if name == 'FLOAT_NAN': return self.wr(out, 'FNAN(%d, %s)' % (sz(0), r(inp[0])))
        if name in ('INT2FLOAT', 'FLOAT_INT2FLOAT'): return self.wr(out, 'FFROMI(SX%d(%s), %d)' % (sz(0), r(inp[0]), osz))
        if name in ('TRUNC', 'FLOAT_TRUNC'): return self.wr(out, 'FTRUNC(%d, %s, %d)' % (sz(0), r(inp[0]), osz))
        if name in ('FLOAT2FLOAT', 'FLOAT_FLOAT2FLOAT'): return self.wr(out, 'FCONV(%d, %s, %d)' % (sz(0), r(inp[0]), osz))
        if name == 'CALLOTHER':
            u = self.userops.get(vn(inp[0])[1], '?')
            self.note('CALLOTHER %s' % u)
            if u in ('setISAMode', 'SYNC', 'synch', 'hazzard', 'prefetch'): return ';'
            if u in ('trap', 'break'):
                return 'rr_mips_trap(0x%XU, (uint64_t)%s);' % (A, r(inp[1]) if len(inp) > 1 else '0ULL')
            if u == 'cacheOp':
                return 'rr_mips_cacheOp(0x%XU, %s, %s);' % (A, r(inp[1]) if len(inp) > 1 else '0ULL', r(inp[2]) if len(inp) > 2 else '0ULL')
            if u == 'wait': return '{ rr_budget = 0; RR_POLL(); }'
            if u == 'setCopReg' and len(inp) >= 4:
                sp, off, _ = vn(inp[2])
                if sp == 'register':                           # mtc0 rt, <named CP0 register>: the register, then the host hears of it
                    return 'RS8(0x%X, (uint64_t)SX4(%s)); rr_mips_cop0_write(0x%XU, 0x%XU);' % (off, r(inp[3]), A, off)
                return 'rr_mips_setCopReg(0x%XU, %s, %s, %s);' % (A, r(inp[1]), r(inp[2]), r(inp[3]))
            return '{ rr_trap(0x%XU, 0x%XU, "a MIPS instruction the lifter does not translate (%s)"); return; }' % (A, A, u)
        return super().op(ins, k, o)

    def rd(self, s):
        # PIECES OF A WIDER TEMPORARY. Ghidra's unique space is byte-addressed, big-endian: multu / mult write the 64-bit product to
        # unique:X:8 and then read its low half as unique:X+4:4 (and dmult-style code reads other pieces). Read such a piece out of the wide
        # temporary instead of as a variable of its own (which nothing writes: it read 0 -- every 32x32 product's LO and HI were wrong).
        sp, off, sz = vn(s)
        if sp == 'unique' and (off, sz) not in self.uwritten:
            for (b, bs) in self.uwritten:
                if b <= off and off + sz <= b + bs and bs > sz:
                    self.temps.add(b)
                    sh = 8 * (b + bs - off - sz)
                    return '((u%x >> %d) & 0x%XULL)' % (b, sh, (1 << (8 * sz)) - 1)
        return super().rd(s)

    def function(self, f, out):
        self.uwritten = set()
        for i in f.ins:
            for o in i.ops:
                if o[1] != '-':
                    sp, off, sz = vn(o[1])
                    if sp == 'unique': self.uwritten.add((off, sz))
        # a branch's delay slot is counted with the branch (one budget unit each), as the instructions run
        n0 = len(out)
        super().function(f, out)
        ds = {i.addr for i in f.ins if i.text.startswith('_')}
        if not ds: return
        for n in range(n0, len(out)):
            line = out[n]
            if line.startswith('A_') and ': RR_INS(0x' in line:
                a = int(line[2:line.index(':')], 16)
                if a + 4 in ds and not line.split('/*', 1)[1].lstrip().startswith('_'):
                    out[n] = line + '\n  RR_INS(0x%XU); /* delay slot */' % (a + 4)

HEAD = L68.HEAD.replace('tools/gen/lift68k.py', 'tools/gen/liftmips.py').replace('#include "lift_rt.h"', '#include "lift_rt.h"\n#include "s23_mips_rt.h"\nvoid rr_trap(uint32_t at, uint32_t t, const char *why);')

def main():
    L68.HEAD = HEAD
    L68.Lifter = MipsLifter
    L68.main()

if __name__ == '__main__':
    main()
