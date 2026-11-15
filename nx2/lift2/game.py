"""The game's lift and build (nx2/lift.py, nx2/build.py): every function of the game through the lifter.

At lift time write_plan() keeps what the build needs from the discovery (nx2/lift2/discover.py): the entries, jump
tables, PLT stubs, setjmp owners, hooked addresses, the hot list, and every function's instructions, in
<work>/sa/plan.pickle. A function the
lifter cannot take (build_ir.scope: a word it does not decode) is lifted as a trap at its entry and listed.

At build time prepare() lifts the functions into partitions by guest address, each `f_<addr>` (the name the function
table, the hooks and the host call), and streams each partition's text into `clang -x ir -` (no IR on disk; at a
level whose pipeline has no DSE, -O1, through `opt` with clang's pipeline plus DSE first: opt_argv); a
partition whose inputs are unchanged comes from the object cache by an input key. The hot list's functions go into
their own partitions, compiled at the hot level (-O2). It returns the objects to link.

The whole-program summaries (nx2/lift2/summary.py) are computed by the first build after a lift, in a process of
their own (finish_summaries, summary.job: workers over the components' DAG), and cached beside the plan, keyed by the
image, the discovery's facts, the lifter code they run (source_key) and the patches' declared effects
(summary_effects); a build whose patches declare other effects than the plan's summaries count computes them again
from the input the lift left, without a lift. HOOKSHOT_LIFT_SUMMARY_JOBS: their workers (8).
A partition's object is cached by what its IR is made of (part_key): the lifter's code, the discovery's facts, its
functions, their rows, PATCH_FUNCs and summaries, and the call-boundary records of every function they branch or call
to; editing a row or a hook's declaration recompiles the partitions whose IR it changes.
HOOKSHOT_LIFT_VERIFY=1 (at build time): every function with a full summary is checked at run time
(host/lift2_verify.c).
HOOKSHOT_HOT_FUNCS: the hot list (one f_<addr> a line; default the game description's, work/<hot>).
"""
import ast
import concurrent.futures
import hashlib
import json
import os
import pickle
import re
import subprocess
import sys
import time
from array import array

from . import bounds, build_ir, code_ptrs, emit_llvm, enc, host_effects, llvm, patch_scan, probes

PART_INSNS = 120000
# (dead context stores: LLVM drops a store a later one overwrites only if it can see that nothing reads the context
# in between. Capture tracking gives up past N uses of the context pointer (a large function has thousands) and then
# assumes the context escaped; DSE stops after so many definitions in a block or steps of its walk. Measured on the
# market's hot functions: context stores after -O2 -40 to -47 %, compile time of the largest 1.6 s -> 3.8 s)
DSE_FLAGS = ["-mllvm", "-capture-tracking-max-uses-to-explore=100000", "-mllvm", "-dse-memoryssa-defs-per-block-limit=100000",
             "-mllvm", "-dse-memoryssa-scanlimit=1000", "-mllvm", "-dse-memoryssa-walklimit=1000",
             "-mllvm", "-dse-memoryssa-otherbb-cost=1"]
_PIPELINES = {}


def opt_argv(clang, level, march_flags):
    """the `opt` command a partition's IR goes through before clang's code generation, or None (clang optimises).
    clang's -O1 pipeline has no DSE, so every context store a later one overwrites stayed in the cold partitions: at
    a level whose pipeline lacks `dse`, the IR runs through clang's own pipeline for that level with `dse` after
    `memcpyopt`, where -O2 has it (6 cold partitions: .text -13 %, context stores -33 %, compile time unchanged; -O2
    gave -16 % at 2.25 times the time)"""
    k = (clang, level, tuple(march_flags))
    if k not in _PIPELINES:
        r = subprocess.run([clang, "-" + level] + march_flags + ["-Wno-everything", "-mllvm", "-print-pipeline-passes",
                            "-x", "ir", os.devnull, "-c", "-o", os.devnull], capture_output=True, text=True, check=True)
        p = r.stdout.strip()
        names = set(re.findall(r"[\w-]+", p))
        argv = None
        if "dse" not in names and "memcpyopt" in names:
            assert p.count("memcpyopt,") == 1, "clang -%s: memcpyopt not once in its pipeline" % level
            cpu = [f.replace("-march=", "-mcpu=") for f in march_flags if f.startswith("-march=")]
            argv = [llvm.check("opt")] + cpu + ["-passes=" + p.replace("memcpyopt,", "memcpyopt,dse,")] + \
                [f for f in DSE_FLAGS if f != "-mllvm"]
        _PIPELINES[k] = argv
    return _PIPELINES[k]


def compile_ir(clang, text, out, fl, pre, emit=("-c",)):
    """clang on IR text (fl: its flags; emit: what it writes to `out`), after `pre` (opt_argv) when given; the
    completed process of the step that failed, else of clang"""
    data = text.encode() if isinstance(text, str) else text
    cl = [clang] + fl
    if pre:
        r = subprocess.run(pre + ["-", "-o", "-"], input=data, capture_output=True)
        if r.returncode:
            return r
        data, cl = r.stdout, cl + ["-Xclang", "-disable-llvm-passes"]
    return subprocess.run(cl + ["-x", "ir", "-"] + list(emit) + ["-o", out], input=data, capture_output=True)


CLASSES = ("int", "fp", "neon", "crypto")


callee_fn = bounds.callee_fn


def reachable(entry, pcs, word, jt):
    """the instructions of a body reachable from its entry by branches, jump tables and fall-through: the module
    heads' words after their first (udf #1, then the MOD0 header's offsets) are in the discovery's body but are data"""
    body, seen, todo = set(pcs), set(), [entry]
    while todo:
        pc = todo.pop()
        if pc in seen or pc not in body:
            continue
        seen.add(pc)
        i = enc.decode(word(pc))
        if i is None:
            continue
        t = i.entry.template
        tg = build_ir.target(i, pc)
        if tg is not None:
            todo.append(tg)
        todo.extend(jt.get(pc, ()))
        if t not in build_ir.TERMINATORS or t in ("bcond", "cbz", "tbz", "blr") or (t == "exc" and i.f["opc"] == 0) \
                or (t == "b" and i.f["op"]):
            todo.append(pc + 4)
    return [pc for pc in pcs if pc in seen]


# the lifter's files the summaries depend on (the summary cache's key; the host's patch declarations (patch_scan) enter
# by their effects, summary_effects)
SUMMARY_SOURCES = re.compile(r"^(enc.*|sem.*|ir|bounds|build_ir|summary|probes|host_effects)\.py$")


def source_key(name, text):
    """a lifter source as the keys take it: its syntax tree, without comments and docstrings (they change no IR)"""
    tree = ast.parse(text)
    for n in ast.walk(tree):
        body = getattr(n, "body", None)
        if isinstance(body, list) and body and isinstance(body[0], ast.Expr) and \
                isinstance(body[0].value, ast.Constant) and isinstance(body[0].value.value, str):
            n.body = body[1:] or [ast.Pass()]
    return ast.dump(tree, annotate_fields=False)


def code_key(match=None):
    """the lifter's code (every nx2/lift2 source, or those match accepts), as source_key takes it"""
    h = hashlib.sha1(usedforsecurity=False)
    here = os.path.dirname(os.path.abspath(__file__))
    for name in sorted(os.listdir(here)):
        if name.endswith(".py") and (match is None or match.match(name)):
            with open(os.path.join(here, name), encoding="utf-8") as f:
                h.update(name.encode() + b"\0" + source_key(name, f.read()).encode() + b"\0")
    return h.hexdigest()


def replaced(gen):
    """the PATCH_FUNCs as the summaries take them ({address: summary.func_facts}): the functions declared now"""
    from . import summary
    return summary.func_facts(patch_scan.funcs(gen)[0])


def summary_effects(hle, funcs):
    """what the summaries take from the host's patches, as a key: every row's declared reads, writes, guest callees and
    replace (summary.rows), every HLE'd function's summary (summary.host_summary; hle: {address: symbol}) and every
    PATCH_FUNC's reads, result, orig and guest callees (funcs: replaced)"""
    from . import summary
    facts = (sorted(summary.rows().items()), sorted((e, summary.pack(summary.host_summary(n))) for e, n in hle.items()),
             sorted(funcs.items()))
    return hashlib.sha1(repr(facts).encode(), usedforsecurity=False).hexdigest()


_SUMMARY_CODE = []


def summary_code():
    """the code the summaries run as it is now (code_key of SUMMARY_SOURCES; once a process)"""
    if not _SUMMARY_CODE:
        _SUMMARY_CODE.append(code_key(SUMMARY_SOURCES))
    return _SUMMARY_CODE[0]


def summary_cache(plan_path, base, effects):
    """the summaries' cache file for the lift's key (base), the patches' effects and the summaries' code as it is now:
    a build after an edit of summary.py, bounds.py, ... computes them again from the input the lift left (base holds
    the code as the lift had it)"""
    k = hashlib.sha1((base + effects + summary_code()).encode(), usedforsecurity=False).hexdigest()[:16]
    return os.path.join(os.path.dirname(plan_path), "summaries_%s.pickle" % k)


def plan_summaries(L, img, path, hooks, no_call, classes):
    """the summaries' key and input for the plan: (base, input path, {address: HLE symbol}). base is the image, the
    discovery's facts, the HLE'd functions and the lifter code the summaries run (SUMMARY_SOURCES); the patches'
    effects (the PATCH_FUNCs among them) complete the cache's key at build time (summary_cache). The input (every
    function's instructions and the facts) is written beside the plan unless it is there, and stays until the next
    lift: a build whose patches declare other effects computes the summaries again from it (finish_summaries), after
    the lift's memory is gone"""
    hle = host_effects.hle_symbols(L.hooks)
    names = getattr(img, "names", {}) or {}
    h = hashlib.sha1(bytes(img.mem), usedforsecurity=False)
    h.update(code_key(SUMMARY_SOURCES).encode())
    h.update(repr((sorted(L.entries), sorted(hooks), sorted(L.jt.items()), sorted(L.plt.items()), sorted(no_call),
                   sorted(names.items()), sorted(hle.items()))).encode())
    base = h.hexdigest()
    d = os.path.dirname(path)
    inp = os.path.join(d, "summary_input_%s.pickle" % base[:16])
    for old in [x for x in os.listdir(d) if x.startswith("summary_input_") and x != os.path.basename(inp)
                or x.startswith("summaries_") and x.endswith(".in")]:
        os.remove(os.path.join(d, old))
    g = os.path.join(d, "gen")
    cache = summary_cache(path, base, summary_effects(hle, replaced(g)))
    if os.path.exists(cache):
        print("summaries: in the cache %s" % os.path.basename(cache))
    if os.path.exists(inp):
        return base, inp, hle
    with open(inp + ".tmp", "wb") as f:
        fs = {}
        for e, (b, _) in L.funcs.items():
            pcs = array("Q", sorted(b))
            fs[e] = (pcs, array("I", map(img.word, pcs)))
        pickle.dump({"funcs": fs, "entries": set(L.entries), "jt": dict(L.jt), "plt": dict(L.plt), "no_call": no_call,
                     "classes": classes, "hle": hle, "names": dict(names)}, f, protocol=pickle.HIGHEST_PROTOCOL)
    os.replace(inp + ".tmp", inp)
    if not os.path.exists(cache):
        print("summaries: to be computed by the build (%s)" % os.path.basename(inp))
    return base, inp, hle


def finish_summaries(plan_path, plan):
    """the plan with the summaries of the patches' declared effects as they are now (summary_step, in a process of its
    own: the summaries' objects never enter the build's process, which keeps only the packed tuples)"""
    if "summary_base" not in plan:
        raise SystemExit("build: the plan predates the summaries' content keys: lift again (scripts/relift.sh)")
    gen = os.path.join(os.path.dirname(plan_path), "gen")
    cache = summary_cache(plan_path, plan["summary_base"], summary_effects(plan["hle"], replaced(gen)))
    if plan.get("summaries") and plan.get("summary_cache") == os.path.basename(cache):
        return plan
    if plan.get("summary_cache"):
        print("summaries: the patches' declared effects differ from those the plan's summaries count", flush=True)
    nx2 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    subprocess.run([sys.executable, "-I", "-u", "-c", "import sys; sys.path.insert(0, %r); from lift2 import game; "
                    "game.summary_step(%r, %r)" % (nx2, plan_path, cache)], check=True)
    del plan
    _PLAN.pop(plan_path, None)
    import gc
    gc.collect()
    return _plan(plan_path)


def summary_step(plan_path, cache):
    """the plan's summaries from their cache, or computed by summary.job (HOOKSHOT_LIFT_SUMMARY_JOBS workers, default
    8) from the input the lift left; the plan file is rewritten with them, packed"""
    from . import summary
    plan = _plan(plan_path)
    inp = plan["summary_input"]
    t1 = time.time()
    if not os.path.exists(cache):
        if inp is None or not os.path.exists(inp):
            raise SystemExit("build: the plan's summaries are neither cached nor computable (%s): lift again" % cache)
        jobs = int(os.environ.get("HOOKSHOT_LIFT_SUMMARY_JOBS") or 8)
        out = cache + ".out"
        nx2 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        gen = os.path.join(os.path.dirname(plan_path), "gen")
        # (the PATCH_FUNCs declared now)
        subprocess.run([sys.executable, "-I", "-c", "import sys; sys.path.insert(0, %r); from lift2 import summary; "
                        "summary.job(%r, %r, %d, %r)" % (nx2, inp, out, jobs, replaced(gen))], check=True)
        with open(out, "rb") as f:
            sums, stats, edges = pickle.load(f)
        os.remove(out)
        d = os.path.dirname(cache)
        keep = sorted((x for x in os.listdir(d) if x.startswith("summaries_") and x.endswith(".pickle")),
                      key=lambda x: os.path.getmtime(os.path.join(d, x)))[-2:]   # (and the one before: an edit undone)
        for old in [x for x in os.listdir(d) if x.startswith("summaries_") and x not in keep]:
            os.remove(os.path.join(d, old))
        with open(cache + ".tmp", "wb") as f:
            pickle.dump((sums, edges, stats), f, protocol=pickle.HIGHEST_PROTOCOL)
        os.replace(cache + ".tmp", cache)   # (the input stays until the next lift: builds and tools recompute from it)
        summary.report(sums, edges, plan["insns"], plan.get("hot", ()))
        print("summaries: %(functions)d functions, %(sccs)d components (largest %(largest scc)d) in %(levels)d "
              "levels; graph %(graph s).0fs, fixed point %(fixed point s).0fs, results %(results s).0fs" % stats)
        print("summaries: %.0fs in all; peak memory of the summary process %s MB, of its largest worker %s MB" % (
            time.time() - t1, stats.get("main peak MB", "?"), stats.get("worker peak MB", "?")), flush=True)
    else:
        with open(cache, "rb") as f:
            sums, _, _ = pickle.load(f)
    plan["summaries"] = {e: summary.pack(s) for e, s in sums.items()}
    plan["summary_cache"] = os.path.basename(cache)
    with open(plan_path + ".tmp", "wb") as f:
        pickle.dump(plan, f, protocol=pickle.HIGHEST_PROTOCOL)
    os.replace(plan_path + ".tmp", plan_path)


def hot_list(work):
    """the hot list's functions (the game description's hot_path: HOOKSHOT_HOT_FUNCS, else work/<hot>; a missing file
    stops)"""
    import gamedesc
    path = gamedesc.current().hot_path(work)
    return {int(w[2:], 16) for line in open(path) for w in line.split()[:1] if w.startswith("f_")}


def check_complete(entries, bodies, hooks):
    """SystemExit unless every entry the discovery found has a body in the plan (lifted, or a trap at its entry) or is
    an HLE'd function (hooks): a function the plan left out would be a call into nothing"""
    lost = sorted(set(entries) - set(bodies) - set(hooks))
    if lost:
        raise SystemExit("plan: %d entries have no body and no HLE, first f_%08x" % (len(lost), lost[0]))


def write_plan(L, img, path, hot=()):
    """the discovery's facts and the instructions of every function; hot: the hot list's functions"""
    t0 = time.time()
    no_call = set(getattr(L, "setjmp_owner", {}) or ())
    classes = CLASSES
    prog = build_ir.Program(entries=L.entries, jt=L.jt, callee=L.callee, plt=L.plt, no_call=no_call, classes=classes,
                            jt_index=L.jt_index)
    # (a BL to a setjmp owner is lifted as a call of hos_sj_call, host/hos_setjmp.c: scope need not refuse it)
    scope_prog = build_ir.Program(entries=L.entries, jt=L.jt, callee=L.callee, plt=L.plt, classes=classes)
    sj_rets = {e: tuple((list(r) + [0, 0, 0, 0])[:4]) for e, r in (getattr(L, "setjmp_owner", {}) or {}).items()}
    hooks = set(L.hooks)
    bodies, why = {}, {}
    for e, (body, _) in L.funcs.items():
        if e in hooks:
            why["hooked"] = why.get("hooked", 0) + 1
            continue
        pcs = sorted(body)
        r = build_ir.scope(e, pcs, img.word, scope_prog)
        if r is not None and "not decoded" in r:   # (data the discovery took for code: unreachable from the entry)
            live = reachable(e, pcs, img.word, prog.jt)
            if len(live) < len(pcs) and build_ir.scope(e, live, img.word, scope_prog) is None:
                pcs, r = live, None
        if r is not None and enc.decode(img.word(e)) is None:
            # (an "entry" whose first word is not an instruction: data the discovery took for code, f_80020008 on main's
            # MOD0 header. It traps as the hardware would: lifted as UDF #0 at the entry)
            bodies[e] = (array("Q", [e]), array("I", [0]))
            continue
        if r is not None:   # (lifted as a trap at its entry: listed below)
            k = re.sub(r" \(.*| at .*| [0-9a-f]{8}.*|0x[0-9a-f]+", "", r)
            why[k] = why.get(k, 0) + 1
            print("lift: f_%08x is a trap: %s" % (e, r), flush=True)
            bodies[e] = (array("Q", [e]), array("I", [0]))
            continue
        bodies[e] = (array("Q", pcs), array("I", (img.word(pc) for pc in pcs)))
    check_complete(L.entries, bodies, hooks)
    # the whole program's summaries: computed (or taken from their cache) by the build, finish_summaries
    base, inp, hle = plan_summaries(L, img, path, hooks, no_call, classes)
    plan = {"summaries": {}, "summary_base": base, "summary_input": inp, "hle": hle, "entries": sorted(L.entries), "jt": dict(L.jt), "jt_index": dict(L.jt_index), "plt": dict(L.plt), "no_call": sorted(no_call), "sj_rets": sj_rets,
            "hooks": sorted(hooks), "bodies": bodies, "why": why, "hot": sorted(set(hot) & set(bodies)),
            "insns": {e: len(b) for e, (b, _) in L.funcs.items()},
            "code_excluded": sorted(m.base for m in img.modules)}   # (code_ptrs.targets: the modules' bases stay guest)
    plan["facts_key"] = facts_key(plan)
    with open(path + ".tmp", "wb") as f:
        pickle.dump(plan, f, protocol=pickle.HIGHEST_PROTOCOL)
    os.replace(path + ".tmp", path)
    n = sum(len(p) for p, _ in bodies.values())
    print("plan: %d functions (%d instructions; %d hooked) (%.0fs)%s" % (
        len(bodies), n, len(hooks), time.time() - t0,
        "; traps: " + ", ".join("%s %d" % kv for kv in sorted(why.items(), key=lambda kv: -kv[1]) if kv[0] != "hooked")
        if len(why) > ("hooked" in why) else ""), flush=True)


def compile_part(job):
    """one partition: IR text streamed into clang, the object cached by the text's hash"""
    clang, text, obj, fl, pre, cache = job
    key = hashlib.sha1((" ".join(fl + (pre or [])) + "\n").encode() + text.encode(), usedforsecurity=False).hexdigest()
    stamp = obj + ".sha1"
    if os.path.exists(obj) and os.path.exists(stamp) and open(stamp).read() == key:
        return obj, 0.0, None
    cached = os.path.join(cache, key + ".o")
    if os.path.exists(cached):
        if os.path.exists(obj):
            os.remove(obj)
        os.link(cached, obj)
        open(stamp, "w").write(key)
        return obj, 0.0, None
    t = time.time()
    r = compile_ir(clang, text, obj, fl, pre)
    if r.returncode:
        return obj, time.time() - t, (r.stderr or r.stdout)[-3000:].decode(errors="replace")
    open(stamp, "w").write(key)
    try:
        os.link(obj, cached + ".tmp")
        os.replace(cached + ".tmp", cached)
    except OSError:
        pass
    return obj, time.time() - t, None


VERIFY_WRAPPER = """@sum_%(a)08x = private constant [7 x i64] [i64 %(inputs)d, i64 %(may)d, i64 %(res)d, i64 %(pres)d, i64 %(flags)d, i64 %(a)d, i64 %(must)d]
define void @%(name)s(ptr %%ctx) {
  %%fr = alloca [32 x i64], align 8
  call void @lift2_check_enter(ptr %%ctx, ptr @sum_%(a)08x, ptr %%fr)
  call void @body_%(a)08x(ptr %%ctx)
  call void @lift2_check_exit(ptr %%ctx, ptr @sum_%(a)08x, ptr %%fr)
  ret void
}"""
# (a PATCH_FUNC's f_<addr>: its host adapter, hos_func_<key>, host/patch.h)
FUNC_STUB = """declare void @%(fn)s(ptr)
define void @f_%(a)08x(ptr %%ctx) {
  musttail call void @%(fn)s(ptr %%ctx)
  ret void
}"""
GPR = (1 << 32) - 1
HOST_CHECK = """define internal void @hchk_%(a)08x(ptr %%ctx) {
  %%fr = alloca [32 x i64], align 8
  call void @lift2_host_enter(ptr %%ctx, ptr %%fr)
  call void @f_%(a)08x(ptr %%ctx)
  call void @lift2_host_exit(ptr %%ctx, ptr %%fr, i64 %(a)d)
  ret void
}"""


def lift_part(args):
    """(lifted in a worker process) the IR text of one partition"""
    plan_path, names, funcs = args
    plan = _plan(plan_path)
    plt = plan["plt"]
    verify = bool(os.environ.get("HOOKSHOT_LIFT_VERIFY"))
    sj = plan.get("sj_rets", {})
    key = (plan_path, tuple(sorted((a, f.name) for a, f in funcs.items())), verify)
    if key not in _BOUNDS:   # (once a worker process)
        _BOUNDS.clear()
        hle = host_effects.hle_names(os.path.join(os.path.dirname(plan_path), "gen"))
        _BOUNDS[key] = bounds.call_bounds(plan, hle, verify, funcs)
    bound = _BOUNDS[key]
    prog = build_ir.Program(entries=set(plan["entries"]), jt=plan["jt"], callee=callee_fn(plt), plt=plt,
                            jt_index=plan.get("jt_index"), bound=bound, omit=True, verify=verify)
    prog.code_targets = code_ptrs.targets(plan)
    probes.resolve(plan, plan_path)   # (each site's function, once a worker)
    m = emit_llvm.Module()
    extra = []
    n_code = 0
    for e in names:
        pcs, words = plan["bodies"][e]
        w = dict(zip(pcs, words))
        prog.code_sites = code_ptrs.sites(list(pcs), list(words), prog.code_targets, plan["jt"])
        prog.probes = probes.find(e, pcs, w.__getitem__)
        prog.before = probes.at(e, pcs, w.__getitem__, before=True, jt=prog.jt, entries=prog.entries)
        prog.after = probes.at(e, pcs, w.__getitem__)
        prog.replace = probes.at(e, pcs, w.__getitem__, replace=True)
        fn = build_ir.build(e, list(pcs), w.__getitem__, prog)
        n_code += fn.code_sites
        fn.name = "f_%08x" % e
        f = funcs.get(e)
        if f is not None:   # (a PATCH_FUNC: f_ is its host adapter; the body stays lifted as its orig)
            fn.name = f.orig_name
            extra.append(FUNC_STUB % {"a": e, "fn": f.name})
        if verify and bounds.verified(plan, e):
            wrapper = fn.name
            fn.name = "body_%08x" % e
            s = plan["summaries"][e]
            extra.append(VERIFY_WRAPPER % {"a": e, "name": wrapper, "inputs": s[0] & GPR, "may": s[1] & GPR,
                                           "res": s[4] & GPR, "pres": s[3] & GPR, "flags": 1 if s[5] else 0,
                                           "must": s[2] & GPR})
        m.add(fn)
        m.defined = m.defined | {"f_%08x" % e}   # (with verification the wrapper defines f_; drop declarations of it)
    if verify:
        extra.append("declare void @lift2_check_enter(ptr, ptr, ptr)\ndeclare void @lift2_check_exit(ptr, ptr, ptr)")
    checked = sorted(int(d.split("@hchk_")[1][:8], 16) for d in m.decls if d.startswith("declare void @hchk_"))
    if checked:
        m.defined = m.defined | {"hchk_%08x" % e for e in checked}
        extra.append("declare void @lift2_host_enter(ptr, ptr)\ndeclare void @lift2_host_exit(ptr, ptr, i64)")
        for e in checked:
            if "f_%08x" % e not in m.defined:
                m.decls.add("declare void @f_%08x(ptr)" % e)
            extra.append(HOST_CHECK % {"a": e})
    stubs = sorted(int(d.split("@sjcall_")[1][:8], 16) for d in m.decls if d.startswith("declare void @sjcall_"))
    if stubs:
        m.defined = m.defined | {"sjcall_%08x" % e for e in stubs}
        extra.append("declare void @hos_sj_call(ptr, ptr, i64, i64, i64, i64)")
        for e in stubs:
            t = "f_%08x" % e
            if t not in m.defined:
                m.decls.add("declare void @%s(ptr)" % t)
            extra.append("define internal void @sjcall_%08x(ptr %%ctx) {\n  call void @hos_sj_call(ptr %%ctx, ptr @%s, "
                         "i64 %d, i64 %d, i64 %d, i64 %d)\n  ret void\n}" % ((e, t) + sj[e]))
    return m.text() + emit_llvm.no_capture("\n".join(extra)) + "\n; code pointer sites: %d\n" % n_code


def census_part(args):
    """(in a worker process) code_ptrs' census of one partition: (sites, {other kind: count}, [(pc, why)] unknown)"""
    plan_path, names, texts = args
    a, p, kinds, odd = code_ptrs.count(_plan(plan_path), names, texts)
    return a + p, kinds, odd


CODE_SITES = re.compile(r"^; code pointer sites: (\d+)$", re.M)


_PLAN = {}
_BOUNDS = {}


def _plan(path):
    if path not in _PLAN:
        with open(path, "rb") as f:
            _PLAN[path] = pickle.load(f)
    return _PLAN[path]


# the plan's fields that are not the discovery's facts (the summaries come in by function: part_key)
NOT_FACTS = ("summaries", "summary_cache", "facts_key")


def facts_key(plan):
    """the discovery's facts in the plan (everything the lift wrote but the summaries) as a key"""
    k = plan.get("facts_key")
    if k:
        return k
    return hashlib.sha1(pickle.dumps({n: v for n, v in plan.items() if n not in NOT_FACTS},
                                     protocol=pickle.HIGHEST_PROTOCOL), usedforsecurity=False).hexdigest()


def branch_targets(pcs, words):
    """the addresses a body's code may go to outside it (B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ targets, and the next word
    where the body does not continue: a fall into the next function), and every BL target (a call of itself
    included): the functions whose call-boundary records its IR may read"""
    body = set(pcs)
    out = set()
    for pc, w in zip(pcs, words):
        if w & 0x7C000000 == 0x14000000:   # (B, BL)
            t = pc + (((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) * 4
            if w & 0x80000000 or t not in body:
                out.add(t)
        elif w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000:   # (B.cond, CBZ / CBNZ)
            t = pc + ((((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000) * 4
            if t not in body:
                out.add(t)
        elif w & 0x7E000000 == 0x36000000:   # (TBZ / TBNZ)
            t = pc + ((((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000) * 4
            if t not in body:
                out.add(t)
        if pc + 4 not in body:
            out.add(pc + 4)
    return {t & 0xFFFFFFFFFFFFFFFF for t in out}


def patch_rows():
    """{function: [each Value probe and At row in it, its fields as a string]}: the sites resolved (probes.resolve); the
    declaration's place in its file is not in it (editing a body above it changes no IR)"""
    out = {}
    values, ats = probes.tables()
    for p in values + ats:
        out.setdefault(p.func, []).append(type(p).__name__ + repr(sorted(
            (k, v) for k, v in vars(p).items() if k != "where")))
    return out


def part_key(plan, names, funcs, bound, rows):
    """what a partition's IR is made of beyond the lifter's code and the discovery's facts: for each of its functions
    its rows, its PATCH_FUNC's names, its summary, and the call-boundary record of every function it may branch or call to
    (branch_targets: what the lift reads of the callees)"""
    h = hashlib.sha1(usedforsecurity=False)
    callee = callee_fn(plan["plt"])
    summ = plan.get("summaries", {})
    for e in names:
        pcs, words = plan["bodies"][e]
        f = funcs.get(e)
        h.update(repr((e, rows.get(e), f and (f.name, f.orig_name), summ.get(e),
                       [(t, bound(callee(t))) for t in sorted(branch_targets(pcs, words))])).encode())
    return h.hexdigest()


def prepare(gen, out, jobs, opt, hot_opt, march_flags, cache):
    """(the C files to compile, the objects to link, a report line); see the module's description"""
    t0 = time.time()
    clang = llvm.check("clang")
    plan_path = os.path.join(os.path.dirname(gen), "plan.pickle")
    if not os.path.exists(plan_path):
        raise SystemExit("build: no plan %s: lift the game first (scripts/relift.sh)" % plan_path)
    plan = finish_summaries(plan_path, _plan(plan_path))
    t_sum = time.time() - t0
    if os.environ.get("HOOKSHOT_LIFT_VERIFY") and not plan.get("summaries"):
        raise SystemExit("build: HOOKSHOT_LIFT_VERIFY needs summaries in the plan: lift again")
    chosen = set(plan["bodies"])
    hot = set(plan.get("hot", ()))
    # (the PATCH_FUNCs: f_<addr> is the host function's adapter, the body stays lifted as its orig; each names a lifted
    # function whose first word and body are the declaration's, or the build stops with its file:line. The summaries
    # took the same: finish_summaries)
    funcs = patch_scan.check_funcs(plan, gen)
    srcs = []
    # partitions: by address, the hot functions apart, about PART_INSNS instructions each
    parts = []
    for group, tag in ((sorted(chosen - hot), "c"), (sorted(chosen & hot), "h")):
        cur, n = [], 0
        for e in group:
            cur.append(e)
            n += len(plan["bodies"][e][0])
            if n >= PART_INSNS:
                parts.append((tag, cur))
                cur, n = [], 0
        if cur:
            parts.append((tag, cur))
    obj_dir = os.path.join(out, "obj")
    # (a partition whose inputs are unchanged, the plan, its functions, the lifter's own source and the
    # flags, is neither lifted nor compiled again: its object comes from the cache by an input key. A host-only
    # rebuild then costs the hashing, not the 2-3 minutes of lifting)
    hle = host_effects.hle_names(gen)   # (the HLE'd functions' names decide what a call of one keeps)
    host_effects.check(hle)
    t_key = time.time()
    verify = bool(os.environ.get("HOOKSHOT_LIFT_VERIFY"))   # (lift_part reads it)
    base = ("%s %s %s\n" % (code_key(), facts_key(plan), "verify" if verify else "")).encode()
    probes.resolve(plan, plan_path)
    bound, rows = bounds.call_bounds(plan, hle, verify, funcs), patch_rows()
    flags, pres, keys, objs_k = [], [], [], []
    for k, (tag, names) in enumerate(parts):
        level = hot_opt if tag == "h" else opt
        fl = march_flags + ["-" + level, "-g0", "-Wno-everything", "-ffunction-sections",
                            "-mllvm", "-rotation-max-header-size=0"] + DSE_FLAGS
        pre = opt_argv(clang, level, march_flags)
        flags.append(fl)
        pres.append(pre)
        keys.append(hashlib.sha1(base + (" ".join(fl + (pre or [])) + "\n" + ",".join("%x" % e for e in names) + "\n" +
                                         part_key(plan, names, funcs, bound, rows)).encode(),
                                 usedforsecurity=False).hexdigest())
        objs_k.append(os.path.join(obj_dir, "lift_%s%03d.o" % (tag, k)))
    t_lift_start = time.time()
    todo = []
    lifted_sites = {}   # (partition -> the code pointer sites its lift translated: from its text, or beside its cached object)
    for k in range(len(parts)):
        hit = os.path.join(cache, "liftin_%s.o" % keys[k])
        if os.path.exists(hit) and os.path.exists(hit + ".cp"):
            lifted_sites[k] = int(open(hit + ".cp").read())
            if os.path.exists(objs_k[k]):
                os.remove(objs_k[k])
            os.link(hit, objs_k[k])
        else:
            todo.append(k)
    with concurrent.futures.ProcessPoolExecutor(min(jobs, 16)) as ex:
        texts = list(ex.map(lift_part, [(plan_path, parts[k][1], funcs) for k in todo])) if todo else []
        texts_ = [(seg["addr"], seg["addr"] + seg["size"]) for mod in json.load(open(os.path.join(
            os.path.dirname(plan_path), "image", "image.json")))["modules"] for seg in mod["segments"] if seg["name"] == ".text"]
        audits = list(ex.map(census_part, [(plan_path, names, texts_) for _, names in parts]))
    census = [c for c, _, _ in audits]
    odd = sorted(o for _, _, os_ in audits for o in os_)
    if odd:
        raise SystemExit("build: %d code addresses formed in a way lift2/code_ptrs.py does not know, first %#x: %s"
                         % (len(odd), odd[0][0], odd[0][1]))
    other = {}
    for _, kinds, _ in audits:
        for kk, v in kinds.items():
            other[kk] = other.get(kk, 0) + v
    for k, text in zip(todo, texts):
        lifted_sites[k] = int(CODE_SITES.search(text).group(1))
    # (every code pointer site of the census translated, and no other: an equality guard `cmp xF, known` must see both
    # sides translated or neither; lift2/code_ptrs.py)
    bad = [(parts[k][1][0], census[k], lifted_sites[k]) for k in range(len(parts)) if census[k] != lifted_sites[k]]
    if bad:
        raise SystemExit("build: code pointer sites translated differ from the census (code_ptrs.sites) in %d partitions, "
                         "first from f_%08x: census %d, translated %d" % ((len(bad),) + bad[0]))
    t_lift = time.time() - t0
    cjobs = [(clang, text, objs_k[k], flags[k], pres[k], cache) for k, text in zip(todo, texts)]
    objs, failed, t1 = [o for k, o in enumerate(objs_k) if k not in set(todo)], [], time.time()
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for (obj, secs, err), k in zip(ex.map(compile_part, cjobs), todo):
            objs.append(obj)
            if err:
                failed.append((obj, err))
                continue
            try:
                hit = os.path.join(cache, "liftin_%s.o" % keys[k])
                with open(hit + ".cp.tmp", "w") as f:
                    f.write("%d\n" % lifted_sites[k])
                os.replace(hit + ".cp.tmp", hit + ".cp")
                os.link(obj, hit + ".tmp")
                os.replace(hit + ".tmp", hit)
            except OSError:
                pass
    n_insn = sum(len(plan["bodies"][e][0]) for e in chosen)
    line = ("lift: %d functions (%d instructions; %d of them hot) in %d partitions (%d from the cache by their inputs); "
            "summaries %.0fs, keys %.0fs, lifted (and the code pointer census) in %.0fs, compiled in %.0fs; %d code pointer "
            "sites translated (the census's); code addresses left guest: %s" % (
                len(chosen), n_insn, len(chosen & hot), len(parts), len(parts) - len(todo), t_sum, t_lift_start - t_key,
                t_lift - (t_lift_start - t0), time.time() - t1, sum(census),
                ", ".join("%s %d" % kv for kv in sorted(other.items()))))
    if failed:
        raise SystemExit("lift: %d partitions failed to compile:\n%s" % (len(failed), failed[0][1]))
    print(bounds.call_report(plan, bounds.call_bounds(plan, hle, False, funcs), chosen), flush=True)
    return srcs, objs, line
