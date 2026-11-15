"""A guest function -> IR.

Input: the function's entry, its instruction addresses (the present discovery's walk), a word reader and a Program
(what the whole-program stage knows: the other functions' entries, the jump tables, which function a call reaches and
the symbol of its generic entry). Blocks start at the entry, at every branch target inside the body and after every
branch.

Control leaving the function:
- BL: the callee's generic entry with the context (x30 set to the return address first); every register is the
  context's again after it;
- BLR: the runtime's a64_icall(ctx, target); the same after it;
- B, or falling through, to another function's entry: a tail call of its generic entry (musttail: all generic entries
  share one prototype);
- BR: a jump table's switch when the discovery resolved one (the cases inside the function are branches, the others
  tail calls), else the runtime's a64_ijump(ctx, target);
- RET: return; BRK, HLT, UDF: the runtime's a64_trap;
- SVC: the runtime's a64_svc(ctx, imm) with the flags stored first; every register the context's again after it.
A branch back to an earlier address polls (nx2/poll.py's rule: the loops' back edges).
scope() says why a function cannot be lifted yet.
"""
from . import enc, ir, sem, sem_int

TERMINATORS = {"b", "bcond", "cbz", "tbz", "ret", "udf", "br", "blr", "exc"}


class Program:
    """What lifting one function needs to know about the others. entries: every function entry; jt: BR address ->
    jump table targets; callee(t): the function a call or tail branch to t reaches (PLT stubs resolved);
    symbol(e): the generic entry's symbol of function e; no_lift: functions to pass over (setjmp owners: milestone 7)"""

    def __init__(self, entries=(), jt=None, callee=None, symbol=None, no_call=(), plt=None, classes=None,
                 tail_symbol=None):
        self.tail_symbol = tail_symbol or symbol or (lambda e: "g_%08x" % e)   # (a tail call's target symbol)
        self.classes = set(classes) if classes is not None else None   # (None: every class of sem.CLASS)
        # (entries, jt and plt are shared, not copied: the discovery fills them as it walks)
        self.entries = entries if isinstance(entries, set) else set(entries)
        self.plt = plt if plt is not None else {}   # PLT stub entry -> (final target, GOT slot)
        self.jt = jt if jt is not None else {}
        self.callee = callee or (lambda t: t)
        self.symbol = symbol or (lambda e: "g_%08x" % e)
        self.no_call = set(no_call)
        self.probes = {}   # pc -> fn(builder, insn, pc): the instruction replaced by a probe (probes.py, section 9)
        self.before = {}   # pc -> [fn(builder)]: run before the instruction at pc, whatever it is (probes.splice_before, At)
        self.after = {}   # pc -> [fn(builder)]: run after the instruction at pc, after a BL / BLR when it returns (probes.post)


LEAF = Program()


def sx(v, bits):
    return v - (1 << bits) if v >> (bits - 1) & 1 else v


def target(insn, pc):
    f, t = insn.f, insn.entry.template
    if t == "b":
        return pc + (sx(f["imm26"], 26) << 2)
    if t in ("bcond", "cbz"):
        return pc + (sx(f["imm19"], 19) << 2)
    if t == "tbz":
        return pc + (sx(f["imm14"], 14) << 2)
    return None


def inside(prog, entry, body, t):
    """a branch to t stays in the function (a goto), or leaves it (a tail call)"""
    return t in body and (t == entry or t not in prog.entries)


def scope(entry, pcs, word, prog=LEAF):
    """None if the function can be lifted, else the reason it cannot"""
    if entry in prog.plt:
        return None
    body = set(pcs)
    for pc in pcs:
        i = enc.decode(word(pc))
        if i is None:
            return "word %08x at %#x not decoded" % (word(pc), pc)
        t = i.entry.template
        if t not in TERMINATORS and sem.TEMPLATES.get(t) is None:
            return "%s at %#x" % (i.entry.name, pc)
        if prog.classes is not None and t not in TERMINATORS and sem.CLASS.get(t) not in prog.classes:
            return "class %s (%s) at %#x" % (sem.CLASS.get(t), i.entry.name, pc)
        tg = target(i, pc)
        if t == "b" and i.f["op"]:
            if prog is LEAF:
                return "BL at %#x" % pc
            c = prog.callee(tg)
            if c not in prog.entries:
                return "BL to a non-entry at %#x" % pc
            if c in prog.no_call:
                return "BL to a setjmp owner at %#x" % pc
            continue
        if t == "blr" and prog is LEAF:
            return "BLR at %#x" % pc
        if t == "br" and prog is LEAF:
            return "BR at %#x" % pc
        if tg is not None and not inside(prog, entry, body, tg):
            if prog is LEAF or prog.callee(tg) not in prog.entries:
                return "branch out of the body at %#x" % pc
        falls = t not in TERMINATORS or t in ("bcond", "cbz", "tbz", "blr") or (t == "exc" and i.f["opc"] == 0)
        if falls and pc + 4 not in body:
            if prog is LEAF or prog.callee(pc + 4) not in prog.entries:
                return "falls out of the body at %#x" % pc
    return None


def build(entry, pcs, word, prog=LEAF):
    """the IR Function of a function in scope"""
    pcs = sorted(pcs)
    body = set(pcs)
    fn = ir.Function(entry)
    b = ir.Builder(fn)
    if entry in prog.plt:   # (a PLT stub reached indirectly: x16 = the GOT slot, x17 = the target, then the target)
        t, slot = prog.plt[entry]
        b.at(fn.block(entry))
        b.set("x16", b.const(slot))
        b.set("x17", b.const(t))
        fn.blocks[entry].term = ("tail", prog.tail_symbol(prog.callee(t)))
        ir.finish(fn)
        return fn

    def goto(t, frm):
        """the block a branch from frm to t goes to: one of the function's, or a tail-call exit"""
        if inside(prog, entry, body, t):
            return fn.block(t)
        return exit_block("t", t, ("tail", prog.tail_symbol(prog.callee(t))))

    def exit_block(kind, t, term):
        key = (kind, t)
        blk = fn.blocks.get(key)
        if blk is None:
            blk = fn.block(key)
            blk.term = term
            exits.append(blk)
        return blk

    exits = []
    leaders = {entry}
    for pc in pcs:
        i = enc.decode(word(pc))
        t = i.entry.template
        if t in TERMINATORS:
            tg = target(i, pc)
            if tg is not None and tg in body:
                leaders.add(tg)
            if t == "br" and pc in prog.jt:
                leaders.update(x for x in prog.jt[pc] if x in body)
            leaders.add(pc + 4)
    fn.block(entry)
    cur = None
    for pc in pcs:
        if pc in leaders or cur is None:
            nxt = fn.block(pc)
            if cur is not None and cur.term is None:
                cur.term = ("br", nxt)
            cur = nxt
            b.at(cur)
        for pre in prog.before.get(pc, ()):
            pre(b)
        i = enc.decode(word(pc))
        t = i.entry.template
        f = i.f
        if t not in TERMINATORS:
            probe = prog.probes.get(pc)
            if probe is not None:
                probe(b, i, pc)
            else:
                sem.TEMPLATES[t](b, f, pc, i.entry.params)
            for post in prog.after.get(pc, ()):
                post(b)
            if pc + 4 not in body:   # (falls through into another function: a tail call)
                cur.term = ("br", goto(pc + 4, pc))
                cur = None
            continue
        if t == "b" and f["op"]:   # BL
            b.set("x30", b.const(pc + 4))
            b.op("gcall", "void", sym=prog.symbol(prog.callee(target(i, pc))))
            b.after_call()
            for post in prog.after.get(pc, ()):
                post(b)
            if pc + 4 not in body:
                cur.term = ("br", goto(pc + 4, pc))
                cur = None
            continue
        if t == "blr":
            tv = b.get("x%d" % f["Rn"]) if f["Rn"] != 31 else b.const(0)
            b.set("x30", b.const(pc + 4))
            b.op("icall", "void", tv)
            b.after_call()
            for post in prog.after.get(pc, ()):
                post(b)
            if pc + 4 not in body:
                cur.term = ("br", goto(pc + 4, pc))
                cur = None
            continue
        if t == "exc" and f["opc"] == 0:   # SVC: the OS layer may switch threads (fibers), so the flags go too
            b.op("flagsout", "void", *[b.get(fl) for fl in "nzcv"])
            b.op("svc", "void", imm=f["imm16"])
            b.after_call()
            if pc + 4 not in body:
                cur.term = ("br", goto(pc + 4, pc))
                cur = None
            continue
        if pc in prog.after:   # (a branch: nothing runs after it on every path)
            raise ValueError("code after the branch at %#x" % pc)
        if t == "b":
            cur.term = ("br", goto(target(i, pc), pc))
        elif t == "bcond":
            cur.term = ("condbr", sem_int.cond(b, f["cond"]), goto(target(i, pc), pc), goto(pc + 4, pc))
        elif t == "cbz":
            v = sem_int.getx(b, f["Rt"], f["sf"])
            c = b.icmp("ne" if f["op"] else "eq", v, b.const(0, v.ty))
            cur.term = ("condbr", c, goto(target(i, pc), pc), goto(pc + 4, pc))
        elif t == "tbz":
            bit = f["b5"] << 5 | f["b40"]
            v = b.bin("and", b.get("x%d" % f["Rt"]) if f["Rt"] != 31 else b.const(0), b.const(1 << bit))
            c = b.icmp("ne" if f["op"] else "eq", v, b.const(0))
            cur.term = ("condbr", c, goto(target(i, pc), pc), goto(pc + 4, pc))
        elif t == "ret":
            cur.term = ("ret",)
        elif t == "br":
            x = b.get("x%d" % f["Rn"]) if f["Rn"] != 31 else b.const(0)
            ij = exit_block("j", pc, ("ijump", None))
            targets = sorted(set(prog.jt.get(pc, ())))
            if targets:
                # the switch's index: (x - lowest) rotated right by 2, so a misaligned target is a large index and
                # takes the default; the cases are dense, which LLVM turns into a table
                lo = targets[0]
                d = b.bin("sub", x, b.const(lo))
                idx = b.intr("fshr", "i64", d, d, b.const(2))
                cur.term = ("switch", idx, [((t2 - lo) >> 2, goto(t2, pc)) for t2 in targets], ij)
            else:
                cur.term = ("br", ij)
            ij_b = b.blk
            b.at(ij)
            ij.term = ("ijump", b.get("x%d" % f["Rn"]) if f["Rn"] != 31 else b.const(0))
            b.blk = ij_b
        else:   # UDF, BRK, HLT
            cur.term = ("trap", pc, i.word)
        cur = None
    # leaving for another function: the flags too, if this function writes them (the code there may read them)
    for blk in exits:
        if fn.written & {"n", "z", "c", "v"}:
            b.at(blk)
            b.op("flagsout", "void", *[b.get(fl) for fl in "nzcv"])
    # a branch back to an earlier address (a loop's back edge) polls first
    for blk in list(fn.blocks.values()):
        if blk.term is None:
            raise ValueError("block %s has no terminator" % blk.name)
        if blk.name.startswith("b") and any(s.name.startswith("b") and s.pc <= blk.pc for s in blk.succs()):
            blk.ops.append(fn.new_value("void", "poll", ()))
    ir.finish(fn)
    return fn


