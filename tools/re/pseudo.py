"""One function of the guest image as compact pseudo-C, for tools/re/re (`re fn`).

The body comes from the lifter's discovery (ounce-lift re-export). The blocks are translated in address order with
the register values kept as expressions: a value is written out (`x19 = ...;`) only when it is live past its block,
used twice, or about to be overwritten while something still reads it; loads are written out before a store or a
call. The prologue and epilogue (saves of x19-x30 to the stack, pointer authentication, the stack adjustment) are
left out. The control flow becomes if/else and loops where the blocks nest in address order, goto elsewhere.

Conventions of the text: x0..x7 at the start are the arguments; `p->+0x1c:u32` is a 32-bit load
at p + 0x1c; `local_10` is the stack slot at sp + 0x10 (after the prologue); `<u`, `>=u` are unsigned compares;
calls name the callee (our label, the SDK symbol, else f_<address>) with x0.. as far as the call site sets them.
"""
import bisect

import capstone
from capstone import arm64 as A

MD = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
MD.detail = True

FLAGS = 64
SP = 31

# ---- expressions


class E:
    __slots__ = ("size", "_key")

    def key(self):
        if self._key is None:
            self._key = self._k()
        return self._key


class Const(E):
    __slots__ = ("v", "addr")

    def __init__(s, v, addr=False):
        s.v, s.addr, s.size, s._key = v, addr, 1, None

    def _k(s):
        return "#%x%s" % (s.v, "a" if s.addr else "")


class Var(E):
    __slots__ = ("n",)

    def __init__(s, n):
        s.n, s.size, s._key = n, 1, None

    def _k(s):
        return s.n


class Raw(E):
    __slots__ = ("t",)

    def __init__(s, t):
        s.t, s.size, s._key = t, 1, None

    def _k(s):
        return "raw:" + s.t


class Bin(E):
    __slots__ = ("op", "a", "b")

    def __init__(s, op, a, b):
        s.op, s.a, s.b, s._key = op, a, b, None
        s.size = 1 + a.size + b.size

    def _k(s):
        return "(%s %s %s)" % (s.a.key(), s.op, s.b.key())


class Un(E):
    __slots__ = ("op", "a")

    def __init__(s, op, a):
        s.op, s.a, s._key = op, a, None
        s.size = 1 + a.size

    def _k(s):
        return "(%s%s)" % (s.op, s.a.key())


class Ld(E):
    __slots__ = ("a", "ty")

    def __init__(s, a, ty):
        s.a, s.ty, s._key = a, ty, None
        s.size = 1 + a.size

    def _k(s):
        return "[%s]%s" % (s.a.key(), s.ty)


class Call(E):
    __slots__ = ("f", "args", "pure")

    def __init__(s, f, args, pure=False):
        s.f, s.args, s.pure, s._key = f, args, pure, None
        s.size = 1 + sum(x.size for x in args) + (f.size if isinstance(f, E) else 0)

    def _k(s):
        return "%s(%s)" % (s.f.key() if isinstance(s.f, E) else s.f, ",".join(x.key() for x in s.args))


class Tern(E):
    __slots__ = ("c", "a", "b")

    def __init__(s, c, a, b):
        s.c, s.a, s.b, s._key = c, a, b, None
        s.size = 1 + c.size + a.size + b.size

    def _k(s):
        return "(%s ? %s : %s)" % (s.c.key(), s.a.key(), s.b.key())


def kids(e):
    if isinstance(e, (Bin,)):
        return (e.a, e.b)
    if isinstance(e, (Un, Ld)):
        return (e.a,)
    if isinstance(e, Call):
        return tuple(e.args) + ((e.f,) if isinstance(e.f, E) else ())
    if isinstance(e, Tern):
        return (e.c, e.a, e.b)
    return ()


def walk_any(e, pred):
    if pred(e):
        return True
    return any(walk_any(k, pred) for k in kids(e))


def refs(e, name):
    return walk_any(e, lambda x: isinstance(x, Var) and x.n == name)


def has_load(e):
    return walk_any(e, lambda x: isinstance(x, Ld) or (isinstance(x, Call) and not x.pure))


def rebuild(e, f):
    """e with each node n replaced by f(n) when that is not None (top-down)"""
    r = f(e)
    if r is not None:
        return r
    if isinstance(e, Bin):
        return Bin(e.op, rebuild(e.a, f), rebuild(e.b, f))
    if isinstance(e, Un):
        return Un(e.op, rebuild(e.a, f))
    if isinstance(e, Ld):
        return Ld(rebuild(e.a, f), e.ty)
    if isinstance(e, Call):
        return Call(rebuild(e.f, f) if isinstance(e.f, E) else e.f, [rebuild(x, f) for x in e.args], e.pure)
    if isinstance(e, Tern):
        return Tern(rebuild(e.c, f), rebuild(e.a, f), rebuild(e.b, f))
    return e


def subst(e, key, new):
    return rebuild(e, lambda n: new if n.key() == key else None)


def add(a, b):
    if isinstance(b, Const) and isinstance(a, Const):
        return Const((a.v + b.v) & 0xFFFFFFFFFFFFFFFF, a.addr or b.addr)
    if isinstance(a, Const) and not isinstance(b, Const):
        a, b = b, a
    if isinstance(b, Const):
        if b.v == 0:
            return a
        if isinstance(a, Bin) and a.op == "+" and isinstance(a.b, Const):
            return add(a.a, Const((a.b.v + b.v) & 0xFFFFFFFFFFFFFFFF))
    return Bin("+", a, b)


def sub(a, b):
    if isinstance(b, Const):
        return add(a, Const((-b.v) & 0xFFFFFFFFFFFFFFFF, b.addr))
    return Bin("-", a, b)


def binop(op, a, b):
    if isinstance(a, Const) and isinstance(b, Const):
        x, y = a.v, b.v
        r = {"<<": lambda: x << y, ">>": lambda: x >> y, "&": lambda: x & y, "|": lambda: x | y,
             "^": lambda: x ^ y, "*": lambda: x * y}.get(op)
        if r:
            return Const(r() & 0xFFFFFFFFFFFFFFFF)
    if op in ("|", "^", "+", "<<", ">>") and isinstance(b, Const) and b.v == 0:
        return a
    return Bin(op, a, b)


INV = {"==": "!=", "!=": "==", "<": ">=", ">=": "<", ">": "<=", "<=": ">", "<u": ">=u", ">=u": "<u", ">u": "<=u",
       "<=u": ">u"}


def neg(c):
    if isinstance(c, Bin) and c.op in INV:
        return Bin(INV[c.op], c.a, c.b)
    if isinstance(c, Un) and c.op == "!":
        return c.a
    if isinstance(c, Bin) and c.op == "&&":
        return Bin("||", neg(c.a), neg(c.b))
    if isinstance(c, Bin) and c.op == "||":
        return Bin("&&", neg(c.a), neg(c.b))
    if isinstance(c, Const):
        return Const(0 if c.v else 1)
    return Un("!", c)


def cmp(op, a, b):
    return Bin(op, a, b)


# condition codes (capstone ARM64_CC_*) on a compare a - b
CC = {1: "eq", 2: "ne", 3: "hs", 4: "lo", 5: "mi", 6: "pl", 7: "vs", 8: "vc", 9: "hi", 10: "ls", 11: "ge",
      12: "lt", 13: "gt", 14: "le", 15: "al", 16: "nv"}
CMP_OP = {"eq": "==", "ne": "!=", "hs": ">=u", "lo": "<u", "hi": ">u", "ls": "<=u", "ge": ">=", "lt": "<", "gt": ">",
          "le": "<="}
FCMP_OP = {"eq": "==", "ne": "!=", "ge": ">=", "gt": ">", "mi": "<", "lo": "<", "ls": "<=", "hi": ">", "lt": "<",
           "le": "<=", "pl": ">=", "hs": ">="}


def nzcv_holds(imm, cc):
    n, z, c, v = (imm >> 3) & 1, (imm >> 2) & 1, (imm >> 1) & 1, imm & 1
    return {"eq": z, "ne": not z, "hs": c, "lo": not c, "mi": n, "pl": not n, "vs": v, "vc": not v,
            "hi": c and not z, "ls": not (c and not z), "ge": n == v, "lt": n != v, "gt": not z and n == v,
            "le": not (not z and n == v), "al": True, "nv": True}[cc]


def flags_cmp(a, b, fp=False):
    def f(cc):
        if fp:
            op = FCMP_OP.get(cc)
            return cmp(op, a, b) if op else Raw("fcmp_%s(%s)" % (cc, "?"))
        if cc in CMP_OP:
            return cmp(CMP_OP[cc], a, b)
        if cc == "mi":
            return cmp("<", sub(a, b), Const(0))
        if cc == "pl":
            return cmp(">=", sub(a, b), Const(0))
        return Raw("%s(%s - %s)" % (cc, "...", "..."))
    return f


def flags_test(x):
    def f(cc):
        if cc == "eq":
            return cmp("==", x, Const(0))
        if cc == "ne":
            return cmp("!=", x, Const(0))
        if cc in ("mi", "lt"):
            return cmp("<", x, Const(0))
        if cc in ("pl", "ge"):
            return cmp(">=", x, Const(0))
        if cc == "gt":
            return cmp(">", x, Const(0))
        if cc == "le":
            return cmp("<=", x, Const(0))
        if cc in ("hs", "lo", "hi", "ls"):     # (C = 0 after a logical op)
            return Const(1 if cc in ("lo", "ls") else 0) if cc != "ls" else cmp("==", x, Const(0)) if False else Const(1)
        return Raw("%s(tst)" % cc)
    return f


def flags_ccmp(prev, a, b, imm, cc_in, fp=False):
    inner = flags_cmp(a, b, fp)

    def f(cc):
        c0 = prev(cc_in) if prev else Raw("flags")
        if nzcv_holds(imm, cc):
            return Bin("||", neg(c0), inner(cc))
        return Bin("&&", c0, inner(cc))
    return f


# ---- rendering

def hexs(v):
    if v >= 1 << 63:
        v -= 1 << 64
    if -10 < v < 10:
        return str(v)
    return ("-%#x" % -v) if v < 0 else ("%#x" % v)


PREC = {"||": 1, "&&": 2, "|": 3, "^": 4, "&": 5, "==": 6, "!=": 6, "<": 7, ">": 7, "<=": 7, ">=": 7, "<u": 7,
        ">u": 7, "<=u": 7, ">=u": 7, "<<": 8, ">>": 8, ">>s": 8, "+": 9, "-": 9, "*": 10, "/": 10, "%": 10}


class Render:
    def __init__(self, g):
        self.g = g

    def addr(self, v):
        return self.g.addr_text(v)

    def __call__(self, e, prec=0):
        r = self
        if isinstance(e, Const):
            if e.addr:
                return self.addr(e.v)
            return hexs(e.v)
        if isinstance(e, Var):
            return e.n
        if isinstance(e, Raw):
            return e.t
        if isinstance(e, Bin):
            p = PREC.get(e.op, 9)
            if e.op == "+" and isinstance(e.b, Const) and not e.b.addr and e.b.v >= 1 << 63:
                t = "%s - %s" % (r(e.a, p), hexs((1 << 64) - e.b.v))
            elif e.op == "+" and isinstance(e.a, Var) and e.a.n == "sp" and isinstance(e.b, Const):
                return "&local_%x" % e.b.v
            else:
                t = "%s %s %s" % (r(e.a, p), e.op, r(e.b, p + 1))
            return "(%s)" % t if p < prec or (prec and p == prec and e.op not in ("+", "*", "&&", "||", "|", "&")) else t
        if isinstance(e, Un):
            return "%s%s" % (e.op, r(e.a, 11))
        if isinstance(e, Ld):
            a = e.a
            if isinstance(a, Const) and a.addr:
                return "%s:%s" % (self.g.data_text(a.v), e.ty)
            if isinstance(a, Var) and a.n == "sp":
                return "local_0"
            if isinstance(a, Bin) and a.op == "+" and isinstance(a.b, Const) and not a.b.addr:
                if isinstance(a.a, Var) and a.a.n == "sp":
                    return "local_%x" % a.b.v
                off = a.b.v - (1 << 64) if a.b.v >= 1 << 63 else a.b.v
                return "%s->%s%#x:%s" % (r(a.a, 12), "-" if off < 0 else "+", abs(off), e.ty)
            return "%s->+0:%s" % (r(a, 12), e.ty)
        if isinstance(e, Call):
            if isinstance(e.f, E):
                f = e.f
                if isinstance(f, Ld) and isinstance(f.a, Bin) and isinstance(f.a.a, Ld) and isinstance(f.a.b, Const):
                    fn = "%s->vt[%#x]" % (r(f.a.a.a, 12), f.a.b.v)        # (*(obj->vtable + off))(...)
                elif isinstance(f, Ld) and isinstance(f.a, Ld):
                    fn = "%s->vt[0]" % r(f.a.a, 12)
                else:
                    fn = "(*%s)" % r(f, 12)
            else:
                fn = e.f
            return "%s(%s)" % (fn, ", ".join(r(x) for x in e.args))
        if isinstance(e, Tern):
            t = "%s ? %s : %s" % (r(e.c, 1), r(e.a, 1), r(e.b, 1))
            return "(%s)" % t if prec else t
        return "?"


# ---- registers

def regkey(name):
    if name in ("xzr", "wzr"):
        return None
    if name in ("sp", "wsp"):
        return SP
    if name == "fp":
        return 29
    if name == "lr":
        return 30
    if name == "nzcv":
        return FLAGS
    c, n = name[0], name[1:]
    if n.isdigit():
        if c in "xw":
            return int(n)
        if c in "bhsdqv":
            return 32 + int(n)
    return None


def keyname(k):
    if k == SP:
        return "sp"
    if k < 31:
        return "x%d" % k
    if k < 64:
        return "v%d" % (k - 32)
    return "flags"


LOAD_TY = {"ldrb": "u8", "ldrh": "u16", "ldrsb": "s8", "ldrsh": "s16", "ldrsw": "s32", "ldurb": "u8",
           "ldurh": "u16", "ldursb": "s8", "ldursh": "s16", "ldursw": "s32", "ldarb": "u8", "ldarh": "u16",
           "ldaprb": "u8", "ldaprh": "u16", "ldxrb": "u8", "ldxrh": "u16", "ldaxrb": "u8", "ldaxrh": "u16",
           "ldapurb": "u8", "ldapurh": "u16", "ldapursw": "s32"}
STORE_TY = {"strb": "u8", "strh": "u16", "sturb": "u8", "sturh": "u16", "stlrb": "u8", "stlrh": "u16"}
LOADS = {"ldr", "ldur", "ldar", "ldapr", "ldxr", "ldaxr", "ldapur", "ldtr"} | set(LOAD_TY)
STORES = {"str", "stur", "stlr", "stllr", "sttr", "stlur"} | set(STORE_TY)
PAIR_LD = {"ldp", "ldnp", "ldpsw", "ldxp", "ldaxp"}
PAIR_ST = {"stp", "stnp"}
EXCL_ST = {"stxr", "stlxr", "stxrb", "stlxrb", "stxrh", "stlxrh", "stxp", "stlxp"}
CMPS = {"cmp", "cmn", "tst", "fcmp", "fcmpe", "ccmp", "ccmn", "fccmp", "fccmpe"}
SETS_FLAGS = {"adds", "subs", "ands", "bics", "negs", "adcs", "sbcs", "ngcs"}
USES_FLAGS = {"csel", "csinc", "csinv", "csneg", "cset", "csetm", "cinc", "cinv", "cneg", "fcsel", "adc", "sbc",
              "adcs", "sbcs", "ngc", "ngcs", "ccmp", "ccmn", "fccmp", "fccmpe"}
DROP = {"pacibsp", "paciasp", "autibsp", "autiasp", "hint", "nop", "bti", "prfm", "prfum", "xpaclri", "pacia",
        "pacib", "autia", "autib", "dmb", "dsb", "isb", "clrex", "yield", "sevl", "wfe", "pacibz", "paciaz", "csdb"}
ATOMIC_PREFIX = ("ldadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldsmin", "ldumax", "ldumin", "swp")


def width_ty(regname, signed=False):
    c = regname[0]
    if c == "x":
        return "s64" if signed else "u64"
    if c == "w":
        return "s32" if signed else "u32"
    return {"b": "u8", "h": "f16", "s": "f32", "d": "f64", "q": "v128", "v": "v128"}.get(c, "u64")


class Insn:
    """one decoded instruction: the capstone insn (or None), its uses/defs (register keys) and its kind"""
    __slots__ = ("pc", "w", "i", "mn", "uses", "defs", "kind", "target", "cc")


def decode(g, pcs):
    """{pc: Insn} for the pcs (sorted), decoding runs of consecutive pcs at once"""
    out = {}
    k = 0
    while k < len(pcs):
        n = 1
        while k + n < len(pcs) and pcs[k + n] == pcs[k] + 4 * n:
            n += 1
        lo = pcs[k]
        data = g.read(lo, 4 * n)
        got = {i.address: i for i in MD.disasm(data, lo)}
        for j in range(n):
            pc = lo + 4 * j
            ins = got.get(pc)
            if ins is None and pc not in got:      # (capstone stops at an undecodable word: go on after it)
                for i in MD.disasm(data[4 * j + 4:], pc + 4):
                    got[i.address] = i
            out[pc] = classify(pc, int.from_bytes(data[4 * j:4 * j + 4], "little"), ins)
        k += n
    return out


def classify(pc, w, i):
    x = Insn()
    x.pc, x.w, x.i, x.target, x.cc = pc, w, i, None, 0
    if i is None:
        x.mn, x.uses, x.defs, x.kind = ".word", set(), set(), "other"
        return x
    mn = i.mnemonic
    x.mn = mn
    x.cc = i.cc
    ops = i.operands
    regs = []        # (index, key) of register operands
    mem = []
    for n, o in enumerate(ops):
        if o.type == A.ARM64_OP_REG:
            regs.append((n, regkey(i.reg_name(o.reg))))
        elif o.type == A.ARM64_OP_MEM:
            if o.mem.base:
                mem.append(regkey(i.reg_name(o.mem.base)))
            if o.mem.index:
                mem.append(regkey(i.reg_name(o.mem.index)))
    allr = {k for _, k in regs if k is not None} | {k for k in mem if k is not None}
    uses, defs = set(), set()
    kind = "other"
    if mn == "b" or mn.startswith("b."):
        kind = "cond" if mn.startswith("b.") and i.cc not in (0, 15) else "b"
        x.target = ops[0].imm
        if kind == "cond":
            uses = {FLAGS}
    elif mn in ("cbz", "cbnz"):
        kind, x.target, uses = "cond", ops[1].imm, allr
    elif mn in ("tbz", "tbnz"):
        kind, x.target, uses = "cond", ops[2].imm, allr
    elif mn == "bl":
        kind, x.target = "bl", ops[0].imm
    elif mn in ("blr", "blraa", "blrab", "blraaz", "blrabz"):
        kind, uses = "blr", allr
    elif mn in ("br", "braa", "brab", "braaz", "brabz"):
        kind, uses = "br", allr
    elif mn in ("ret", "retab", "retaa", "eret"):
        kind = "ret"
    elif mn in DROP:
        pass
    elif mn in CMPS:
        uses, defs = allr | ({FLAGS} if mn in USES_FLAGS else set()), {FLAGS}
    elif mn in STORES or mn in STORE_TY or mn in PAIR_ST or mn.startswith("st1") or mn.startswith("st2"):
        uses = allr
        if i.writeback and mem:
            defs = {mem[0]}
    elif mn in EXCL_ST:
        uses, defs = allr, {regs[0][1]} if regs and regs[0][1] is not None else set()
    elif mn.startswith(ATOMIC_PREFIX) or mn.startswith("cas"):
        uses = allr
        d = regs[0][1] if mn.startswith("cas") else (regs[1][1] if len(regs) > 1 else None)
        defs = {d} if d is not None else set()
    elif mn in PAIR_LD:
        uses = {k for k in mem if k is not None}
        defs = {k for _, k in regs[:2] if k is not None}
        if i.writeback and mem:
            defs.add(mem[0])
    elif regs and regs[0][0] == 0:
        d = regs[0][1]
        uses = {k for n, k in regs[1:] if k is not None} | {k for k in mem if k is not None}
        if mn in ("movk", "bfi", "bfxil", "bfm", "fmla", "fmls", "mla", "mls", "ins", "mov") and mn != "mov":
            uses.add(d)
        if mn == "mov" and ops and ops[0].type == A.ARM64_OP_REG and "." in i.op_str.split(",")[0]:
            uses.add(d)     # (mov v0.s[1], w8: an element insert)
        defs = {d} if d is not None else set()
        if mn in SETS_FLAGS:
            defs.add(FLAGS)
        if mn in USES_FLAGS:
            uses.add(FLAGS)
        if i.writeback and mem:
            defs.add(mem[0])
    else:
        uses = allr
    uses.discard(None)
    defs.discard(None)
    x.uses, x.defs, x.kind = uses, defs, kind
    return x


# ---- the function


class Block:
    __slots__ = ("start", "pcs", "succs", "preds", "term", "live_in", "live_out", "idx", "stmts", "cond", "tail",
                 "switch", "ret")


def build_blocks(g, entry, pcs, ins):
    body = set(pcs)
    entries = g.entry_set
    jt = g.jt

    def inside(t):
        return t in body and (t == entry or t not in entries)
    leaders = {entry}
    for pc in pcs:
        x = ins[pc]
        if pc - 4 not in body:
            leaders.add(pc)
        if x.kind in ("cond", "b") and inside(x.target):
            leaders.add(x.target)
        if x.kind in ("cond", "b", "br", "ret") or (x.kind == "bl" and pc + 4 not in body):
            if pc + 4 in body:
                leaders.add(pc + 4)
        if x.kind == "br" and pc in jt:
            for t in jt[pc][2]:
                if t in body:
                    leaders.add(t)
    order = sorted(leaders & body, key=lambda a: (a < entry, a))
    blocks = {}
    starts = sorted(leaders & body)
    for n, s in enumerate(starts):
        b = Block()
        b.start, b.pcs, b.succs, b.preds = s, [], [], []
        b.term, b.cond, b.tail, b.switch, b.ret = None, None, None, None, False
        pc = s
        nxt = starts[n + 1] if n + 1 < len(starts) else None
        while pc in body:
            b.pcs.append(pc)
            x = ins[pc]
            if x.kind in ("cond", "b", "br", "ret"):
                break
            pc += 4
            if pc == nxt:
                break
        last = ins[b.pcs[-1]]
        b.term = last
        fall = b.pcs[-1] + 4
        if last.kind == "cond":
            b.succs = ([last.target] if inside(last.target) else []) + ([fall] if fall in body else [])
            if not inside(last.target):
                b.tail = last.target            # a conditional tail branch
        elif last.kind == "b":
            if inside(last.target):
                b.succs = [last.target]
            else:
                b.tail = last.target
        elif last.kind == "br":
            if b.pcs[-1] in jt:
                b.switch = jt[b.pcs[-1]]
                seen = []
                for t in b.switch[2]:
                    if t in body and t not in seen:
                        seen.append(t)
                b.succs = seen
        elif last.kind == "ret":
            b.ret = True
        elif fall in body:
            b.succs = [fall]
        blocks[s] = b
    for b in blocks.values():
        for t in b.succs:
            if t in blocks:
                blocks[t].preds.append(b.start)
    ordered = [blocks[s] for s in order]
    for n, b in enumerate(ordered):
        b.idx = n
    return ordered, blocks


def returns_value(g, blocks, bmap, ins):
    def writes_x0_after_call(b):
        wrote = None
        for pc in b.pcs:
            x = ins[pc]
            if x.kind in ("bl", "blr"):
                wrote = False
            elif 0 in x.defs:
                wrote = True
        return wrote
    for b in blocks:
        if not b.ret:
            continue
        w = writes_x0_after_call(b)
        if w:
            return True
        if w is None:
            for p in b.preds:
                if writes_x0_after_call(bmap[p]):
                    return True
    return False


ARGS = set(range(8))
CALL_CLOBBER = set(range(19)) | {30, FLAGS} | set(range(32, 40)) | set(range(48, 64))


def uses_defs(x, retval):
    if x.kind in ("bl", "blr"):
        return x.uses | ARGS, CALL_CLOBBER
    if x.kind == "ret":
        return ({0} if retval else set()), set()
    if x.kind == "br":
        return x.uses | ARGS, set()
    return x.uses, x.defs


def liveness(blocks, bmap, ins, retval):
    ud = {}
    for b in blocks:
        use, df = set(), set()
        for pc in b.pcs:
            u, d = uses_defs(ins[pc], retval)
            if b.tail is not None and pc == b.pcs[-1]:
                u = u | ARGS
            use |= u - df
            df |= d
        ud[b.start] = (use, df)
        b.live_in, b.live_out = set(), set()
    changed = True
    while changed:
        changed = False
        for b in reversed(blocks):
            out = set()
            for s in b.succs:
                out |= bmap[s].live_in
            use, df = ud[b.start]
            li = use | (out - df)
            if out != b.live_out or li != b.live_in:
                b.live_out, b.live_in, changed = out, li, True


class Fn:
    """the translation of one function into statements per block, then the structured text"""

    def __init__(self, g, entry):
        self.g = g
        self.entry = entry
        self.pcs = g.body(entry)
        self.ins = decode(g, self.pcs)
        self.blocks, self.bmap = build_blocks(g, entry, self.pcs, self.ins)
        self.retval = returns_value(g, self.blocks, self.bmap, self.ins)
        liveness(self.blocks, self.bmap, self.ins, self.retval)
        self.r = Render(g)
        self.tmp = 0
        self.saved_slots = set()
        self.fp = None
        self.calls = []     # (pc, callee address or None)
        eb = self.bmap.get(entry)
        self.args = list(range(g.callee_inputs(entry)))
        for b in self.blocks:
            self.translate(b)

    # -- one block

    def translate(self, b):
        g, ins = self.g, self.ins
        st = {}
        if b.start == self.entry:
            pass
        elif self.fp is not None:
            st[29] = self.fp
        out = []
        self.out, self.st, self.flags = out, st, None
        live_after = []
        live = set(b.live_out)
        if b.tail is not None:
            live |= ARGS
        for pc in reversed(b.pcs):
            live_after.append(set(live))
            u, d = uses_defs(ins[pc], self.retval)
            live = (live - d) | u
        live_after.reverse()
        # uses of each def before it is redefined (in the block), +1 when it reaches the block's end live
        self.nuse = {}
        for n, pc in enumerate(b.pcs):
            for k in ins[pc].defs:
                cnt = 0
                for m in range(n + 1, len(b.pcs)):
                    y = ins[b.pcs[m]]
                    u, d = uses_defs(y, self.retval)
                    if k in u:
                        cnt += 1
                    if k in d:
                        break
                else:
                    if k in b.live_out:
                        cnt += 1
                self.nuse[(pc, k)] = cnt
        self.written_args = set()
        self._epi = b.ret or b.tail is not None
        for n, pc in enumerate(b.pcs):
            self.live = live_after[n]
            self.pc = pc
            self.step(ins[pc], b, n == len(b.pcs) - 1)
        if b.term.kind not in ("cond", "b", "br", "ret"):
            self.live = set(b.live_out)
            self.end_of_block(b, [])
        if b.start == self.entry and 29 in st and not isinstance(st[29], Var):
            self.fp = st[29]
        b.stmts = out

    def get(self, k):
        if k is None:
            return Const(0)
        return self.st.get(k) or Var(keyname(k))

    def emit(self, text):
        self.out.append(text)

    def assign(self, k, e, pc=None):
        """the register's new value; written out now when used twice and not simple"""
        if k is None:
            return
        if e.size > 24:
            self.st[k] = e
            self.materialize(k)
            return
        self.st[k] = e
        uses = self.nuse.get((self.pc, k), 1)
        if uses >= 2 and not simple(e):
            self.materialize(k)

    def materialize(self, k, busy=None):
        e = self.st.get(k)
        name = keyname(k)
        if e is None or (isinstance(e, Var) and e.n == name):
            return
        busy = busy or set()
        busy.add(k)
        for q, qe in list(self.st.items()):
            if q == k or q not in self.live or not refs(qe, name):
                continue
            if q in busy:
                self.tmp += 1
                t = "t%d" % self.tmp
                self.emit("%s = %s;" % (t, name))
                for q2 in list(self.st):
                    if q2 != k:
                        self.st[q2] = subst(self.st[q2], name, Var(t))
                continue
            self.materialize(q, busy)
        e = self.st.get(k)
        self.emit("%s = %s;" % (name, self.r(e)))
        self.st[k] = Var(name)

    def flush_loads(self, keep_clobbered=True):
        for q in sorted(self.st):
            if q in self.live and has_load(self.st[q]) and (keep_clobbered or q not in CALL_CLOBBER):
                self.materialize(q)

    # -- operands

    def opnd(self, i, o):
        if o.type == A.ARM64_OP_REG:
            name = i.reg_name(o.reg)
            e = self.get(regkey(name))
            if regkey(name) == SP:
                e = Var("sp")
            if o.ext:
                cast = {1: "(u8)", 2: "(u16)", 3: "(u32)", 4: "(u64)", 5: "(s8)", 6: "(s16)", 7: "(s32)", 8: ""}
                c = cast.get(o.ext, "")
                if c:
                    e = Un(c, e)
            if o.shift.type and o.shift.value:
                op = {1: "<<", 2: ">>", 3: ">>s", 4: "ror", 5: "ror"}.get(o.shift.type, "<<")
                e = binop(op, e, Const(o.shift.value)) if op != "ror" else Call("ror", [e, Const(o.shift.value)], True)
            return e
        if o.type == A.ARM64_OP_IMM:
            v = o.imm & 0xFFFFFFFFFFFFFFFF
            if o.shift.type and o.shift.value:
                v = (v << o.shift.value) & 0xFFFFFFFFFFFFFFFF
            return Const(v)
        if o.type == A.ARM64_OP_FP:
            return Raw(repr(o.fp))
        if o.type == A.ARM64_OP_MEM:
            return self.memaddr(i, o)
        return Raw("?")

    def memaddr(self, i, o, post=False):
        base = Var("sp") if regkey(i.reg_name(o.mem.base)) == SP else self.get(regkey(i.reg_name(o.mem.base)))
        a = base
        if o.mem.index:
            ix = self.get(regkey(i.reg_name(o.mem.index)))
            if o.ext:
                c = {3: "(u32)", 7: "(s32)"}.get(o.ext, "")
                if c:
                    ix = Un(c, ix)
            if o.shift.value:
                ix = binop("<<", ix, Const(o.shift.value))
            a = Bin("+", a, ix)
        if o.mem.disp and not post:
            a = add(a, Const(o.mem.disp & 0xFFFFFFFFFFFFFFFF))
        return a

    def dst(self, i, n=0):
        o = i.operands[n]
        return regkey(i.reg_name(o.reg)), i.reg_name(o.reg)

    # -- the instruction

    def step(self, x, b, last):
        i = x.i
        mn = x.mn
        if i is None:
            self.emit("/* %#x: .word %#010x */" % (x.pc, x.w))
            return
        ops = i.operands
        k = x.kind
        if k not in ("bl", "blr"):
            self.written_args |= {d for d in x.defs if d < 8}
        if k in ("bl", "blr"):
            return self.call(x, b, last)
        if k in ("cond", "b", "br", "ret"):
            return self.terminator(x, b)
        if mn in DROP:
            return
        if mn in ("sub", "add") and ops[0].type == A.ARM64_OP_REG and regkey(i.reg_name(ops[0].reg)) == SP:
            if len(ops) > 1 and regkey(i.reg_name(ops[1].reg)) == SP:
                return                                           # the frame's adjustment
        if mn in CMPS:
            return self.compare(x)
        if mn in LOADS or mn in PAIR_LD:
            return self.load(x)
        if mn in STORES or mn in PAIR_ST:
            return self.store(x)
        if mn in EXCL_ST:
            d, _ = self.dst(i)
            self.flush_loads()
            a = self.opnd(i, ops[-1])
            vals = [self.r(self.opnd(i, o)) for o in ops[1:-1]]
            self.st[d] = Var(keyname(d))
            self.emit("%s = %s(%s, %s);" % (keyname(d), mn, self.r(a), ", ".join(vals)))
            return
        if mn.startswith(ATOMIC_PREFIX) or mn.startswith("cas"):
            self.flush_loads()
            a = self.opnd(i, ops[-1])
            vals = [self.opnd(i, o) for o in ops[:-1]]
            d = (x.defs or {None}).pop() if x.defs else None
            dname = keyname(d) if d is not None else None
            text = "%s(%s, %s)" % (mn, self.r(a), self.r(vals[0]) if not mn.startswith("cas") else
                                    ", ".join(self.r(v) for v in vals))
            if d is not None and d in self.live:
                self.materialize_dependents(d)
                self.emit("%s = %s;" % (dname, text))
                self.st[d] = Var(dname)
            else:
                self.emit(text + ";")
            return
        if not x.defs or ops[0].type != A.ARM64_OP_REG:
            self.emit("/* %s %s */" % (mn, i.op_str))
            return
        d, dname = self.dst(i)
        if d is None:
            if FLAGS in x.defs:        # (subs xzr ...: a compare)
                return self.compare(x)
            return
        e = self.value(x, i, ops, dname)
        if FLAGS in x.defs:
            if mn in ("ands", "bics"):
                self.flags = flags_test(e)
            elif mn == "adds":
                self.flags = flags_cmp(self.opnd(i, ops[1]), Un("-", self.opnd(i, ops[2])))
            else:
                self.flags = flags_cmp(self.opnd(i, ops[1]), self.opnd(i, ops[2])) if len(ops) > 2 else self.flags
        if i.writeback:
            pass
        if d not in self.live and FLAGS not in x.defs:
            return
        if d in self.live:
            self.materialize_dependents(d)
            self.assign(d, e)

    def materialize_dependents(self, d):
        """before d's register gets a new value: the live registers whose value reads d's current variable"""
        name = keyname(d)
        for q in list(self.st):
            if q != d and q in self.live and refs(self.st[q], name):
                self.materialize(q)

    def value(self, x, i, ops, dname):
        mn = x.mn
        S = lambda n: self.opnd(i, ops[n])
        n = len(ops)
        if mn in ("mov", "fmov", "movz") and n == 2:
            return S(1)
        if mn == "movn":
            return Const(~S(1).v & (0xFFFFFFFF if dname[0] == "w" else 0xFFFFFFFFFFFFFFFF))
        if mn == "movk":
            cur = self.get(regkey(dname))
            imm = ops[1].imm << ops[1].shift.value
            if isinstance(cur, Const):
                m = 0xFFFF << ops[1].shift.value
                return Const((cur.v & ~m) | imm)
            return Bin("|", cur, Const(imm))
        if mn in ("adrp", "adr"):
            return Const(ops[1].imm, True)
        if mn in ("add", "adds") and n == 3:
            return add(S(1), S(2))
        if mn in ("sub", "subs") and n == 3:
            return sub(S(1), S(2))
        if mn in ("neg", "negs"):
            return Un("-", S(1))
        if mn in ("mvn",):
            return Un("~", S(1))
        two = {"and": "&", "ands": "&", "orr": "|", "eor": "^", "mul": "*", "udiv": "/", "sdiv": "/", "lsl": "<<",
               "lsr": ">>", "asr": ">>s", "fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/"}
        if mn in two and n == 3:
            return binop(two[mn], S(1), S(2))
        if mn in ("bic", "bics") and n == 3:
            return Bin("&", S(1), Un("~", S(2)))
        if mn == "orn" and n == 3:
            return Bin("|", S(1), Un("~", S(2)))
        if mn in ("madd", "fmadd", "umaddl", "smaddl") and n == 4:
            return add(S(3), Bin("*", S(1), S(2))) if mn[0] != "f" else Bin("+", S(3), Bin("*", S(1), S(2)))
        if mn in ("msub", "fmsub", "umsubl", "smsubl") and n == 4:
            return Bin("-", S(3), Bin("*", S(1), S(2)))
        if mn in ("umull", "smull", "mneg") and n == 3:
            return Bin("*", S(1), S(2)) if mn != "mneg" else Un("-", Bin("*", S(1), S(2)))
        if mn in ("ubfx", "sbfx") and n == 4:
            lsb, w = ops[2].imm, ops[3].imm
            return binop("&", binop(">>" if mn == "ubfx" else ">>s", S(1), Const(lsb)), Const((1 << w) - 1))
        if mn in ("ubfiz", "sbfiz") and n == 4:
            lsb, w = ops[2].imm, ops[3].imm
            return binop("<<", binop("&", S(1), Const((1 << w) - 1)), Const(lsb))
        casts = {"sxtw": "(s32)", "uxtw": "(u32)", "sxth": "(s16)", "uxth": "(u16)", "sxtb": "(s8)", "uxtb": "(u8)",
                 "scvtf": "(float)", "ucvtf": "(float)", "fcvtzs": "(int)", "fcvtzu": "(uint)", "fcvt": "(fcvt)",
                 "fneg": "-"}
        if mn in casts and n == 2:
            return Un(casts[mn], S(1))
        if mn in ("csel", "fcsel") and n == 3:
            return Tern(self.cond(x.cc), S(1), S(2))
        if mn in ("csinc", "csinv", "csneg") and n == 3:
            alt = {"csinc": lambda e: add(e, Const(1)), "csinv": lambda e: Un("~", e), "csneg": lambda e: Un("-", e)}
            return Tern(self.cond(x.cc), S(1), alt[mn](S(2)))
        if mn == "cset":
            return self.cond(x.cc)
        if mn == "csetm":
            return Un("-", self.cond(x.cc))
        if mn in ("cinc", "cinv", "cneg") and n == 2:
            alt = {"cinc": lambda e: add(e, Const(1)), "cinv": lambda e: Un("~", e), "cneg": lambda e: Un("-", e)}
            return Tern(self.cond(x.cc), alt[mn](S(1)), S(1))
        return Call(mn, [S(j) for j in range(1, n)], pure=True)

    def cond(self, cc):
        c = CC.get(cc, "al")
        if self.flags is None:
            return Raw("cond_%s" % c)
        return self.flags(c)

    def compare(self, x):
        i, ops, mn = x.i, x.i.operands, x.mn
        if FLAGS not in self.live and mn not in ("ccmp", "ccmn", "fccmp", "fccmpe"):
            pass
        fp = mn.startswith("f")
        if mn in ("cmp", "fcmp", "fcmpe", "subs"):
            a, bb = self.opnd(i, ops[-2]), self.opnd(i, ops[-1])
            self.flags = flags_cmp(a, bb, fp)
        elif mn in ("cmn", "adds"):
            self.flags = flags_cmp(self.opnd(i, ops[-2]), Un("-", self.opnd(i, ops[-1])))
        elif mn in ("tst", "ands"):
            self.flags = flags_test(binop("&", self.opnd(i, ops[-2]), self.opnd(i, ops[-1])))
        else:          # ccmp a, b, #nzcv, cc
            a, bb = self.opnd(i, ops[0]), self.opnd(i, ops[1])
            if mn == "ccmn":
                bb = Un("-", bb)
            self.flags = flags_ccmp(self.flags, a, bb, ops[2].imm, CC.get(x.cc, "al"), fp)

    def load(self, x):
        i, ops, mn = x.i, x.i.operands, x.mn
        regs = [o for o in ops if o.type == A.ARM64_OP_REG]
        memo = next(o for o in ops if o.type == A.ARM64_OP_MEM) if any(o.type == A.ARM64_OP_MEM for o in ops) else None
        post = i.writeback and ops[-1].type == A.ARM64_OP_IMM
        if memo is None:        # ldr x, =literal
            a = Const(ops[1].imm, True)
            base_k = None
        else:
            a = self.memaddr(i, memo, post)
            base_k = regkey(i.reg_name(memo.mem.base))
        names = [i.reg_name(o.reg) for o in regs]
        excl = mn.startswith(("ldx", "ldax"))
        width = []
        for nm in names[:2 if mn in PAIR_LD else 1]:
            signed = mn in ("ldpsw",)
            ty = LOAD_TY.get(mn) or ("s32" if signed else width_ty(nm))
            width.append((regkey(nm), nm, ty))
        size = {"u8": 1, "s8": 1, "u16": 2, "s16": 2, "f16": 2, "u32": 4, "s32": 4, "f32": 4, "u64": 8, "f64": 8,
                "s64": 8, "v128": 16}
        off = 0
        restore = b_end = self.ins_is_epilogue()
        for k, nm, ty in width:
            addr = add(a, Const(off)) if off else a
            off += size.get(ty, 8) if mn != "ldpsw" else 4
            if k is None:
                continue
            if b_end and 19 <= k <= 30 and isinstance(addr, (Var, Bin)) and is_sp(addr):
                self.st[k] = Var(keyname(k))       # (the epilogue's restore)
                continue
            if k not in self.live:
                continue
            e = Ld(addr, ty)
            self.materialize_dependents(k)
            if excl:
                self.st[k] = Call(mn, [addr])
                self.materialize(k)
            else:
                self.assign(k, e)
        if i.writeback and base_k is not None and base_k != SP:
            step = ops[-1].imm if post else memo.mem.disp
            self.materialize_dependents(base_k)
            self.assign(base_k, add(self.get(base_k), Const(step & 0xFFFFFFFFFFFFFFFF)))

    def ins_is_epilogue(self):
        return self._epi

    def store(self, x):
        i, ops, mn = x.i, x.i.operands, x.mn
        memo = next(o for o in ops if o.type == A.ARM64_OP_MEM)
        post = i.writeback and ops[-1].type == A.ARM64_OP_IMM
        a = self.memaddr(i, memo, post)
        base_k = regkey(i.reg_name(memo.mem.base))
        regs = [o for o in ops if o.type == A.ARM64_OP_REG]
        off = 0
        items = []
        for o in regs[:2 if mn in PAIR_ST else 1]:
            nm = i.reg_name(o.reg)
            k = regkey(nm)
            ty = STORE_TY.get(mn) or width_ty(nm)
            addr = add(a, Const(off)) if off else a
            off += {"u8": 1, "u16": 2, "u32": 4, "f16": 2, "f32": 4, "u64": 8, "f64": 8, "v128": 16}.get(ty, 8)
            v = self.get(k) if k is not None else Const(0)
            if k is not None and 19 <= k <= 30 and isinstance(v, Var) and v.n == keyname(k) and is_sp(addr):
                continue                     # (the prologue's save)
            items.append((addr, ty, v))
        if items:
            self.flush_loads()
        for addr, ty, v in items:
            lhs = self.r(Ld(addr, ty))
            self.emit("%s = %s;" % (lhs, self.r(v)))
        if i.writeback and base_k is not None and base_k != SP:
            step = ops[-1].imm if post else memo.mem.disp
            self.materialize_dependents(base_k)
            self.assign(base_k, add(self.get(base_k), Const(step & 0xFFFFFFFFFFFFFFFF)))

    def call_args(self, target):
        n = max(self.written_args) + 1 if self.written_args else 0
        if target is not None:
            n = max(n, self.g.callee_inputs(target))
        return [self.get(k) for k in range(n)]

    def call(self, x, b, last):
        i = x.i
        if x.kind == "bl":
            target = self.g.resolve(x.target)
            f = self.g.name(target)
            self.calls.append((x.pc, target))
        else:
            target = None
            f = self.get(regkey(i.reg_name(i.operands[0].reg)))
            self.calls.append((x.pc, None))
        args = self.call_args(target)
        e = Call(f, args)
        clob = {keyname(k) for k in CALL_CLOBBER if k != FLAGS}
        for q in list(self.st):
            if q not in CALL_CLOBBER and q in self.live and (
                    has_load(self.st[q]) or walk_any(self.st[q], lambda n: isinstance(n, Var) and n.n in clob)):
                self.materialize(q)
        for q in list(self.st):
            if q in CALL_CLOBBER:
                del self.st[q]
        self.flags = None
        self.written_args = set()
        if 0 in self.live:
            self.materialize_dependents(0)
            self.emit("x0 = %s;" % self.r(e))
            self.st[0] = Var("x0")
        else:
            self.emit("%s;" % self.r(e))

    def end_of_block(self, b, exprs):
        """write out the live-out registers; exprs (conditions, the switch index) kept true after it"""
        for k in sorted(b.live_out):
            if k in (SP, FLAGS):
                continue
            if k == 29 and self.fp is not None:
                continue
            e = self.st.get(k)
            if e is None or (isinstance(e, Var) and e.n == keyname(k)):
                continue
            old = e.key()
            self.materialize(k)
            exprs = [subst(c, old, Var(keyname(k))) if c is not None else None for c in exprs]
            # a condition still reading the old variable: keep its value in a temporary first
        return exprs

    def terminator(self, x, b):
        i, ops = x.i, x.i.operands
        if x.kind == "cond":
            mn = x.mn
            if mn.startswith("b."):
                c = self.cond(x.cc)
            elif mn in ("cbz", "cbnz"):
                c = cmp("==" if mn == "cbz" else "!=", self.opnd(i, ops[0]), Const(0))
            else:
                bit = ops[1].imm
                t = binop("&", self.opnd(i, ops[0]), Const(1 << bit))
                c = cmp("==" if mn == "tbz" else "!=", t, Const(0))
            if b.tail is not None:
                args = self.call_args(self.g.resolve(b.tail))
                tail = Call(self.g.name(self.g.resolve(b.tail)), args)
                c, = self.end_of_block(b, [c])
                b.cond = c
                b.tail = self.r(tail)
                return
            c, = self.end_of_block(b, [c])
            b.cond = c
            return
        if x.kind == "b":
            if b.tail is not None:
                target = self.g.resolve(b.tail)
                self.calls.append((x.pc, target))
                e = Call(self.g.name(target), self.call_args(target))
                b.tail = self.r(e)
                return
            self.end_of_block(b, [])
            return
        if x.kind == "ret":
            v = self.get(0) if self.retval else None
            b.cond = v
            return
        if x.kind == "br":
            reg = self.get(regkey(i.reg_name(ops[0].reg)))
            if b.switch is not None:
                load, idx, targets = b.switch
                ix = self.get(idx) if idx < 31 else Var("index")
                ix, = self.end_of_block(b, [ix])
                b.cond = ix
                return
            b.tail = "(*%s)(...)" % self.r(reg)
            return


def simple(e):
    if isinstance(e, (Var, Const, Raw)):
        return True
    if isinstance(e, Bin) and e.op == "+" and isinstance(e.a, Var) and isinstance(e.b, Const):
        return True
    return False


def is_sp(a):
    if isinstance(a, Var):
        return a.n == "sp"
    if isinstance(a, Bin) and a.op == "+":
        return is_sp(a.a)
    return False


# ---- the structure

class Node:
    __slots__ = ("k", "a", "b", "c", "addr")

    def __init__(s, k, a=None, b=None, c=None, addr=None):
        s.k, s.a, s.b, s.c, s.addr = k, a, b, c, addr


def structure(fn):
    blocks = fn.blocks
    idx = {b.start: n for n, b in enumerate(blocks)}
    need = set()
    loops_at = {}

    def preds_ok(lo, hi, src):
        for j in range(lo, hi):
            for p in blocks[j].preds:
                pi = idx.get(p)
                if pi is None:
                    return False
                if lo <= pi < hi:
                    continue
                if j == lo and pi == src:
                    continue
                return False
        return True

    def jump(t, ctx, at_end):
        follow, header, exit_ = ctx
        if t == header and header is not None:
            return [] if at_end and follow == header else [Node("continue")]
        if t == exit_ and exit_ is not None:
            return [Node("break")]
        if t == follow and at_end:
            return []
        need.add(t)
        return [Node("goto", addr=t)]

    def emit(lo, hi, ctx, skip_loop=None):
        out = []
        i = lo
        while i < hi:
            b = blocks[i]
            if skip_loop != i:
                ks = [k for k in range(i, hi) if any(s == b.start for s in blocks[k].succs) and k >= i]
                if ks:
                    k = max(ks)
                    ex = blocks[k + 1].start if k + 1 < len(blocks) else None
                    body = emit(i, k + 1, (b.start, b.start, ex), skip_loop=i)
                    out.append(Node("loop", body))
                    i = k + 1
                    continue
            skip_loop = None
            out.append(Node("label", addr=b.start))
            out.extend(Node("stmt", s) for s in b.stmts)
            end = blocks[hi].start if hi < len(blocks) else None
            end_target = ctx[0]
            at_end = i == hi - 1
            if b.term.kind == "cond":
                if b.tail is not None:
                    out.append(Node("if", b.cond, [Node("ret", b.tail)]))
                    if not b.succs:
                        pass
                    elif at_end and b.succs[-1] != end_target:
                        out.extend(jump(b.succs[-1], ctx, True))
                    i += 1
                    continue
                t = b.term.target
                ti = idx.get(t)
                fall = b.pcs[-1] + 4
                if ti is not None and i < ti and (ti < hi or (ti == hi and end == end_target)) and \
                        fall in idx and idx[fall] == i + 1 and preds_ok(i + 1, ti, i):
                    lastb = blocks[ti - 1]
                    m = None
                    if ti - 1 > i and lastb.term.kind == "b" and lastb.tail is None:
                        mi = idx.get(lastb.term.target)
                        if mi is not None and ti < mi and (mi < hi or (mi == hi and end == end_target)) and \
                                preds_ok(ti, mi, i):
                            m = mi
                    if m is not None:
                        mt = blocks[m].start if m < len(blocks) else None
                        th = emit(i + 1, ti, (mt, ctx[1], ctx[2]))
                        el = emit(ti, m, (mt, ctx[1], ctx[2]))
                        out.append(Node("if", neg(b.cond), th, el))
                        if m == hi and m - 1 >= 0:
                            pass
                        i = m
                        continue
                    th = emit(i + 1, ti, (t, ctx[1], ctx[2]))
                    out.append(Node("if", neg(b.cond), th))
                    i = ti
                    if ti == hi and end != end_target:
                        out.extend(jump(t, ctx, True))
                    continue
                out.append(Node("if", b.cond, jump(t, ctx, False) if t in idx else [Node("goto", addr=t)]))
                if t in idx and not jump(t, ctx, False):
                    pass
                if fall not in idx:
                    pass
                elif at_end and fall != end_target:
                    out.extend(jump(fall, ctx, True))
                elif not at_end and idx[fall] != i + 1:
                    out.extend(jump(fall, ctx, False))
                i += 1
                continue
            if b.tail is not None:
                out.append(Node("ret", b.tail))
            elif b.ret:
                out.append(Node("ret", fn.r(b.cond) if b.cond is not None else None))
            elif b.switch is not None:
                cases = {}
                for n, t in enumerate(b.switch[2]):
                    cases.setdefault(t, []).append(n)
                sw = []
                for t, ns in cases.items():
                    sw.append((ns, jump(t, ctx, False) if t in idx else [Node("goto", addr=t)]))
                out.append(Node("switch", b.cond, sw))
            elif b.term.kind == "b":
                t = b.term.target
                if at_end:
                    out.extend(jump(t, ctx, True))
                elif idx.get(t) == i + 1:
                    pass
                else:
                    out.extend(jump(t, ctx, False))
            elif b.succs:
                t = b.succs[0]
                if at_end:
                    if t != end_target:
                        out.extend(jump(t, ctx, True))
                elif idx.get(t) != i + 1:
                    out.extend(jump(t, ctx, False))
            i += 1
        return out

    tree = emit(0, len(blocks), (None, None, None))
    return tree, need


def simplify(nodes):
    out = []
    for n in nodes:
        if n.k == "loop":
            body = simplify(n.a)
            # do { ... } while (c): the body ends `if (c) continue; break;`, no other continue at this level
            if len(body) >= 2 and body[-1].k == "break" and body[-2].k == "if" and body[-2].c is None and \
                    len(body[-2].b) == 1 and body[-2].b[0].k == "continue" and not has_continue(body[:-2]):
                out.append(Node("dowhile", body[:-2], body[-2].a))
                continue
            out.append(Node("loop", body))
        elif n.k == "if":
            out.append(Node("if", n.a, simplify(n.b), simplify(n.c) if n.c else None))
        elif n.k == "switch":
            out.append(Node("switch", n.a, [(ns, simplify(b)) for ns, b in n.b]))
        else:
            out.append(n)
    return out


def has_continue(nodes):
    for n in nodes:
        if n.k == "continue":
            return True
        if n.k == "if" and (has_continue(n.b) or (n.c and has_continue(n.c))):
            return True
        if n.k == "switch" and any(has_continue(b) for _, b in n.b):
            return True
    return False


def ranges(ns):
    out, k = [], 0
    while k < len(ns):
        j = k
        while j + 1 < len(ns) and ns[j + 1] == ns[j] + 1:
            j += 1
        out.append(str(ns[k]) if j == k else "%d..%d" % (ns[k], ns[j]))
        k = j + 1
    return ",".join(out)


def text(fn, tree, need):
    r = fn.r
    lines = []

    def put(depth, s):
        lines.append("  " * depth + s)

    def go(nodes, d):
        for n in nodes:
            k = n.k
            if k == "label":
                if n.addr in need:
                    lines.append("L_%x:" % n.addr)
            elif k == "stmt":
                put(d, n.a)
            elif k == "goto":
                put(d, "goto L_%x;" % n.addr)
            elif k in ("break", "continue"):
                put(d, k + ";")
            elif k == "ret":
                put(d, "return %s;" % n.a if n.a else "return;")
            elif k == "if":
                c = r(n.a)
                if not n.c and len(n.b) == 1 and n.b[0].k in ("goto", "break", "continue", "ret"):
                    sub_ = []
                    m = n.b[0]
                    one = {"goto": "goto L_%x;" % (m.addr or 0), "break": "break;", "continue": "continue;",
                           "ret": ("return %s;" % m.a) if m.a else "return;"}[m.k]
                    put(d, "if (%s) %s" % (c, one))
                    continue
                put(d, "if (%s) {" % c)
                go(n.b, d + 1)
                if n.c:
                    if len(n.c) == 1 and n.c[0].k == "if":
                        put(d, "} else")
                        go(n.c, d)
                        continue
                    put(d, "} else {")
                    go(n.c, d + 1)
                put(d, "}")
            elif k == "loop":
                put(d, "while (1) {")
                go(n.a, d + 1)
                put(d, "}")
            elif k == "dowhile":
                put(d, "do {")
                go(n.a, d + 1)
                put(d, "} while (%s);" % r(n.b))
            elif k == "switch":
                put(d, "switch (%s) {" % r(n.a))
                for ns, body in n.b:
                    if len(body) == 1 and body[0].k == "goto":
                        put(d + 1, "case %s: goto L_%x;" % (ranges(ns), body[0].addr))
                    else:
                        put(d + 1, "case %s:" % ranges(ns))
                        go(body, d + 2)
                put(d, "}")
    go(tree, 1)
    return lines


def pseudo(g, entry):
    """(header lines, body lines, the Fn) of the function at entry"""
    fn = Fn(g, entry)
    tree, need = structure(fn)
    tree = simplify(tree)
    lines = text(fn, tree, need)
    return fn, lines
