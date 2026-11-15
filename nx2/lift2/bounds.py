"""Call boundaries: one record per direct callee (BL target), computed once from the plan's summaries and the host's
declarations (nx2/lift2/host_effects.py), and read by the lift (what stays in SSA across a BL, the trap after a call
that never returns, the call's symbol), the verification build, the build's call report and nx2/lift2/dump.py.

A record (Bound) says, as summary masks (nx2/lift2/summary.py: x0-x30, SP, the flags, v0-v31):
- kind: why it is what it is (the report's reason);
- reads: the registers the callee may read; clobbers: those it may write; must: those it writes on every path that
  returns; results: those its callers may read after it returns;
- keeps: the x19-x29 and SP bits a BL keeps in SSA across the call;
- noreturn: proven never to return (the BL is followed by a trap);
- symbol: the symbol a BL calls; tail: the symbol a tail call jumps to;
- answer: a PATCH_ANSWER's value (host/patch.h): a BL to it is no call, the lift writes the value to x0 and x30 as the BL
  does (and a tail call is a return with x0 the value); None for any other callee.
Anything without a declaration or a full summary reads and writes every register, writes none on every path, keeps
nothing and returns (UNKNOWN): a callee nobody classified can only cost speed.
"""
from collections import namedtuple

from . import host_effects, summary

ALL = summary.ALL
KEEP = [(r, "x%d" % r) for r in range(19, 30)] + [(31, "sp")]   # (summary bits: x19-x29, SP)
KEEP_ALL = sum(1 << r for r, _ in KEEP)

Bound = namedtuple("Bound", "kind reads clobbers keeps must results noreturn symbol tail answer", defaults=(None,))


def unknown(kind, symbol, tail=None):
    """the record of a callee nothing is known of: reads and writes everything, keeps nothing"""
    return Bound(kind, ALL, ALL, 0, 0, ALL, False, symbol, tail or symbol)


def keep_vars(b):
    """the guest variables a BL to b's callee keeps in SSA"""
    return [v for r, v in KEEP if b.keeps >> r & 1]


def mask(names):
    return sum(summary.BIT[v] for v in names)


def full(s):
    """a packed summary with every fact known and no demotion, of a function that returns"""
    return s is not None and not s[6] and not s[7] and bool(s[5])


def verified(plan, e):
    """a lifted function the verification build checks: a full summary (no demotion)"""
    s = plan.get("summaries", {}).get(e)
    return e in plan.get("bodies", {}) and s is not None and not s[6] and not s[7]


def call_bounds(plan, hle, verify=False, funcs=None):
    """bound(callee) -> Bound, memoised. plan: the build's plan (summaries, HLE'd functions as "hooks", setjmp owners
    as "sj_rets"); hle: {address: HLE symbol} (host_effects.hle_names); verify:
    HOOKSHOT_LIFT_VERIFY (a BL to a clean HLE'd function goes through its hchk_ stub, a tail call of a checked
    function to its body); funcs: {address: patch_scan.Func} (PATCH_FUNCs)."""
    summ = plan.get("summaries", {})
    funcs = funcs or {}
    sj = plan.get("sj_rets", {})
    hle_set = set(plan.get("hooks", ()))
    memo = {}

    def make(c, body=False):
        sym = "f_%08x" % c
        tail = "body_%08x" % c if verify and verified(plan, c) else sym
        if c in sj:   # (hos_sj_call through a per-owner stub, host/hos_setjmp.c)
            return unknown("setjmp owner", "sjcall_%08x" % c, tail)
        if c in hle_set:
            name = hle.get(c)
            if host_effects.stops(name):   # (a generated import stub, an NVN export without a proc: it stops the game)
                return Bound("stop", ALL, ALL, 0, 0, ALL, True, sym, tail)
            value = host_effects.answer(name)
            if value is not None:   # (no call: x0 the answer, x30 the BL's; summary.host_summary the same)
                x0 = summary.X[0]
                return Bound("answer", 0, x0 | summary.X[30], KEEP_ALL, x0 | summary.X[30], x0, False, sym, tail, value)
            if name is None or host_effects.clobbers(name) is not None:
                return unknown("HLE declared clobbering", sym, tail)
            # (AAPCS-clean by declaration, as the summaries take it (summary.host_summary): its arguments and the
            # declared extra reads, the caller-saved registers written; the verification build checks x19-x29 and SP
            # on its return through the hchk_ stub)
            h = summary.host_summary(name)
            return Bound("HLE", h.inputs, h.clobbers, KEEP_ALL, 0, summary.ABI_RESULTS, False,
                         "hchk_%08x" % c if verify else sym, tail)
        if c in funcs and not body:
            return func_bound(funcs, summ, c, "hchk_%08x" % c if verify else sym, sym)
        s = summ.get(c)
        if s is None:
            return unknown("no summary", sym, tail)
        if s[5] is False:
            return Bound("noreturn", ALL, ALL, 0, 0, ALL, True, sym, tail)
        if s[6] or s[7]:
            return unknown("demoted", sym, tail)
        return Bound("lifted", s[0], s[1], s[3] & KEEP_ALL, s[2], s[4], False, sym, tail)

    def bound(c):
        b = memo.get(c)
        if b is None:
            b = memo[c] = make(c)
        return b

    def own(c):
        """the record of c's lifted body itself (a PATCH_FUNC's body: what its orig call reads and keeps)"""
        return make(c, True) if c in funcs else bound(c)
    bound.own = own
    return bound


def func_bound(funcs, summ, c, sym, tail, _seen=()):
    """the record of a function replaced by a PATCH_FUNC (host/patch.h), from its declaration: it reads its operands and
    what the guest functions it calls read (guest_call puts every register back), writes its result and nothing else
    of the context (its adapter writes the result; the host function takes values), keeps x19-x29 and SP (the
    verification build checks it as an HLE'd callee); with orig, its lifted body may run: the body's record joins it
    with x0-x8 (guest_orig's arguments), and a body without a full summary makes it unknown (summary.func_summary is
    the same rule)"""
    f = funcs[c]
    reads = mask(f.reads())
    for g in f.guest:
        if g in funcs:   # (a replaced function: what its record reads; a cycle of them once)
            if g not in _seen and g != c:
                reads |= func_bound(funcs, summ, g, sym, tail, _seen + (c,)).reads
            continue
        sg = summ.get(g)
        reads |= sg[0] if sg is not None and not sg[7] else ALL
    result = summary.BIT[probes_var(f.result)] if f.result else 0
    clobbers, keeps = summary.X[30] | result, KEEP_ALL
    if f.orig:
        s = summ.get(c)
        if not full(s):
            return unknown("PATCH_FUNC with orig, body not proven", sym, tail)
        reads, clobbers, keeps = reads | s[0], clobbers | s[1] | summary.ORIG_ARGS, keeps & s[3]
    return Bound("PATCH_FUNC", reads, clobbers, keeps, summary.X[30] | result, result, False, sym, tail)


def probes_var(r):
    from . import probes
    return probes._var(r)


# the reasons call_report gives a site that keeps nothing, by its record's kind
REPORT = {"lifted": "full, nothing preserved", "noreturn": "noreturn (trap after)",
          "stop": "a stop (noreturn, trap after)"}


def call_report(plan, bound, chosen):
    """one line: the direct call sites (BL) of the lifted functions, those keeping registers across the call (and how
    many on average; how many of them call host code), and those reloading every register, by reason; BLR sites"""
    import collections
    callee = callee_fn(plan["plt"])
    why, kept, blr, host, answered = collections.Counter(), 0, 0, 0, 0
    for e in chosen:
        pcs, words = plan["bodies"][e]
        for pc, w in zip(pcs, words):
            if (w & 0xFC000000) != 0x94000000:
                blr += (w & 0xFFFFFC1F) == 0xD63F0000 or (w & 0xFFFFF800) == 0xD73F0800
                continue
            b = bound(callee((pc + ((w & 0x3FFFFFF) - ((w & 0x2000000) << 1)) * 4) & 0xFFFFFFFFFFFFFFFF))
            if b.answer is not None:
                answered += 1
                continue
            if b.keeps:
                why["kept"] += 1
                kept += bin(b.keeps).count("1")
                host += b.kind in ("HLE", "PATCH_FUNC")
            else:
                why[REPORT.get(b.kind, b.kind)] += 1
    return ("calls: %d direct sites, %d keep registers across the call (%.1f a site of x19-x29, SP; %d of them to host "
            "callees); reload all: %s; %d BLR sites reload all; %d BL sites of an answered function make no call" % (
                sum(why.values()), why["kept"], kept / max(1, why["kept"]), host,
                ", ".join("%s %d" % kv for kv in why.most_common() if kv[0] != "kept"), blr, answered))


def callee_fn(plt):
    """the function a call of t reaches: PLT stubs followed to their target"""
    def callee(t):
        seen = 0
        while t in plt and seen < 4:
            t = plt[t][0]
            seen += 1
        return t
    return callee
