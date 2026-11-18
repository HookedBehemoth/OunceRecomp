"""The decoded instructions and basic blocks of one guest function, for tools/re/re: capstone's decoding with each
instruction's register uses and definitions (keys: 0-30 x/w, 31 sp, 32-63 the vector registers, 64 the flags) and
its kind (bl, blr, b, cond, br, ret, other)."""
import capstone
from capstone import arm64 as A

MD = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
MD.detail = True

FLAGS = 64
SP = 31


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


STORE_TY = {"strb": "u8", "strh": "u16", "sturb": "u8", "sturh": "u16", "stlrb": "u8", "stlrh": "u16"}
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


class Block:
    __slots__ = ("start", "pcs", "succs", "preds", "term", "idx", "cond", "tail", "switch", "ret")


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


def ranges(ns):
    out, k = [], 0
    while k < len(ns):
        j = k
        while j + 1 < len(ns) and ns[j + 1] == ns[j] + 1:
            j += 1
        out.append(str(ns[k]) if j == k else "%d..%d" % (ns[k], ns[j]))
        k = j + 1
    return ",".join(out)

