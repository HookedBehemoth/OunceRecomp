"""Patch tools: what a patch site is, whether the host's declarations fit the lifted plan, and what each costs; from the
plan and its summaries and the scan of the host's declarations (nx2/lift2/patch_scan.py), with no compile and no lift.

    python -m nx2.patch --game games/oot site <address or label>   (or python -I nx2/patch.py ...)
    python -m nx2.patch --game games/oot check [<file:line> | --all | -]   (-: a proposed declaration on stdin)
    python -m nx2.patch --game games/oot report

site: the instruction, its function, the paths into it, the registers live into and out of it, the function's
call-boundary record, the declarations near it, and the declaration to paste with the prototype its macro generates.
check: every declaration (or one) against the plan, with the checks the build makes, each error with its file:line;
with one declaration (file:line, or one on stdin) also what it costs: the summaries computed again without it (or with
the proposed one) by the build's own summary job from the input the lift left, and the difference (functions whose
record changes, summaries demoted, call sites whose kept registers change). That takes about two minutes and the
summary job's memory (several GB).
report: every declaration with its local cost: the registers it reads and writes beyond its instruction's own, whether
its function's record changes without it, and how many call sites keep other registers because of it.

--plan: the plan (default <work>/sa/plan.pickle); its gen/ beside it gives the HLE'd functions and the replaced
functions' symbols.
Nothing is written beside the plan.
"""
import argparse
import collections
import os
import pickle
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
from lift2 import enc  # noqa: E402  (first: its tables import asm)
from lift2 import asm, bounds, build_ir, host_effects, patch_scan, probes, summary  # noqa: E402


# ---- the game: its labels and its declarations (the one place that knows where they come from)
def use_game(path):
    """(labels {address: label}, the PatchSet) of the game described at path (nx2/gamedesc.py)"""
    import gamedesc
    import label_syms
    gamedesc.use(path)
    return label_syms.read(), patch_scan.patches()


# ---- the plan, as the tools read it
class Lazy(dict):
    """the plan's packed summaries as summary.Summary objects, unpacked when read"""
    def __init__(self, packed):
        super().__init__()
        self.packed = packed

    def __contains__(self, e):
        return e in self.packed

    def __getitem__(self, e):
        if not dict.__contains__(self, e):
            dict.__setitem__(self, e, summary.unpack(self.packed[e]))
        return dict.__getitem__(self, e)

    def get(self, e, default=None):
        return self[e] if e in self.packed else default


class Plan:
    """the plan and what the tools derive from it: the program (entries, jump tables, PLT), the HLE'd functions'
    summaries, the replaced functions of the declarations (PATCH_FUNC), the call-boundary records"""
    def __init__(self, plan, gen, labels=None, ps=None):
        self.plan, self.gen, self.labels = plan, gen, labels or {}
        self.ps = ps or patch_scan.patches()
        plt = plan["plt"]
        self.callee = bounds.callee_fn(plt)
        self.prog = build_ir.Program(entries=set(plan["entries"]), jt=plan["jt"], callee=self.callee, plt=plt)
        self.hle = dict(plan.get("hle") or {})
        self.hle_c = host_effects.hle_names(gen) or self.hle
        self.funcs = patch_scan.funcs(gen, self.ps)[0]
        self.host = summary.patched(summary.host_summaries(self.hle), summary.func_facts(self.funcs))
        self.sums = Lazy(plan.get("summaries") or {})
        self.bound = bounds.call_bounds(plan, self.hle_c, False, self.funcs)
        self._funcs, self._callers = {}, None
        summary._ROWS = None

    def name(self, e):
        lab = self.labels.get(e)
        return "f_%08x%s" % (e, " (%s)" % lab if lab else "")

    def word(self, pc):
        """the instruction word at pc from the plan's bodies (None outside them)"""
        hits = patch_scan.containing(self.plan, [pc])[pc]
        if not hits:
            return None
        pcs, words = self.plan["bodies"][hits[0]]
        return words[list(pcs).index(pc)]

    def func(self, e):
        """summary.Func of the function at e, with the declarations' rows"""
        fn = self._funcs.get(e)
        if fn is None:
            pcs, words = self.plan["bodies"][e]
            fn = self._funcs[e] = summary.Func(e, list(pcs), list(words), self.prog, self.host)
        return fn

    def live(self, e):
        """(live-in, live-out) per instruction of the function at e: what its callers read after it is live at its
        returns (its summary's results)"""
        fn = self.func(e)
        s = self.sums.get(e)
        ret = s.results if s is not None else summary.ABI_RESULTS
        li = summary.liveness(fn, self.sums, self.host, ret)
        return li, summary.live_out(fn, self.sums, self.host, li, ret)

    def callers(self):
        """{callee: number of BL sites to it} over every body"""
        if self._callers is None:
            n = collections.Counter()
            for e, (pcs, words) in self.plan["bodies"].items():
                for pc, w in zip(pcs, words):
                    if w & 0xFC000000 == 0x94000000:
                        n[self.callee((pc + ((w & 0x3FFFFFF) - ((w & 0x2000000) << 1)) * 4) & 0xFFFFFFFFFFFFFFFF)] += 1
            self._callers = n
        return self._callers


def regs(m):
    """a register mask as text: x0-x3,x8,sp,nzcv,v0-v1"""
    names = summary.mask_names(m)
    out, run = [], []

    def flush():
        if run:
            out.append(run[0] if len(run) == 1 else "%s-%s" % (run[0], run[-1]))
        run.clear()
    for prefix in ("x", "v"):
        last = None
        for k in range(32):
            if "%s%d" % (prefix, k) in names:
                if last is not None and last != k - 1:
                    flush()
                run.append("%s%d" % (prefix, k))
                last = k
        flush()
        if prefix == "x":
            if "sp" in names:
                out.append("sp")
            if all(f in names for f in "nzcv"):
                out.append("nzcv")
            else:
                out.extend(f for f in "nzcv" if f in names)
    return ",".join(out) or "-"


def text(w, pc):
    i = enc.decode(w)
    return asm.render(i, pc) if i is not None else "(not decoded)"


def record(b):
    return ("%s: reads %s | clobbers %s | keeps %s | must %s | results %s%s" % (
        b.kind, regs(b.reads), regs(b.clobbers), regs(b.keeps), regs(b.must), regs(b.results),
        " | noreturn" if b.noreturn else ""))


def resolve_addr(arg, labels):
    by = {v: k for k, v in labels.items()}
    if arg in by:
        return by[arg]
    try:
        return int(arg.replace("f_", "0x") if arg.startswith("f_") else arg, 16 if not arg.startswith("0x") else 0)
    except ValueError:
        raise SystemExit("patch: %s is neither an address nor a label" % arg)


# ---- site
def suggest(P, e, k, li, lo):
    """(mode, result, operands, why) for a PATCH_AT at instruction k of e, a suggestion from what holds there:
    at a BL, instead, the callee's read argument registers and its first result (its call-boundary record); at a BLR,
    instead, the AAPCS arguments live into it and the target register; elsewhere, after, the registers the instruction
    reads and writes, then the general registers live into it; the result only a register the build accepts (the
    instruction's destination, live after it)"""
    fn = P.func(e)
    w = fn.words[k]
    gpr = lambda m: [r for r in range(31) if m >> r & 1]
    if probes.is_call(w):
        i = enc.decode(w)
        c = P.callee(build_ir.target(i, fn.pcs[k])) if i.entry.template == "b" else None
        if c is not None:
            b = P.bound(c)
            args = [r for r in gpr(b.reads) if r < 8] if b.reads != summary.ALL else [r for r in range(8) if li[k] >> r & 1]
            res = next(("x%d" % r for r in gpr(b.results & lo[k]) if r < 8), "none")
            why = "the callee %s: %s" % (P.name(c), "reads %s, results %s" % (regs(b.reads), regs(b.results))
                                         if b.reads != summary.ALL else "%s, its record unknown: the live arguments" % b.kind)
        else:
            rn = enc.decode(w).f["Rn"]
            args = [r for r in range(8) if li[k] >> r & 1] + ([rn] if rn < 31 and rn >= 8 else [])
            res = "x0" if lo[k] & 1 else "none"
            why = "an indirect call (x%d): the AAPCS arguments live into it and its target" % rn
        return "instead", res, ["x%d" % r for r in args][:8], why
    d = probes.dest(w)
    _, use, dfn = summary.def_use(w)
    res, first = "none", []
    if d is not None and lo[k] >> d[0] & 1:
        res = ("w" if d[1] == 32 else "x") + str(d[0])
        first = [d[0]]
    order = first + [r for r in gpr(use | dfn) if r not in first]
    order += [r for r in gpr(li[k]) if r not in order]
    return "after", res, ["x%d" % r for r in order][:6], "the instruction's registers, then those live into it"


def verdict(P, e, decl):
    """the build's checks of one declaration's text at its function e: [errors] (empty: it passes)"""
    s = patch_scan.Site(patch_scan.declarations(decl + " {}", "<site>")[0])
    values, ats = patch_scan.rows([s])
    p = (values + ats)[0]
    p.func = e
    pcs, words = P.plan["bodies"][e]
    w = dict(zip(pcs, words))
    return probes.errors(p, w, w.__getitem__, P.plan["jt"], P.prog.entries)


def paste(pc, w, mode, res, fn_name, ops):
    decl = "PATCH_AT(%#x, word(%#010x), %s, %s, %s%s)" % (pc, w, mode, res, fn_name, "".join(", " + o for o in ops))
    s = patch_scan.Site(patch_scan.declarations(decl + " {}", "<site>")[0])
    return decl, patch_scan.prototype(s)


def site(P, pc, out=print):
    hits = patch_scan.containing(P.plan, [pc])[pc]
    plan = P.plan
    if not hits:
        raise SystemExit("patch: %#x is in no lifted function%s" % (
            pc, " (an HLE'd function's entry)" if pc in set(plan.get("hooks", ())) else ""))
    e = hits[0]
    fn = P.func(e)
    k = fn.pcs.index(pc)
    w = fn.words[k]
    i = enc.decode(w)
    tg = build_ir.target(i, pc) if i is not None else None
    out("%#x  %08x  %s%s" % (pc, w, text(w, pc), "" if tg is None else "   (to %s)" % (
        P.name(P.callee(tg)) if P.callee(tg) in P.prog.entries else "%#x" % tg)))
    out("in %s, +%#x of %d instructions%s" % (P.name(e), pc - e, len(fn.pcs),
                                              "" if len(hits) == 1 else "; also in " + ", ".join(P.name(h) for h in hits[1:])))
    for j in range(max(0, k - 3), min(len(fn.pcs), k + 4)):
        out("  %s %#x  %08x  %s" % ("->" if j == k else "  ", fn.pcs[j], fn.words[j], text(fn.words[j], fn.pcs[j])))
    # (the paths into the instruction: a before row runs on every one of them)
    into = []
    for q, wq in zip(fn.pcs, fn.words):
        i = enc.decode(wq)
        if i is not None and build_ir.target(i, q) == pc and q != pc - 4 and not probes.is_call(wq):
            into.append("a branch at %#x" % q)
    for br, tgs in plan["jt"].items():
        if pc in tgs:
            into.append("the jump table of the BR at %#x" % br)
    if pc in P.prog.entries:
        into.append("the function's entry (its callers)" if pc == e else "the entry of a function of its own")
    if len(hits) > 1:
        into.append("the code of %d functions (split or shared: rows here run in each)" % len(hits))
    out("paths into it other than from the instruction before: %s" % ("; ".join(into) or "none"))
    li, lo = P.live(e)
    _, use, dfn = summary.def_use(w)
    out("live in:  %s" % regs(li[k]))
    out("live out: %s" % regs(lo[k]))
    out("reads %s; writes %s%s" % (regs(use), regs(dfn), " (a call: and the callee's clobbers)" if fn.kind[k] == "call" else ""))
    s = P.sums.get(e)
    b = P.bound(e)
    n = P.callers()[e]
    if b.keeps == 0 and b.reads == summary.ALL:
        why = ("demoted (%s)" % ", ".join(s.demote)) if s is not None and s.demote else b.kind
        out("record of %s: %s: callers reload everything; a row here cannot make it worse (%d BL sites)"
            % (P.name(e), why, n))
    else:
        out("record of %s (%s summary, %d BL sites): %s" % (P.name(e), "full" if bounds.full(P.plan["summaries"].get(e))
                                                              else "no full", n, record(b)))
    body = set(fn.pcs)
    near = [d for d in P.ps.sites if d.pc in body] + [f for f in P.ps.funcs if P.funcs.get(e) is f]
    for d in near:
        if isinstance(d, patch_scan.Func):
            out("declared: %s:%d %s of %s" % (d.where[0], d.where[1], d.kind, P.name(e)))
        else:
            out("declared: %s:%d %s %#x %s %s%s" % (d.where[0], d.where[1], d.kind, d.pc, d.mode, d.fn,
                                                   "  <- this instruction" if d.pc == pc else ""))
    if not near:
        out("declared: nothing in %s" % P.name(e))
    mode, res, ops, why = suggest(P, e, k, li, lo)
    decl, proto = paste(pc, w, mode, res, "my_fn", ops)
    errs = verdict(P, e, decl)
    out("")
    out("/* suggested operands: %s */" % why)
    out("/* in %s */" % P.name(e))
    out("%s {   /* %s */" % (decl, text(w, pc)))
    out("}")
    out("/* generates: %s */" % proto)
    out("/* the build's checks: %s */" % ("; ".join(errs) if errs else "it passes"))
    if pc == e:
        pcs, words = P.plan["bodies"][e]
        args = [r for r in ("x%d" % i for i in range(9)) if s is not None and s.inputs & summary.BIT[r]]
        out("/* or, the function replaced: orig its body, run with the call's registers; the operands are those the")
        out("   host function uses (the body reads %s: what a call of it passes) */" % (", ".join(args) or "none of x0-x8"))
        out("PATCH_FUNC(%#x, word(%#010x), body(%#018x), aapcs, x0, my_fn, orig) {" % (
            pc, w, patch_scan.body_hash(list(words))))
        out("    return guest_orig(orig);")
        out("}")
        out("/* generates: static uint64_t my_fn(GuestFn *orig), called by hos_func_%#x(Ctx *) */" % pc)


# ---- check
def errors(P, only=None):
    """[(where, message)] of every declaration (or those at only, (file, line)) against the plan: what the build stops
    on"""
    plan = P.plan
    out = []
    values, ats = patch_scan.rows(P.ps.sites)
    rows = values + ats
    hits = patch_scan.containing(plan, {p.pc for p in rows})
    for p in rows:
        if only is not None and p.where != only:
            continue
        h = hits[p.pc]
        if len(h) != 1:
            out.append((p.where, "%#x is in %s" % (p.pc, "no lifted function" if not h else
                                                   "more than one function: " + ", ".join("f_%08x" % x for x in h))))
            continue
        p.func = h[0]
        pcs, words = plan["bodies"][p.func]
        w = dict(zip(pcs, words))
        for m in probes.errors(p, w, w.__getitem__, plan["jt"], P.prog.entries):
            out.append((p.where, m))
    for where, m in patch_scan.func_errors(plan, P.gen, P.ps):
        if only is None or where == only:
            out.append((where, m))
    for where, m in host_effects.errors(P.hle_c):
        if only is None or where == only:
            out.append((where, m))
    return out


def find_decl(ps, where):
    for d in list(ps.sites) + list(ps.funcs) + list(ps.hle.values()) + list(ps.nv.values()) + list(ps.declared.values()):
        if d.where == where:
            return d
    return None


def without(ps, where):
    """the PatchSet without the declaration at where"""
    keep = lambda d: d.where != where
    return patch_scan.PatchSet([s for s in ps.sites if keep(s)],
                               [r for r in list(ps.hle.values()) + list(ps.nv.values()) + list(ps.declared.values())
                                if keep(r)], [f for f in ps.funcs if keep(f)], [a for a in ps.answers.values() if keep(a)])


def with_text(text, name="<stdin>"):
    """the host's PatchSet with the declarations of text added (checked as the build checks the host's)"""
    texts = [(os.path.relpath(p, os.path.dirname(patch_scan.HOST)), patch_scan._read(p)) for p in patch_scan.host_sources()]
    return patch_scan.scan_texts(texts + [(name, text)])


def job(arg):
    """(in a process of its own) the summary job (game.finish_summaries' recompute: summary.job) with a PatchSet of its
    own: arg, a pickle of {inp, out, jobs, gen, ps}"""
    with open(arg, "rb") as f:
        a = pickle.load(f)
    patch_scan.use(a["ps"])
    probes._SET = None
    summary._ROWS = None
    summary.job(a["inp"], a["out"], a["jobs"], summary.func_facts(patch_scan.funcs(a["gen"], a["ps"])[0]))


def summaries(plan, gen, ps, tmp, jobs):
    """{entry: packed summary}, edges of the summaries the build computes from the plan's summary input with ps"""
    inp = plan.get("summary_input")
    if not inp or not os.path.exists(inp):
        raise SystemExit("patch: the plan's summary input %s is gone: the build needs a lift too" % inp)
    a = os.path.join(tmp, "job.pickle")
    out = os.path.join(tmp, "sums.pickle")
    with open(a, "wb") as f:
        pickle.dump({"inp": inp, "out": out, "jobs": jobs, "gen": gen, "ps": ps}, f)
    subprocess.run([sys.executable, "-I", "-c", "import sys; sys.path.insert(0, %r); import patch; patch.job(%r)"
                    % (HERE, a)], check=True)
    with open(out, "rb") as f:
        sums, _, edges = pickle.load(f)
    return {e: summary.pack(s) for e, s in sums.items()}, edges


def diff(P, before, after, funcs_before, funcs_after, edges, ps_after, out=print, top=20):
    """the difference of two summary sets: functions whose record fields change, demotions, call sites whose kept
    registers change"""
    fields = (("inputs", 0), ("clobbers", 1), ("must", 2), ("results", 4))
    changed = collections.Counter()
    rows = []
    for e in sorted(set(before) | set(after)):
        a, b = before.get(e), after.get(e)
        if a == b:
            continue
        what = []
        for name, k in fields:
            if a is None or b is None or a[k] != b[k]:
                changed[name] += 1
                if a is not None and b is not None:
                    plus, minus = b[k] & ~a[k], a[k] & ~b[k]
                    what.append("%s%s%s" % (name, " +" + regs(plus) if plus else "", " -" + regs(minus) if minus else ""))
        fa, fb = bounds.full(a), bounds.full(b)
        if fa and not fb:
            changed["demoted"] += 1
            what.append("demoted (%s)" % ", ".join(b[6]) if b and b[6] else "demoted")
        elif fb and not fa:
            changed["promoted"] += 1
            what.append("now full")
        if what:
            rows.append((e, what))
    calls = collections.Counter(c for ed in edges.values() for kind, c in ed if kind == "call" and c is not None)
    pa = dict(P.plan, summaries=before)
    pb = dict(P.plan, summaries=after)
    ba = bounds.call_bounds(pa, P.hle_c, False, funcs_before)
    old = patch_scan.use(ps_after)
    try:
        bb = bounds.call_bounds(pb, P.hle_c, False, funcs_after)
        sites = funcs = 0
        for c in set(e for e, _ in rows) | set(funcs_before) ^ set(funcs_after):
            if ba(c).keeps != bb(c).keeps or ba(c).reads != bb(c).reads:
                sites += calls[c]
                funcs += 1
    finally:
        patch_scan.use(old)
    out("functions whose summary changes: %d (%s)" % (len(rows), ", ".join("%s %d" % kv for kv in sorted(changed.items()))
                                                       or "-"))
    out("call sites whose kept registers or reads change: %d (calls of %d functions)" % (sites, funcs))
    for e, what in sorted(rows, key=lambda r: -calls[r[0]])[:top]:
        out("  %s (%d BL sites): %s" % (P.name(e), calls[e], "; ".join(what)))
    return len(rows), changed, sites


def cost(P, plan_path, ps_before, ps_after, jobs, out=print):
    """the summaries computed with ps_after against those of ps_before; a side whose declarations are those the plan's
    summaries count takes the plan's"""
    from lift2 import game

    def held(ps):
        old = patch_scan.use(ps)
        try:
            probes._SET = None
            summary._ROWS = None
            eff = game.summary_effects(P.plan["hle"], summary.func_facts(patch_scan.funcs(P.gen, ps)[0]))
        finally:
            patch_scan.use(old)
            probes._SET = None
            summary._ROWS = None
        cache = game.summary_cache(plan_path, P.plan["summary_base"], eff)
        return os.path.basename(cache) == P.plan.get("summary_cache") and bool(P.plan.get("summaries"))
    with tempfile.TemporaryDirectory() as tmp:
        sides = []
        for ps in (ps_before, ps_after):
            if held(ps):
                sides.append((P.plan["summaries"], None))
            else:
                sides.append(summaries(P.plan, P.gen, ps, tmp, jobs))
    edges = sides[1][1] or sides[0][1]
    if edges is None:
        raise SystemExit("patch: both sides are the plan's summaries: the declaration changes no summary input")
    return diff(P, sides[0][0], sides[1][0], patch_scan.funcs(P.gen, ps_before)[0],
                patch_scan.funcs(P.gen, ps_after)[0], edges, ps_after, out)


# ---- report
def local(P, e, ps):
    """the packed-summary key of e analysed with the declarations ps (its callees' summaries the plan's)"""
    old = patch_scan.use(ps)
    try:
        probes._SET = None
        summary._ROWS = None
        host = summary.patched(summary.host_summaries(P.hle), summary.func_facts(patch_scan.funcs(P.gen, ps)[0]))
        pcs, words = P.plan["bodies"][e]
        fn = summary.Func(e, list(pcs), list(words), P.prog, host)
        s = summary.analyse(fn, P.sums, host)
        return (s.inputs, s.clobbers, s.mustdef, s.preserved, s.returns, tuple(s.demote))
    finally:
        patch_scan.use(old)
        probes._SET = None
        summary._ROWS = None
        summary.patched(summary.host_summaries(P.hle), summary.func_facts(P.funcs))


def kept(key):
    """(what a BL keeps, what it reads) of a callee with that local key: bounds' rule for a lifted callee"""
    full = key[4] and not key[5]
    return (key[3] & bounds.KEEP_ALL, key[0]) if full else (0, summary.ALL)


def report(P, out=print):
    """one line per declaration with its local cost, costliest first"""
    ps = P.ps
    values, ats = patch_scan.rows(ps.sites)
    hits = patch_scan.containing(P.plan, {p.pc for p in values + ats})
    calls = P.callers()
    lines = []
    base = {}
    for p in values + ats:
        h = hits[p.pc]
        if len(h) != 1:
            lines.append((1 << 30, 0, 0, p.where, "site", "%#x" % p.pc, "?", "not in one function: check"))
            continue
        e = h[0]
        _, use, dfn = summary.def_use(p.word)
        r, wr = p.effects()
        m = lambda names: sum(summary.BIT[v] for v in names)
        extra_r, extra_w = m(r) & ~use, m(wr) & ~dfn
        if isinstance(p, probes.At) and not p.before and not p.replace and not probes.is_call(p.word):
            extra_r &= ~dfn   # (an after row's reads of what its instruction wrote: summary.Func)
        if e not in base:
            base[e] = local(P, e, ps)
        alone = local(P, e, without(ps, p.where))
        what = []
        for name, k in (("inputs", 0), ("clobbers", 1), ("must", 2)):
            if alone[k] != base[e][k]:
                what.append("%s %s" % (name, regs(base[e][k] ^ alone[k])))
        if alone[5] != base[e][5]:
            what.append("demotion %s" % ",".join(base[e][5]))
        sites = calls[e] if kept(base[e]) != kept(alone) else 0
        mode = "instead" if isinstance(p, probes.Value) else "before" if p.before else "instead" if p.replace else "after"
        lines.append((sites, len(what), bin(extra_r | extra_w).count("1"), p.where, "PATCH_AT %s" % mode, "%#x" % p.pc,
                      P.name(e), "reads %s, writes %s beyond the instruction; record: %s" % (
                          regs(extra_r) if extra_r else "-", regs(extra_w) if extra_w else "-",
                          "; ".join(what) or "the same without it")))
    for f in ps.funcs:   # (a replaced function: its record against its lifted body's own)
        e = next((a for a, g in P.funcs.items() if g is f), None)
        if e is None or e not in P.plan["bodies"]:
            lines.append((0, 0, 0, f.where, f.kind, f.symbol or "%#x" % f.addr, "?", "no lifted function: check"))
            continue
        b, own = P.bound(e), P.bound.own(e)
        n = calls[e] if (b.keeps, b.reads) != (own.keeps, own.reads) else 0
        lines.append((n, 0, len(f.args), f.where, f.kind, "%#x" % e, P.name(e),
                      "record %s reads %s, keeps %s at %d BL sites; the body's own: %s reads %s, keeps %s" % (
                          b.kind, regs(b.reads), regs(b.keeps), calls[e], own.kind, regs(own.reads), regs(own.keeps))))
    by_sym = collections.defaultdict(list)
    for a, s in P.hle_c.items():
        by_sym[s].append(a)
    for r in list(ps.hle.values()) + list(ps.nv.values()) + list(ps.declared.values()):
        if not r.reads and r.clobbers is None:
            continue
        addrs = by_sym.get(r.symbol, [])
        n = sum(calls[a] for a in addrs)
        lines.append((n if r.clobbers is not None else 0, 0, len(r.reads), r.where, r.kind, r.symbol,
                      ", ".join(P.name(a) for a in addrs[:2]) or "-",
                      ("clobbers: %d BL sites keep nothing" % n) if r.clobbers is not None else
                      "reads %s at %d BL sites" % (",".join(r.reads), n)))
    lines.sort(key=lambda l: (-l[0], -l[1], -l[2], l[3]))
    out("%-34s %-22s %-11s %-40s %7s  %s" % ("declaration", "kind", "site", "function", "sites", "cost"))
    for sites, nf, nr, where, kind, at, name, what in lines:
        out("%-34s %-22s %-11s %-40s %7d  %s" % ("%s:%d" % where, kind, at, name[:40], sites, what))
    out("(sites: BL sites whose call record (keeps, reads) differs from the function's without the declaration; record: its function's summary "
        "analysed again without it, the callees' summaries the plan's; `check <file:line>` gives the whole program's "
        "difference. Context stores kept by a declaration are not measured here)")
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(prog="nx2.patch", description=__doc__.split("\n")[0])
    import gamedesc
    gamedesc.add_argument(ap)
    ap.add_argument("--plan", help="the plan (default <work>/sa/plan.pickle)")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("HOOKSHOT_LIFT_SUMMARY_JOBS") or 8))
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("site")
    s.add_argument("addr", nargs="+")
    c = sub.add_parser("check")
    c.add_argument("which", nargs="?", help="file:line, - (a declaration on stdin), or nothing / --all: every one")
    c.add_argument("--all", action="store_true")
    sub.add_parser("report")
    a = ap.parse_args(argv)
    labels, ps = use_game(a.game)
    plan_path = a.plan
    if plan_path is None:
        import kit
        plan_path = os.path.join(kit.WORK, "sa", "plan.pickle")
    if not os.path.exists(plan_path):
        raise SystemExit("patch: no plan %s: lift first (scripts/relift.sh) or give --plan" % plan_path)
    with open(plan_path, "rb") as f:
        plan = pickle.load(f)
    gen = os.path.join(os.path.dirname(plan_path), "gen")
    if a.cmd != "check" or a.which != "-":
        P = Plan(plan, gen, labels, ps)
    if a.cmd == "site":
        for k, x in enumerate(a.addr):
            if k:
                print()
            site(P, resolve_addr(x, labels))
        return
    if a.cmd == "report":
        report(P)
        return
    if a.which == "-":
        txt = sys.stdin.read()
        after = with_text(txt)
        P = Plan(plan, gen, labels, after)
        new = [d for d in list(after.sites) + list(after.funcs) + list(after.hle.values()) + list(after.nv.values())
               + list(after.declared.values()) if d.where[0] == "<stdin>"]
        errs = [x for d in new for x in errors(P, d.where)]
        for where, m in errs:
            print("%s:%d: %s" % (where[0], where[1], m))
        if errs:
            raise SystemExit(1)
        print("proposed: %d declarations, no error the build would stop on" % len(new))
        P = Plan(plan, gen, labels, ps)
        cost(P, plan_path, ps, after, a.jobs)
        return
    only = None
    if a.which and not a.all:
        f, _, line = a.which.rpartition(":")
        only = (f, int(line))
        if find_decl(ps, only) is None:
            raise SystemExit("patch: no declaration at %s" % a.which)
    errs = errors(P, only)
    for where, m in errs:
        print("%s:%d: %s" % (where[0], where[1], m))
    n = len(ps.sites) + len(ps.funcs) + len(ps.hle) + len(ps.nv) + len(ps.declared) if only is None else 1
    print("%d declarations checked against the plan, %d errors%s" % (
        n, len(errs), "" if only else "; a guest function a host function calls without declaring it is not "
        "checkable here (tests/test_host_context_reads.py compiles the host for that)"))
    if only is not None and not errs:
        d = find_decl(ps, only)
        print("cost of %s %s:%d (the summaries computed again without it):" % (d.kind, only[0], only[1]))
        cost(P, plan_path, without(ps, only), ps, a.jobs)
    if errs:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
