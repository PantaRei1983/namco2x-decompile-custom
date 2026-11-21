#!/usr/bin/env python3
"""fix_cmp2b.py [gen/rr_lifted.c] -- Ridge Racer 2's four cmp2.b, rewritten in the lifted C (in place; a second run changes nothing).

The first Ridge Racer 2 lift had Ghidra's CMP2 corrected for cmp2.w only (the "cmp2: no pointer load" blocks). The byte forms were
left as Ghidra wrote them: the byte AT the effective address taken as a POINTER to the bounds, and the whole 32-bit register compared
as a signed pair. All four are the test menu's "value + wheel step, wrap at the ends" (move.b (A1),D1 / add.b D0,D1 / cmp2.b (A0),D1 /
bcc / move.b D2,D1): with the garbage bounds every step was "out of range", so the value jumped to one end and back -- GAME OPTIONS and
COIN OPTIONS flickered between two values and never moved (spacestate1/namco22-decompile issue #101).
Rewritten from the 68020 manual, as ../../tools/gen/lift68k.py fix_cmp2 does for every lift it makes: the bounds AT <ea> (lower, then
upper), the low byte of the data register sign-extended, Z = equal to either bound, C = out of bounds modulo 256."""
import re, sys

REG = {'D%d' % n: 4 * n + 3 for n in range(8)}          # the low byte of Dn in the lifted register file (big-endian, 4 bytes each)

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'gen/rr_lifted.c'
    L = open(path).read().split('\n')
    n_fix = 0
    for i, l in enumerate(L):
        m = re.match(r'A_([0-9A-F]+): RR_INS\(0x[0-9A-F]+U\); /\* cmp2\.b .*,(D[0-7]) \*/', l)
        if not m: continue
        end = next(j for j in range(i + 1, len(L)) if L[j].strip().startswith('A_END_'))
        if any('cmp2.b: the bounds AT <ea>' in L[j] for j in range(i + 1, end)): continue
        k = next(j for j in range(i + 1, end) if 'MRD1(' in L[j])
        ea = re.search(r'MRD1\(\(uint32_t\)(.*)\)\) & 0xFFULL;', L[k]).group(1)
        lab = re.match(r'\s*(P_[0-9A-F]+_\d+):', L[k]).group(1)
        r = REG[m.group(2)]
        L[k:end] = ['  %s: { uint32_t ea_ = (uint32_t)(%s); int64_t lo_ = (int8_t)MRD1(ea_), hi_ = (int8_t)MRD1(ea_ + 1), x_ = (int8_t)RG1(0x%X); '
                    'RS1(0x45, x_ == lo_ || x_ == hi_); RS1(0x47, ((uint64_t)(x_ - lo_) & 0xFFULL) > ((uint64_t)(hi_ - lo_) & 0xFFULL)); }'
                    '   /* cmp2.b: the bounds AT <ea>, the low byte of %s, C = out of bounds modulo 256 (tools/gen/fix_cmp2b.py) */'
                    % (lab, ea, r, m.group(2))]
        n_fix += 1
        print('cmp2.b at 0x%s rewritten' % m.group(1))
    open(path, 'w').write('\n'.join(L))
    print('%d rewritten' % n_fix)

if __name__ == '__main__':
    main()
