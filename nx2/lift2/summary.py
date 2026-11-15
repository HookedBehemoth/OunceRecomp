"""Whole-program summaries of the guest functions.

For every function, over the call graph to a fixed point (strongly connected components callees first, iterated
inside a component until nothing changes):
- inputs: the registers some path reads before writing them (a call reads its callee's inputs);
- clobbers: the registers some path may write (a call adds its callee's; BL writes X30);
- mustdef: the registers every path to a return writes (used to kill liveness after a call);
- preserved: the callee-saved registers (X19-X29, SP, D8-D15) that hold their entry value at every return,
  proven by tracking SP, X29 as SP plus a constant, and which frame slots hold which register's entry value
  (a restore from such a slot gives the entry value back). Calls keep what their callee preserves; the caller's
  frame is assumed untouched by its callees (C's object rules; the verification build checks);
- results: the registers some caller reads after a call that the callee may write (plus the AAPCS64 results X0,
  X1, V0-V3, for callers the program cannot see);
- returns (not noreturn), frame size, whether the frame's address is taken, the callee-saved save/restore pairs
  that a fast entry could delete, whether a generic-only boundary is reachable (BLR, unresolved BR, SVC, a host
  function or hook), FP use (writes FPCR; uses FP/NEON instructions).

Demotion, by detection, to the generic entry only (`DEMOTE` reasons):
- sp: SP not tracked to a constant offset, or not back at its entry value at a return;
- cs_clobber: a callee-saved register not proven preserved at every return;
- save_ptr: an entry value of X19-X30 or SP stored through a pointer that is not the frame (setjmp, context
  captures);
- lr_data: X30's entry value read as data (anything but saving it to the frame or returning through it);
- flags_in: the flags read at entry; tmp_in: X9-X18 read at entry (veneers, non-AAPCS helpers);
- fallthrough_entry: an entry reached by falling through from the previous instruction of another function;
- unknown callee: calls a function whose facts are unknown (its own sp/save_ptr/lr_data demotion): the caller
  cannot prove its own registers.
A function demoted for sp, save_ptr or lr_data has unknown facts: its callers treat it as reading and writing
everything and preserving nothing. One demoted for the other reasons keeps its proven facts (only its interface
is outside the AAPCS64).

Registers are bit masks over ir.VARS (X0-X30 bits 0-30, SP 31, N Z C V 32-35, V0-V31 36-67).
"""
import os
import time

from . import enc, host_effects, ir, sem, sem_int

BIT = {v: 1 << i for i, v in enumerate(ir.VARS)}
X = [BIT["x%d" % i] for i in range(31)]
SP, NZCV = BIT["sp"], BIT["n"] | BIT["z"] | BIT["c"] | BIT["v"]
V = [BIT["v%d" % i] for i in range(32)]
ALL = (1 << len(ir.VARS)) - 1
CALLEE_SAVED = sum(X[19:30]) | SP | sum(V[8:16])
TRACKED = ["x%d" % i for i in range(19, 31)] + ["v%d" % i for i in range(8, 16)]   # (entry values tracked)
ARGS = sum(X[0:9]) | SP | sum(V[0:8])
CALLER_SAVED = sum(X[0:19]) | X[30] | NZCV | sum(V[0:8]) | sum(V[16:32])
TMPS = sum(X[9:19])
ABI_RESULTS = X[0] | X[1] | sum(V[0:4])
UNKNOWN_FACTS = ("sp", "save_ptr", "lr_data", "lr_ret", "split_frame")
# functions that do not return whatever their code says (they unwind, exit or abort through the host): seeds of the
# noreturn fixed point, by symbol name (the image's dynamic symbols)
NORETURN_NAMES = ("abort", "__assert_fail", "_Exit", "exit", "__cxa_throw", "__cxa_rethrow", "_Unwind_RaiseException",
                  "_Unwind_ForcedUnwind", "_Unwind_Resume", "png_error", "png_chunk_error", "png_fixed_error")
NORETURN_PARTS = ("AbortImpl", "UnexpectedDefaultImpl", "ExitProcess", "QuickExit", "terminateEv", "__throw_",
                  "CriticalSectionAbort")


def noreturn_seed(name):
    if not name:
        return False
    base = name.split("@")[0]
    return base in NORETURN_NAMES or any(p in base for p in NORETURN_PARTS)


def mask_names(m):
    return [v for v in ir.VARS if m & BIT[v]]


# ---- per-instruction register use and definition, from the semantic templates themselves
_du = {}
CACHE_MAX = 200000
TERMS = {"b", "bcond", "cbz", "tbz", "ret", "udf", "br", "blr", "exc"}


def def_use(w):
    """(insn, use mask, def mask) of a word: the registers its template reads and writes"""
    got = _du.get(w)
    if got is not None:
        return got
    i = enc.decode(w)
    use = dfn = 0
    if i is not None:
        t, f = i.entry.template, i.f
        if t in TERMS:
            if t == "b" and f["op"]:
                dfn = X[30]
            elif t == "bcond" or t == "cbz" or t == "tbz":
                fn = ir.Function(0)
                b = ir.Builder(fn)
                b.at(fn.block(0))
                if t == "bcond":
                    sem_int.cond(b, f["cond"])
                else:
                    sem_int.getx(b, f["Rt"], 1)
                use = sum(BIT[v] for v in fn.block(0).params)
            elif t in ("br", "blr"):
                use = X[f["Rn"]] if f["Rn"] != 31 else 0
                dfn = X[30] if t == "blr" else 0
            elif t == "exc" and f["opc"] == 0:   # SVC: Horizon's arguments and results in X0-X7
                use, dfn = sum(X[0:8]), sum(X[0:8])
        elif sem.TEMPLATES.get(t) is not None:
            fn = ir.Function(0)
            b = ir.Builder(fn)
            b.at(fn.block(0))
            sem.TEMPLATES[t](b, f, 0x1000, i.entry.params)
            use = sum(BIT[v] for v in fn.block(0).params)
            dfn = sum(BIT[v] for v in fn.written)
    got = (i, use, dfn)
    if len(_du) >= CACHE_MAX:   # (bounded: a worker of the parallel pass would otherwise grow by ~1 GB)
        _du.clear()
        enc._cache.clear()
    _du[w] = got
    return got


_ROWS = None


def rows():
    """{pc: (use mask, def mask, guest calls, replace)}: the Value probes' and At rows' declared reads and writes
    (nx2/lift2/probes.py), counted as their instruction's: a row reads its arguments there and writes its result (a
    before row's writes are over-approximated as the instruction's, which only adds definitions the code after it may
    also make). guest calls: the guest functions the row's host function calls (guest_call: their inputs are read
    there, every register is put back), "computed" (any), or (); replace: the row stands in place of its BL / BLR"""
    global _ROWS
    if _ROWS is None:
        from . import probes
        _ROWS = {}
        for pc, effs in probes.row_effects().items():
            u = d = 0
            calls, repl = (), False
            for r, w, _, guest, replace in effs:
                u |= sum(BIT[v] for v in r)
                d |= sum(BIT[v] for v in w)
                if guest == "computed" or calls == "computed":
                    calls = "computed" if guest is not None else calls
                elif guest:
                    calls = calls + tuple(guest)
                repl = repl or replace
            _ROWS[pc] = (u, d, calls, repl)
    return _ROWS


# ---- the host's code at a function's entry (PATCH_HOOK) and HLE'd functions (nx2/lift2/host_effects.py)
ENTRY = {}   # entry -> (what the hook reads, what it may write, the guest functions it calls): summarise() sets it


def entry_hooks(hooks):
    """{entry: (reads, may-writes, guest calls)} of the entry hooks ({entry: hook C name}): the hook reads its declared
    registers (host_effects.hook_reads) at the entry, may write x0-x7 (hook_return, hook_set_arg) and calls the guest
    functions its guest(...) names (hook_call_guest: their inputs are read, every register is put back); hook_run
    runs the function's own body, whose effects the summary has already"""
    return {e: (sum(BIT[v] for v in host_effects.hook_reads(n)), sum(X[0:8]), host_effects.hook_calls(n))
            for e, n in hooks.items()}


def host_summary(symbol):
    """the summary of an HLE'd function (symbol: its name, as in imp_<symbol>; None: not known): AAPCS64 facts with
    what its declaration reads; one declared clobbers("...") or unknown reads every register"""
    s = Summary()
    clean = symbol is not None and host_effects.clobbers(symbol) is None
    s.inputs = sum(BIT[v] for v in host_effects.hle_reads(symbol)) if clean else ALL
    s.clobbers, s.mustdef, s.preserved = CALLER_SAVED, X[30], CALLEE_SAVED
    s.returns = s.boundary = True
    return s


def host_summaries(hle):
    """{entry: Summary} of the HLE'd functions ({entry: symbol}), for summarise()'s hooks"""
    return {e: host_summary(n) for e, n in hle.items()}


# ---- the function's instructions, with control flow and calls
class Func:
    """kinds per instruction: 'op', 'goto' (succs), 'call' (callee or None: unknown; then the next instruction),
    'svc', 'tail' (callee or None), 'ret', 'trap'"""
    __slots__ = ("entry", "pcs", "words", "kind", "succ", "callee", "use", "dfn", "insn", "fallthrough_entry",
                 "fp", "writes_fpcr", "host_calls", "ctail", "falls_into", "hook", "guest")

    def __init__(self, entry, pcs, word, prog, hooks):
        self.entry = entry
        self.pcs = pcs
        idx = {pc: k for k, pc in enumerate(pcs)}
        n = len(pcs)
        self.words = [word(pc) for pc in pcs] if callable(word) else list(word)   # (or the words themselves)
        self.kind, self.succ, self.callee = [None] * n, [()] * n, [None] * n
        self.use, self.dfn, self.insn = [0] * n, [0] * n, [None] * n
        self.fp = self.writes_fpcr = False
        self.host_calls = 0
        self.ctail = {}         # call index -> the function its return falls into (the body ends after the call)
        self.falls_into = set() # entries this function falls into without a branch
        body = set(pcs)
        extra = rows()
        self.hook = ENTRY.get(entry)   # (a PATCH_HOOK: (reads, may-writes, guest calls), or None)
        self.guest = {}         # instruction index -> the guest functions a row there calls, or "computed"
        for k, pc in enumerate(pcs):
            i, u, d = def_use(self.words[k])
            row = extra.get(pc)
            if row is not None:   # (a probe or At row there: its declared reads and writes)
                u, d = u | row[0], d | row[1]
                if row[2]:
                    self.guest[k] = row[2]
                if row[3]:   # (in place of the call: the BL / BLR writes x30, the row what it declares)
                    u, d = row[0], row[1] | X[30]
            self.insn[k], self.use[k], self.dfn[k] = i, u, d
            if i is None:
                self.kind[k] = "trap"
                continue
            t, f = i.entry.template, i.f
            if sem.CLASS.get(t) in ("fp", "neon"):
                self.fp = True
            if t == "sysreg" and not f["L"] and (f["CRn"], f["op2"]) == (4, 0) and f["CRm"] == 4:
                self.writes_fpcr = True

            def to(tg, k=k):
                if tg in body and (tg == entry or tg not in prog.entries):
                    return idx[tg]
                return ("tail", prog.callee(tg))

            nxt = to(pc + 4) if pc + 4 in body or prog.callee(pc + 4) in prog.entries else None
            if t not in TERMS:
                self._edge(k, [nxt])
                if self.kind[k] == "tail":
                    self.falls_into.add(self.callee[k])
            elif row is not None and row[3]:   # (a row replaced the BL / BLR: no call)
                self._edge(k, [nxt])
            elif t == "b" and f["op"]:
                c = prog.callee(pc + (sx(f["imm26"], 26) << 2))
                self.kind[k], self.callee[k] = "call", c
                if c in hooks:
                    self.host_calls += 1
                self._edge(k, [nxt], keep_kind=True)
            elif t == "b":
                self._edge(k, [to(pc + (sx(f["imm26"], 26) << 2))])
            elif t in ("bcond", "cbz", "tbz"):
                off = f["imm19"] if t != "tbz" else f["imm14"]
                tg = pc + (sx(off, 19 if t != "tbz" else 14) << 2)
                self._edge(k, [to(tg), nxt])
            elif t == "blr":
                self.kind[k], self.callee[k] = "call", None
                self._edge(k, [nxt], keep_kind=True)
            elif t == "br":
                tgs = prog.jt.get(pc)
                if tgs:   # (the table's default, a64_ijump for an index out of range, is not a real path)
                    self._edge(k, [to(x) for x in sorted(set(tgs))])
                else:
                    self.kind[k], self.callee[k] = "tail", None
            elif t == "ret":
                self.kind[k] = "ret"
            elif t == "exc" and f["opc"] == 0:
                self.kind[k] = "svc"
                self._edge(k, [nxt], keep_kind=True)
            else:
                self.kind[k] = "trap"
        self.fallthrough_entry = False

    def _edge(self, k, targets, keep_kind=False):
        """successors of instruction k: local indices; ('tail', callee) exits become kind 'tail' when alone"""
        local, tails = [], []
        for t in targets:
            if t is None:
                continue
            if isinstance(t, tuple):
                tails.append(t[1])
            else:
                local.append(t)
        if keep_kind:   # (a call or SVC: its return falls through, maybe into another function)
            self.succ[k] = tuple(local)
            if tails:
                self.ctail[k] = tails[0]   # (a fall-through only if the callee returns: summarise() decides)
            return
        self.kind[k] = "goto" if local or not tails else "tail"
        if self.kind[k] == "tail" and not local:
            self.callee[k] = tails[0] if len(set(tails)) == 1 else None
            self.succ[k] = ()
            return
        self.succ[k] = tuple(local)
        if tails:   # (a branch that may leave or stay: the leaving edges as extra tail exits)
            self.callee[k] = ("tails", tuple(tails))


def sx(v, bits):
    return v - (1 << bits) if v >> (bits - 1) & 1 else v


class Summary:
    __slots__ = ("inputs", "clobbers", "mustdef", "preserved", "results", "returns", "demote", "unknown",
                 "frame", "frame_taken", "pairs", "boundary", "fp", "writes_fpcr", "lr_data")

    def __init__(self):
        # (the optimistic start of the fixed point: nothing read or written, everything preserved)
        self.inputs = self.clobbers = self.results = 0
        self.mustdef = ALL
        self.preserved = CALLEE_SAVED
        self.returns = False
        self.demote = []
        self.unknown = False
        self.frame = 0
        self.frame_taken = False
        self.pairs = 0
        self.boundary = False
        self.fp = self.writes_fpcr = False
        self.lr_data = False

    def key(self):
        return (self.inputs, self.clobbers, self.mustdef, self.preserved, self.returns, self.unknown, self.boundary,
                self.writes_fpcr)


UNKNOWN = Summary()
UNKNOWN.inputs, UNKNOWN.clobbers, UNKNOWN.mustdef, UNKNOWN.preserved = ALL, ALL, 0, 0
UNKNOWN.returns, UNKNOWN.unknown, UNKNOWN.boundary = True, True, True
AAPCS = Summary()   # (a callee the program cannot see: BLR, unresolved BR, host functions)
AAPCS.inputs, AAPCS.clobbers, AAPCS.mustdef, AAPCS.preserved = ARGS, CALLER_SAVED, X[30], CALLEE_SAVED
AAPCS.returns, AAPCS.boundary = True, True


NORETURN_HOST = Summary()   # (a seeded fatal function: AAPCS facts, never returns)
NORETURN_HOST.inputs, NORETURN_HOST.clobbers, NORETURN_HOST.mustdef = ARGS, CALLER_SAVED, ALL
NORETURN_HOST.returns, NORETURN_HOST.boundary = False, True
NORETURN = set()


def callee_summary(sums, c, hooks):
    if c in NORETURN:
        return NORETURN_HOST
    if c is None:
        return AAPCS
    h = hooks.get(c)
    if h is not None:   # (an HLE'd function: host_summary)
        return h
    if c not in sums:
        return AAPCS
    s = sums[c]
    return UNKNOWN if s.unknown else s


def site_summary(sums, fn, k, hooks):
    c = fn.callee[k]
    if isinstance(c, tuple):   # (a branch with some edges leaving the function)
        return None
    return callee_summary(sums, c, hooks)


# ---- the analyses of one function, given its callees' summaries
def liveness(fn, sums, hooks, ret_live=0):
    """live-in per instruction (backward, to a fixed point); the inputs are the entry's. ret_live: live at the
    returns (ALL: what may reach a return unwritten, the registers a call's value may pass on to the caller's own)"""
    use = fn.use
    n = len(fn.pcs)
    live = [0] * n
    changed = True
    while changed:
        changed = False
        for k in range(n - 1, -1, -1):
            kind = fn.kind[k]
            if kind == "ret":
                v = use[k] & ~X[30] | ret_live   # (RET's X30 is the host's return, not data)
            elif kind == "trap":
                v = use[k]
            elif kind == "tail":
                s = callee_summary(sums, fn.callee[k], hooks)
                v = use[k] | ((s.inputs | (ret_live & ~s.mustdef)) & ~fn.dfn[k])
            else:
                out = 0
                for j in fn.succ[k]:
                    out |= live[j]
                if k in fn.ctail:
                    out |= callee_summary(sums, fn.ctail[k], hooks).inputs | ret_live
                c = fn.callee[k]
                if isinstance(c, tuple):   # (leaving edges of a conditional or switch)
                    for t in c[1]:
                        out |= callee_summary(sums, t, hooks).inputs | ret_live
                if kind == "call" or kind == "svc":
                    s = AAPCS if kind == "svc" else callee_summary(sums, c, hooks)
                    if kind == "svc":
                        v = use[k] | (out & ~fn.dfn[k])
                    else:
                        if s.returns is False:
                            out = 0
                        v = use[k] | s.inputs | (out & ~(s.mustdef | X[30]))
                else:
                    v = use[k] | (out & ~fn.dfn[k])
            g = fn.guest.get(k)
            if g:   # (a row's guest calls read their inputs there)
                v |= guest_reads(sums, g, hooks)
            if v != live[k]:
                live[k] = v
                changed = True
    return live


def guest_reads(sums, calls, hooks):
    """what guest functions host code calls (a row's or a hook's) read: their inputs; "computed" every register"""
    if calls == "computed":
        return ALL
    m = 0
    for c in calls:
        m |= callee_summary(sums, c, hooks).inputs
    return m


def must_and_may(fn, sums, hooks):
    """(mustdef at the exits, clobbers, returns) forward"""
    n = len(fn.pcs)
    must = [None] * n
    must[0] = 0
    order = list(range(n))
    exit_must, may, returns = ALL, 0, False
    changed = True
    while changed:
        changed = False
        for k in order:
            if must[k] is None:
                continue
            kind = fn.kind[k]
            m = must[k]
            if kind == "call":
                s = callee_summary(sums, fn.callee[k], hooks)
                if not s.returns:
                    continue
                out = m | fn.dfn[k] | s.mustdef | X[30]
            elif kind == "svc":
                out = m | fn.dfn[k]
            else:
                out = m | fn.dfn[k]
            if kind in ("call", "svc") and k in fn.ctail:
                s2 = callee_summary(sums, fn.ctail[k], hooks)
                if s2.returns:
                    exit_must &= out | s2.mustdef
                    returns = True
            if kind == "ret":
                exit_must &= out
                returns = True
                continue
            if kind == "tail":
                s = callee_summary(sums, fn.callee[k], hooks)
                if s.returns:
                    exit_must &= out | s.mustdef
                    returns = True
                continue
            c = fn.callee[k]
            if isinstance(c, tuple):
                for t in c[1]:
                    s = callee_summary(sums, t, hooks)
                    if s.returns:
                        exit_must &= out | s.mustdef
                        returns = True
            for j in fn.succ[k]:
                nm = out if must[j] is None else must[j] & out
                if nm != must[j]:
                    must[j] = nm
                    changed = True
    if fn.hook is not None:   # (the hook handled the call: a return at the entry, having written at most x0-x7)
        exit_must, returns = 0, True
        may |= fn.hook[1]
    for k in range(n):
        may |= fn.dfn[k]
        if k in fn.ctail and (fn.kind[k] == "svc" or callee_summary(sums, fn.callee[k], hooks).returns):
            may |= callee_summary(sums, fn.ctail[k], hooks).clobbers   # (only if the call comes back)
        kind = fn.kind[k]
        if kind in ("call", "tail"):
            may |= callee_summary(sums, fn.callee[k], hooks).clobbers | (X[30] if kind == "call" else 0)
        c = fn.callee[k]
        if isinstance(c, tuple):
            for t in c[1]:
                may |= callee_summary(sums, t, hooks).clobbers
    return (exit_must if returns else ALL), may, returns


# frame tracking: state = (sp offset, x29 offset, frozenset of registers holding their entry value,
#                          tuple of (slot offset, register) pairs holding entry values)
TRACKED_BITS = {r: BIT[r] for r in TRACKED}
LDST_SIZES = {"ldst": None, "ldst_pair": None, "ldst_v": None, "ldst_v_pair": None}


def mem_access(i):
    """(base register, kind 'st'/'ld', [(register name, offset delta, size)], offset, writeback 'pre'/'post'/None)
    of an integer or vector load/store with an immediate offset, else None"""
    t, f = i.entry.template, i.f
    if t == "ldst":
        mode = i.entry.params["mode"]
        if mode == "reg" or (f["size"] == 3 and f["opc"] == 2):
            return None
        size = 1 << f["size"]
        off = f["imm12"] << f["size"] if mode == "uimm" else sx(f["imm9"], 9)
        wb = mode if mode in ("pre", "post") else None
        return (f["Rn"], "st" if f["opc"] == 0 else "ld", [("x%d" % f["Rt"], 0, size)], off, wb)
    if t == "ldst_pair":
        sc = 3 if f["opc"] == 2 else 2
        off = sx(f["imm7"], 7) << sc
        wb = {1: "post", 3: "pre"}.get(f["mode"])
        regs = [("x%d" % f["Rt"], 0, 1 << sc), ("x%d" % f["Rt2"], 1 << sc, 1 << sc)]
        return (f["Rn"], "ld" if f["L"] else "st", regs, off, wb)
    if t == "ldst_v":
        mode = i.entry.params["mode"]
        if mode == "reg":
            return None
        from .enc_fp import v_scale
        sc = v_scale(f["size"], f["opc"])
        off = f["imm12"] << sc if mode == "uimm" else sx(f["imm9"], 9)
        wb = mode if mode in ("pre", "post") else None
        return (f["Rn"], "ld" if f["opc"] & 1 else "st", [("v%d" % f["Rt"], 0, 1 << sc)], off, wb)
    if t == "ldst_mult":   # (LD1/ST1 of 1-4 registers: hand-written NEON saves d8-d15 so)
        from .enc_fp import MULT
        nreg, selem = MULT[f["opcode"]]
        if selem != 1:
            return None
        rb = 16 if f["Q"] else 8
        regs = [("v%d" % ((f["Rt"] + j) % 32), j * rb, rb) for j in range(nreg)]
        wb = None
        if i.entry.params.get("post"):
            if f["Rm"] != 31:
                return None
            wb = "post"
        return (f["Rn"], "ld" if f["L"] else "st", regs, nreg * rb if wb else 0, wb)
    if t == "ldst_v_pair":
        sc = 2 + f["opc"]
        off = sx(f["imm7"], 7) << sc
        wb = {1: "post", 3: "pre"}.get(f["mode"])
        regs = [("v%d" % f["Rt"], 0, 1 << sc), ("v%d" % f["Rt2"], 1 << sc, 1 << sc)]
        return (f["Rn"], "ld" if f["L"] else "st", regs, off, wb)
    return None


TRACE = None   # (debugging: a list here collects where frame_pass loses SP, its meets and exits)


def frame_pass(fn, sums, hooks):
    """(preserved, demotion reasons, frame size, frame address taken, deletable save/restore pairs, LR read as data)"""
    n = len(fn.pcs)
    start = (0, (), frozenset(TRACKED + ["sp"]), ())
    state = [None] * n
    state[0] = start
    work = [0]
    reasons = set()
    exits = []
    lowest = 0
    taken = False
    lr_data = False
    saves, restores, other_reads = {}, {}, set()
    while work:
        k = work.pop()
        sp, fr_t, holds, slots = state[k]
        fregs = dict(fr_t)   # (registers holding entry SP plus a constant: x29, and copies such as mov x9, sp)
        slots = dict(slots)
        holds = set(holds)
        i = fn.insn[k]
        kind = fn.kind[k]
        if i is not None:
            t, f = i.entry.template, i.f
            acc = mem_access(i)
            handled = False
            if t == "addsub_imm" and not f["S"]:
                imm = f["imm12"] << (12 if f["sh"] else 0)
                imm = -imm if f["op"] else imm
                rd, rn = f["Rd"], f["Rn"]
                src = sp if rn == 31 else fregs.get(rn)
                if rn == 31 or rn in fregs:
                    if rd == 31:
                        sp = src + imm if src is not None else None
                    else:
                        holds.discard("x%d" % rd)
                        if src is not None:
                            fregs[rd] = src + imm
                        else:
                            fregs.pop(rd, None)
                    handled = True
            if acc is not None and (acc[0] == 31 or acc[0] in fregs):
                base, rw, regs, off, wb = acc
                cur = sp if base == 31 else fregs.get(base)
                if cur is None:
                    if rw == "st" and any(r in ("x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27",
                                               "x28", "x29", "x30") and r in holds for r, _, _ in regs):
                        pass
                    addr = None
                else:
                    addr = cur + (off if wb != "post" else 0)
                for r, d, size in regs:
                    if addr is None:
                        if rw == "ld":
                            holds.discard(r)
                        continue
                    a = addr + d
                    if rw == "st":
                        for so in [x for x, (_, xs) in slots.items() if x < a + size and a < x + xs]:
                            del slots[so]
                        if size >= 8 and r in holds and r in TRACKED_BITS:
                            slots[a] = (r, size)
                            saves.setdefault((r, a), set()).add(k)
                    else:
                        if slots.get(a, (None,))[0] == r and size >= 8:
                            holds.add(r)
                            restores.setdefault((r, a), set()).add(k)
                        else:
                            holds.discard(r)
                            other_reads.add(a)
                if wb is not None:
                    if base == 31:
                        sp = sp + off if sp is not None else None
                    elif fregs.get(base) is not None:
                        fregs[base] += off
                if rw == "ld":
                    for r, _, _ in regs:
                        if r[0] == "x":
                            fregs.pop(int(r[1:]), None)
                handled = True
            elif acc is not None and acc[1] == "st":
                # a store through another pointer: of an entry value of X19-X30 (setjmp, context captures)
                if any(r in holds and r in TRACKED_BITS for r, _, _ in acc[2]):
                    reasons.add("save_ptr")
            if not handled:
                d = fn.dfn[k]
                if "x30" in holds and fn.use[k] & X[30] and kind not in ("ret",) and t not in ("blr", "br"):
                    if not (acc is not None and acc[1] == "st"):
                        lr_data = True
                if fn.use[k] & SP and t not in ("addsub_imm",) and acc is None:
                    if not (t == "ldst" and i.entry.params.get("mode") == "reg"):
                        taken = True
                for r in ir.VARS:
                    if d & BIT[r]:
                        holds.discard(r)
                if d & SP:
                    sp = None
                for rr in [x for x in fregs if d & X[x]]:
                    del fregs[rr]
            if sp is None and state[k][0] is not None and TRACE is not None and len(TRACE) < 50:
                TRACE.append(("%08x" % fn.pcs[k], i.entry.name if i else None))
            if sp is not None:
                lowest = min(lowest, sp)
        if kind == "call" or kind == "svc":
            s = AAPCS if kind == "svc" else callee_summary(sums, fn.callee[k], hooks)
            if kind == "call" and not s.returns:
                continue
            for r in list(holds):
                if r not in ("sp", "x30") and not (s.preserved & BIT[r]):
                    holds.discard(r)
            if not (s.preserved & SP):
                sp = None
            if kind == "call":
                holds.discard("x30")
                if any(r < 8 for r in fregs):   # (a frame address passed to the callee: the frame escapes)
                    taken = True
            for rr in [x for x in fregs if s.clobbers & X[x]]:
                del fregs[rr]
            if k in fn.ctail:   # (a return that falls into the next function: dangling_bl, not an exit)
                continue
        if kind == "ret":
            if "x30" not in holds or (i is not None and i.entry.name == "RET" and i.f["Rn"] != 30):
                reasons.add("lr_ret")
            if sp != 0 and TRACE is not None and len(TRACE) < 50:
                TRACE.append(("ret %08x" % fn.pcs[k], sp))
            exits.append((sp, holds))
            continue
        if kind == "tail":
            s = callee_summary(sums, fn.callee[k], hooks)
            if sp != 0 and fn.callee[k] is not None:
                reasons.add("split_frame")   # (a tail call with the frame still allocated: the target shares it)
            if s.returns:
                exits.append((sp, {r for r in holds if r == "sp" or s.preserved & BIT[r]} if s.preserved & SP else set()))
            continue
        c = fn.callee[k]
        if isinstance(c, tuple):
            for tc in c[1]:
                s = callee_summary(sums, tc, hooks)
                if s.returns:
                    exits.append((sp, {r for r in holds if s.preserved & BIT[r]}))
        new = (sp, tuple(sorted(fregs.items())), frozenset(holds), tuple(sorted(slots.items())))
        for j in fn.succ[k]:
            old = state[j]
            if old is None:
                state[j] = new
                work.append(j)
                continue
            if old[0] != new[0] and TRACE is not None and len(TRACE) < 50:
                TRACE.append(("meet %08x" % fn.pcs[j], old[0], new[0], "from %08x" % fn.pcs[k]))
            m = (old[0] if old[0] == new[0] else None, tuple(sorted(set(old[1]) & set(new[1]))), old[2] & new[2],
                 tuple(sorted(set(old[3]) & set(new[3]))))
            if m != old:
                state[j] = m
                work.append(j)
    if TRACE is not None and len(TRACE) < 50:
        TRACE.append(("exits", [(e_sp, sorted(h)) for e_sp, h in exits][:4]))
    preserved = CALLEE_SAVED
    for sp, holds in exits:
        p = sum(BIT[r] for r in holds if r in TRACKED_BITS)
        if sp == 0:
            p |= SP
        preserved &= p
    if exits and not preserved & SP:
        reasons.add("sp")
    pairs = 0
    for (r, a), ks in saves.items():
        if (r, a) in restores and a not in other_reads and preserved & BIT[r]:
            pairs += 1
    return preserved, reasons, -lowest, taken, pairs, lr_data


def analyse(fn, sums, hooks):
    s = Summary()
    live = liveness(fn, sums, hooks)
    s.inputs = live[0] if fn.pcs else 0
    if fn.hook is not None:   # (the hook reads its declared registers and what the guest code it calls reads)
        s.inputs |= fn.hook[0] | guest_reads(sums, fn.hook[2], hooks)
    s.mustdef, s.clobbers, s.returns = must_and_may(fn, sums, hooks)
    pres, reasons, s.frame, s.frame_taken, s.pairs, s.lr_data = frame_pass(fn, sums, hooks)
    s.preserved = pres if s.returns else CALLEE_SAVED
    if s.lr_data:
        reasons.add("lr_data")
    if s.returns and (CALLEE_SAVED & ~SP) & ~s.preserved:
        reasons.add("cs_clobber")
    if s.clobbers & s.preserved & ~s.inputs & sum(X[19:30]):
        # (a register it writes and claims preserved without reading it: no save is possible, the facts disagree)
        reasons.add("inconsistent")
    if s.inputs & NZCV:
        reasons.add("flags_in")
    if s.inputs & TMPS:
        reasons.add("tmp_in")
    if fn.fallthrough_entry:
        reasons.add("fallthrough_entry")
    for k, c in fn.ctail.items():   # (a BL with nothing after it must reach a callee that does not return)
        if fn.kind[k] == "call" and callee_summary(sums, fn.callee[k], hooks).returns:
            reasons.add("dangling_bl")
    for k, kind in enumerate(fn.kind):   # (x9-x18 or the flags read after a call that may change them)
        if kind == "call":
            cs = callee_summary(sums, fn.callee[k], hooks)
            out = 0
            for j in fn.succ[k]:
                out |= live[j]
            if out & ~cs.mustdef & cs.clobbers & (TMPS | NZCV):
                reasons.add("tmp_after_call")
    boundary = fn.hook is not None or any(g == "computed" for g in fn.guest.values())
    for k, kind in enumerate(fn.kind):
        c = fn.callee[k]
        if kind == "svc" or (kind in ("call", "tail") and (c is None or c in hooks)):
            boundary = True
        elif kind in ("call", "tail"):
            cs = sums.get(c)
            if cs is not None and (cs.boundary or cs.unknown):
                boundary = True
            if cs is not None and cs.unknown:
                reasons.add("unknown callee")
            if cs is not None and cs.writes_fpcr:
                s.writes_fpcr = True
    s.boundary = boundary
    s.fp = fn.fp
    s.writes_fpcr = s.writes_fpcr or fn.writes_fpcr
    s.unknown = bool(reasons & set(UNKNOWN_FACTS))
    s.demote = sorted(reasons)
    return s


# ---- the whole program
def sccs(nodes, succ):
    """Tarjan's strongly connected components, iteratively, callees before callers"""
    index, low, on, stack, out = {}, {}, set(), [], []
    counter = 0
    for root in nodes:
        if root in index:
            continue
        work = [(root, iter(succ(root)))]
        index[root] = low[root] = counter
        counter += 1
        stack.append(root)
        on.add(root)
        while work:
            v, it = work[-1]
            for w in it:
                if w not in index:
                    index[w] = low[w] = counter
                    counter += 1
                    stack.append(w)
                    on.add(w)
                    work.append((w, iter(succ(w))))
                    break
                if w in on:
                    low[v] = min(low[v], index[w])
            else:
                work.pop()
                if work:
                    low[work[-1][0]] = min(low[work[-1][0]], low[v])
                if low[v] == index[v]:
                    comp = []
                    while True:
                        w = stack.pop()
                        on.discard(w)
                        comp.append(w)
                        if w == v:
                            break
                    out.append(comp)
    return out


def patched(hooks, entry):
    """hooks as {entry: Summary} (a set: AAPCS facts for each); ENTRY set from entry ({entry: hook C name})"""
    ENTRY.clear()
    ENTRY.update(entry_hooks(entry or {}))
    return hooks if isinstance(hooks, dict) else {c: AAPCS for c in hooks}


def guest_callees(fn):
    """the guest functions host code in fn calls (its rows', its entry hook's)"""
    out = [c for g in fn.guest.values() if g != "computed" for c in g]
    return out + list(fn.hook[2]) if fn.hook is not None else out


def summarise(funcs, prog, hooks, word, names=None, log=print, entry=None):
    """funcs: entry -> sorted pcs; hooks: the HLE'd functions ({entry: Summary}, host_summaries; or a set: AAPCS);
    names: entry -> symbol name (the noreturn seeds); entry: {entry: hook C name} (PATCH_HOOKs). Returns (summaries,
    Funcs, statistics)"""
    t0 = time.time()
    hooks = patched(hooks, entry)
    NORETURN.clear()
    NORETURN.update(e for e, n in (names or {}).items() if noreturn_seed(n))
    F = {}
    for e, pcs in funcs.items():
        F[e] = Func(e, pcs, word, prog, hooks)
    # noreturn by the callers' evidence: a BL with nothing after it in its function (the compiler knew the callee
    # does not return); these seed the fixed point beside the fatal functions' names
    by_callers = set()
    for fn in F.values():
        for k in fn.ctail:
            if fn.kind[k] == "call" and fn.callee[k] is not None:
                by_callers.add(fn.callee[k])
    NORETURN.update(by_callers)
    # an entry another function falls into without a branch (after an instruction or a call that returns)
    for fn in F.values():
        for e in fn.falls_into:
            if e in F and e != fn.entry:
                F[e].fallthrough_entry = True
    t1 = time.time()

    def callees(e):
        fn = F[e]
        out = []
        for k, kind in enumerate(fn.kind):
            c = fn.callee[k]
            if isinstance(c, tuple):
                out += [t for t in c[1] if t in F]
            elif kind in ("call", "tail") and c in F:
                out.append(c)
        out += [t for t in fn.ctail.values() if t in F]
        return out + [t for t in guest_callees(fn) if t in F]
    comps = sccs(sorted(F), callees)
    sums = {}
    iters = 0
    for comp in comps:
        for e in comp:
            sums[e] = Summary()
        while True:
            iters += 1
            changed = False
            for e in comp:
                s = analyse(F[e], sums, hooks)
                if s.key() != sums[e].key() or s.demote != sums[e].demote:
                    changed = True
                sums[e] = s
            if not changed or (len(comp) == 1 and not _self_call(F[comp[0]])):
                break
    t2 = time.time()
    # results: what callers read after their calls, and what callers pass on as their own results
    passes = []
    for e, fn in F.items():
        for c, m, pt in call_reads(fn, sums, hooks):
            sums[c].results |= m
            passes.append((e, c, pt))
        for k, kind in enumerate(fn.kind):
            if kind == "tail" and fn.callee[k] in sums:
                passes.append((e, fn.callee[k], ALL))
    propagate_results(sums, passes)
    # a call whose return falls into another function's entry: that entry is entered by falling through
    for e, fn in F.items():
        for k, c in fn.ctail.items():
            if c in sums and callee_summary(sums, fn.callee[k], hooks).returns and "fallthrough_entry" not in sums[c].demote:
                sums[c].demote = sorted(sums[c].demote + ["fallthrough_entry"])
    t3 = time.time()
    disagree = sorted(c for c in by_callers if c in sums and sums[c].returns)
    stats = {"noreturn by callers": len(by_callers), "of them returning by their own code": len(disagree),
             "disagree sample": ["%08x" % c for c in disagree[:12]], "functions": len(F), "sccs": len(comps), "largest scc": max(len(c) for c in comps) if comps else 0,
             "iterations": iters, "build s": t1 - t0, "fixed point s": t2 - t1, "results s": t3 - t2}
    return sums, F, stats


def pack(s):
    """a summary as the plan keeps it (a tuple: inputs, clobbers, mustdef, preserved, results, returns, demotion
    reasons, unknown facts, frame, pairs, boundary, FP use, writes FPCR)"""
    return (s.inputs, s.clobbers, s.mustdef, s.preserved, s.results, s.returns, tuple(s.demote), s.unknown, s.frame,
            s.pairs, s.boundary, s.fp, s.writes_fpcr)


def _self_call(fn):
    return any(fn.callee[k] == fn.entry for k in range(len(fn.kind)))


def walk(entries, word, jt=None):
    """function bodies the way the present discovery walks them (for tests and tools without the lifter): from each
    entry, following branches, stopping at returns, traps, BR and other entries"""
    entries = set(entries)
    out = {}
    for e in entries:
        body, todo = set(), [e]
        while todo:
            pc = todo.pop()
            while pc not in body and (pc == e or pc not in entries):
                try:
                    w = word(pc)
                except (KeyError, IndexError):
                    break
                body.add(pc)
                i = enc.decode(w)
                if i is None:
                    break
                t, f = i.entry.template, i.f
                if t == "b" and not f["op"]:
                    todo.append(pc + (sx(f["imm26"], 26) << 2))
                    break
                if t in ("bcond", "cbz", "tbz"):
                    off = f["imm14"] if t == "tbz" else f["imm19"]
                    todo.append(pc + (sx(off, 14 if t == "tbz" else 19) << 2))
                if t in ("ret", "br", "udf") or (t == "exc" and f["opc"] != 0):
                    break
                pc += 4
        out[e] = sorted(body)
    return out


def edges_of(F):
    """(kind, callee) of every call and tail call of each function (report()'s input)"""
    return {e: [(kind, fn.callee[k]) for k, kind in enumerate(fn.kind)
                if kind in ("call", "tail") and not isinstance(fn.callee[k], tuple)] for e, fn in F.items()}


def report(sums, edges_by_fn, n_ins, hot=(), log=print):
    """the distribution of the summaries over the program; n_ins: entry -> instructions"""
    import collections
    total = sum(n_ins.values())
    full = [e for e, s in sums.items() if not s.demote]
    reasons = collections.Counter(r for s in sums.values() for r in s.demote)
    hot = [e for e in hot if e in sums]
    hot_full = [e for e in hot if not sums[e].demote]
    edges = collections.Counter()
    for e, ed in edges_by_fn.items():
        for kind, c in ed:
            tag = "tail " if kind == "tail" else ""
            if c is None:
                edges[tag + "indirect"] += 1
            elif c in NORETURN:
                edges[tag + "to a noreturn seed"] += 1
            elif c not in sums:
                edges[tag + "to host / hook"] += 1
            elif sums[c].demote:
                edges[tag + "direct to demoted"] += 1
            else:
                edges[tag + "direct to summarised"] += 1
    noret = sum(1 for s in sums.values() if not s.returns)
    log("summaries: %d functions; full %d (%.1f %% of %d instructions); hot set %d of %d full (%.1f %% of its "
        "instructions)" % (len(sums), len(full), 100.0 * sum(n_ins[e] for e in full) / max(total, 1), total,
                           len(hot_full), len(hot), 100.0 * sum(n_ins[e] for e in hot_full) /
                           max(1, sum(n_ins[e] for e in hot))))
    log("demoted by reason (a function can have several): %s" % ", ".join("%s %d" % kv for kv in reasons.most_common()))
    log("noreturn %d; boundary reachable %d; writes FPCR %d; uses FP/NEON %d" % (
        noret, sum(1 for s in sums.values() if s.boundary), sum(1 for s in sums.values() if s.writes_fpcr),
        sum(1 for s in sums.values() if s.fp)))
    log("callee-saved save/restore pairs deletable: %d (in %d functions)" % (
        sum(s.pairs for s in sums.values() if not s.demote), sum(1 for s in sums.values() if s.pairs and not s.demote)))
    log("call edges: %s" % ", ".join("%s %d" % kv for kv in sorted(edges.items(), key=lambda kv: -kv[1])))
    return {"full": len(full), "reasons": dict(reasons), "edges": dict(edges)}


def explain(e, sums, F, hooks, depth=2, log=print, _seen=None):
    """print a function's summary and, for each call and tail call, the callee's (debugging)"""
    _seen = _seen if _seen is not None else set()
    if e in _seen or e not in sums:
        return
    _seen.add(e)
    s, fn = sums[e], F[e]
    g = lambda m: ",".join(mask_names(m & 0xFFFFFFFF))
    log("%s f_%08x in %s | may %s | pres %s | %s | returns %s" % ("  " * (2 - depth), e, g(s.inputs), g(s.clobbers),
                                                                g(s.preserved), s.demote, s.returns))
    for k, kind in enumerate(fn.kind):
        c = fn.callee[k]
        if kind in ("call", "tail") or k in fn.ctail or isinstance(c, tuple):
            cs = [c] if not isinstance(c, tuple) else list(c[1])
            if k in fn.ctail:
                cs.append(("ctail", fn.ctail[k]))
            for cc in cs:
                t = cc[1] if isinstance(cc, tuple) else cc
                tag = "unknown" if t is None else "host" if t in hooks else "noreturn seed" if t in NORETURN else \
                    "not summarised" if t not in sums else ""
                log("%s   %08x %s -> %s %s" % ("  " * (2 - depth), fn.pcs[k], kind if not isinstance(cc, tuple) else cc[0],
                                            "%08x" % t if t is not None else "?", tag))
                if depth > 0 and t in sums:
                    explain(t, sums, F, hooks, depth - 1, log, _seen)


# ---- the same, in parallel: processes forked with the program's facts; the fixed point
# runs level by level over the DAG of strongly connected components (a component's callees are in earlier levels)
_G = {}


def _graph_part(entries):
    """(worker) per function: its callees, the functions it falls into, its call-continuation targets"""
    out = []
    for e in entries:
        fn = Func(e, *_G["funcs"][e], _G["prog"], _G["hooks"])
        callees = []
        for k, kind in enumerate(fn.kind):
            c = fn.callee[k]
            if isinstance(c, tuple):
                callees += list(c[1])
            elif kind in ("call", "tail") and c is not None:
                callees.append(c)
        dangling = [fn.callee[k] for k in fn.ctail if fn.kind[k] == "call" and fn.callee[k] is not None]
        tails = [fn.callee[k] for k, kind in enumerate(fn.kind) if kind == "tail" and isinstance(fn.callee[k], int)]
        ctails = [(fn.callee[k], c, fn.kind[k]) for k, c in fn.ctail.items()]
        edges = [(kind, fn.callee[k]) for k, kind in enumerate(fn.kind)
                 if kind in ("call", "tail") and not isinstance(fn.callee[k], tuple)]
        out.append((e, callees + list(fn.ctail.values()) + guest_callees(fn), list(fn.falls_into), dangling, tails,
                    ctails, edges))
    return out


def _analyse_part(batch):
    """(worker) the fixed point of each component of a batch, given its callees' summaries"""
    comps, known = batch
    sums = dict(known)
    hooks = _G["hooks"]
    out = {}
    for comp in comps:
        F = {}
        for e in comp:
            F[e] = Func(e, *_G["funcs"][e], _G["prog"], hooks)
            F[e].fallthrough_entry = e in _G["fallthrough"]
            sums[e] = Summary()
        while True:
            changed = False
            for e in comp:
                s = analyse(F[e], sums, hooks)
                if s.key() != sums[e].key() or s.demote != sums[e].demote:
                    changed = True
                sums[e] = s
            if not changed or (len(comp) == 1 and not _self_call(F[comp[0]])):
                break
        for e in comp:
            out[e] = sums[e]
    return out


def call_reads(fn, sums, hooks):
    """[(callee, what the caller reads after the call, what may reach the caller's returns unwritten)] of a
    function's calls of functions with summaries"""
    out = []
    live = pt = None
    for k, kind in enumerate(fn.kind):
        if kind == "call" and fn.callee[k] in sums:
            if live is None:
                live = liveness(fn, sums, hooks)
                pt = liveness(fn, sums, hooks, ALL)
            o = p = 0
            for j in fn.succ[k]:
                o |= live[j]
                p |= pt[j]
            g = sums[fn.callee[k]]
            out.append((fn.callee[k], o & g.clobbers, p & g.clobbers))
    return out


def propagate_results(sums, passes):
    """the AAPCS64's results for callers the program cannot see; then, to a fixed point, a caller's results that a
    callee's value may become (passes: (caller, callee, mask): what may reach the caller's returns unwritten after
    the call; everything after a tail call). A preserved register is never a result (read after the call, it is the
    caller's own value)."""
    for s in sums.values():
        s.results = (s.results | (ABI_RESULTS & s.clobbers)) & ~s.preserved
    changed = True
    while changed:
        changed = False
        for e, c, m in passes:
            g = sums[c]
            r = g.results | (sums[e].results & m & g.clobbers & ~g.preserved)
            if r != g.results:
                g.results = r
                changed = True


def _results_part(batch):
    """(worker) what each function's callers read after its calls and pass on: [(caller, callee, read, passed)]"""
    entries, known = batch
    sums = known
    hooks = _G["hooks"]
    out = []
    for e in entries:
        fn = Func(e, *_G["funcs"][e], _G["prog"], hooks)
        out.extend((e, c, m, pt) for c, m, pt in call_reads(fn, sums, hooks))
    return out


def summarise_parallel(funcs, prog, hooks, word, names=None, jobs=16, log=print, entry=None):
    """summarise() over processes: the same summaries; funcs: entry -> sorted pcs (any sequence), or with word None
    entry -> (pcs, their words). The workers are forked with the functions as flat arrays (pages they only read,
    shared)."""
    import multiprocessing as mp
    from array import array
    t0 = time.time()
    ctx = mp.get_context("fork")
    _G.clear()
    if word is None:
        fs = {e: (array("Q", p), array("I", w)) for e, (p, w) in funcs.items()}
    else:
        fs = {e: (array("Q", pcs), array("I", (word(pc) for pc in pcs))) for e, pcs in funcs.items()}
    _G.update(funcs=fs, prog=prog, hooks=patched(hooks, entry), fallthrough=set())
    hooks = _G["hooks"]
    NORETURN.clear()
    NORETURN.update(e for e, n in (names or {}).items() if noreturn_seed(n))
    order = sorted(_G["funcs"])
    chunks = [order[k::jobs * 8] for k in range(jobs * 8)]
    with ctx.Pool(jobs) as pool:
        graph = [r for part in pool.map(_graph_part, chunks) for r in part]
    callees, tails, ctails, edges = {}, {}, {}, {}
    by_callers = set()
    for e, cs, falls, dangling, tl, ct, ed in graph:
        edges[e] = ed
        callees[e] = [c for c in cs if c in _G["funcs"]]
        tails[e] = tl
        ctails[e] = ct
        by_callers.update(dangling)
        for f in falls:
            if f in _G["funcs"] and f != e:
                _G["fallthrough"].add(f)
    NORETURN.update(by_callers)
    t1 = time.time()
    comps = sccs(order, lambda e: callees[e])
    comp_of = {e: k for k, c in enumerate(comps) for e in c}
    level = [0] * len(comps)
    for k, c in enumerate(comps):   # (callees first: a component's level is one above its deepest callee's)
        lv = 0
        for e in c:
            for g in callees[e]:
                kk = comp_of[g]
                if kk != k:
                    lv = max(lv, level[kk] + 1)
        level[k] = lv
    levels = {}
    for k, lv in enumerate(level):
        levels.setdefault(lv, []).append(comps[k])
    sums = {}
    with ctx.Pool(jobs) as pool:
        for lv in sorted(levels):
            cs = levels[lv]
            nb = max(1, min(len(cs), jobs * 4))
            batches = []
            for b in range(nb):
                part = cs[b::nb]
                need = {g for c in part for e in c for g in callees[e]}
                batches.append((part, {g: sums[g] for g in need if g in sums}))
            for got in pool.map(_analyse_part, batches):
                sums.update(got)
        t2 = time.time()
        nb = jobs * 8
        batches = []
        for b in range(nb):
            part = order[b::nb]
            need = {g for e in part for g in callees[e]}
            batches.append((part, {g: sums[g] for g in need}))
        passes = []
        for got in pool.map(_results_part, batches):
            for e, g, m, pt in got:
                sums[g].results |= m
                passes.append((e, g, pt))
    passes += [(e, c, ALL) for e, tl in tails.items() for c in tl if c in sums]
    propagate_results(sums, passes)
    for e, ct in ctails.items():   # (a call whose return falls into another function's entry)
        for callee, c, kind in ct:
            if c in sums and (kind == "svc" or callee_summary(sums, callee, hooks).returns) \
                    and "fallthrough_entry" not in sums[c].demote:
                sums[c].demote = sorted(sums[c].demote + ["fallthrough_entry"])
    t3 = time.time()
    stats = {"functions": len(sums), "sccs": len(comps), "largest scc": max(len(c) for c in comps) if comps else 0,
             "levels": len(levels), "noreturn by callers": len(by_callers),
             "graph s": t1 - t0, "fixed point s": t2 - t1, "results s": t3 - t2}
    return sums, stats, edges


def job(inp, out, jobs, entry):
    """the summaries in a process of their own (game.finish_summaries): inp, a pickle of the functions (entry -> (pcs,
    words)) and the discovery's facts; out, a pickle of (summaries, stats, edges). Its workers fork from a process
    that holds only this, not the lifter."""
    import pickle
    import resource
    from . import build_ir
    with open(inp, "rb") as f:
        d = pickle.load(f)
    plt = d["plt"]

    def callee(t):
        seen = 0
        while t in plt and seen < 4:
            t = plt[t][0]
            seen += 1
        return t
    prog = build_ir.Program(entries=d["entries"], jt=d["jt"], callee=callee, plt=plt, no_call=d["no_call"],
                            classes=d["classes"])
    funcs, names = d["funcs"], d["names"]
    hooks = host_summaries(d["hle"])
    del d
    sums, stats, edges = summarise_parallel(funcs, prog, hooks, None, names, jobs=jobs, entry=entry)
    stats["main peak MB"] = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss >> 10
    stats["worker peak MB"] = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss >> 10
    with open(out + ".tmp", "wb") as f:
        pickle.dump((sums, stats, edges), f, protocol=pickle.HIGHEST_PROTOCOL)
    os.replace(out + ".tmp", out)
