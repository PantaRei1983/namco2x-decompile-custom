#!/usr/bin/env python3
"""lift68k.py PCODE OUTDIR PREFIX [PARTS [PROGRAM.BIN]]

A 68020 program, as Ghidra's RAW p-code (tools/gen/ghidra/DumpPcode.java), to C for the shared runtime engine/lift_rt.h +
engine/lift_cpu.c -- the same shape as the lifted programs of the other games (gen/<prefix>_lifted_NN.c, _tab.c, .h):

  - one C function per Ghidra function, L_<ENTRY>_at(pc): a switch on pc resumes at any instruction of it;
  - registers live in R[] at Ghidra's register offsets (RG1/2/4, RS1/2/4), memory goes through MRDn / MWRn (rr_read / rr_write);
  - a call is a C call with a shadow-stack entry (rr_call_push / rr_after_call), rts asks rr_return, a jump out of the function goes
    through rr_jump (binary search over every instruction address -> the function that owns it);
  - every instruction spends one unit of rr_budget (RR_INS); calls and backward branches poll the scheduler (RR_POLL).

This is an independent implementation written from that runtime's contract (the original generator is not public).
"""
import sys, os, re

def mask(sz): return (1 << (8 * sz)) - 1

class Ins:
    __slots__ = ('addr', 'len', 'ft', 'text', 'ops')

class Fn:
    __slots__ = ('entry', 'name', 'ins')

def parse(path):
    fns, userops, cur = [], {}, None
    for line in open(path, encoding='utf-8'):
        t = line[0]
        if t == 'P':
            p = line.split()
            cur.ins[-1].ops.append((p[1], p[2], p[3:]))
        elif t == 'I':
            p = line.rstrip('\n').split(' ', 4)
            i = Ins(); i.addr = int(p[1], 16); i.len = int(p[2]); i.ft = None if p[3] == '-' else int(p[3], 16); i.text = p[4]; i.ops = []
            cur.ins.append(i)
        elif t == 'F':
            p = line.split()
            cur = Fn(); cur.entry = int(p[1], 16); cur.name = p[2]; cur.ins = []
            fns.append(cur)
        elif t == 'U':
            p = line.split(); userops[int(p[1])] = p[2]
    return fns, userops

def vn(s):
    sp, off, sz = s.split(':')
    return sp, int(off, 16), int(sz)

class Lifter:
    def __init__(self, fns, userops):
        self.fns, self.userops = fns, userops
        self.owner = {}                      # instruction address -> function index
        self.entries = {}
        for k, f in enumerate(fns):
            self.entries[f.entry] = k
            for i in f.ins: self.owner.setdefault(i.addr, k)
        self.warn = {}
        self.rom = None                      # the program image (for instruction bytes), or None

    def note(self, what): self.warn[what] = self.warn.get(what, 0) + 1

    # ---- varnodes ----
    def rd(self, s):
        sp, off, sz = vn(s)
        if sp == 'const': return '0x%XULL' % (off & mask(min(sz, 8)))
        if sp == 'register':
            if sz in (1, 2, 4, 8): return 'RG%d(0x%X)' % (sz, off)
            self.note('register read of size %d' % sz); return 'RG8(0x%X)' % off
        if sp == 'unique':
            self.temps.add(off)
            return 'u%x' % off if sz >= 8 else '(u%x & 0x%XULL)' % (off, mask(sz))
        if sp == 'ram':
            if sz in (1, 2, 4, 8): return '(uint64_t)MRD%d(0x%XU)' % (sz, off)
        self.note('read of %s' % sp); return '0ULL'

    def wr(self, s, expr):
        sp, off, sz = vn(s)
        if sp == 'register':
            if sz in (1, 2, 4, 8): return 'RS%d(0x%X, %s);' % (sz, off, expr)
            self.note('register write of size %d' % sz); return 'RS8(0x%X, %s);' % (off, expr)
        if sp == 'unique':
            self.temps.add(off)
            return 'u%x = (%s)%s;' % (off, expr, '' if sz >= 8 else ' & 0x%XULL' % mask(sz))
        if sp == 'ram' and sz in (1, 2, 4, 8): return 'MWR%d(0x%XU, %s);' % (sz, off, expr)
        self.note('write of %s' % sp); return '(void)(%s);' % expr

    # ---- control flow ----
    def goto_addr(self, t, at, poll):
        """C for 'continue at 68K address t' from instruction `at` of the current function"""
        p = 'RR_POLL(); ' if poll else ''
        if t in self.cur_addrs: return '{ %sgoto A_%X; }' % (p, t)
        if t in self.entries: return '{ %sL_%X(); return; }' % (p, t)
        return '{ %srr_jump(0x%XU, 0x%XU); return; }' % (p, t, at)

    def after_call(self, at):
        return ('if (rr_after_call(j_)) return; if (rr_ret_to >= 0x%XU && rr_ret_to <= 0x%XU) { pc_ = rr_ret_to; goto resume_; } '
                'rr_jump(rr_ret_to, 0x%XU); return;' % (self.lo, self.hi, at))

    def ret_addr(self, ins):
        return ins.addr + ins.len

    def op(self, ins, k, o):
        """one p-code op -> a C statement"""
        name, out, inp = o
        A = ins.addr
        r = self.rd
        def sz(i): return vn(inp[i])[2]
        osz = vn(out)[2] if out != '-' else 0
        def m(e): return e
        bin_ = {'INT_ADD': '+', 'INT_SUB': '-', 'INT_MULT': '*', 'INT_AND': '&', 'INT_OR': '|', 'INT_XOR': '^'}
        if name == 'COPY': return self.wr(out, r(inp[0]))
        if name in bin_: return self.wr(out, '%s %s %s' % (r(inp[0]), bin_[name], r(inp[1])))
        if name == 'LOAD':
            if osz not in (1, 2, 4, 8): self.note('LOAD of size %d' % osz); return self.wr(out, '0ULL')
            return self.wr(out, '(uint64_t)MRD%d((uint32_t)%s)' % (osz, r(inp[1])))
        if name == 'STORE':
            s = sz(2)
            if s not in (1, 2, 4, 8): self.note('STORE of size %d' % s); return ';'
            return 'MWR%d((uint32_t)%s, %s);' % (s, r(inp[1]), r(inp[2]))
        if name == 'INT_EQUAL': return self.wr(out, '%s == %s' % (r(inp[0]), r(inp[1])))
        if name == 'INT_NOTEQUAL': return self.wr(out, '%s != %s' % (r(inp[0]), r(inp[1])))
        if name == 'INT_LESS': return self.wr(out, '%s < %s' % (r(inp[0]), r(inp[1])))
        if name == 'INT_LESSEQUAL': return self.wr(out, '%s <= %s' % (r(inp[0]), r(inp[1])))
        if name == 'INT_SLESS': return self.wr(out, 'SX%d(%s) < SX%d(%s)' % (sz(0), r(inp[0]), sz(1), r(inp[1])))
        if name == 'INT_SLESSEQUAL': return self.wr(out, 'SX%d(%s) <= SX%d(%s)' % (sz(0), r(inp[0]), sz(1), r(inp[1])))
        if name == 'INT_ZEXT': return self.wr(out, r(inp[0]))
        if name == 'INT_SEXT': return self.wr(out, '(uint64_t)SX%d(%s)' % (sz(0), r(inp[0])))
        if name == 'INT_2COMP': return self.wr(out, '0 - %s' % r(inp[0]))
        if name == 'INT_NEGATE': return self.wr(out, '~%s' % r(inp[0]))
        if name == 'INT_CARRY': return self.wr(out, 'CARRY(%s, %s, %d)' % (r(inp[0]), r(inp[1]), sz(0)))
        if name == 'INT_SCARRY': return self.wr(out, 'SCARRY(%s, %s, %d)' % (r(inp[0]), r(inp[1]), sz(0)))
        if name == 'INT_SBORROW': return self.wr(out, 'SBORROW(%s, %s, %d)' % (r(inp[0]), r(inp[1]), sz(0)))
        if name == 'INT_LEFT': return self.wr(out, 'SHL(%s, %s, %d)' % (r(inp[0]), r(inp[1]), sz(0)))
        if name == 'INT_RIGHT': return self.wr(out, 'SHR(%s, %s, %d)' % (r(inp[0]), r(inp[1]), sz(0)))
        if name == 'INT_SRIGHT': return self.wr(out, 'SAR(%s, %s, %d)' % (r(inp[0]), r(inp[1]), sz(0)))
        if name == 'INT_DIV': return self.wr(out, "UDIVREM(%s, %s, '/')" % (r(inp[0]), r(inp[1])))
        if name == 'INT_REM': return self.wr(out, "UDIVREM(%s, %s, '%%')" % (r(inp[0]), r(inp[1])))
        if name == 'INT_SDIV': return self.wr(out, "(uint64_t)SDIVREM(SX%d(%s), SX%d(%s), '/')" % (sz(0), r(inp[0]), sz(1), r(inp[1])))
        if name == 'INT_SREM': return self.wr(out, "(uint64_t)SDIVREM(SX%d(%s), SX%d(%s), '%%')" % (sz(0), r(inp[0]), sz(1), r(inp[1])))
        if name == 'BOOL_NEGATE': return self.wr(out, '(%s ^ 1) & 1' % r(inp[0]))
        if name == 'BOOL_AND': return self.wr(out, '(%s & %s) & 1' % (r(inp[0]), r(inp[1])))
        if name == 'BOOL_OR': return self.wr(out, '(%s | %s) & 1' % (r(inp[0]), r(inp[1])))
        if name == 'BOOL_XOR': return self.wr(out, '(%s ^ %s) & 1' % (r(inp[0]), r(inp[1])))
        if name == 'SUBPIECE':
            sh = vn(inp[1])[1] * 8
            return self.wr(out, '%s >> %d' % (r(inp[0]), sh) if sh < 64 else '0ULL')
        if name == 'PIECE': return self.wr(out, '(%s << %d) | %s' % (r(inp[0]), sz(1) * 8, r(inp[1])))
        if name == 'POPCOUNT': return self.wr(out, '(uint64_t)__builtin_popcountll(%s)' % r(inp[0]))
        if name == 'LZCOUNT': return self.wr(out, '(%s ? (uint64_t)__builtin_clzll(%s) - %d : %d)' % (r(inp[0]), r(inp[0]), 64 - 8 * sz(0), 8 * sz(0)))
        if name in ('BRANCH', 'CBRANCH'):
            sp, off, _ = vn(inp[0])
            if sp == 'const':                                     # p-code relative: within this instruction
                rel = off - (1 << 32) if off >= (1 << 31) else off
                j = k + rel
                tgt = 'goto P_%X_%d;' % (A, j) if 0 <= j < len(ins.ops) else 'goto A_END_%X;' % A
                tgt = '{ %s }' % tgt
            else:
                tgt = self.goto_addr(off, A, off <= A)
            if name == 'BRANCH': return tgt
            return 'if (%s) %s' % (r(inp[1]), tgt)
        if name == 'CALL':
            t = vn(inp[0])[1]; ret = self.ret_addr(ins)
            call = 'L_%X();' % t if t in self.entries else 'rr_call_ind(0x%XU, 0x%XU);' % (t, A)
            return '{ int j_ = rr_call_push(0x%XU); RR_POLL(); %s %s }' % (ret, call, self.after_call(A))
        if name == 'CALLIND':
            ret = self.ret_addr(ins)
            return '{ uint32_t t_ = (uint32_t)%s; int j_ = rr_call_push(0x%XU); RR_POLL(); rr_call_ind(t_, 0x%XU); %s }' % (r(inp[0]), ret, A, self.after_call(A))
        if name == 'BRANCHIND':
            return ('{ uint32_t t_ = (uint32_t)%s; RR_POLL(); if (t_ >= 0x%XU && t_ <= 0x%XU) { pc_ = t_; goto resume_; } rr_jump(t_, 0x%XU); return; }'
                    % (r(inp[0]), self.lo, self.hi, A))
        if name == 'RETURN':
            return ('{ uint32_t t_ = (uint32_t)%s; if (rr_return(t_)) return; RR_POLL(); if (t_ >= 0x%XU && t_ <= 0x%XU) { pc_ = t_; goto resume_; } '
                    'rr_jump(t_, 0x%XU); return; }' % (r(inp[0]), self.lo, self.hi, A))
        if name == 'RAWC': return inp[0]                           # C written by fix_divide
        if name == 'CALLOTHER':
            u = self.userops.get(vn(inp[0])[1], '?')
            self.note('CALLOTHER %s' % u)
            if u == 'bcdAdjust' and out != '-': return self.wr(out, r(inp[1]))      # (abcd / sbcd / nbcd are emitted whole: see special())
            if u in ('reset', 'invalidateCacheLines', 'pushInvalidateCaches'): return ';'
            return '{ rr_trap(0x%XU, 0x%XU, "an instruction the lifter does not translate (%s)"); return; }' % (A, A, u)
        self.note('p-code op %s' % name)
        return '{ rr_trap(0x%XU, 0x%XU, "p-code op %s is not translated"); return; }' % (A, A, name)

    def bitfield(self, ins):
        """68020 BFEXTU / BFEXTS / BFINS on a MEMORY operand, (d16,An) or (An), with an immediate offset and width (the 68020
        manual: bit 0 of the field is the most significant bit of the byte at the address, the offset counts from there, a width
        of 0 means 32). N = the field's top bit, Z = the field is 0, V = C = 0. Other forms: None (left untranslated)."""
        num = r'(-?0x[0-9a-fA-F]+|\d+)'
        ea = r'(?:\(' + num + r',A(\d)\)|\(A(\d)\))'
        fld = r'\{' + num + ':' + num + r'\}'
        m = re.match(r'(bfextu|bfexts) ' + ea + fld + r',D(\d)$', ins.text)
        if m:
            op, d, an1, an0, off, w, dn = m.groups()
        else:
            m = re.match(r'bfins D(\d),' + ea + fld + r'$', ins.text)
            if not m: return None
            dn, d, an1, an0, off, w = m.groups(); op = 'bfins'
        an = int(an1 if an1 is not None else an0)
        disp = int(d, 0) if d is not None else 0
        off, w = int(off, 0), int(w, 0) or 32
        if not (0 <= off < 32 and 1 <= w <= 32): return None
        nb = ((off & 7) + w + 7) // 8                                  # the bytes the field touches (1..5)
        sh = nb * 8 - (off & 7) - w                                    # the field's lowest bit within them
        mask = (1 << w) - 1
        dreg = int(dn) * 4
        addr = '(uint32_t)(RG4(0x%X) + 0x%XU)' % (0x20 + an * 4, (disp + (off >> 3)) & 0xFFFFFFFF)
        rd = 'uint32_t a_ = %s; uint64_t v_ = 0; for (int i_ = 0; i_ < %d; i_++) v_ = v_ << 8 | MRD1(a_ + (uint32_t)i_);' % (addr, nb)
        flags = 'RS1(0x44, (f_ >> %d) & 1); RS1(0x45, (f_ & 0x%XU) == 0); RS1(0x46, 0); RS1(0x47, 0);' % (w - 1, mask)
        if op == 'bfins':
            return ['{ %s uint32_t f_ = (uint32_t)RG4(0x%X) & 0x%XU; const uint64_t mk_ = (uint64_t)0x%XULL << %d; '
                    'v_ = (v_ & ~mk_) | ((uint64_t)f_ << %d); for (int i_ = 0; i_ < %d; i_++) MWR1(a_ + (uint32_t)i_, (v_ >> (8 * (%d - 1 - i_))) & 0xFF); %s }'
                    % (rd, dreg, mask, mask, sh, sh, nb, nb, flags)]
        sx = (' if (f_ & 0x%XU) f_ |= 0x%XU;' % (1 << (w - 1), (~mask) & 0xFFFFFFFF)) if op == 'bfexts' and w < 32 else ''
        return ['{ %s uint32_t f_ = (uint32_t)((v_ >> %d) & 0x%XULL); %s%s RS4(0x%X, f_); }' % (rd, sh, mask, flags, sx, dreg)]

    def special(self, ins):
        """instructions emitted whole instead of from their p-code; None = use the p-code"""
        mn = ins.text.split()[0] if ins.text else ''
        A = ins.addr
        if mn == 'rte': return ['rr_rte(); return;']
        if mn in ('abcd', 'sbcd'):                                        # Ghidra's bcdAdjust leaves the byte binary (engine/lift_rt.h)
            m = re.match(r'(abcd|sbcd) D(\d)b,D(\d)b$', ins.text)
            if m:
                s, d = int(m.group(2)) * 4 + 3, int(m.group(3)) * 4 + 3
                return ['RS1(0x%X, rr_%s((uint32_t)RG1(0x%X), (uint32_t)RG1(0x%X)));' % (d, m.group(1), d, s)]
            m = re.match(r'(abcd|sbcd) -\(A(\d)\),-\(A(\d)\)$', ins.text)       # the memory form too (Ridge Racer's TIME countdown,
            if m:                                                              # sbcd -(A3),-(A2) at 0x11FCC: it went 50 -> 4F -> 4E ...)
                ys, xs = int(m.group(2)), int(m.group(3))
                ry, rx = 0x20 + ys * 4, 0x20 + xs * 4
                dy, dx = (2 if ys == 7 else 1), (2 if xs == 7 else 1)        # (A7 stays word aligned)
                self.note('%s memory form' % m.group(1))
                return ['RS4(0x%X, RG4(0x%X) - %d); RS4(0x%X, RG4(0x%X) - %d);' % (ry, ry, dy, rx, rx, dx),
                        '{ uint32_t s_ = (uint32_t)MRD1((uint32_t)RG4(0x%X)), d_ = (uint32_t)MRD1((uint32_t)RG4(0x%X)); MWR1((uint32_t)RG4(0x%X), rr_%s(d_, s_)); }'
                        % (ry, rx, rx, m.group(1))]
        if mn == 'stop':                                                  # wait for an interrupt: the scheduler runs until one is taken
            m = re.search(r'#(-?0x[0-9a-fA-F]+|-?\d+)', ins.text)
            sr = int(m.group(1), 0) & 0xFFFF if m else 0x2000
            return ['rr_set_sr(0x%XU);' % sr, '{ rr_budget = 0; RR_POLL(); }']
        if mn.startswith('f') and mn not in ('ffo',) or mn in ('callm', 'rtm', 'cas', 'cas2', 'pack', 'unpk'):
            self.note('untranslated %s' % mn)
            return ['{ rr_trap(0x%XU, 0x%XU, "%s: not a 68EC020 instruction this program runs (data read as code?)"); return; }' % (A, A, mn)]
        if mn in ('bfextu', 'bfexts', 'bfins'):                          # (Aqua Jet) a memory field at a fixed offset and width
            c = self.bitfield(ins)
            if c: self.note('bit-field %s' % mn); return c
        if mn.startswith('bf'):
            self.note('bit-field %s (untranslated)' % mn)
            return ['{ rr_trap(0x%XU, 0x%XU, "bit-field instruction not translated yet"); return; }' % (A, A)]
        if re.match(r'move(\.w)? SR,', ins.text):
            self.note('move from SR')
        return None

    def fix_preindexed(self, ins):
        """GHIDRA'S 68020 MEMORY-INDIRECT IS WRONG FOR THE PRE-INDEXED FORM. ([bd,An,Xn]) / ([bd,PC,Xn]) -- full extension word,
        I/IS = 1..3 with the index not suppressed -- means mem[bd + base + Xn]; Ghidra's p-code loads mem[bd + base] and THEN adds
        Xn (the post-indexed form), and prints it "([bd,PC],Xn)". A game's dispatch tables are exactly this instruction
        (jsr ([table,PC],D0.w*4)), so every table call past entry 0 went to table[0] + 4n. Rewrite: index first, then load.
        AND THE OTHER WAY ROUND (Alpine Racer 2): the POST-indexed form, I/IS = 5..7, ([bd,An],Xn) = mem[bd + An] + Xn, gets the
        pre-indexed p-code and is printed "([bd,An,Xn])". Rewrite: load, then index. Which form the p-code has is read from the
        p-code itself (does the address of the first load use the index?), so a form Ghidra gets right is left alone.
        The extension word follows the opcode, or an immediate source (cmpi.w #n,([...]) and the like)."""
        if self.rom is None or '([' not in ins.text or ins.len < 4: return
        mn = ins.text.split()[0]
        at = 2
        if mn.split('.')[0] in ('ori', 'andi', 'subi', 'addi', 'eori', 'cmpi') and '#' in ins.text.split(None, 1)[1].split(',')[0]:
            at = 6 if mn.endswith('.l') else 4
        nmi = ins.text.count('([')
        if nmi not in (1, 2) or ins.len < at + 2: self.note('memory indirect: not checked (%s at %X)' % (mn, ins.addr)); return
        exts = [int.from_bytes(self.rom[ins.addr + at:ins.addr + at + 2], 'big')]
        if nmi == 2:                                                   # move ([..]),([..]) (Aqua Jet): the source's extension block, then the destination's
            e = exts[0]
            if not (e & 0x100): self.note('memory indirect: not checked (%s at %X)' % (mn, ins.addr)); return
            ln = 2 + {0: 0, 1: 0, 2: 2, 3: 4}[(e >> 4) & 3] + {0: 0, 1: 0, 2: 2, 3: 4}[e & 3]
            exts.append(int.from_bytes(self.rom[ins.addr + at + ln:ins.addr + at + ln + 2], 'big'))
        ops = ins.ops
        indexed = [i for i, e in enumerate(exts) if (e & 0x100) and not (e & 0x40) and (e & 7) in (1, 2, 3, 5, 6, 7)]
        if not indexed: return                                         # brief format / index suppressed / no memory indirection: no difference
        muls = [n for n, o in enumerate(ops) if o[0] == 'INT_MULT']
        # which INT_MULT is which operand's index: the operands' p-code comes in order (source, then destination), and another
        # operand can carry an index of its own ((d8,An,Xn): move.b ([..]),(1,A0,D2)). One memory-indirect operand: the first
        # multiply when it is the source, the last when it is the destination.
        if nmi == 2: picks = [muls[k] for k in range(len(indexed))] if len(muls) == len(indexed) else None
        else:
            body = ins.text.split(None, 1)[1] if ' ' in ins.text else ''
            depth = 0; comma = len(body)
            for ci, ch in enumerate(body):
                if ch in '([': depth += 1
                elif ch in ')]': depth -= 1
                elif ch == ',' and depth == 0: comma = ci; break
            is_src = body.find('([') < comma
            picks = ([muls[0]] if is_src else [muls[-1]]) if muls else None
        if not picks: self.note('memory indirect: not checked (%s at %X)' % (mn, ins.addr)); return
        out = ops[:picks[0]]
        for j, i in enumerate(indexed):
            end = picks[j + 1] if j + 1 < len(picks) else next((m for m in muls if m > picks[j]), len(ops))
            seg = ops[picks[j]:end]
            fixed = self.fix_mi_seg(seg, exts[i], j, mn, ins.addr)
            out += fixed if fixed is not None else seg
            if j + 1 == len(picks): out += ops[end:]
        ins.ops = out

    def fix_mi_seg(self, ops, ext, i, mn, addr):
        """one memory-indirect operand's p-code (from its index's INT_MULT on): the form Ghidra built, put right; None = unchanged"""
        if not (ext & 0x100) or (ext & 0x40): return None              # brief format / index suppressed: no difference
        iis = ext & 7
        if iis not in (1, 2, 3, 5, 6, 7): return None
        k = next((n for n, o in enumerate(ops) if o[0] == 'LOAD'), None)
        if k is None or ops[0][0] != 'INT_MULT': self.note('memory indirect: not checked (%s at %X)' % (mn, addr)); return None
        idx = ops[0][1]
        pcode_pre = any(idx in ops[n][2] for n in range(1, k))
        o = ops[k]
        if iis <= 3 and not pcode_pre:                                 # wanted pre-indexed, p-code post-indexed
            for m in range(k + 1, len(ops)):
                if ops[m][0] == 'INT_ADD' and o[1] in ops[m][2]:
                    other = [v for v in ops[m][2] if v != o[1]]
                    if len(other) != 1: return None
                    tmp = 'unique:fffff%d:4' % (0 + 8 * i)
                    self.note('pre-indexed memory indirect corrected')
                    return (ops[:k] + [('INT_ADD', tmp, [o[2][1], other[0]]), ('LOAD', o[1], [o[2][0], tmp])] + ops[k + 1:m]
                            + [('COPY', ops[m][1], [o[1]])] + ops[m + 1:])
            return None
        if iis >= 5 and pcode_pre:                                     # wanted post-indexed, p-code pre-indexed
            j = next((n for n in range(1, k) if ops[n][0] == 'INT_ADD' and idx in ops[n][2]), None)
            if j is None: self.note('memory indirect: not checked (%s at %X)' % (mn, addr)); return None
            other = [v for v in ops[j][2] if v != idx]
            if len(other) != 1: return None
            tmp = 'unique:fffff%d:4' % (4 + 8 * i)
            self.note('post-indexed memory indirect corrected')
            return (ops[:j] + [('COPY', ops[j][1], [other[0]])] + ops[j + 1:k]
                    + [('LOAD', tmp, o[2]), ('INT_ADD', o[1], [tmp, idx])] + ops[k + 1:])
        return None

    def fix_divide(self, ins):
        """GHIDRA'S 68020 DIVIDES ARE WRONG IN TWO WAYS. (1) divs.l <ea>,Dr:Dq with the 64-bit dividend builds it as
        (sext(Dr) << 32) | SEXT(Dq): whenever Dq's top bit is set, its sign bits wipe out Dr (the unsigned form zero-extends,
        correctly). (2) No form handles overflow: the CPU leaves the registers as they were and sets V when the quotient does not
        fit (16 bits for divs.w / divu.w, 32 for the long forms) -- Ghidra writes the truncated quotient. A game's fixed-point
        a * b / c (muls.l to 64 bits, then divs.l) is exactly (1); a racing game's physics is full of it.
        Rewrite every divide from the 68020 manual: the p-code is kept up to the divisor's value (its effective address), then C."""
        mn = ins.text.split()[0] if ins.text else ''
        if not mn.startswith(('divs', 'divu')) or self.rom is None: return
        ops = ins.ops
        k = next((n for n, o in enumerate(ops) if o[0] in ('INT_SDIV', 'INT_DIV', 'INT_SREM', 'INT_REM')), None)
        if k is None: return
        op = int.from_bytes(self.rom[ins.addr:ins.addr + 2], 'big')
        A = ins.addr
        flags_ok = 'RS1(0x44, (q_ >> %d) & 1); RS1(0x45, q_ == 0); RS1(0x46, 0); RS1(0x47, 0);'
        ovf = 'RS1(0x46, 1); RS1(0x47, 0);'
        zero = 'rr_trap(0x%XU, 0x%XU, "divide by zero (the CPU would take exception 5)"); return;' % (A, A)
        if (op & 0xFFC0) == 0x4C40:                                    # divs.l / divu.l / divsl.l / divul.l
            ext = int.from_bytes(self.rom[A + 2:A + 4], 'big')
            dq, dr, sgn, big = (ext >> 12) & 7, ext & 7, (ext >> 11) & 1, (ext >> 10) & 1
            if sgn:
                num = '(int64_t)(((uint64_t)RG4(0x%X) << 32) | RG4(0x%X))' % (dr * 4, dq * 4) if big else '(int64_t)(int32_t)RG4(0x%X)' % (dq * 4)
                c = ('{ int64_t n_ = %s; int64_t d_ = (int32_t)RG4(0x300); if (d_ == 0) { %s } '
                     'if (n_ == INT64_MIN && d_ == -1) { %s } else { int64_t q_ = n_ / d_, r_ = n_ %% d_; '
                     'if (q_ < INT32_MIN || q_ > INT32_MAX) { %s } else { %sRS4(0x%X, (uint32_t)q_); %s } } }'
                     % (num, zero, ovf, ovf, 'RS4(0x%X, (uint32_t)r_); ' % (dr * 4) if dr != dq else '', dq * 4, flags_ok % 31))
            else:
                num = '(((uint64_t)RG4(0x%X) << 32) | RG4(0x%X))' % (dr * 4, dq * 4) if big else '(uint64_t)RG4(0x%X)' % (dq * 4)
                c = ('{ uint64_t n_ = %s; uint64_t d_ = RG4(0x300); if (d_ == 0) { %s } uint64_t q_ = n_ / d_, r_ = n_ %% d_; '
                     'if (q_ > 0xFFFFFFFFULL) { %s } else { %sRS4(0x%X, (uint32_t)q_); %s } }'
                     % (num, zero, ovf, 'RS4(0x%X, (uint32_t)r_); ' % (dr * 4) if dr != dq else '', dq * 4, flags_ok % 31))
        elif (op & 0xF0C0) in (0x81C0, 0x80C0):                        # divs.w / divu.w <ea>,Dn: 32 / 16 -> rem:quot
            dn = (op >> 9) & 7
            dv = self.rd(ops[k][2][1])
            if op & 0x0100:
                c = ('{ int64_t n_ = (int32_t)RG4(0x%X); int64_t d_ = (int16_t)(%s); if (d_ == 0) { %s } int64_t q_ = n_ / d_, r_ = n_ %% d_; '
                     'if (q_ < -32768 || q_ > 32767) { %s } else { RS4(0x%X, ((uint32_t)(uint16_t)r_ << 16) | (uint16_t)q_); %s } }'
                     % (dn * 4, dv, zero, ovf, dn * 4, flags_ok % 15))
            else:
                c = ('{ uint64_t n_ = RG4(0x%X); uint64_t d_ = (uint16_t)(%s); if (d_ == 0) { %s } uint64_t q_ = n_ / d_, r_ = n_ %% d_; '
                     'if (q_ > 0xFFFF) { %s } else { RS4(0x%X, ((uint32_t)r_ << 16) | (uint32_t)q_); %s } }'
                     % (dn * 4, dv, zero, ovf, dn * 4, flags_ok % 15))
        else:
            return
        ins.ops = ops[:k] + [('RAWC', '-', [c])]
        self.note('divide rewritten (%s)' % mn)

    def fix_cmp2(self, ins):
        """GHIDRA'S CMP2 / CHK2 ARE WRONG IN THREE WAYS. (1) Every addressing mode loads a word or long FROM the effective address and
        uses it as a pointer to the bounds -- the bounds are AT the effective address (lower, then upper). (2) A data register is compared
        whole; the CPU compares only its low byte / word (sign-extended), the whole register only for .l or an address register.
        (3) C (out of bounds) is a signed <= pair; the 68020 checks the range modulo the size, so an unsigned or wrapping pair works too.
        (4) A PC-relative bound pair is read 2 bytes early (the PC base is taken before the extension word).
        Ridge Racer's course culling (the visible blocks of road and ground: cmp2.w (d16,PC),Dn) drew no road with (1).
        Rewritten from the 68020 manual (the fix the first Ridge Racer 2 lift had): the p-code is kept up to the effective address."""
        mn = ins.text.split()[0] if ins.text else ''
        if not mn.startswith(('cmp2', 'chk2')) or self.rom is None: return
        ops = ins.ops
        k = next((n for n, o in enumerate(ops) if o[0] == 'LOAD'), None)
        if k is None: return
        A = ins.addr
        op = int.from_bytes(self.rom[A:A + 2], 'big')
        ext = int.from_bytes(self.rom[A + 2:A + 4], 'big')
        if (op & 0xF9C0) != 0x00C0: return
        sz = {0: 1, 1: 2, 2: 4}.get((op >> 9) & 3)
        if sz is None: return
        an, rn, chk = (ext >> 15) & 1, (ext >> 12) & 7, (ext >> 11) & 1
        ea = self.rd(ops[k][2][1])
        if (op & 0x3F) in (0x3A, 0x3B):                                # (d16,PC) / (d8,PC,Xn): Ghidra takes PC = the extension word's
            ea = '(%s) + 2' % ea                                       # address; the displacement follows it, so the base is 2 further
        sx = {1: '(int64_t)(int8_t)', 2: '(int64_t)(int16_t)', 4: '(int64_t)(int32_t)'}[sz]
        reg = 'RG4(0x%X)' % ((0x20 if an else 0) + rn * 4)
        x = '(int64_t)(int32_t)%s' % reg if an else '%s(%s)' % (sx, reg)
        msk = '0xFFFFFFFFULL' if an or sz == 4 else ('0xFFFFULL' if sz == 2 else '0xFFULL')
        c = ('{ uint32_t ea_ = (uint32_t)(%s); int64_t lo_ = %s(MRD%d(ea_)), hi_ = %s(MRD%d(ea_ + %d)), x_ = %s; '
             'RS1(0x45, x_ == lo_ || x_ == hi_); RS1(0x47, ((uint64_t)(x_ - lo_) & %s) > ((uint64_t)(hi_ - lo_) & %s));'
             % (ea, sx, sz, sx, sz, sz, x, msk, msk))
        if chk:
            c += ' if (RG1(0x47)) { rr_trap(0x%XU, 0x%XU, "chk2 out of bounds (the CPU would take exception 6)"); return; }' % (A, A)
        c += ' }'
        ins.ops = ops[:k] + [('RAWC', '-', [c])]
        self.note('%s rewritten' % mn.split('.')[0])

    def fix_xflag(self, ins):
        """GHIDRA'S X FLAG: CMP / CMPI / CMPA / CMPM leave X unchanged on a 68000 (Ghidra writes X = C), and ADDX / SUBX / NEGX set
        X = C (Ghidra leaves it). A BCD timer that runs  moveq #0,D4 / addx.w D4,D4 / cmpi.w #$60,D2 / abcd D4b,D3b  then adds one
        unit too many: Ridge Racer's and Ridge Racer 2's lap time gained a minute (the first Ridge Racer 2 lift was fixed after the fact,
        fix_lifted_x_flag.py; this does it in the translator)."""
        mn = (ins.text.split()[0] if ins.text else '').split('.')[0]
        if mn in ('cmp', 'cmpi', 'cmpa', 'cmpm'):
            n = len(ins.ops)
            ins.ops = [o for o in ins.ops if not (o[1].startswith('register:43:'))]
            if len(ins.ops) != n: self.note('X flag kept by %s' % mn)
        elif mn in ('addx', 'subx', 'negx'):
            if not any(o[1].startswith('register:43:') for o in ins.ops):
                ins.ops = ins.ops + [('RAWC', '-', ['RS1(0x43, RG1(0x47));'])]
                self.note('X flag set by %s' % mn)

    def function(self, f, out):
        ins = f.ins
        for i in ins: self.fix_preindexed(i)
        for i in ins: self.fix_divide(i)
        for i in ins: self.fix_cmp2(i)
        for i in ins: self.fix_xflag(i)
        self.cur_addrs = {i.addr for i in ins}
        self.lo, self.hi = min(self.cur_addrs), max(self.cur_addrs)
        self.temps = set()
        body = []
        for n, i in enumerate(ins):
            body.append('A_%X: RR_INS(0x%XU); /* %s */' % (i.addr, i.addr, i.text.replace('*/', '* /')))
            sp = self.special(i)
            terminal = False
            if sp is not None:
                for k, st in enumerate(sp): body.append('  P_%X_%d: %s' % (i.addr, k, st))
                terminal = sp[-1].rstrip().endswith('return;') or sp[-1].rstrip().endswith('return; }')
            else:
                for k, o in enumerate(i.ops):
                    body.append('  P_%X_%d: %s' % (i.addr, k, self.op(i, k, o)))
            body.append('  A_END_%X: ;' % i.addr)
            nxt = ins[n + 1].addr if n + 1 < len(ins) else None
            if i.ft is not None and not terminal and i.ft != nxt:      # the next instruction in memory is not the next one here
                body.append('  ' + self.goto_addr(i.ft, i.addr, False))
        out.append('/* ---- %s @ 0x%06X ---- */' % (f.name, f.entry))
        out.append('void L_%X_at(uint32_t pc_)' % f.entry)
        out.append('{')
        for t in sorted(self.temps): out.append('  uint64_t u%x = 0;' % t)
        out.append('resume_:')
        out.append('  switch (pc_) {')
        for i in ins: out.append('  case 0x%XU: goto A_%X;' % (i.addr, i.addr))
        out.append('  default: rr_jump(pc_, pc_); return;   /* another function owns it */')
        out.append('  }')
        out += body
        out.append('  rr_trap(0x%XU, 0x%XU, "ran off the end of a lifted function"); return;' % (self.hi, self.hi))
        out.append('}')
        out.append('')

HEAD = '''/* GENERATED by tools/gen/lift68k.py from Ghidra raw p-code -- do not edit.
 * One C function per Ghidra function; registers in R[], memory via rr_read / rr_write. */
#include "lift_rt.h"
#include "%s_lifted.h"
void rr_jump(uint32_t t, uint32_t at);
#ifdef LIFT_LAST_PC                 /* the instruction last started, for a host that wants to say where the program is */
extern uint32_t g_lift_pc;
extern uint8_t g_lift_cov[];        /* one byte per even address: this instruction has run */
#undef RR_INS
#ifdef RR_TRACE                     /* (the trace build: also the per-instruction hook) */
#define RR_INS(a) (g_lift_cov[(a) >> 1] = 1, g_lift_pc = (a), rr_trace_ins(a), --rr_budget)
#else
#define RR_INS(a) (g_lift_cov[(a) >> 1] = 1, g_lift_pc = (a), --rr_budget)
#endif
#endif
'''

def supplement_rename(text, supp):
    """SUPPLEMENT mode: our functions get their own names (XL_...), and our table its own lookup, so they link beside a game's own
    lifted program (which owns L_..., rr_jump, rr_call_ind)"""
    text = re.sub(r'\bL_([0-9A-F]+)', r'XL_\1', text)
    return text.replace('void rr_jump(uint32_t t, uint32_t at)\n{', 'int %s(uint32_t t, uint32_t at)\n{' % supp)

def main():
    pcode, outdir, prefix = sys.argv[1:4]
    parts = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    fns, userops = parse(pcode)
    fns = [f for f in fns if f.ins]
    # SUPPLEMENT mode (LIFT_SUPPLEMENT=<file of instruction addresses the game's own lifted program already has>, LIFT_SUPP_NAME=<the
    # lookup function's name>): only the functions that program lacks (code it never found: Ace Driver's run from a copy in RAM). Every
    # call or jump outside them goes through the game's own rr_jump / rr_call_ind; the host calls <name>(target, at) when that program
    # has no instruction there (it returns 0 when we have none either).
    supp = os.environ.get('LIFT_SUPP_NAME')
    if os.environ.get('LIFT_SUPPLEMENT'):
        known = {int(x, 16) for x in open(os.environ['LIFT_SUPPLEMENT']).read().split()}
        n0 = len(fns); fns = [f for f in fns if f.entry not in known]
        print('supplement: %d of %d functions are new' % (len(fns), n0))
    L = Lifter(fns, userops)
    if len(sys.argv) > 5: L.rom = open(sys.argv[5], 'rb').read()
    os.makedirs(outdir, exist_ok=True)
    # the header
    with open(os.path.join(outdir, prefix + '_lifted.h'), 'w', newline='\n') as h:
        h.write('/* GENERATED by tools/gen/lift68k.py -- do not edit */\n#ifndef %s_LIFTED_H\n#define %s_LIFTED_H\n#include <stdint.h>\n' % (prefix.upper(), prefix.upper()))
        for f in fns:
            h.write('void L_%X_at(uint32_t pc);   /* %s */\nstatic inline void L_%X(void) { L_%X_at(0x%XU); }\n' % (f.entry, f.name, f.entry, f.entry, f.entry))
        h.write('#endif\n')
    # the functions, in PARTS files of about equal size
    chunks = [[] for _ in range(parts)]
    total = sum(len(f.ins) for f in fns); acc = 0
    for f in fns:
        out = chunks[min(parts - 1, acc * parts // max(total, 1))]
        L.function(f, out)
        acc += len(f.ins)
    for n, c in enumerate(chunks):
        with open(os.path.join(outdir, '%s_lifted_%02d.c' % (prefix, n)), 'w', newline='\n') as o:
            o.write(HEAD % prefix)
            o.write('\n'.join(c)); o.write('\n')
    # the table: every instruction address -> its function
    addrs = sorted(L.owner)
    with open(os.path.join(outdir, prefix + '_lifted_tab.c'), 'w', newline='\n') as t:
        t.write(HEAD % prefix)
        t.write('static void (*const rr_fnp_[])(uint32_t) = {\n')
        for f in fns: t.write('  L_%X_at,\n' % f.entry)
        t.write('};\nstatic const uint32_t rr_ia_[] = {\n')
        for i in range(0, len(addrs), 8): t.write('  ' + ' '.join('0x%XU,' % a for a in addrs[i:i + 8]) + '\n')
        t.write('};\nstatic const uint16_t rr_if_[] = {\n')
        for i in range(0, len(addrs), 16): t.write('  ' + ' '.join('%d,' % L.owner[a] for a in addrs[i:i + 16]) + '\n')
        t.write('''};
void rr_jump(uint32_t t, uint32_t at)
{
  int lo = 0, hi = (int)(sizeof rr_ia_ / sizeof rr_ia_[0]) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (t < rr_ia_[mid]) hi = mid - 1;
    else if (t > rr_ia_[mid]) lo = mid + 1;
    else { rr_fnp_[rr_if_[mid]](t); return; }
  }
  rr_trap(at, t, "jump to an address that is no known instruction");
}
void rr_call_ind(uint32_t t, uint32_t at) { rr_jump(t, at); }
void (*rd_lifted_entry(uint32_t ep))(uint32_t)
{
  int lo = 0, hi = (int)(sizeof rr_ia_ / sizeof rr_ia_[0]) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (ep < rr_ia_[mid]) hi = mid - 1;
    else if (ep > rr_ia_[mid]) lo = mid + 1;
    else return rr_fnp_[rr_if_[mid]];
  }
  return 0;
}
''')
    if supp:                                    # SUPPLEMENT mode: our own names, and our table answers <supp>() instead of rr_jump
        files = [os.path.join(outdir, prefix + '_lifted.h'), os.path.join(outdir, prefix + '_lifted_tab.c')]
        files += [os.path.join(outdir, '%s_lifted_%02d.c' % (prefix, n)) for n in range(parts)]
        for p in files:
            s = open(p, encoding='utf-8').read()
            if p.endswith('_tab.c'):
                s = s[:s.index('void rr_jump(uint32_t t, uint32_t at)\n{')] + '''int %s(uint32_t t, uint32_t at)   /* 1: ours, run; 0: not ours either */
{
  (void)at;
  int lo = 0, hi = (int)(sizeof rr_ia_ / sizeof rr_ia_[0]) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (t < rr_ia_[mid]) hi = mid - 1;
    else if (t > rr_ia_[mid]) lo = mid + 1;
    else { rr_fnp_[rr_if_[mid]](t); return 1; }
  }
  return 0;
}
''' % supp
            open(p, 'w', encoding='utf-8', newline='\n').write(supplement_rename(s, supp))
    print('%d functions, %d instructions -> %s/%s_lifted_*.c' % (len(fns), len(addrs), outdir, prefix))
    for w, n in sorted(L.warn.items(), key=lambda x: -x[1]): print('  note: %s x%d' % (w, n))

if __name__ == '__main__':
    main()
