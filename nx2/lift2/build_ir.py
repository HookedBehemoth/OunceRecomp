"""A guest function -> IR.

Input: the function's entry, its instruction addresses (the present discovery's walk), a word reader and a Program
(what the whole-program stage knows: the other functions' entries, the jump tables, which function a call reaches and
the symbol of its generic entry). Blocks start at the entry, at every branch target inside the body and after every
branch.

Control leaving the function:
- BL: the callee's generic entry with the context (x30 set to the return address first); every register is the
  context's again after it;
- BLR: a call of the target when it is a host function pointer (code pointers are, lift2/code_ptrs.py), else the
  runtime's a64_icall_guest(ctx, target, pc); the same after it;
- B, or falling through, to another function's entry: a tail call of its generic entry (musttail: all generic entries
  share one prototype);
- BR: a jump table's switch when the discovery resolved one (the cases inside the function are branches, the others
  tail calls), else a tail call of the target when it is a host function pointer, else the runtime's
  a64_ijump_guest(ctx, target, pc);
- RET: return; BRK, HLT, UDF: the runtime's a64_trap;
- SVC: the runtime's a64_svc(ctx, imm) with the flags stored first; every register the context's again after it.
Every edge that closes a cycle carries a compiler barrier (cycle_edges).
scope() says why a function cannot be lifted yet.
"""
from . import bounds, enc, ir, sem, sem_int

TERMINATORS = {"b", "bcond", "cbz", "tbz", "ret", "udf", "br", "blr", "exc"}
# (an entry's address formed by address arithmetic other than a census site stops the lift: an ADR, a 64-bit ADD/SUB
# immediate. Not MOVZ/MOVK or ORR: the game's code is position-independent, so those make integers, and integers equal
# to an entry occur, 0x80000100 at 0x81701194)
CODE_FORMS = {"adr", "addsub_imm"}


class Program:
    """What lifting one function needs to know about the others. entries: every function entry; jt: BR address ->
    jump table targets; callee(t): the function a call or tail branch to t reaches (PLT stubs resolved); bound(e):
    the call-boundary record of a direct callee e (nx2/lift2/bounds.py: what a BL keeps, whether it returns, the
    symbols of its call and tail call); without one every callee is unknown, called and tail-called as symbol(e);
    no_call: the setjmp owners (scope refuses a BL to one unless a bound routes it through its stub)"""

    def __init__(self, entries=(), jt=None, callee=None, symbol=None, no_call=(), plt=None, classes=None,
                 jt_index=None, bound=None, omit=False, verify=False):
        self.classes = set(classes) if classes is not None else None   # (None: every class of sem.CLASS)
        # (entries, jt and plt are shared, not copied: the discovery fills them as it walks)
        self.entries = entries if isinstance(entries, set) else set(entries)
        self.plt = plt if plt is not None else {}   # PLT stub entry -> (final target, GOT slot)
        self.jt = jt if jt is not None else {}
        # BR address -> (load address, index register, LDR extend, [target per index]): the switch on the index
        self.jt_index = jt_index if jt_index is not None else {}
        symbol = symbol or (lambda e: "g_%08x" % e)
        self.bound = bound or (lambda e: bounds.unknown("unknown", symbol(e)))
        self.omit = omit       # (omit_stores: the context stores no reader reads go)
        self.verify = verify   # (HOOKSHOT_LIFT_VERIFY: omitted fields poisoned before a call, omit_stores)
        self.callee = callee or (lambda t: t)
        self.no_call = set(no_call)
        self.probes = {}   # pc -> fn(builder, insn, pc): the instruction replaced by a probe (probes.py, section 9)
        self.before = {}   # pc -> [fn(builder)]: run before the instruction at pc, whatever it is (probes.splice_before, At)
        self.after = {}   # pc -> [fn(builder)]: run after the instruction at pc, after a BL / BLR when it returns (probes.post)
        self.replace = {}   # pc -> [fn(builder)]: run in place of the BL / BLR at pc (probes.At replace)
        # (code pointers, lift2/code_ptrs.py) the function addresses translated to host function pointers, and this
        # function's sites: pc -> (entry, ADRP page or None for an ADR)
        self.code_targets = set()
        self.code_sites = {}


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
    fn.code_sites = 0
    b = ir.Builder(fn)
    if entry in prog.plt:   # (a PLT stub reached indirectly: x16 = the GOT slot, x17 = the target, then the target)
        t, slot = prog.plt[entry]
        b.at(fn.block(entry))
        b.set("x16", b.const(slot))
        b.set("x17", b.codeaddr(t) if t in prog.code_targets else b.const(t))   # (the GOT slot holds the host pointer)
        fn.blocks[entry].term = ("tail", prog.bound(prog.callee(t)).tail)
        ir.finish(fn)
        return fn

    def goto(t, frm):
        """the block a branch from frm to t goes to: one of the function's, or a tail-call exit"""
        if inside(prog, entry, body, t):
            return fn.block(t)
        return exit_block("t", t, ("tail", prog.bound(prog.callee(t)).tail))

    def exit_block(kind, t, term):
        key = (kind, t)
        blk = fn.blocks.get(key)
        if blk is None:
            blk = fn.block(key)
            blk.term = term
            exits.append(blk)
        return blk

    exits = []
    # a table BR's index, read at its load (the register may be reused before the BR): BR -> (block, value)
    index_at, index_load = {}, {}
    for br, (load, reg, opt, _) in prog.jt_index.items():
        if br in body and load in body and not any(
                q in prog.probes or q in prog.before or q in prog.after for q in range(load, br + 4, 4)):
            index_load[load] = (br, reg, opt)
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
        if pc in index_load:
            br, reg, opt = index_load[pc]
            v = b.get("x%d" % reg)
            if opt in (2, 6):   # UXTW, SXTW
                v = (b.zext if opt == 2 else b.sext)(b.trunc(v, "i32"), "i64")
            index_at[br] = (cur, v)
        i = enc.decode(word(pc))
        t = i.entry.template
        f = i.f
        if t not in TERMINATORS:
            site = prog.code_sites.get(pc)
            if site is not None and site[1] is not None and pc not in prog.probes:
                v = b.get("x%d" % f["Rn"])
                if v.op != "const" or v.args[0] != site[1]:
                    raise ValueError("code pointer at %#x: x%d does not hold the page %#x here (code_ptrs.sites)"
                                     % (pc, f["Rn"], site[1]))
            probe = prog.probes.get(pc)
            if probe is not None:
                probe(b, i, pc)
            else:
                sem.TEMPLATES[t](b, f, pc, i.entry.params)
            if site is not None and probe is None:   # (the function's address: its host function pointer)
                sem_int.setx(b, f["Rd"], b.codeaddr(site[0]))
                fn.code_sites += 1
            elif (t in CODE_FORMS and prog.code_targets and f.get("Rd", 31) != 31 and not (t == "adr" and f["op"])
                  and f.get("sf", 1)):
                # (an ADRP's page may be a page-aligned function's address: it is a page, the ADD after it decides)
                v = cur.defs.get("x%d" % f["Rd"])
                if v is not None and v.op == "const" and v.args[0] in prog.code_targets:
                    raise ValueError("f_%08x's address formed at %#x, not a code_ptrs.sites shape" % (v.args[0], pc))
            for post in prog.after.get(pc, ()):
                post(b)
            if pc + 4 not in body:   # (falls through into another function: a tail call)
                cur.term = ("br", goto(pc + 4, pc))
                cur = None
            continue
        if pc in prog.replace and (t == "blr" or t == "b" and f["op"]):   # (a row in place of the call)
            b.set("x30", b.const(pc + 4))
            for rep in prog.replace[pc]:
                rep(b)
            for post in prog.after.get(pc, ()):
                post(b)
            if pc + 4 not in body:
                cur.term = ("br", goto(pc + 4, pc))
                cur = None
            continue
        if t == "b" and f["op"]:   # BL
            callee = prog.callee(target(i, pc))
            bd = prog.bound(callee)
            held = {}
            for var in bounds.keep_vars(bd):   # (written through to the context before the call, as every register)
                v = cur.defs.get(var)
                if v is None and not cur.called:
                    v = b.get(var)   # (a parameter of the block: no load)
                if v is not None:
                    held[var] = v
            b.set("x30", b.const(pc + 4))
            b.op("gcall", "void", sym=bd.symbol, callee=callee, pc=pc)
            b.after_call()
            cur.defs.update(held)
            for post in prog.after.get(pc, ()):
                post(b)
            if pc not in prog.after and bd.noreturn:
                cur.term = ("noreturn", pc, i.word)   # (a64_trap, then unreachable: a wrong summary stops loudly)
                cur = None
                continue
            if pc + 4 not in body:
                cur.term = ("br", goto(pc + 4, pc))
                cur = None
            continue
        if t == "blr":
            tv = b.get("x%d" % f["Rn"]) if f["Rn"] != 31 else b.const(0)
            b.set("x30", b.const(pc + 4))
            b.op("icall", "void", tv, pc=pc)
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
            at = index_at.get(pc)
            if at is not None and at[0] is cur:
                # on the index the guest loaded the entry with: cases 0..n-1, dense (LLVM lowers a table); an index
                # out of range takes the default
                cur.term = ("switch", at[1], [(n, goto(t2, pc)) for n, t2 in enumerate(prog.jt_index[pc][3])], ij)
            elif targets:
                # on the target's offset: (x - lowest) rotated right by 2, so a misaligned target is a large index and
                # takes the default; with sparse offsets LLVM lowers a tree of compares
                lo = targets[0]
                d = b.bin("sub", x, b.const(lo))
                idx = b.intr("fshr", "i64", d, d, b.const(2))
                cur.term = ("switch", idx, [((t2 - lo) >> 2, goto(t2, pc)) for t2 in targets], ij)
            else:
                cur.term = ("br", ij)
            ij_b = b.blk
            b.at(ij)
            ij.term = ("ijump", b.get("x%d" % f["Rn"]) if f["Rn"] != 31 else b.const(0), pc)
            b.blk = ij_b
        else:   # UDF, BRK, HLT
            cur.term = ("trap", pc, i.word)
        cur = None
    if any(blk.term and blk.term[0] == "noreturn" for blk in fn.blocks.values()):
        # blocks only a noreturn call's return reached: gone
        seen, todo = {fn.blocks[entry]}, [fn.blocks[entry]]
        while todo:
            for s in todo.pop().succs():
                if s not in seen:
                    seen.add(s)
                    todo.append(s)
        for k in [k for k, blk in fn.blocks.items() if blk not in seen]:
            del fn.blocks[k]
        exits = [blk for blk in exits if blk in seen]
    # leaving for another function: the flags too, if this function writes them (the code there may read them)
    for blk in exits:
        if fn.written & {"n", "z", "c", "v"}:
            b.at(blk)
            b.op("flagsout", "void", *[b.get(fl) for fl in "nzcv"])
    for blk in fn.blocks.values():
        if blk.term is None:
            raise ValueError("block %s has no terminator" % blk.name)
    # every cycle a compiler barrier: a guest spin loop on a plain load stays a loop that loads each time round
    for blk in cycle_edges(fn):
        blk.ops.append(fn.new_value("void", "barrier", ()))
    ir.finish(fn)
    omit_stores(fn, prog)
    return fn


# ---- stores no reader can read
# (register variables: x0-x30, SP, V0-V31 by their summary bit; the flags are never omitted)
_BIT = {v: 1 << k for k, v in enumerate(ir.VARS)}
_REGS = sum(m for v, m in _BIT.items() if v not in ("n", "z", "c", "v"))
_EXITS = ("ret", "tail", "ijump", "noreturn", "trap")
_CALLEE_SAVED = sum(_BIT["x%d" % r] for r in range(19, 30)) | _BIT["sp"] | sum(_BIT["v%d" % r] for r in range(8, 16))
# (verification: the value an omitted field holds at the call: 0xDEAF, the register, the call's guest address)
SITE_POISON = 0xDEAF


def _reads(v, prog):
    """(read, killed) by an operation as register masks: read, every register (_REGS) or what a BL's callee reads
    (its call-boundary record); killed, what it writes on every path before anything reads the context's old value.
    A call taking the context reads every register unless the template that made it says which it reads (`regs`:
    the runtime's helpers of the monitor, FPCR/FPSR, the counter read only the context's other fields)"""
    op = v.op
    if op == "regload":
        return _BIT[v.attrs["var"]], 0
    if op == "gcall":
        bd = prog.bound(v.attrs["callee"])
        return bd.reads & _REGS, bd.must & _REGS
    if op in ("icall", "svc"):
        return _REGS, 0
    if op in ("call", "rowio") and v.attrs.get("ctx"):
        return v.attrs.get("regs", _REGS) & _REGS, 0
    return 0, 0


def omit_stores(fn, prog):
    """Deletes each register's write-through store (`writeback`) that no reader can read before the register's next
    store: overwritten first, or dead at a direct call whose callee does not read it and the caller does not reload
    it before the callee writes it on every path, or dead at a return: neither a result the function's callers read
    nor a callee-saved register or SP. What a call reads and writes is its callee's call-boundary record
    (prog.bound); every register is read by BLR, SVC, a call taking the context unless it says otherwise, tail calls,
    indirect jumps, traps, and the returns of a function whose record does not know its results. With prog.verify a
    BL is preceded by `sitepoison` of the fields no reader reads there (host/lift2_verify.c names the site)."""
    if not prog.omit:
        return
    own = getattr(prog.bound, "own", prog.bound)(fn.entry)   # (a PATCH_FUNC's body: its own, not the function's)
    at_ret = (own.results | _CALLEE_SAVED) & _REGS
    own_may = own.clobbers   # (the poison writes only what the function may write itself)
    blocks = list(fn.blocks.values())
    info = {}
    for blk in blocks:   # (per block, backward: what it reads before writing, and what it writes first)
        gen = kill = 0
        for v in reversed(blk.ops):
            if v.op == "writeback":
                m = _BIT[v.attrs["var"]] & _REGS
                gen &= ~m
                kill |= m
                continue
            rd, k = _reads(v, prog)
            if rd == _REGS:
                gen, kill = _REGS, 0
            elif rd or k:
                gen = rd | (gen & ~k)
                kill = (kill | k) & ~rd
        info[id(blk)] = (gen, kill)
    live_in = {id(b): 0 for b in blocks}

    def live_out(blk):
        if blk.term[0] == "ret":
            return at_ret
        if blk.term[0] in _EXITS:
            return _REGS
        out = 0
        for s in blk.succs():
            out |= live_in[id(s)]
        return out
    changed = True
    while changed:
        changed = False
        for blk in reversed(blocks):
            gen, kill = info[id(blk)]
            li = gen | (live_out(blk) & ~kill)
            if li != live_in[id(blk)]:
                live_in[id(blk)] = li
                changed = True
    for blk in blocks:
        live = live_out(blk)
        keep = []
        for v in reversed(blk.ops):
            if v.op == "writeback":
                m = _BIT[v.attrs["var"]]
                if m & _REGS and not live & m:
                    continue
                live &= ~m
                keep.append(v)
                continue
            if v.op == "gcall" and prog.verify:
                keep.append(v)
                bd = prog.bound(v.attrs["callee"])
                # (the fields no reader reads at the call)
                dead = _REGS & ~bd.reads & ~(live & ~bd.must) & ~_BIT["sp"] & own_may
                if dead:
                    keep.append(fn.new_value("void", "sitepoison", (), {"mask": dead, "pc": v.attrs["pc"]}))
                rd, k = _reads(v, prog)
                live = _REGS if rd == _REGS else rd | (live & ~k)
                continue
            rd, k = _reads(v, prog)
            if rd == _REGS:
                live = _REGS
            elif rd or k:
                live = rd | (live & ~k)
            keep.append(v)
        keep.reverse()
        blk.ops = keep


def cycle_edges(fn):
    """The blocks whose terminator has an edge closing a cycle of the function's CFG: the back edges of a depth-first
    search (to a block still on the search's stack), from the entry and then from any block it did not reach.

    Invariant: every cycle of the CFG contains one of these edges. In any depth-first search the cycle's first
    visited block is an ancestor of every other block of the cycle, so the cycle's edge into it is a back edge. This
    holds for natural loops, irreducible regions (several entries), self-loops, cycles through a jump table's switch
    and a branch back to the function's own entry alike. An edge to an earlier address that closes no cycle gets no
    barrier."""
    blocks = list(fn.blocks.values())
    entry = fn.entry_block()
    order = [entry] + [b for b in blocks if b is not entry]
    state = {}   # id -> 1 on the stack, 2 done
    out = []
    for root in order:
        if id(root) in state:
            continue
        state[id(root)] = 1
        stack = [(root, iter(root.succs()))]
        while stack:
            blk, it = stack[-1]
            nxt = next(it, None)
            if nxt is None:
                state[id(blk)] = 2
                stack.pop()
                continue
            s = state.get(id(nxt))
            if s == 1:
                if not out or out[-1] is not blk:
                    out.append(blk)
            elif s is None:
                state[id(nxt)] = 1
                stack.append((nxt, iter(nxt.succs())))
    seen, uniq = set(), []
    for b in out:
        if id(b) not in seen:
            seen.add(id(b))
            uniq.append(b)
    return uniq


