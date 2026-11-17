"""The host's patch declarations, read from its C source (host/patch.h says how each is written; docs/patches.md).

A declaration is a macro of host/patch.h at file scope: the lifter reads it with a token scanner of a fixed grammar
(comments, string and character literals, line continuations and nested parentheses handled; no C parser, no regular
expression over the source), and the compiler expands the same text into the host function's prototype and a record
in the object section `hos_patch`, which the build compares with the scan (hookshot-builder, patch::check_records). The scan
stops with file:line on a malformed declaration, on one inside conditional compilation or made by another macro, on a
duplicate site, and on an unknown register or kind.

Kinds:
  PATCH_AT(pc, word(W), mode, result, fn, operand...) { body }    mode: after | before | instead
  PATCH_AT_SITE(pc, word(W), mode, result, fn, operand...);        another site of fn, checked against its prototype
  PATCH_FUNC(addr, word(W), body(H), aapcs, result, fn, [orig,] [guest(A, ...),] operand...) { body }
                                                                   the function at addr replaced by fn
  PATCH_FUNC_SYM(symbol, aapcs, result, fn, [orig,] [guest(A, ...),] operand...) { body }
                                                                   the same, the function an export of the image
  HLE(symbol, effect...) { body }                                  the function symbol replaced (imp_<symbol>)
  NV(name, effect...) { body }                                     an NVN proc (n_<name>), the function name too
  HLE_EFFECTS(symbol, effect...);                                  the effects of a replaced function written elsewhere
  PATCH_ANSWER(symbol, value, "why");                              the function symbol replaced by a constant answer in x0
operands: xN wN sN qN sp ld(xN|sp, K) add(xN, K) k(K) as(<operand>, name) guest(A, ...) | guest(computed) io(r, ...)
results: none xN wN sN qN
effects (HLE, NV, HLE_EFFECTS): reads(reg..., "why") (registers beyond an HLE'd function's AAPCS arguments);
  clobbers("why") (a call of it keeps no register)

An `instead` site on a BL or BLR replaces the call (an At row, replace), on a BR a tail call (the function returns after
the row); on another instruction it replaces the
instruction's result (a Value probe: its result is the instruction's destination). The containing function is not
written: resolve() finds it from the plan, and a site in no function or in more than one stops the build.
"""
import os

KINDS = ("PATCH_AT", "PATCH_AT_SITE", "PATCH_FUNC", "PATCH_FUNC_SYM", "HLE", "NV", "HLE_EFFECTS", "PATCH_ANSWER")
FUNCS = ("PATCH_FUNC", "PATCH_FUNC_SYM")
SITES = ("PATCH_AT", "PATCH_AT_SITE")
REPLACED = ("HLE", "NV", "HLE_EFFECTS")
MODES = ("after", "before", "instead")
HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(os.path.dirname(os.path.dirname(HERE)), "host")


class ScanError(SystemExit):
    pass


def fail(where, msg):
    raise ScanError("patch declarations: %s:%d: %s" % (where[0], where[1], msg))


# ---- tokens
PUNCT3 = ("<<=", ">>=", "...")
PUNCT2 = ("->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=",
          "^=", "##")


def tokens(text, path):
    """[(kind, text, line, bol)]: kind "id", "num", "str", "chr", "p" (punctuator), "pp" (a preprocessor directive: its
    text, comments out, continuations joined); bol: first token of its line. Comments and line continuations go."""
    out = []
    i, n, line, bol = 0, len(text), 1, True

    def splice(j):   # (past backslash-newline pairs)
        nonlocal line
        while text.startswith("\\\n", j):
            j += 2
            line += 1
        return j
    while i < n:
        i = splice(i)
        if i >= n:
            break
        c = text[i]
        if c == "\n":
            line += 1
            bol = True
            i += 1
            continue
        if c in " \t\r\f\v":
            i += 1
            continue
        if text.startswith("//", i):
            while i < n and text[i] != "\n":
                i = splice(i + 1) if text[i] != "\\" else (splice(i) if text.startswith("\\\n", i) else i + 1)
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                fail((path, line), "a comment that does not end")
            line += text.count("\n", i, j)
            i = j + 2
            continue
        start = line
        if c == "#" and bol:   # (a directive: to the end of its logical line, comments dropped)
            buf, i = [], i + 1
            while i < n and text[i] != "\n":
                if text.startswith("\\\n", i):
                    i = splice(i)
                    buf.append(" ")
                    continue
                if text.startswith("/*", i):
                    j = text.find("*/", i + 2)
                    if j < 0:
                        fail((path, line), "a comment that does not end")
                    line += text.count("\n", i, j)
                    i = j + 2
                    buf.append(" ")
                    continue
                if text.startswith("//", i):
                    while i < n and text[i] != "\n":
                        i += 1
                    break
                buf.append(text[i])
                i += 1
            out.append(("pp", "".join(buf).strip(), start, True))
            bol = False
            continue
        if c in "\"'":
            j = i + 1
            while True:
                if j >= n or text[j] == "\n":
                    fail((path, start), "a literal that does not end")
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == c:
                    break
                j += 1
            out.append(("str" if c == '"' else "chr", text[i:j + 1], start, bol))
            i = j + 1
        elif c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            out.append(("id", text[i:j], start, bol))
            i = j
        elif c.isdigit() or (c == "." and i + 1 < n and text[i + 1].isdigit()):
            j = i
            while j < n and (text[j].isalnum() or text[j] in "._" or (text[j] in "+-" and text[j - 1] in "eEpP")):
                j += 1
            out.append(("num", text[i:j], start, bol))
            i = j
        else:
            p = next((q for q in PUNCT3 + PUNCT2 if text.startswith(q, i)), c)
            out.append(("p", p, start, bol))
            i += len(p)
        bol = False
    return out


# ---- declarations
class Decl:
    """one declaration: kind, its arguments (each a token list), where (file, line)"""
    def __init__(self, kind, args, where):
        self.kind, self.args, self.where = kind, args, where

    def canonical(self):
        """the declaration as its tokens, for the comparison with the object records"""
        return canonical(self.kind, self.args)


def canonical(kind, args):
    return kind + "(" + ",".join(" ".join(t[1] for t in a) for a in args) + ")"


def declarations(text, path):
    """[Decl] of one file's source"""
    toks = tokens(text, path)
    out, depth, cond = [], 0, []
    i = 0
    while i < len(toks):
        kind, val, line, _ = toks[i]
        if kind == "pp":
            word = val.split(None, 1)[0] if val else ""
            if word in ("if", "ifdef", "ifndef"):
                cond.append(line)
            elif word == "endif":
                if not cond:
                    fail((path, line), "#endif without #if")
                cond.pop()
            elif word == "define":
                w = _words(val)
                if len(w) > 2 and w[1] not in KINDS and set(w[2:]) & set(KINDS):
                    fail((path, line), "a declaration made by another macro (#define %s)" % w[1])
            i += 1
            continue
        if kind == "p":
            depth += {"{": 1, "}": -1}.get(val, 0)
            i += 1
            continue
        if kind == "id" and val in KINDS:
            where = (path, line)
            if depth:
                fail(where, "%s inside a function or an initializer: declarations stand at file scope" % val)
            if cond:
                fail(where, "%s inside conditional compilation (#if at line %d)" % (val, cond[-1]))
            if i + 1 >= len(toks) or toks[i + 1][1] != "(":
                fail(where, "%s without its arguments" % val)
            args, cur, par, j = [], [], 0, i + 2
            while True:
                if j >= len(toks):
                    fail(where, "%s's arguments do not end" % val)
                t = toks[j]
                if t[0] == "pp":
                    fail(where, "a directive inside %s's arguments" % val)
                if t[1] == "(":
                    par += 1
                elif t[1] == ")":
                    if par == 0:
                        break
                    par -= 1
                if t[1] == "," and par == 0:
                    args.append(cur)
                    cur = []
                else:
                    cur.append(t)
                j += 1
            if cur or args:
                args.append(cur)
            out.append(Decl(val, args, where))
            i = j + 1
            continue
        i += 1
    if cond:
        fail((path, cond[-1]), "#if without #endif")
    return out


def _words(s):
    w, cur = [], ""
    for c in s:
        if c.isalnum() or c == "_":
            cur += c
        else:
            if cur:
                w.append(cur)
            cur = ""
    if cur:
        w.append(cur)
    return w


# ---- the grammar
def _regs(prefix, n):
    return {"%s%d" % (prefix, i) for i in range(n)}


XREG, WREG, SREG, QREG, VREG = _regs("x", 31), _regs("w", 31), _regs("s", 32), _regs("q", 32), _regs("v", 32)
RESULTS = XREG | WREG | SREG | QREG | {"none"}


def _num(arg, where, what):
    if len(arg) != 1 or arg[0][0] != "num":
        fail(where, "%s: a number, not %r" % (what, " ".join(t[1] for t in arg)))
    try:
        return int(arg[0][1].rstrip("uUlL"), 0)
    except ValueError:
        fail(where, "%s: not a number: %s" % (what, arg[0][1]))


def _call(arg, name):
    """the arguments of `name(...)` written as one argument, or None"""
    if len(arg) < 3 or arg[0][1] != name or arg[1][1] != "(" or arg[-1][1] != ")":
        return None
    inner, cur, par = [], [], 0
    for t in arg[2:-1]:
        if t[1] == "(":
            par += 1
        elif t[1] == ")":
            par -= 1
        if t[1] == "," and par == 0:
            inner.append(cur)
            cur = []
        else:
            cur.append(t)
    inner.append(cur)
    return inner


def _ident(arg, where, what):
    if len(arg) != 1 or arg[0][0] != "id":
        fail(where, "%s: a name, not %r" % (what, " ".join(t[1] for t in arg)))
    return arg[0][1]


def operand(arg, where):
    """("arg", the row's argument string) | ("guest", tuple or "computed") | ("io", (regs,))"""
    for name in ("as",):
        inner = _call(arg, name)
        if inner is not None:
            if len(inner) != 2:
                fail(where, "as(<operand>, name)")
            _ident(inner[1], where, "as's name")
            o = operand(inner[0], where)
            if o[0] != "arg":
                fail(where, "as() names a value operand, not %s" % o[0])
            return o
    inner = _call(arg, "ld")
    if inner is not None:
        if len(inner) != 2:
            fail(where, "ld(xN or sp, K)")
        base = _ident(inner[0], where, "ld's base")
        if base not in XREG and base != "sp":
            fail(where, "ld's base: xN or sp, not %s" % base)
        return ("arg", "[%s+%s]" % (base, inner[1][0][1] if len(inner[1]) == 1 else _num(inner[1], where, "ld's offset")))
    inner = _call(arg, "add")
    if inner is not None:
        if len(inner) != 2:
            fail(where, "add(xN, K)")
        base = _ident(inner[0], where, "add's base")
        if base not in XREG:
            fail(where, "add's base: xN, not %s" % base)
        return ("arg", "%s+%d" % (base, _num(inner[1], where, "add's offset")))
    inner = _call(arg, "k")
    if inner is not None:
        if len(inner) != 1:
            fail(where, "k(K)")
        _num(inner[0], where, "k's value")
        return ("arg", "#" + inner[0][0][1])
    inner = _call(arg, "guest")
    if inner is not None:
        if len(inner) == 1 and len(inner[0]) == 1 and inner[0][0][1] == "computed":
            return ("guest", "computed")
        return ("guest", tuple(_num(a, where, "a guest callee") for a in inner))
    inner = _call(arg, "io")
    if inner is not None:
        regs = tuple(_ident(a, where, "an io register") for a in inner)
        bad = [r for r in regs if r not in XREG | WREG | QREG]
        if bad:
            fail(where, "io registers: xN, wN or qN, not %s" % ", ".join(bad))
        return ("io", regs)
    r = _ident(arg, where, "an operand")
    if r not in XREG | WREG | SREG | QREG and r != "sp":
        fail(where, "unknown operand %s" % r)
    return ("arg", r)


class Site:
    """a PATCH_AT / PATCH_AT_SITE declaration, parsed: pc, word, mode, result (None), fn, args, guest, io, where"""
    def __init__(self, d):
        a, where = d.args, d.where
        if len(a) < 5:
            fail(where, "%s(pc, word(W), mode, result, fn, operand...)" % d.kind)
        self.kind, self.where, self.decl = d.kind, where, d
        self.pc = _num(a[0], where, "pc")
        w = _call(a[1], "word")
        if w is None or len(w) != 1:
            fail(where, "the instruction word is written word(0x...), not %r" % " ".join(t[1] for t in a[1]))
        self.word = _num(w[0], where, "word")
        self.mode = _ident(a[2], where, "mode")
        if self.mode not in MODES:
            fail(where, "mode: after, before or instead, not %s" % self.mode)
        res = _ident(a[3], where, "result")
        if res not in RESULTS:
            fail(where, "unknown result %s" % res)
        self.result = None if res == "none" else res
        self.fn = _ident(a[4], where, "the host function")
        self.args, self.guest, self.io = [], None, ()
        ops = [operand(x, where) for x in a[5:] if x]
        for k, (what, v) in enumerate(ops):
            if what == "guest":
                if k != 0:
                    fail(where, "guest(...) comes first")
                self.guest = v
            elif what == "io":
                if k != len(ops) - 1:
                    fail(where, "io(...) comes last")
                if d.kind == "PATCH_AT_SITE":
                    fail(where, "io(...) only in the PATCH_AT of its function")
                self.io = v
            else:
                self.args.append(v)


def ctype(a):
    """an operand's or result's C type (host/patch.h): xN, sp, ld, add uint64_t; wN, k uint32_t; sN float; qN hos_q"""
    if a is None:
        return "void"
    if a[0] in "wsq" and a[1:].isdigit() or a[0] == "#":
        return {"w": "uint32_t", "s": "float", "q": "hos_q", "#": "uint32_t"}[a[0]]
    return "uint64_t"


def signature(s):
    """a site's C function type: its result's and operands' types (a further site must have its function's)"""
    return (ctype(s.result), s.guest is not None, tuple(ctype(a) for a in s.args), s.io)


def host_sources():
    """the host's C files the build compiles and the scan reads: the game description's"""
    import gamedesc
    return gamedesc.current().host_sources()


def _effects(args, where, allowed):
    """(reads, why, guest, clobbers) of a declaration's effect clauses: reads(reg..., "why"), guest(A, ...) or
    guest(computed), clobbers("why"), each at most once and only those allowed"""
    out = {}
    for a in args:
        name = a[0][1] if a and a[0][0] == "id" else None
        inner = _call(a, name) if name else None
        if inner is None or name not in allowed:
            fail(where, "an effect: %s, not %r" % (" or ".join("%s(...)" % k for k in allowed),
                                                    " ".join(t[1] for t in a)))
        if name in out:
            fail(where, "%s(...) twice" % name)
        if name == "reads":
            if len(inner) < 2 or len(inner[-1]) != 1 or inner[-1][0][0] != "str":
                fail(where, 'reads(reg..., "why"): the registers, then the reason as a string')
            regs = tuple(_ident(r, where, "a register read") for r in inner[:-1])
            bad = [r for r in regs if r not in XREG | VREG | {"sp"}]
            if bad:
                fail(where, "reads: xN, vN or sp, not %s" % ", ".join(bad))
            out[name] = (regs, inner[-1][0][1])
        elif name == "clobbers":
            if len(inner) != 1 or len(inner[0]) != 1 or inner[0][0][0] != "str":
                fail(where, 'clobbers("why"): the reason as a string')
            out[name] = inner[0][0][1]
        else:
            out[name] = operand(a, where)[1]
    return out.get("reads", ((), None)), out.get("guest", ()), out.get("clobbers")


# a PATCH_FUNC's operands (host/patch.h HOS_A_): the AAPCS argument registers, x8, the return address, SP
FUNC_OPERANDS = _regs("x", 9) | _regs("w", 9) | _regs("s", 8) | _regs("q", 8) | {"x30", "sp"}
FUNC_RESULTS = ("none", "x0", "w0", "s0", "q0")


def body_hash(words):
    """a function's body as PATCH_FUNC's body(H) names it: the first 64 bits of the SHA-1 of its instruction words (in
    address order, 32-bit little-endian)"""
    import hashlib
    import struct
    return int(hashlib.sha1(struct.pack("<%dI" % len(words), *words), usedforsecurity=False).hexdigest()[:16], 16)


class Func:
    """a PATCH_FUNC / PATCH_FUNC_SYM declaration: addr, word and body hash (by address) or symbol; result (None); fn;
    orig (the host function may call the lifted body); guest (the guest functions it calls); args (operand registers,
    in order); the C names of its adapter (hos_func_<key>) and of its lifted body (hos_orig_<key>); where"""
    def __init__(self, d):
        a, where = d.args, d.where
        self.kind, self.where, self.decl = d.kind, where, d
        self.addr = self.word = self.body = self.symbol = None
        if d.kind == "PATCH_FUNC":
            if len(a) < 6:
                fail(where, "PATCH_FUNC(addr, word(W), body(H), aapcs, result, fn, operand...)")
            self.addr = _num(a[0], where, "the function's address")
            w = _call(a[1], "word")
            if w is None or len(w) != 1:
                fail(where, "the function's first word is written word(0x...), not %r" % " ".join(t[1] for t in a[1]))
            self.word = _num(w[0], where, "word")
            b = _call(a[2], "body")
            if b is None or len(b) != 1:
                fail(where, "the function's body hash is written body(0x...), not %r" % " ".join(t[1] for t in a[2]))
            self.body = _num(b[0], where, "body")
            key = a[0][0][1]
            rest = a[3:]
        else:
            if len(a) < 4 or not a[0]:
                fail(where, "PATCH_FUNC_SYM(symbol, aapcs, result, fn, operand...)")
            self.symbol = _ident(a[0], where, "the symbol")
            key = "sym_" + self.symbol
            rest = a[1:]
        self.name, self.orig_name = "hos_func_" + key, "hos_orig_" + key
        abi = _ident(rest[0], where, "abi")
        if abi != "aapcs":
            fail(where, "abi: aapcs, not %s (a function of the context is an HLE(...))" % abi)
        res = _ident(rest[1], where, "result")
        if res not in FUNC_RESULTS:
            fail(where, "a function's result: %s, not %s" % (", ".join(FUNC_RESULTS), res))
        self.result = None if res == "none" else res
        self.fn = _ident(rest[2], where, "the host function")
        self.orig, self.guest, self.args = False, (), []
        ops = [x for x in rest[3:] if x]
        if len(ops) > 16:
            fail(where, "%d operands: host/patch.h takes at most 16" % len(ops))
        for k, x in enumerate(ops):
            if len(x) == 1 and x[0][1] == "orig":
                if k != 0:
                    fail(where, "orig comes first")
                self.orig = True
                continue
            what, v = operand(x, where)
            if what == "guest":
                if k != int(self.orig):
                    fail(where, "guest(...) comes first (after orig)")
                if v == "computed":
                    fail(where, "a function names the guest functions it calls")
                self.guest = v
            elif what == "io":
                fail(where, "io(...) is a PATCH_AT's: a function returns its result")
            elif v not in FUNC_OPERANDS:
                fail(where, "a function's operand: x0-x8, w0-w8, s0-s7, q0-q7, x30 or sp, not %s" % v)
            else:
                self.args.append(v)
        if self.guest and "sp" not in self.args:
            fail(where, "guest(...) runs on the call's stack: sp is an operand (guest_call's sp)")

    def reads(self):
        """the guest variables the function reads (its operands)"""
        from . import probes
        return {probes._var(r) for r in self.args}


class Replaced:
    """an HLE / NV / HLE_EFFECTS declaration: the symbol (an NVN proc's name), the registers a call of it reads beyond
    the AAPCS arguments, clobbers (the reason a call of it keeps no register, or None), where"""
    def __init__(self, d):
        a, where = d.args, d.where
        self.kind, self.where, self.decl = d.kind, where, d
        if not a or not a[0]:
            fail(where, "%s(symbol, effect...)" % d.kind)
        self.symbol = _ident(a[0], where, "the symbol")
        (self.reads, self.why), _, self.clobbers = _effects([x for x in a[1:] if x], where, ("reads", "clobbers"))
        if d.kind == "HLE_EFFECTS" and not self.reads and self.clobbers is None:
            fail(where, "HLE_EFFECTS without an effect")


class Answer:
    """a PATCH_ANSWER declaration: the symbol, the value its replacement leaves in x0, the reason, where"""
    def __init__(self, d):
        a, where = d.args, d.where
        self.kind, self.where, self.decl = d.kind, where, d
        if len(a) != 3:
            fail(where, 'PATCH_ANSWER(symbol, value, "why")')
        self.symbol = _ident(a[0], where, "the symbol")
        self.value = _num(a[1], where, "the answer")
        if len(a[2]) != 1 or a[2][0][0] != "str" or len(a[2][0][1]) <= 2:
            fail(where, 'PATCH_ANSWER(symbol, value, "why"): the reason as a string')
        self.why = a[2][0][1]


class PatchSet:
    """the scan's result: sites ([Site], declaration order), funcs ([Func]), replaced ({symbol: Replaced}: HLE and
    HLE_EFFECTS by symbol, NV by proc name), answers ({symbol: Answer})"""
    def __init__(self, sites=(), replaced=(), funcs=(), answers=()):
        self.sites, self.funcs = list(sites), list(funcs)
        self.answers = {a.symbol: a for a in answers}
        self.hle = {r.symbol: r for r in replaced if r.kind == "HLE"}
        self.nv = {r.symbol: r for r in replaced if r.kind == "NV"}
        self.declared = {r.symbol: r for r in replaced if r.kind == "HLE_EFFECTS"}

    def effects(self, symbol):
        """the declaration of the replaced function symbol (its HLE, HLE_EFFECTS or NV), or None"""
        return self.hle.get(symbol) or self.declared.get(symbol) or self.nv.get(symbol)

    def hle_symbols(self):
        """the symbols HLE(...) and PATCH_ANSWER(...) replace (imp_<symbol>)"""
        return set(self.hle) | set(self.answers)


def _read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def scan(paths=None):
    """the PatchSet of every declaration in the files (in their order, each file's in its order); SystemExit on an
    error"""
    root = os.path.dirname(HOST)
    return scan_texts((os.path.relpath(p, root) if os.path.abspath(p).startswith(root + os.sep) else p, _read(p))
                      for p in (paths if paths is not None else host_sources()))


def scan_text(text, path="t.c"):
    """the PatchSet of one source text (tests)"""
    return scan_texts([(path, text)])


def scan_texts(texts):
    """the PatchSet of [(file name, its text)]"""
    out, replaced, funcs, answers = [], [], [], []
    for rel, text in texts:
        if not any(k in text for k in KINDS):   # (a file without a declaration's name has none)
            continue
        for d in declarations(text, rel):
            if d.kind in SITES:
                out.append(Site(d))
            elif d.kind in FUNCS:
                funcs.append(Func(d))
            elif d.kind == "PATCH_ANSWER":
                answers.append(Answer(d))
            else:
                replaced.append(Replaced(d))
    first = {}
    for s in out:
        if s.kind == "PATCH_AT":
            if s.fn in first:
                fail(s.where, "a second PATCH_AT of %s (the first at %s:%d): further sites are PATCH_AT_SITE"
                     % ((s.fn,) + first[s.fn].where))
            first[s.fn] = s
    for s in out:
        if s.kind == "PATCH_AT_SITE":
            f = first.get(s.fn)
            if f is None or (f.where[0] == s.where[0] and f.where[1] > s.where[1]):
                fail(s.where, "PATCH_AT_SITE of %s before or without its PATCH_AT" % s.fn)
            if signature(f) != signature(s):
                fail(s.where, "PATCH_AT_SITE of %s: its result or operands differ from the PATCH_AT at %s:%d"
                     % ((s.fn,) + f.where))
    seen = {}
    for s in out:
        k = (s.pc, s.mode, s.fn, tuple(s.args), s.result)
        if k in seen:
            fail(s.where, "the same site as %s:%d" % seen[k])
        seen[k] = s.where
    seen = {}
    for h in funcs:
        k = h.addr if h.addr is not None else h.symbol
        if k in seen:
            fail(h.where, "a second declaration of %s (the first at %s:%d)" % ((h.name,) + seen[k]))
        seen[k] = h.where
    fns = {}
    for f in funcs:
        if f.fn in fns:
            fail(f.where, "%s replaces a second function (the first at %s:%d)" % ((f.fn,) + fns[f.fn]))
        fns[f.fn] = f.where
    seen = {}
    for r in replaced:
        k = (r.kind, r.symbol)
        if k in seen:
            fail(r.where, "%s declared again (the first at %s:%d)" % ((r.symbol,) + seen[k]))
        seen[k] = r.where
    hle = {r.symbol: r for r in replaced if r.kind == "HLE"}
    for r in replaced:
        if r.kind == "HLE_EFFECTS" and r.symbol in hle:
            fail(r.where, "HLE_EFFECTS of %s, which HLE declares at %s:%d: its effects go there"
                 % ((r.symbol,) + hle[r.symbol].where))
    seen = {}
    for a in answers:
        if a.symbol in seen:
            fail(a.where, "PATCH_ANSWER of %s again (the first at %s:%d)" % ((a.symbol,) + seen[a.symbol]))
        seen[a.symbol] = a.where
        other = hle.get(a.symbol) or next((r for r in replaced if r.kind == "HLE_EFFECTS" and r.symbol == a.symbol), None)
        if other is not None:
            fail(a.where, "PATCH_ANSWER of %s, which %s declares at %s:%d" % ((a.symbol, other.kind) + other.where))
    return PatchSet(out, replaced, funcs, answers)


_PATCHES = None


def patches():
    """the host's PatchSet (scanned on first use in a process), or the one use() gave"""
    global _PATCHES
    if _PATCHES is None:
        _PATCHES = scan()
    return _PATCHES


def use(ps):
    """ps (a test's PatchSet, e.g. scan_text's) in place of the host's declarations; returns the one before"""
    global _PATCHES
    before = patches()
    _PATCHES = ps
    from . import summary
    summary.FUNCS.clear()
    return before


def _exports(gen):
    import json
    meta = os.path.join(os.path.dirname(gen), "image", "image.json")
    if not os.path.exists(meta):
        return {}
    with open(meta) as f:
        return json.load(f).get("exports", {})


def funcs(gen, ps=None):
    """({address: Func}, [Func by a symbol the image does not export]): the PATCH_FUNC addresses, and the PATCH_FUNC_SYM
    symbols through the image's exports (<gen>/../image/image.json)"""
    ps = ps or patches()
    exports = _exports(gen) if any(f.symbol for f in ps.funcs) else {}
    out, missing = {}, []
    for f in ps.funcs:
        a = f.addr if f.symbol is None else exports.get(f.symbol)
        if a is None:
            missing.append(f)
        else:
            out[a] = f
    return out, missing


def func_errors(plan, gen, ps=None):
    """[(where, what is wrong)] of the PATCH_FUNCs against the plan: a symbol the image does not export, no lifted
    function at the address, a first word or body hash other than the declared one"""
    ps = ps or patches()
    found, missing = funcs(gen, ps)
    out = [(f.where, "%s: the image exports no %s" % (f.kind, f.symbol)) for f in missing]
    for a, f in sorted(found.items()):
        body = plan["bodies"].get(a)
        if body is None or body[0][0] != a:
            out.append((f.where, "%s at %#x: no lifted function there" % (f.kind, a)))
            continue
        if f.word is not None and body[1][0] != f.word:
            out.append((f.where, "PATCH_FUNC at %#x: the function's first word is %#010x, not %#010x"
                        % (a, body[1][0], f.word)))
        elif f.body is not None and body_hash(body[1]) != f.body:
            out.append((f.where, "PATCH_FUNC at %#x: the function's body is body(%#018x), not body(%#018x) (%d "
                        "instructions)" % (a, body_hash(body[1]), f.body, len(body[1]))))
    return out


def check_funcs(plan, gen, ps=None):
    """SystemExit with file:line on the first of func_errors; else {address: Func}"""
    ps = ps or patches()
    for where, msg in func_errors(plan, gen, ps)[:1]:
        fail(where, msg)
    return funcs(gen, ps)[0]


def param(arg, where):
    """(C type, parameter name) of one operand as host/patch.h makes it"""
    inner = _call(arg, "as")
    if inner is not None:
        return param(inner[0], where)[0], _ident(inner[1], where, "as's name")
    if _call(arg, "guest") is not None:
        return "GuestCall *", "g"
    if _call(arg, "io") is not None:
        return "struct hos_io_%d *" % where[1], "io"
    for name, ty in (("ld", "uint64_t"), ("add", "uint64_t"), ("k", "uint32_t")):
        inner = _call(arg, name)
        if inner is not None:
            return ty, "_".join([name] + [t[1] for a in inner for t in a])
    r = _ident(arg, where, "an operand")
    return ctype(r), r


def prototype(s):
    """the C prototype a PATCH_AT's macro generates for its function"""
    ps = [param(a, s.where) for a in s.decl.args[5:] if a]
    return "%s %s(%s)" % (ctype(s.result), s.fn, ", ".join(
        ty + ("" if ty.endswith("*") else " ") + name for ty, name in ps) or "void")


def rows(sites):
    """(Value probes, At rows) of the sites (nx2/lift2/probes.py), their functions not yet known (resolve)"""
    from . import probes
    values, ats = [], []
    for s in sites:
        call = probes.is_call(s.word) or probes.is_tail(s.word)
        if s.mode == "instead" and not call:
            if s.guest is not None or s.io:
                fail(s.where, "an instruction's result replaced (instead, not a call) takes no guest or io")
            if s.result is None:
                fail(s.where, "an instruction's result replaced (instead, not a call) names its destination as result")
            p = probes.Value(None, s.pc, s.word, s.fn, tuple(s.args), s.result)
            values.append(p)
        else:
            p = probes.At(None, s.pc, s.word, s.fn, s.args, s.result, before=s.mode == "before", io=s.io,
                          guest=s.guest, replace=s.mode == "instead")
            ats.append(p)
        p.where = s.where
    return values, ats


def containing(plan, pcs):
    """{pc: [entries of the functions whose body holds it]} for a few pcs"""
    import bisect
    bodies = plan["bodies"]
    entries = sorted(bodies)
    span = getattr(containing, "_span", {}).get(id(plan))
    if span is None:
        lo = {}
        for e in entries:
            p = bodies[e][0]
            lo[e] = (p[0], p[-1]) if len(p) else (e, e)
        width = max(hi - min(a, e) for e, (a, hi) in lo.items())
        span = (lo, width)
        containing._span = {id(plan): span}
    lo, width = span
    out = {}
    for pc in pcs:
        hits = []
        k = bisect.bisect_right(entries, pc + width)
        while k > 0:
            k -= 1
            e = entries[k]
            a, hi = lo[e]
            if e < pc - width:
                break
            if a <= pc <= hi:
                p = bodies[e][0]
                j = bisect.bisect_left(p, pc)
                if j < len(p) and p[j] == pc:
                    hits.append(e)
        out[pc] = sorted(hits)
    return out


# ---- the build's cross-check: the records the compiler made of the declarations against the scan
def _section(path, name=b"hos_patch"):
    """the bytes of an ELF object's sections of that name (an object without one: b"")"""
    import struct
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit("patch declarations: %s is not a 64-bit ELF object" % path)
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3a)
    sec = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + k * shentsize) for k in range(shnum)]
    strtab = sec[shstrndx]
    out = b""
    for h in sec:
        start = strtab[4] + h[0]
        if data[start:data.index(b"\0", start)] == name and h[1] != 8:   # (not SHT_NOBITS)
            out += data[h[4]:h[4] + h[5]]
    return out


def _key(text):
    """a declaration as tokens (a trailing comma before the closing parenthesis dropped: an empty operand list)"""
    vals = [t[1] for t in tokens(text, "<record>")]
    if len(vals) >= 2 and vals[-2] == ",":
        del vals[-2]
    return tuple(vals)


def check_records(objects, paths=None):
    """SystemExit unless the hos_patch records of the host's objects are the scanned declarations (as many, the same
    text as tokens): a declaration the compiler saw and the scan did not (or the reverse) cannot ship"""
    import collections
    import hashlib
    got = collections.Counter()
    for o in objects:
        for r in _section(o).split(b"\0"):
            if r:
                got[_key(r.decode("utf-8", "replace"))] += 1
    want = collections.Counter()
    for path in paths if paths is not None else host_sources():
        text = _read(path)
        if any(k in text for k in KINDS):
            for d in declarations(text, path):
                want[_key(d.canonical())] += 1
    if got != want:
        only_c = sorted(" ".join(k) for k in (got - want).elements())
        only_s = sorted(" ".join(k) for k in (want - got).elements())
        raise SystemExit("patch declarations: the compiled host's records differ from the scan: %d records, %d declarations"
                         "\n  compiled only: %s\n  scanned only: %s" % (sum(got.values()), sum(want.values()),
                                                                     "; ".join(only_c[:4]) or "-", "; ".join(only_s[:4]) or "-"))
    h = hashlib.sha1(repr(sorted(want.elements())).encode(), usedforsecurity=False).hexdigest()[:12]
    return "patch declarations: %d, the compiled host's records the same (%s)" % (sum(want.values()), h)
