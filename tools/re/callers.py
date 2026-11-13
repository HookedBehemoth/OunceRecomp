# Find BL/B callers and 8-byte absolute pointers (base 0x80020000 or raw offset) to a function offset in main.bin.
# usage: python tools/re/callers.py <hex_off> [...]   (main.bin: work/re/main.bin, or MAIN_BIN=<file>)
import sys, os, numpy as np, struct
MAIN = os.environ.get('MAIN_BIN') or os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'work', 're', 'main.bin')
d = open(MAIN, 'rb').read()
TEXT_END = 0x3c46000; BASE = 0x80020000
w = np.frombuffer(d[:TEXT_END], dtype='<u4').astype(np.int64)
pc = np.arange(len(w), dtype=np.int64) * 4
isbl = (w & 0x7c000000) == 0x14000000
imm = w & 0x3ffffff; imm = np.where(imm & (1 << 25), imm - (1 << 26), imm)
tgt = pc + imm * 4
for a in sys.argv[1:]:
    off = int(a, 16)
    bl = [hex(int(x)) for x in pc[isbl & (tgt == off)]]
    ptrs = []
    for v in (BASE + off, off):
        b = struct.pack('<Q', v); i = TEXT_END
        while True:
            i = d.find(b, i)
            if i < 0: break
            if i % 8 == 0: ptrs.append(hex(i))
            i += 1
    print(a, 'calls:', bl[:20], 'ptrs:', ptrs[:20])
