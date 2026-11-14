"""A guest function -> IR.

Input: the function's entry and its instruction addresses (the present discovery's walk: every address of the body),
and a word reader. Blocks start at the entry, at every branch target inside the body and after every branch.
Milestone 1 lifts functions that stay inside themselves: no calls, no branch to another entry, no indirect branch;
scope() says why a function is out.
"""
from . import enc, ir, sem_int

TERMINATORS = {"b", "bcond", "cbz", "tbz", "ret", "udf", "br", "blr"}
M1_TEMPLATES = set(sem_int.TEMPLATES) | {"b", "bcond", "cbz", "tbz", "ret", "udf"}


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


def scope(entry, pcs, word):
    """None if milestone 1 can lift the function, else the reason it cannot"""
    body = set(pcs)
    for pc in pcs:
        i = enc.decode(word(pc))
        if i is None:
            return "word %08x at %#x not decoded" % (word(pc), pc)
        t = i.entry.template
        if t not in M1_TEMPLATES:
            return "%s at %#x" % (i.entry.name, pc)
        if t == "b" and i.f["op"]:
            return "BL at %#x" % pc
        tg = target(i, pc)
        if tg is not None and tg not in body:
            return "branch out of the body at %#x" % pc
        if t not in TERMINATORS and pc + 4 not in body:
            return "falls out of the body at %#x" % pc
        if t in ("bcond", "cbz", "tbz") and pc + 4 not in body:
            return "falls out of the body at %#x" % pc
    return None


def build(entry, pcs, word):
    """the IR Function of a function in milestone 1's scope"""
    pcs = sorted(pcs)
    fn = ir.Function(entry)
    leaders = {entry}
    for pc in pcs:
        i = enc.decode(word(pc))
        t = i.entry.template
        if t in TERMINATORS:
            tg = target(i, pc)
            if tg is not None:
                leaders.add(tg)
            leaders.add(pc + 4)
    b = ir.Builder(fn)
    fn.block(entry)
    cur = None
    for pc in pcs:
        if pc in leaders or cur is None:
            nxt = fn.block(pc)
            if cur is not None and cur.term is None:
                cur.term = ("br", nxt)
            cur = nxt
            b.at(cur)
        i = enc.decode(word(pc))
        t = i.entry.template
        f = i.f
        if t not in TERMINATORS:
            sem_int.TEMPLATES[t](b, f, pc, i.entry.params)
            continue
        if t == "b":
            cur.term = ("br", fn.block(target(i, pc)))
        elif t == "bcond":
            cur.term = ("condbr", sem_int.cond(b, f["cond"]), fn.block(target(i, pc)), fn.block(pc + 4))
        elif t == "cbz":
            v = sem_int.getx(b, f["Rt"], f["sf"])
            c = b.icmp("ne" if f["op"] else "eq", v, b.const(0, v.ty))
            cur.term = ("condbr", c, fn.block(target(i, pc)), fn.block(pc + 4))
        elif t == "tbz":
            bit = f["b5"] << 5 | f["b40"]
            v = b.bin("and", b.get("x%d" % f["Rt"]) if f["Rt"] != 31 else b.const(0), b.const(1 << bit))
            c = b.icmp("ne" if f["op"] else "eq", v, b.const(0))
            cur.term = ("condbr", c, fn.block(target(i, pc)), fn.block(pc + 4))
        elif t == "ret":
            cur.term = ("ret",)
        elif t == "udf":
            cur.term = ("trap", pc, i.word)
        cur = None
    for blk in fn.blocks.values():
        if blk.term is None:
            raise ValueError("block %#x has no terminator" % blk.pc)
    ir.finish(fn)
    group_marks(fn)
    return fn


def base_offset(addr):
    """(base value, constant offset) of an address value: a chain of constant additions walked back"""
    off = 0
    while addr.op == "add" and addr.args[1].op == "const":
        c = addr.args[1].args[0]
        off += c - (1 << 64) if c >> 63 else c
        addr = addr.args[0]
    return addr, off


def group_marks(fn):
    """The page marks of the marked stores of a block that share a base value, within one 4 KB span: one mark (of
    the span's first and last byte) after the group's last store, instead of two per store. Marks only need to
    follow their stores (the renderer reads data a mark announces), so moving them later in the block is safe."""
    for blk in fn.blocks.values():
        groups = {}   # base value id -> [stores]
        order = []
        for v in blk.ops:
            if v.op == "store" and v.attrs["mark"]:
                base, off = base_offset(v.args[0])
                g = groups.get(base.id)
                n = ir.WIDTH[v.args[1].ty] // 8
                if g is not None and max(g[2], off + n) - min(g[1], off) <= 4096:
                    g[0].append(v)
                    g[1], g[2] = min(g[1], off), max(g[2], off + n)
                    continue
                g = groups[base.id] = [[v], off, off + n, base]
                order.append(g)
        if not order:
            continue
        last = {}
        for stores, lo, hi, base in order:
            for v in stores:
                v.attrs["mark"] = False
            last[stores[-1].id] = (base, lo, hi)
        ops = []
        for v in blk.ops:
            ops.append(v)
            if v.id in last:
                base, lo, hi = last[v.id]
                ops.append(fn.new_value("void", "mark", (base,), {"lo": lo, "hi": hi}))
        blk.ops = ops
