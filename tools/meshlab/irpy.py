"""meshlab irpy: an Eden IR print (tools/meshlab/ir_pretty.h; meshtool's <prefix>.pre.c, the flat print taken before the SPIR-V
is made) turned into a Python function and run on the CPU, one lane at a time. For checking a reading of a game stage against
a capture and for trying inputs without a device (cpu_draw.py); nothing here is the game's.

    run, source = irpy.compile_ir('<prefix>.pre.c');  run(irpy.Ops(env))

env supplies the stage's surroundings: cbuf(buffer, byte) -> u32, storage(slot, byte) -> u32, glob(address) -> u32,
isbe_read(byte) -> u32, lane, ballot_value (what a ballot returns: a mask of the lanes taken as active), and collects
ballot (each ballot's argument), writes (IsbeWriteAttribute), map8 / map32 (IsbeWriteMap*).

How the print is run: a value is a Python local; a block's phis are assigned together, each taking the argument of the block
that ran before (B); a loop is `while True` with the header block's phis inside it; the carry pseudo-operation of an IAdd32 is
kept beside the value. Integers are Python ints masked to 32 bits, floats are doubles rounded to 32 bits after every
operation (fma exact). Subgroup operations are those of a single lane: the stages run here use them for uniform loads (the
first active lane's value) and for the task stage's last count, which the caller makes from the lanes' ballots.
"""
import math, re, struct
TOK = re.compile(r"\s*(?:(?P<v>v\d+)|(?P<num>0x[0-9a-f]+|-?(?:inf|nan)f?|-?\d+(?:\.\d+)?(?:e[-+]?\d+)?f?)|(?P<name>[A-Za-z_][A-Za-z0-9_]*)|(?P<at>@B\d+)|(?P<p>[(),]))")
M = 0xffffffff
_f = struct.Struct('<f'); _i = struct.Struct('<I')
def r32(x):
    try: return _f.unpack(_f.pack(x))[0]
    except OverflowError: return math.copysign(math.inf, x)
def f2u(x): return _i.unpack(_f.pack(x))[0]
def u2f(x): return _f.unpack(_i.pack(x & M))[0]
def s32(x): x &= M; return x - (1 << 32) if x & 0x80000000 else x
class Ops:
    def __init__(s, env): s.e = env
    BitCastF32U32 = staticmethod(u2f); BitCastU32F32 = staticmethod(f2u)
    @staticmethod
    def FPFma32(a, b, c):
        try: return r32(math.fma(a, b, c))
        except (ValueError, OverflowError): return r32(a * b + c)
    FPMul32 = staticmethod(lambda a, b: r32(a * b)); FPAdd32 = staticmethod(lambda a, b: r32(a + b))
    FPMax32 = staticmethod(lambda a, b: b if a != a else a if b != b else max(a, b))
    FPMin32 = staticmethod(lambda a, b: b if a != a else a if b != b else min(a, b))
    FPRecip32 = staticmethod(lambda a: r32(1.0 / a) if a != 0 else math.copysign(math.inf, a))
    FPRecipSqrt32 = staticmethod(lambda a: r32(1.0 / math.sqrt(a)) if a > 0 else (math.inf if a == 0 else math.nan))
    FPSqrt = staticmethod(lambda a: r32(math.sqrt(a)) if a >= 0 else math.nan)
    FPTrunc32 = staticmethod(lambda a: a if a != a or math.isinf(a) else float(math.trunc(a)))
    FPClamp32 = staticmethod(lambda a, lo, hi: a if a != a else min(max(a, lo), hi))
    FPIsNan32 = staticmethod(lambda a: a != a)
    FPOrdGreaterThan32 = staticmethod(lambda a, b: a > b); FPOrdLessThan32 = staticmethod(lambda a, b: a < b)
    FPOrdGreaterThanEqual32 = staticmethod(lambda a, b: a >= b); FPOrdLessThanEqual32 = staticmethod(lambda a, b: a <= b)
    FPUnordGreaterThanEqual32 = staticmethod(lambda a, b: not (a < b))
    ConvertS32F32 = staticmethod(lambda a: 0 if a != a else int(min(max(a, -2147483648.0), 2147483647.0)) & M)
    ConvertF32U32 = staticmethod(lambda a: r32(float(a & M))); ConvertF32S32 = staticmethod(lambda a: r32(float(s32(a))))
    IAdd32 = staticmethod(lambda a, b: (a + b) & M); ISub32 = staticmethod(lambda a, b: (a - b) & M)
    IMul32 = staticmethod(lambda a, b: (a * b) & M); INeg32 = staticmethod(lambda a: (-a) & M)
    ShiftLeftLogical32 = staticmethod(lambda a, b: (a << b) & M if b < 32 else 0)
    ShiftRightLogical32 = staticmethod(lambda a, b: (a & M) >> b if b < 32 else 0)
    ShiftRightLogical64 = staticmethod(lambda a, b: a >> b if b < 64 else 0)
    BitFieldUExtract = staticmethod(lambda a, o, n: ((a & M) >> o) & ((1 << n) - 1))
    BitwiseAnd32 = staticmethod(lambda a, b: a & b & M); BitwiseOr32 = staticmethod(lambda a, b: (a | b) & M)
    BitwiseXor32 = staticmethod(lambda a, b: (a ^ b) & M); BitwiseNot32 = staticmethod(lambda a: ~a & M)
    BitCount32 = staticmethod(lambda a: bin(a & M).count('1'))
    SMin32 = staticmethod(lambda a, b: min(s32(a), s32(b)) & M); SMax32 = staticmethod(lambda a, b: max(s32(a), s32(b)) & M)
    UMin32 = staticmethod(lambda a, b: min(a & M, b & M))
    ULessThan = staticmethod(lambda a, b: (a & M) < (b & M)); UGreaterThanEqual = staticmethod(lambda a, b: (a & M) >= (b & M))
    SGreaterThan = staticmethod(lambda a, b: s32(a) > s32(b)); SGreaterThanEqual = staticmethod(lambda a, b: s32(a) >= s32(b))
    IEqual = staticmethod(lambda a, b: (a & M) == (b & M)); INotEqual = staticmethod(lambda a, b: (a & M) != (b & M))
    SelectU32 = staticmethod(lambda c, a, b: a if c else b); SelectU1 = SelectU32
    LogicalAnd = staticmethod(lambda a, b: bool(a) and bool(b)); LogicalOr = staticmethod(lambda a, b: bool(a) or bool(b))
    LogicalNot = staticmethod(lambda a: not a)
    GetZeroFromOp = staticmethod(lambda a: (a & M) == 0)
    CompositeConstructU32x2 = staticmethod(lambda a, b: (a, b)); CompositeExtractU32x2 = staticmethod(lambda t, i: t[i])
    CompositeExtractU32x4 = CompositeExtractU32x2
    PackUint2x32 = staticmethod(lambda t: (t[0] & M) | (t[1] & M) << 32); UnpackUint2x32 = staticmethod(lambda x: (x & M, (x >> 32) & M))
    UndefU32 = staticmethod(lambda: 0); UndefU1 = staticmethod(lambda: False)
    Prologue = Epilogue = staticmethod(lambda: None)
    def GetCbufU32(s, b, o): return s.e.cbuf(b, o)
    def GetCbufF32(s, b, o): return u2f(s.e.cbuf(b, o))
    def GetCbufU32x2(s, b, o): return (s.e.cbuf(b, o), s.e.cbuf(b, o + 4))
    def LoadStorage32(s, i, o): return s.e.storage(i, o)
    def LoadStorage128(s, i, o): return tuple(s.e.storage(i, o + 4 * k) for k in range(4))
    def LoadGlobal32(s, a): return s.e.glob(a)
    def IsbeRead(s, o): return s.e.isbe_read(o)
    def LaneId(s): return s.e.lane
    def SubgroupLtMask(s): return 0
    def SubgroupBallot(s, x): s.e.ballot.append(bool(x)); return s.e.ballot_value
    def IsbeWriteAttribute(s, o, x): s.e.writes.append((o, x))
    def IsbeWriteMapU8(s, o, x): s.e.map8.append((o, x & 255))
    def IsbeWriteMapU32(s, o, x): s.e.map32.append((o, x))
    def LoadStorage64(s, i, o): return (s.e.storage(i, o), s.e.storage(i, o + 4))
    def LoadGlobal128(s, a): return tuple(s.e.glob(a + 4 * k) for k in range(4))
    # (one lane at a time: a ballot of "every lane" is this lane alone, and the first lane's value is this lane's)
    ShuffleIndex = staticmethod(lambda x, i, *a: x); VoteAny = staticmethod(lambda x: bool(x))
    FindUMsb32 = staticmethod(lambda a: (a & M).bit_length() - 1 & M)
    BitFieldInsert = staticmethod(lambda b, x, o, n: (b & ~(((1 << n) - 1) << o) | (x & ((1 << n) - 1)) << o) & M)
    IAdd64 = staticmethod(lambda a, b: (a + b) & 0xffffffffffffffff)
    ShiftLeftLogical64 = staticmethod(lambda a, b: (a << b) & 0xffffffffffffffff if b < 64 else 0)
    UGreaterThan = staticmethod(lambda a, b: (a & M) > (b & M)); UMax32 = staticmethod(lambda a, b: max(a & M, b & M))
    FPSaturate32 = staticmethod(lambda a: 0.0 if a != a else min(max(a, 0.0), 1.0))
    ConvertU32F32 = staticmethod(lambda a: 0 if a != a else int(min(max(a, 0.0), 4294967295.0)))
    ShiftRightArithmetic32 = staticmethod(lambda a, b: (s32(a) >> min(b, 31)) & M)
    @staticmethod
    def FPExp2(a):
        try: return r32(2.0 ** a)
        except OverflowError: return math.inf

def expr(s, i=0):
    m = TOK.match(s, i); i = m.end()
    if m['v']: return m['v'], i
    if m['num']:
        t = m['num']
        if t.startswith('0x'): return str(int(t, 16)), i
        if t.endswith('f') or '.' in t or 'inf' in t or 'nan' in t:
            return "float('%s')" % t.rstrip('f') if ('inf' in t or 'nan' in t) else repr(r32(float(t.rstrip('f')))), i
        return t, i
    name = m['name']
    if name in ('true', 'false'): return name.capitalize(), i
    m2 = TOK.match(s, i); assert m2 and m2['p'] == '(', s[i:i + 40]; i = m2.end()
    args = []; labels = []
    while True:
        m3 = TOK.match(s, i)
        if m3 and m3['p'] == ')': i = m3.end(); break
        a, i = expr(s, i); args.append(a)
        m3 = TOK.match(s, i)
        if m3 and m3['at']: labels.append(int(m3['at'][2:])); i = m3.end(); m3 = TOK.match(s, i)
        if m3 and m3['p'] == ',': i = m3.end()
    if name == 'Phi':
        out = '0'
        for a, l in zip(reversed(args), reversed(labels)): out = '(%s) if B == %d else (%s)' % (a, l, out)
        return '(' + out + ')', i
    if name == 'GetCarryFromOp': return 'c' + args[0][1:], i
    return 'O.%s(%s)' % (name, ', '.join(args)), i

def compile_ir(path):
    lines = open(path).read().split('\n')
    carries = set(re.findall(r'GetCarryFromOp\((v\d+)\)', '\n'.join(lines)))
    allv = sorted(set(re.findall(r'\bv\d+\b', '\n'.join(lines))))
    out = ['def run(O):', '  B = -1'] + ['  %s = 0' % ' = '.join(allv[k:k + 50]) for k in range(0, len(allv), 50)]
    out += ['  %s = False' % ' = '.join('c' + v[1:] for v in carries)] if carries else []
    ind = 1; phis = []; pending = None; start = len(out)
    def flush():
        nonlocal phis, pending
        pad = '  ' * ind
        if phis:
            out.append(pad + ', '.join(v for v, _ in phis) + (',' if len(phis) == 1 else '') + ' = ' + ', '.join(e for _, e in phis) + (',' if len(phis) == 1 else ''))
            phis = []
        if pending is not None: out.append(pad + 'B = %d' % pending); pending = None
    for ln in lines:
        t = ln.strip()
        if not t: continue
        if t.startswith('// B'): flush(); start = len(out); pending = int(t[4:]); continue
        m = re.match(r'(v\d+) = (Phi\(.*)$', t)
        if m and pending is not None: phis.append((m[1], expr(m[2])[0])); continue
        flush(); pad = '  ' * ind
        if t.startswith('if ('): out.append(pad + 'if %s:' % expr(t[4:t.rindex(')')])[0]); ind += 1; out.append('  ' * ind + 'pass')
        elif t.startswith('loop {'):   # (the block before it is the loop's header: its phis run again every trip)
            head = out[start:]; del out[start:]
            out.append(pad + 'while True:'); ind += 1; out.extend('  ' + h for h in head); start = len(out)
        elif t.startswith('} while ('): out.append(pad + 'if not (%s): break' % expr(t[9:t.rindex(')')])[0]); ind -= 1
        elif t == '}': ind -= 1
        elif t == 'return;': out.append(pad + 'return')
        else:
            m = re.match(r'(v\d+) = (.*)$', t)
            if m:
                out.append(pad + '%s = %s' % (m[1], expr(m[2])[0]))
                if m[1] in carries:
                    a = re.match(r'IAdd32\((.*)\)$', m[2]); e1, j = expr(m[2], len('IAdd32(')); e2, _ = expr(m[2], TOK.match(m[2], j).end())
                    out.append(pad + 'c%s = (%s) + (%s) > 0xffffffff' % (m[1][1:], e1, e2))
            else: out.append(pad + expr(t)[0])
    ns = {}
    exec(compile('\n'.join(out), path, 'exec'), ns)
    return ns['run'], out
