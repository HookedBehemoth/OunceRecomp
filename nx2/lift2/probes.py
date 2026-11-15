"""Probes: host code at guest addresses, where the game's code needs the host (docs/lifter.md, "Hooks and probes").

The probes are declared in the host's C next to their bodies (host/patch.h PATCH_AT, PATCH_AT_SITE; read by
nx2/lift2/patch_scan.py): this module holds what they become, their checks and their lowering. Every probe names one
instruction and that instruction's word (its function is the plan's: resolve); a word that differs, or an
instruction missing, stops the build (another game version's code). Every probe declares the guest registers it reads
and writes: the host function takes them as plain values (or a typed struct) and returns what it changes, never the
guest context, so the compiler holds the C to its declaration and the lifter knows what each site needs (the values
stay SSA values; the summaries count a row's reads and writes as the instruction's: ROW_EFFECTS).
- Value(func, pc, word, fn, args): the instruction's result register gets fn(args) instead ("xN", "xN+K" for an
  address, "sN" for Sn as a float);
- At(func, pc, word, fn, args, result, before=False, io=(), guest=None, replace=False): fn(args) after the instruction
  (after a BL / BLR when it returns), or before it, or in place of a BL / BLR (replace: the call is not made; x30 is
  set as the call would), its result in a register; io: registers the host function reads and rewrites through a
  struct; guest: the host function calls guest code (declared callees) with every register as it was after.
"""
import re

from . import enc  # (first: its tables import asm)
from . import ir, sem
from .sem_int import getx, setx
from .sem_fp import vread, vwrite


class Value:
    def __init__(self, func, pc, word, fn, args, result):
        self.func, self.pc, self.word, self.fn, self.args = func, pc, word, fn, args
        self.result = result   # (the instruction's destination as declared: wN or xN of its Rd, its width: dest())

    def lower(self, b, insn, pc):
        vals = []
        for a in self.args:
            m = re.fullmatch(r"x(\d+)(?:\+(\d+))?", a)
            if m:
                v = getx(b, int(m.group(1)), 1)
                if m.group(2):
                    v = b.bin("add", v, b.const(int(m.group(2))))
                vals.append(v)
                continue
            m = re.fullmatch(r"s(\d+)", a)
            vals.append(vread(b, int(m.group(1)), "f32"))
        r = b.call(self.fn, "i32" if self.result[0] == "w" else "i64", *vals)
        setx(b, int(self.result[1:]), r)

    def effects(self):
        return {_var(a.split("+")[0]) for a in self.args}, set()


def dest(w):
    """(register number, bits) of the one general register the instruction word writes as its Rd, else None"""
    i = enc.decode(w)
    if i is None or "Rd" not in i.f or i.entry.template in TERMINATORS:
        return None
    t = i.entry.template
    fn = ir.Function(0)
    b = ir.Builder(fn)
    b.at(fn.block(0))
    sem.TEMPLATES[t](b, i.f, 0x1000, i.entry.params)
    rd = i.f["Rd"]
    if fn.written != {"x%d" % rd}:
        return None
    if "sf" in i.f:
        return rd, 64 if i.f["sf"] else 32
    v = fn.block(0).defs["x%d" % rd]
    return rd, 32 if v.op == "zext" and v.args[0].ty == "i32" else 64


TERMINATORS = ("b", "bcond", "cbz", "tbz", "ret", "br", "blr", "udf", "exc")


# The patch set: the declarations in the host's C (host/patch.h, nx2/lift2/patch_scan.py), read on first use. A site's
# function is the plan's (resolve): until then its func is None, which the summaries do not need.
_SET = None


def tables():
    """(Value probes, At rows) of the host's declarations, in their order (the build's files in order, each file's in
    its order: rows at one instruction run in that order)"""
    global _SET
    if _SET is None:
        from . import patch_scan
        _SET = patch_scan.rows(patch_scan.patches().sites)
    return _SET


def use(values, ats):
    """the patch set given (tests: rows of their own, each with its function), in place of the host's declarations;
    returns the set before, for use(*before)"""
    global _SET
    from . import summary
    before = tables()
    _SET = (list(values), list(ats))
    _RESOLVED.clear()
    _BY_FUNC.clear()
    for p in values:
        _BY_FUNC.setdefault(p.func, []).append(p)
    summary._ROWS = None
    return before


def __getattr__(name):   # (probes.PROBES, probes.AT: the tables, read on first use)
    if name == "PROBES":
        return tables()[0]
    if name == "AT":
        return tables()[1]
    raise AttributeError(name)


_RESOLVED = {}
_BY_FUNC = {}


def resolve(plan, key=None):
    """each site's function from the plan's bodies (once per plan: key): SystemExit with the declaration's file:line for
    a site in no function or in more than one; {function: [Value probes]} (find's index)"""
    if key is not None and key in _RESOLVED:
        return _RESOLVED[key]
    from . import patch_scan
    values, ats = tables()
    hits = patch_scan.containing(plan, {p.pc for p in values + ats})
    by_func = {}
    for p in values + ats:
        h = hits[p.pc]
        if len(h) != 1:
            patch_scan.fail(p.where, "%#x is in %s" % (p.pc, "no lifted function" if not h else
                                                       "more than one function: " + ", ".join("f_%08x" % e for e in h)))
        p.func = h[0]
    for p in values:
        by_func.setdefault(p.func, []).append(p)
    _RESOLVED.clear()
    _RESOLVED[key] = by_func
    _BY_FUNC.clear()
    _BY_FUNC.update(by_func)
    return by_func


def find(func, pcs, word):
    """{pc: lowering} of the Value probes of one function; SystemExit with the declaration's file:line when a probe's
    instruction is not the word it names (another game version) or its result is not the instruction's destination
    (errors)"""
    out = {}
    for p in _BY_FUNC.get(func, ()):
        _stop(p, errors(p, pcs, word))
        out[p.pc] = p.lower
    return out


def _stop(p, errs):
    if errs:
        where = getattr(p, "where", None)
        if where is not None:
            from . import patch_scan
            patch_scan.fail(where, errs[0])
        raise SystemExit("lift2 probes: %s" % errs[0])


def errors(p, pcs, word, jt=None, entries=()):
    """[what is wrong] with one Value probe or At row against its function (p.func) with the instructions pcs (word(pc)
    their words; jt: BR address -> jump-table targets; entries: every function's entry), the checks the lift makes:
    - the instruction is there and holds the declared word (else another game version);
    - Value: the instruction writes one general register, its Rd, and the declared result is that register at the
      instruction's width (wN for a 32-bit destination, xN for 64);
    - At instead: the instruction is a BL or BLR;
    - At before: no branch of the function, no jump table and no other function's entry goes to the instruction (the
      row runs on the edge into it alone);
    - At: a result or io register is caller-saved or written by the instruction (a row never changes what its function
      preserves)"""
    have = pcs if isinstance(pcs, (set, frozenset, dict)) else set(pcs)
    kind = "Value" if isinstance(p, Value) else "At"
    if p.pc not in have or word(p.pc) != p.word:
        return ["%s f_%08x at %#x holds %s, not %08x (another game version?)"
                % (kind, p.func or 0, p.pc, "%08x" % word(p.pc) if p.pc in have else "nothing", p.word)]
    out = []
    if kind == "Value":
        bad = [a for a in p.args if not re.fullmatch(r"x(\d+)(\+\d+)?|s(\d+)", a)]
        if bad:
            out.append("Value at %#x: operands xN, add(xN, K) or sN, not %s" % (p.pc, ", ".join(bad)))
        d = dest(p.word)
        if d is None:
            out.append("Value at %#x: the instruction writes no single general register Rd to replace" % p.pc)
        elif p.result != ("w" if d[1] == 32 else "x") + str(d[0]):
            out.append("Value at %#x: the result is %s, the instruction's destination is %s%d"
                       % (p.pc, p.result, "w" if d[1] == 32 else "x", d[0]))
        return out
    if p.replace and not is_call(p.word):
        out.append("At f_%08x replacing %#x: not a BL or BLR" % (p.func or 0, p.pc))
    if p.before:
        from .build_ir import target
        for pc in sorted(have):
            i = enc.decode(word(pc))
            if i is not None and target(i, pc) == p.pc:
                out.append("At f_%08x before %#x: a branch target (from %#x)" % (p.func or 0, p.pc, pc))
                break
            if jt and p.pc in jt.get(pc, ()):
                out.append("At f_%08x before %#x: a jump-table target (the BR at %#x)" % (p.func or 0, p.pc, pc))
                break
        if p.pc != p.func and p.pc in entries:
            out.append("At f_%08x before %#x: another function's entry" % (p.func or 0, p.pc))
    bad = p.effects()[1] - CALLER_SAVED - (set() if p.before else _written(p.word))
    if bad:
        out.append("At f_%08x at %#x writes %s: callee-saved, not written by the instruction"
                   % (p.func or 0, p.pc, ", ".join(sorted(bad))))
    return out


def _var(a):
    """the guest variable (ir.VARS) a register operand names: xN, wN -> "xN"; sN, qN -> "vN"; sp"""
    if a == "sp":
        return "sp"
    return ("x%s" if a[0] in "xw" else "v%s") % a[1:]


# ---- At: a host function of a few registers after an instruction (after a BL / BLR when it returns), its result in a
# register: "at pc: reg := fn(regs...)". The arguments:
#   "xN" (i64), "wN" (i32, the low half), "sN" (Sn, a float), "qN" (Vn whole, a 16-byte vector: hos_q in C), "sp" (i64),
#   "#K" (an i32 constant), "[xN+K]" / "[sp+K]" (the 64-bit guest word there, loaded by the lifted code: a frame slot
#   the host needs is handed over as a value, never read through SP by the host);
# the result: "xN", "wN" (zero-extended, as every W write), "sN" (a float, the rest of Vn zero, as every scalar FP
# write), "qN" (Vn), or None. io: registers ("xN", "wN" as 64-bit slots, "qN" as 16-byte slots, in that order, each
# at its natural alignment) the host function gets as a pointer to a struct (its last argument) holding their values
# and leaves their new values in. guest: the host function calls guest code: its first argument is a GuestCall
# handle (host/hos.h guest_call), and guest is the tuple of the guest functions it calls, or "computed" (a target
# read from guest memory). guest_call restores every register, so the row writes none but its result and io.
# The host function takes plain values and guest addresses, never the context; it may read and write guest memory
# (the call is never moved or removed). Each row names the instruction's word, checked at lift time: a word that
# differs stops the lift (another game version). A result or io register must be caller-saved (x0-x18, v0-v7,
# v16-v31) or written by the instruction itself: a row never changes what the function preserves.
ARG = re.compile(r"[xw]([0-9]|[12][0-9]|30)|[sq]([0-9]|[12][0-9]|3[01])|sp|#(0x[0-9a-f]+|\d+)|"
                 r"\[(x([0-9]|[12][0-9]|30)|sp)\+(0x[0-9a-f]+|\d+)\]")
RESULT = re.compile(r"[xw]([0-9]|[12][0-9]|30)|[sq]([0-9]|[12][0-9]|3[01])")
IO = re.compile(r"[xwq]([0-9]|[12][0-9]|30)|q31")


class At:
    def __init__(self, func, pc, word, fn, args=(), result=None, before=False, io=(), guest=None, replace=False):
        self.func, self.pc, self.word, self.fn, self.args, self.result = func, pc, word, fn, tuple(args), result
        self.before = before   # (before the instruction instead: pc must not be a branch target, so it runs on one edge)
        self.replace = replace   # (in place of the BL / BLR at pc: the caller's patch of a call it makes)
        assert not (before and replace), hex(pc)
        self.io, self.guest = tuple(io), guest
        for a in self.args:
            assert ARG.fullmatch(a), (hex(pc), a)
        assert result is None or RESULT.fullmatch(result), (hex(pc), result)
        for r in self.io:
            assert IO.fullmatch(r), (hex(pc), r)
        assert guest is None or guest == "computed" or (isinstance(guest, tuple) and guest), (hex(pc), guest)

    def effects(self):
        """(the guest variables the row reads, those it writes)"""
        reads = set()
        for a in self.args:
            if a[0] == "#":
                continue
            reads.add(_var(a[1:].split("+")[0] if a[0] == "[" else a))
        reads |= {_var(r) for r in self.io}
        writes = {_var(r) for r in self.io} | ({_var(self.result)} if self.result else set())
        return reads, writes

    def post(self, b):
        vals = []
        for a in self.args:
            kind, n = a[0], a[1:]
            if kind == "#":
                vals.append(b.const(int(n, 0), "i32"))
            elif a == "sp":
                vals.append(b.get("sp"))
            elif kind == "[":
                base, off = n.rstrip("]").split("+")
                v = b.get("sp") if base == "sp" else getx(b, int(base[1:]), 1)
                vals.append(b.op("load", "i64", b.bin("add", v, b.const(int(off, 0))), order="plain"))
            elif kind in "xw":
                vals.append(getx(b, int(n), kind == "x"))
            elif kind == "q":
                vals.append(vread(b, int(n), "v2i64"))
            else:
                vals.append(vread(b, int(n), "f32"))
        ty = {None: "void", "x": "i64", "w": "i32", "s": "f32", "q": "v2i64"}[self.result and self.result[0]]
        guest = self.guest is not None
        if self.io:
            iov = [vread(b, int(r[1:]), "v2i64") if r[0] == "q" else getx(b, int(r[1:]), 1) for r in self.io]
            call = b.op("rowio", ty, *(vals + iov), fn=self.fn, nargs=len(vals), ctx=guest)
            for k, r in enumerate(self.io):
                v = b.op("rowout", "v2i64" if r[0] == "q" else "i64", call, k=k)
                if r[0] == "q":
                    vwrite(b, int(r[1:]), v)
                else:
                    setx(b, int(r[1:]), b.trunc(v, "i32") if r[0] == "w" else v)
            r = call
        else:
            r = b.call(self.fn, ty, *vals, effects=True, ctx=guest)
        if self.result is None:
            return
        n = int(self.result[1:])
        if self.result[0] in "sq":
            vwrite(b, n, r)
        else:
            setx(b, n, r)


def io_layout(io):
    """[(offset, size)] of an io struct's slots: xN / wN 8 bytes, qN 16 bytes, each at its natural alignment"""
    out, off = [], 0
    for r in io:
        size = 16 if r[0] == "q" else 8
        off = (off + size - 1) // size * size
        out.append((off, size))
        off += size
    return out



# the registers a row may write without changing what its function preserves (and those the instruction writes)
CALLER_SAVED = {"x%d" % i for i in range(19)} | {"v%d" % i for i in list(range(8)) + list(range(16, 32))}


def _written(w):
    """the guest variables the instruction word writes (its template's writes)"""
    i = enc.decode(w)
    t = i.entry.template
    if t == "b" and i.f["op"] or t == "blr":
        return {"x30"} | CALLER_SAVED
    if t in ("b", "bcond", "cbz", "tbz", "ret", "br", "udf", "exc"):
        return set()
    fn = ir.Function(0)
    b = ir.Builder(fn)
    b.at(fn.block(0))
    sem.TEMPLATES[t](b, i.f, 0x1000, i.entry.params)
    return set(fn.written)


def row_effects():
    """{pc: [(reads, writes, before, guest, replace)]} of every Value probe and At row: the summaries count them as the
    instruction's (nx2/lift2/summary.py Func); guest: the guest functions the row's host function calls (their inputs
    are read there), "computed", or None"""
    out = {}
    for p in tables()[0]:
        r, w = p.effects()
        out.setdefault(p.pc, []).append((r, w, False, None, False))
    for p in tables()[1]:
        r, w = p.effects()
        out.setdefault(p.pc, []).append((r, w, p.before, p.guest, p.replace))
    return out


def at(func, pcs, word, before=False, replace=False, jt=None, entries=()):
    """{pc: [fn(builder)]} of a function's At probes, for Program.after (before: the ones for Program.before; replace:
    those for Program.replace); SystemExit with the declaration's file:line on a row errors() finds wrong (jt, entries:
    the program's, for a before row)"""
    out = {}
    have = set(pcs)
    for p in tables()[1]:
        if p.func != func or p.before != before or p.replace != replace:
            continue
        _stop(p, errors(p, have, word, jt, entries))
        out.setdefault(p.pc, []).append(p.post)
    return out


def is_call(w):
    """a BL or BLR: what comes after it runs when the call returns"""
    i = enc.decode(w)
    return i is not None and (i.entry.template == "blr" or i.entry.template == "b" and i.f["op"] == 1)


def merge(*posts):
    """Program.after from several {pc: fn(builder)} (or {pc: [fn]}), in the order given"""
    out = {}
    for d in posts:
        for pc, fn in d.items():
            out.setdefault(pc, []).extend(fn if isinstance(fn, list) else [fn])
    return out
