# Find ADRP+ADD/LDR references in main.bin .text to given file offsets (flat image, offsets == VA - base).
# usage: python tools/re/xref.py <hex_off> [...]   or   xref.py -s <string> [...]   (main.bin: work/re/main.bin, or MAIN_BIN=<file>)
import sys, os, numpy as np
MAIN = os.environ.get('MAIN_BIN') or os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'work', 're', 'main.bin')
TEXT_END = 0x3c46000
d = open(MAIN, 'rb').read()
w = np.frombuffer(d[:TEXT_END], dtype='<u4').astype(np.int64)
pc = np.arange(len(w), dtype=np.int64) * 4
isadrp = (w & 0x9f000000) == 0x90000000
immlo = (w >> 29) & 3; immhi = (w >> 5) & 0x7ffff
imm = (immhi << 2) | immlo
imm = np.where(imm & (1 << 20), imm - (1 << 21), imm)
page = (pc & ~0xfff) + (imm << 12)
rd = w & 31

def targets(off):
    hits = []
    pg = off & ~0xfff; lo = off & 0xfff
    idx = np.nonzero(isadrp & (page == pg))[0]
    for i in idx:
        r = int(rd[i])
        for j in range(i + 1, min(i + 8, len(w))):
            x = int(w[j])
            if (x & 0xffc00000) == 0x91000000 and ((x >> 5) & 31) == r:  # ADD imm (64)
                sh = (x >> 22) & 1; im = ((x >> 10) & 0xfff) << (12 * sh)
                if im == lo: hits.append(int(pc[j]))
                break
    return hits

args = sys.argv[1:]
if args and args[0] == '-s':
    for s in args[1:]:
        i = d.find(b'\0' + s.encode() + b'\0')
        if i < 0: print(s, 'not found'); continue
        off = i + 1
        print(s, hex(off), [hex(h) for h in targets(off)])
else:
    for a in args:
        off = int(a, 16); print(a, [hex(h) for h in targets(off)])
