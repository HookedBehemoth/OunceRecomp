# Disassemble main.bin around an offset, annotating ADRP+ADD string refs. usage: python tools/re/disasm.py <hex_off> [before] [after]
# (main.bin: work/re/main.bin, or MAIN_BIN=<file>; capstone)
import sys, os, capstone, struct
MAIN = os.environ.get('MAIN_BIN') or os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'work', 're', 'main.bin')
d = open(MAIN, 'rb').read()
off = int(sys.argv[1], 16); b = int(sys.argv[2]) if len(sys.argv) > 2 else 40; a = int(sys.argv[3]) if len(sys.argv) > 3 else 40
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); md.detail = False
start = off - 4 * b
regs = {}
def cstr(o):
    if 0 <= o < len(d):
        e = d.find(b'\0', o, o + 120)
        s = d[o:e]
        if e > o and all(32 <= c < 127 or c >= 0x80 for c in s): return s.decode('utf-8', 'replace')
    return None
for ins in md.disasm(d[start:off + 4 * a], start):
    note = ''
    ops = ins.op_str
    if ins.mnemonic == 'adrp':
        r, v = ops.split(', '); regs[r] = int(v.replace('#', ''), 16)
    elif ins.mnemonic == 'add' and ops.count('#') == 1:
        p = [x.strip() for x in ops.split(',')]
        if p[1] in regs and len(p) == 3:
            t = regs[p[1]] + int(p[2].replace('#', ''), 16)
            s = cstr(t); note = f'  ; {t:#x}' + (f' "{s}"' if s else '')
    elif ins.mnemonic == 'bl':
        note = ''
    mark = '>>' if ins.address == off else '  '
    print(f'{mark}{ins.address:8x}: {ins.mnemonic:8} {ops}{note}')
