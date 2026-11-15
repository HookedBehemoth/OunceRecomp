"""Call boundaries: one record per direct callee (BL target), computed once from the plan's summaries and the host's
declarations (nx2/lift2/host_effects.py), and read by the lift (what stays in SSA across a BL, the trap after a call
that never returns, the call's symbol), the verification build, the build's call report and nx2/lift2/dump.py.

A record (Bound) says, as summary masks (nx2/lift2/summary.py: x0-x30, SP, the flags, v0-v31):
- kind: why it is what it is (the report's reason);
- reads: the registers the callee may read; clobbers: those it may write; must: those it writes on every path that
  returns; results: those its callers may read after it returns;
- keeps: the x19-x29 and SP bits a BL keeps in SSA across the call;
- noreturn: proven never to return (the BL is followed by a trap);
- symbol: the symbol a BL calls; tail: the symbol a tail call jumps to.
Anything without a declaration or a full summary reads and writes every register, writes none on every path, keeps
nothing and returns (UNKNOWN): a callee nobody classified can only cost speed.
"""
from collections import namedtuple

from . import host_effects, summary

ALL = summary.ALL
KEEP = [(r, "x%d" % r) for r in range(19, 30)] + [(31, "sp")]   # (summary bits: x19-x29, SP)
KEEP_ALL = sum(1 << r for r, _ in KEEP)

Bound = namedtuple("Bound", "kind reads clobbers keeps must results noreturn symbol tail")


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


def call_bounds(plan, hooks, hle, verify=False):
    """bound(callee) -> Bound, memoised. plan: the build's plan (summaries, HLE'd functions as "hooks", setjmp owners
    as "sj_rets"); hooks: {address: entry hook C name}; hle: {address: HLE symbol} (host_effects.hle_names); verify:
    HOOKSHOT_LIFT_VERIFY (a BL to a clean HLE'd function goes through its hchk_ stub, a tail call of a checked
    function to its body)."""
    summ = plan.get("summaries", {})
    sj = plan.get("sj_rets", {})
    hle_set = set(plan.get("hooks", ()))
    memo = {}

    def make(c):
        sym = "f_%08x" % c
        tail = "body_%08x" % c if verify and verified(plan, c) else sym
        if c in sj:   # (hos_sj_call through a per-owner stub, host/hos_setjmp.c)
            return unknown("setjmp owner", "sjcall_%08x" % c, tail)
        if c in hle_set:
            name = hle.get(c)
            if name is None or name in host_effects.CLOBBERS:
                return unknown("HLE declared clobbering", sym, tail)
            # (AAPCS-clean by declaration, as the summaries take it (summary.host_summary): its arguments and the
            # declared extra reads, the caller-saved registers written; the verification build checks x19-x29 and SP
            # on its return through the hchk_ stub)
            h = summary.host_summary(name)
            return Bound("HLE", h.inputs, h.clobbers, KEEP_ALL, 0, summary.ABI_RESULTS, False,
                         "hchk_%08x" % c if verify else sym, tail)
        if c in hooks:
            # (the function's summary counts its hook: summary.ENTRY. A BL keeps what the body and the guest code the
            # hook calls (host_effects.CALLS) all preserve)
            s = summ.get(c)
            keeps = KEEP_ALL
            for g in (c,) + tuple(host_effects.CALLS.get(hooks[c], ())):
                sg = summ.get(g)
                keeps = keeps & sg[3] if full(sg) else 0
            if not full(s) or not keeps:
                return unknown("entry hook, body not proven", sym, tail)
            return Bound("entry hook", s[0], s[1], keeps, s[2], s[4], False, sym, tail)
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
    return bound


# the reasons call_report gives a site that keeps nothing, by its record's kind
REPORT = {"lifted": "full, nothing preserved", "noreturn": "noreturn (trap after)"}


def call_report(plan, bound, chosen):
    """one line: the direct call sites (BL) of the lifted functions, those keeping registers across the call (and how
    many on average; how many of them call host code), and those reloading every register, by reason; BLR sites"""
    import collections
    callee = callee_fn(plan["plt"])
    why, kept, blr, host = collections.Counter(), 0, 0, 0
    for e in chosen:
        pcs, words = plan["bodies"][e]
        for pc, w in zip(pcs, words):
            if (w & 0xFC000000) != 0x94000000:
                blr += (w & 0xFFFFFC1F) == 0xD63F0000 or (w & 0xFFFFF800) == 0xD73F0800
                continue
            b = bound(callee((pc + ((w & 0x3FFFFFF) - ((w & 0x2000000) << 1)) * 4) & 0xFFFFFFFFFFFFFFFF))
            if b.keeps:
                why["kept"] += 1
                kept += bin(b.keeps).count("1")
                host += b.kind in ("HLE", "entry hook")
            else:
                why[REPORT.get(b.kind, b.kind)] += 1
    return ("calls: %d direct sites, %d keep registers across the call (%.1f a site of x19-x29, SP; %d of them to host "
            "callees); reload all: %s; %d BLR sites reload all" % (
                sum(why.values()), why["kept"], kept / max(1, why["kept"]), host,
                ", ".join("%s %d" % kv for kv in why.most_common() if kv[0] != "kept"), blr))


def callee_fn(plt):
    """the function a call of t reaches: PLT stubs followed to their target"""
    def callee(t):
        seen = 0
        while t in plt and seen < 4:
            t = plt[t][0]
            seen += 1
        return t
    return callee
