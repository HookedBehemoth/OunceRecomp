"""simplify: a meshtool flat print (<prefix>.flat.c) read back as expressions and printed as C-like code with the
translation's noise taken out, for reading a game mesh stage line by line before rewriting it.

    work/venv/bin/python -I tools/shaders/simplify.py <prefix>.flat.c > <prefix>.simple.c

What is folded (each a pattern of the Ampere-to-Maxwell lowering or of the translator, not of the game's logic):
- XMAD multiplies: ((a.lo*b.hi + a.hi*b.lo) << 16) + a.lo*b.lo  ->  a * b   (IMAD lowered to 16-bit halves)
- the uniform-register loads: ShuffleIndex(x, FindUMsb(ballot & -ballot)) (the first active lane's value)  ->  x
- 64-bit funnel shifts: Unpack(Pack(lo, hi) >> min(s, 32|63)).lo  ->  funnel(lo, hi, s)
- the storage buffer word: LoadStorage32(i, (c0[e] + off) - (c0[e] & ~3))  ->  sbuf<i>[off]
- the condition-code emulation (SelectU1 over Void(), empty ifs)  ->  dropped
- constants folded where both operands are numbers; single-use values inlined
"""
import re
import sys

TOKEN = re.compile(r"\s*(?:(?P<num>0x[0-9a-f]+(?![0-9a-z])|-?\d+(?:\.\d+)?(?:e[-+]?\d+)?f?)|(?P<id>v\d+(?:@B\d+)?)|(?P<name>[A-Za-z_][A-Za-z0-9_]*)|(?P<p>[(),]))")


def parse_expr(s, i=0):
    m = TOKEN.match(s, i)
    if not m:
        raise ValueError("bad expression at %r" % s[i:i + 40])
    i = m.end()
    if m["num"]:
        t = m["num"]
        if not t.startswith("0x") and t.endswith("f"):
            return ("f", float(t[:-1])), i
        return ("n", int(t, 16) if t.startswith("0x") else int(t)), i
    if m["id"]:
        return ("v", m["id"].split("@")[0]), i
    name = m["name"]
    m2 = TOKEN.match(s, i)
    if not m2 or m2["p"] != "(":
        return ("n", {"true": 1, "false": 0}.get(name, name)), i
    i = m2.end()
    args = []
    while True:
        m3 = TOKEN.match(s, i)
        if m3 and m3["p"] == ")":
            i = m3.end()
            break
        a, i = parse_expr(s, i)
        args.append(a)
        m3 = TOKEN.match(s, i)
        if m3 and m3["p"] == ",":
            i = m3.end()
    return (name, *args), i


def main():
    lines = re.sub(r"@B\d+", "", open(sys.argv[1]).read()).splitlines()   # (a phi's incoming blocks: dropped)
    defs, order, uses = {}, [], {}
    stmts = []   # (indent, kind, payload)
    for line in lines:
        ind = len(line) - len(line.lstrip())
        t = line.strip()
        m = re.match(r"(v\d+) = (.*)$", t)
        if m:
            e, _ = parse_expr(m[2])
            defs[m[1]] = e
            stmts.append((ind, "def", m[1]))
        elif t.startswith("if (") or t.startswith("} while") or t.startswith("loop"):
            stmts.append((ind, "raw", t))
        elif t in ("}", "return;") or t.startswith("//"):
            stmts.append((ind, "raw", t))
        elif t:
            e, _ = parse_expr(t)
            stmts.append((ind, "eff", e))

    def walk(e, f):
        if isinstance(e, tuple) and e[0] not in ("n", "f", "v"):
            for a in e[1:]:
                walk(a, f)
        f(e)

    def count(e):
        if e[0] == "v":
            uses[e[1]] = uses.get(e[1], 0) + 1
    for k, e in defs.items():
        walk(e, count)
    for ind, kind, p in stmts:
        if kind == "eff":
            walk(p, count)

    def get(e):
        return defs.get(e[1], e) if e[0] == "v" else e

    def num(e):
        e = get(e)
        return e[1] if e[0] == "n" and isinstance(e[1], int) else None

    def is_op(e, name):
        e = get(e)
        return e[0] == name

    def bfe(e, off):   # BitFieldUExtract(x, off, 16) -> x, or a number's half
        e = get(e)
        if e[0] == "BitFieldUExtract" and num(e[2]) == off and num(e[3]) == 16:
            return e[1]
        if e[0] == "n" and isinstance(e[1], int):
            return None
        return None

    def halves(e):   # IMul32(BFE(a,i,16), BFE(b,j,16)) -> (a, i, b, j); a number counts as its own halves
        e = get(e)
        if e[0] != "IMul32":
            return None
        out = []
        for x in e[1:3]:
            x = get(x)
            if x[0] == "BitFieldUExtract" and num(x[3]) == 16 and num(x[2]) in (0, 16):
                out.append((x[1], num(x[2])))
            elif x[0] == "n" and isinstance(x[1], int):
                out.append((("n", x[1]), 0))
            else:
                return None
        return out

    def key(e):
        return repr(e)

    # simplify: returns a new expression tree
    memo = {}

    def S(e):
        if e[0] == "v":
            return e
        if e[0] in ("n", "f"):
            return e
        k = key(e)
        if k in memo:
            return memo[k]
        args = [S(a) for a in e[1:]]
        e2 = (e[0], *args)
        r = rewrite(e2)
        memo[k] = r
        return r

    def deref(e):
        while e[0] == "v" and e[1] in defs and uses.get(e[1], 0) <= 1:
            e = defs[e[1]]
        return e

    def rewrite(e):
        op = e[0]
        a = e[1:]
        # (the first active lane's value: the uniform datapath's loads)
        if op == "ShuffleIndex":
            idx = deref(a[1])
            if idx[0] == "FindUMsb32":
                return a[0]
        # (numbers)
        if op in ("IAdd32", "ISub32", "IMul32", "BitwiseAnd32", "BitwiseOr32", "BitwiseXor32", "ShiftLeftLogical32", "ShiftRightLogical32", "UMin32"):
            x, y = num(a[0]), num(a[1])
            if x is not None and y is not None:
                f = {"IAdd32": lambda p, q: p + q, "ISub32": lambda p, q: p - q, "IMul32": lambda p, q: p * q, "BitwiseAnd32": lambda p, q: p & q,
                     "BitwiseOr32": lambda p, q: p | q, "BitwiseXor32": lambda p, q: p ^ q, "ShiftLeftLogical32": lambda p, q: p << q,
                     "ShiftRightLogical32": lambda p, q: p >> q, "UMin32": min}[op]
                return ("n", f(x, y) & 0xffffffff)
        if op == "BitwiseNot32" and num(a[0]) is not None:
            return ("n", ~num(a[0]) & 0xffffffff)
        if op == "BitFieldUExtract" and num(a[0]) is not None and num(a[1]) is not None and num(a[2]) is not None:
            return ("n", (num(a[0]) >> num(a[1])) & ((1 << num(a[2])) - 1))
        # (XMAD: a.lo*b.lo + ((a.lo*b.hi + a.hi*b.lo) << 16) [+ c])
        if op == "IAdd32":
            for s_, rest in ((a[0], a[1]), (a[1], a[0])):
                sh = deref(s_)
                if sh[0] == "ShiftLeftLogical32" and num(sh[2]) == 16:
                    inner = deref(sh[1])
                    if inner[0] == "IAdd32":
                        h1, h2 = halves(inner[1]), halves(inner[2])
                        r = deref(rest)
                        lo_terms = [(r, None)]
                        if r[0] == "IAdd32":
                            lo_terms = [(deref(r[1]), r[2]), (deref(r[2]), r[1])]
                        for lo, extra in lo_terms:
                            h0 = halves(lo)
                            if h1 and h2 and h0:
                                # h0 = a.lo*b.lo; the high part: a.lo*b.hi + a.hi*b.lo, either order
                                (pa, ia), (pb, ib) = h0
                                if ia == 0 and ib == 0:
                                    want = {key((pa, 0, pb, 16)), key((pa, 16, pb, 0))}
                                    got = {key((h1[0][0], h1[0][1], h1[1][0], h1[1][1])), key((h2[0][0], h2[0][1], h2[1][0], h2[1][1]))}
                                    got2 = {key((h1[1][0], h1[1][1], h1[0][0], h1[0][1])), key((h2[1][0], h2[1][1], h2[0][0], h2[0][1]))}
                                    # (a number's high half is 0: its term is missing)
                                    if want <= got | got2 or len(want & (got | got2)) == 1:
                                        m = ("Mul", pa, pb)
                                        return m if extra is None else ("IAdd32", m, extra)
            # (a number's high half: IAdd32(ShiftLeft(IMul(BFE(a,16,16), n), 16), a.lo*n + c))
        # (64-bit funnel shifts)
        if op == "CompositeExtractU32x2":
            u = deref(a[0])
            if u[0] == "UnpackUint2x32":
                sh = deref(u[1])
                if sh[0] in ("ShiftRightLogical64", "ShiftLeftLogical64"):
                    p = deref(sh[1])
                    amt = deref(sh[2])
                    if amt[0] == "UMin32":
                        amt = amt[1]
                    if p[0] == "PackUint2x32":
                        c = deref(p[1])
                        if c[0] == "CompositeConstructU32x2":
                            name = "funnel_r" if sh[0] == "ShiftRightLogical64" else "funnel_l"
                            return (name + ("_lo" if num(a[1]) == 0 else "_hi"), c[1], c[2], amt)
        # (a storage buffer's word, relative to its bound address)
        if op == "LoadStorage32" or op == "LoadStorage128" or op == "LoadStorage64":
            addr = deref(a[1])
            if addr[0] == "ISub32":
                base = deref(addr[1])
                if base[0] == "IAdd32":
                    for x, off in ((base[1], base[2]), (base[2], base[1])):
                        xx = deref(x)
                        if xx[0] == "CompositeExtractU32x2" or (xx[0] == "GetCbufU32"):
                            return ("sbuf%d" % num(a[0]) + ("" if op == "LoadStorage32" else "_" + op[11:]), off)
                if base[0] == "CompositeExtractU32x2":
                    return ("sbuf%d" % num(a[0]) + ("" if op == "LoadStorage32" else "_" + op[11:]), ("n", 0))
        if op == "SelectU1" and deref(a[0])[0] == "Void":
            return ("CC?",)
        return e

    names = {}

    def P(e, top=False):
        if e[0] == "v":
            v = e[1]
            if v in defs and uses.get(v, 0) <= 1 and v in simple and simple[v][0] not in ("Phi",):
                return P(simple[v])
            return v
        if e[0] == "n":
            x = e[1]
            if isinstance(x, int):
                return str(x) if x < 4096 else hex(x)
            return str(x)
        if e[0] == "f":
            return repr(e[1]) + "f"
        op, a = e[0], e[1:]
        bin_ = {"IAdd32": "+", "ISub32": "-", "Mul": "*", "IMul32": "*", "BitwiseAnd32": "&", "BitwiseOr32": "|", "BitwiseXor32": "^",
                "ShiftLeftLogical32": "<<", "ShiftRightLogical32": ">>", "FPAdd32": "+", "FPMul32": "*", "ULessThan": "<", "UGreaterThan": ">",
                "IEqual": "==", "INotEqual": "!=", "UGreaterThanEqual": ">=", "ULessThanEqual": "<=", "SLessThan": "<s", "SGreaterThan": ">s"}
        if op in bin_ and len(a) == 2:
            return "(%s %s %s)" % (P(a[0]), bin_[op], P(a[1]))
        if op == "GetCbufU32":
            return "c%s.u[%s]" % (P(a[0]), P(a[1]))
        if op == "GetCbufF32":
            return "c%s.f[%s]" % (P(a[0]), P(a[1]))
        if op.startswith("sbuf"):
            return "%s[%s]" % (op, P(a[0]))
        if op == "LogicalNot":
            return "!" + P(a[0])
        if op == "BitwiseNot32":
            return "~" + P(a[0])
        if op == "FPFma32":
            return "fma(%s, %s, %s)" % (P(a[0]), P(a[1]), P(a[2]))
        if op == "BitCastF32U32":
            return "f(%s)" % P(a[0])
        if op == "BitCastU32F32":
            return "u(%s)" % P(a[0])
        return "%s(%s)" % (op, ", ".join(P(x) for x in a))

    simple = {}
    for v, e in defs.items():
        simple[v] = S(e)
    # (uses again, after the folding)
    uses.clear()
    for v, e in simple.items():
        walk(e, count)
    for ind, kind, p in stmts:
        if kind == "eff":
            walk(S(p), count)
    out = []
    for ind, kind, p in stmts:
        pad = " " * ind
        if kind == "raw":
            out.append(pad + p)
        elif kind == "def":
            e = simple[p]
            if uses.get(p, 0) == 0 and e[0] not in ("Phi",):
                continue
            if uses.get(p, 0) == 1 and e[0] != "Phi":
                continue
            if e[0] == "CC?":
                continue
            out.append("%s%s = %s" % (pad, p, P(e)))
        else:
            out.append(pad + P(S(p)))
    # (empty ifs of the condition codes)
    text = "\n".join(out)
    prev = None
    while prev != text:
        prev = text
        text = re.sub(r"\n( *)if \([^\n]*\) \{\n\1  // B\d+\n\1\}", "", text)
    print(text)


if __name__ == "__main__":
    sys.setrecursionlimit(100000)
    main()
