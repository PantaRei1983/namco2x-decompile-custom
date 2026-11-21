"""prof.py -- read a profile.txt written by $S23_PROFILE=1 (s23/src/s23_prof.c) against the UNSTRIPPED build's symbols.
   python tools/prof.py build/gunwars.exe <dir>/profile.txt [top]
Prints, per thread, the share of samples per function (and the lifted MIPS program / the H8 programs as wholes)."""
import sys, subprocess, bisect, collections, re
exe, prof = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
nm = subprocess.run(['nm', '-n', '--defined-only', exe], capture_output=True, text=True).stdout
syms = []
for l in nm.splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in 'tT':
        syms.append((int(p[0], 16) - 0x140000000, p[2]))
syms.sort(); addrs = [a for a, _ in syms]
def name(rva):
    k = bisect.bisect_right(addrs, rva) - 1
    return syms[k][1] if k >= 0 else '?'
def group(n):
    if re.match(r'L_8[0-9A-F]{7}$', n) or n.startswith('rr_') : return 'MIPS (lifted program + its runtime)'
    if 's23sub' in n or 'h8' in n.lower(): return 'H8s (sub-CPU, I/O board)'
    return None
per = collections.defaultdict(collections.Counter)
head = open(prof).readline().strip()
ext = collections.defaultdict(collections.Counter)
for l in open(prof):
    if l.startswith('#'): continue
    if l.startswith('ext '):
        _, tid, dll, n = l.split(); ext[tid][dll] += int(n); continue
    tid, rva, n = l.split(); rva = int(rva, 16); n = int(n)
    per[tid][name(rva) if rva else '(outside the program: drivers, C library)'] += n
print(head)
for tid, c in sorted(per.items(), key=lambda t: -sum(t[1].values())):
    tot = sum(c.values())
    if tot < 50: continue
    print('\n== thread %s: %d samples' % (tid, tot))
    g = collections.Counter()
    for fn, n in c.items(): g[group(fn) or 'other'] += n
    for k, n in g.most_common(): print('   %5.1f%%  %s' % (100.0 * n / tot, k))
    if ext.get(tid):
        print('   -- outside the program, by DLL:', ', '.join('%s %.1f%%' % (d, 100.0 * n / tot) for d, n in ext[tid].most_common(6)))
    print('   -- top functions:')
    for fn, n in c.most_common(top): print('   %5.1f%%  %s' % (100.0 * n / tot, fn))
