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
image, the discovery's facts and the lifter sources they read (plan_summaries). HOOKSHOT_LIFT_SUMMARY_JOBS: their
workers (8).
HOOKSHOT_LIFT_VERIFY=1 (at build time): every function with a full summary is checked at run time
(host/lift2_verify.c).
HOOKSHOT_HOT_FUNCS: the hot list (one f_<addr> a line; default work/hot_funcs_play.txt, else work/hot_funcs.txt).
"""
import concurrent.futures
import hashlib
import os
import pickle
import re
import subprocess
import sys
import time
from array import array

from . import bounds, build_ir, emit_llvm, enc, host_effects, llvm, probes

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


# the lifter's files the summaries depend on (the summary cache's key)
SUMMARY_SOURCES = re.compile(r"^(enc.*|sem.*|ir|bounds|build_ir|summary|probes|host_effects)\.py$")


def plan_summaries(L, img, path, hooks, no_call, classes, entry):
    """the summaries' job (summary.job) for the plan: (cache, input). The cache is keyed by the image, the discovery's
    facts, the host's patches (the HLE'd functions' symbols, the entry hooks: entry, {address: hook C name}; the rows
    are in probes.py) and the sources the summaries read; when it has them, input is None. Else the job's input is
    written beside the plan and the build computes them (finish_summaries) in a process of its own, after the lift's
    memory is gone"""
    hle = host_effects.hle_symbols(L.hooks)
    names = getattr(img, "names", {}) or {}
    h = hashlib.sha1(bytes(img.mem), usedforsecurity=False)
    here = os.path.dirname(os.path.abspath(__file__))
    for name in sorted(os.listdir(here)):
        if SUMMARY_SOURCES.match(name):
            with open(os.path.join(here, name), "rb") as f:
                h.update(name.encode() + f.read())
    h.update(repr((sorted(L.entries), sorted(hooks), sorted(L.jt.items()), sorted(L.plt.items()), sorted(no_call),
                   sorted(names.items()), sorted(hle.items()), sorted(entry.items()))).encode())
    d = os.path.dirname(path)
    cache = os.path.join(d, "summaries_%s.pickle" % h.hexdigest()[:16])
    if os.path.exists(cache):
        print("summaries: from the cache %s" % os.path.basename(cache))
        return cache, None
    inp = cache + ".in"
    for old in [x for x in os.listdir(d) if x.startswith("summaries_") and x.endswith(".in")]:
        os.remove(os.path.join(d, old))
    with open(inp + ".tmp", "wb") as f:
        fs = {}
        for e, (b, _) in L.funcs.items():
            pcs = array("Q", sorted(b))
            fs[e] = (pcs, array("I", map(img.word, pcs)))
        pickle.dump({"funcs": fs, "entries": set(L.entries), "jt": dict(L.jt), "plt": dict(L.plt), "no_call": no_call,
                     "classes": classes, "hle": hle, "entry_hooks": dict(entry),
                     "names": dict(names)}, f, protocol=pickle.HIGHEST_PROTOCOL)
    os.replace(inp + ".tmp", inp)
    print("summaries: to be computed by the build (%s)" % os.path.basename(inp))
    return cache, inp


def finish_summaries(plan_path, plan):
    """the plan with its summaries (summary_step, in a process of its own: the summaries' objects never enter the
    build's process, which keeps only the packed tuples)"""
    cache, inp = plan.get("summary_job") or (None, None)
    if cache is None or plan.get("summaries"):
        return plan
    nx2 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    subprocess.run([sys.executable, "-I", "-u", "-c", "import sys; sys.path.insert(0, %r); from lift2 import game; "
                    "game.summary_step(%r)" % (nx2, plan_path)], check=True)
    del plan
    _PLAN.pop(plan_path, None)
    import gc
    gc.collect()
    return _plan(plan_path)


def summary_step(plan_path):
    """the plan's summaries from their cache, or computed by summary.job (HOOKSHOT_LIFT_SUMMARY_JOBS workers, default
    8) from the input the lift left; the plan file is rewritten with them, packed"""
    from . import summary
    plan = _plan(plan_path)
    cache, inp = plan["summary_job"]
    t1 = time.time()
    if not os.path.exists(cache):
        if inp is None or not os.path.exists(inp):
            raise SystemExit("build: the plan's summaries are neither cached nor computable (%s): lift again" % cache)
        jobs = int(os.environ.get("HOOKSHOT_LIFT_SUMMARY_JOBS") or 8)
        out = cache + ".out"
        nx2 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        subprocess.run([sys.executable, "-I", "-c", "import sys; sys.path.insert(0, %r); from lift2 import summary; "
                        "summary.job(%r, %r, %d)" % (nx2, inp, out, jobs)], check=True)
        with open(out, "rb") as f:
            sums, stats, edges = pickle.load(f)
        os.remove(out)
        d = os.path.dirname(cache)
        for old in [x for x in os.listdir(d) if x.startswith("summaries_") and not x.startswith(os.path.basename(cache))]:
            os.remove(os.path.join(d, old))
        with open(cache + ".tmp", "wb") as f:
            pickle.dump((sums, edges, stats), f, protocol=pickle.HIGHEST_PROTOCOL)
        os.replace(cache + ".tmp", cache)   # (the input stays until the next lift: tools recompute from it)
        summary.report(sums, edges, plan["insns"], plan.get("hot", ()))
        print("summaries: %(functions)d functions, %(sccs)d components (largest %(largest scc)d) in %(levels)d "
              "levels; graph %(graph s).0fs, fixed point %(fixed point s).0fs, results %(results s).0fs" % stats)
        print("summaries: %.0fs in all; peak memory of the summary process %s MB, of its largest worker %s MB" % (
            time.time() - t1, stats.get("main peak MB", "?"), stats.get("worker peak MB", "?")), flush=True)
    else:
        with open(cache, "rb") as f:
            sums, _, _ = pickle.load(f)
    plan["summaries"] = {e: summary.pack(s) for e, s in sums.items()}
    with open(plan_path + ".tmp", "wb") as f:
        pickle.dump(plan, f, protocol=pickle.HIGHEST_PROTOCOL)
    os.replace(plan_path + ".tmp", plan_path)


def hot_list(work):
    """the hot list's functions (HOOKSHOT_HOT_FUNCS, else work/hot_funcs_play.txt, else work/hot_funcs.txt)"""
    path = os.environ.get("HOOKSHOT_HOT_FUNCS") or next(
        (p for p in (os.path.join(work, "hot_funcs_play.txt"), os.path.join(work, "hot_funcs.txt")) if os.path.exists(p)),
        None)
    if not path or not os.path.exists(path):
        return set()
    return {int(w[2:], 16) for line in open(path) for w in line.split()[:1] if w.startswith("f_")}


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
    # the whole program's summaries: computed (or taken from their cache) by the build, finish_summaries
    import entry_hooks   # (host/*.c's ENTRY_HOOKs: the summaries count what each reads and writes)
    entry, missing = entry_hooks.wanted(os.path.join(os.path.dirname(path), "gen"))
    job = plan_summaries(L, img, path, hooks, no_call, classes, entry)
    plan = {"summaries": {}, "summary_job": job, "entries": sorted(L.entries), "jt": dict(L.jt), "jt_index": dict(L.jt_index), "plt": dict(L.plt), "no_call": sorted(no_call), "sj_rets": sj_rets,
            "hooks": sorted(hooks), "entry_hooks": entry, "bodies": bodies, "why": why, "hot": sorted(set(hot) & set(bodies)),
            "insns": {e: len(b) for e, (b, _) in L.funcs.items()}}
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
define void @f_%(a)08x(ptr %%ctx) {
  %%fr = alloca [32 x i64], align 8
  call void @lift2_check_enter(ptr %%ctx, ptr @sum_%(a)08x, ptr %%fr)
  call void @body_%(a)08x(ptr %%ctx)
  call void @lift2_check_exit(ptr %%ctx, ptr @sum_%(a)08x, ptr %%fr)
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
    plan_path, names, hooks = args
    plan = _plan(plan_path)
    plt = plan["plt"]
    verify = bool(os.environ.get("HOOKSHOT_LIFT_VERIFY"))
    sj = plan.get("sj_rets", {})
    key = (plan_path, tuple(sorted(hooks.items())), verify)
    if key not in _BOUNDS:   # (once a worker process)
        _BOUNDS.clear()
        hle = host_effects.hle_names(os.path.join(os.path.dirname(plan_path), "gen"))
        _BOUNDS[key] = bounds.call_bounds(plan, hooks, hle, verify)
    bound = _BOUNDS[key]
    prog = build_ir.Program(entries=set(plan["entries"]), jt=plan["jt"], callee=callee_fn(plt), plt=plt,
                            jt_index=plan.get("jt_index"), bound=bound, omit=True, verify=verify)
    m = emit_llvm.Module()
    extra = []
    for e in names:
        pcs, words = plan["bodies"][e]
        w = dict(zip(pcs, words))
        prog.probes = probes.find(e, pcs, w.__getitem__)
        prog.before = probes.at(e, pcs, w.__getitem__, before=True)
        prog.after = probes.at(e, pcs, w.__getitem__)
        prog.replace = probes.at(e, pcs, w.__getitem__, replace=True)
        fn = build_ir.build(e, list(pcs), w.__getitem__, prog)
        fn.name = "f_%08x" % e
        fn.entry_hook = hooks.get(e)
        if verify and bounds.verified(plan, e):
            fn.name = "body_%08x" % e
            s = plan["summaries"][e]
            extra.append(VERIFY_WRAPPER % {"a": e, "inputs": s[0] & GPR, "may": s[1] & GPR, "res": s[4] & GPR,
                                           "pres": s[3] & GPR, "flags": 1 if s[5] else 0, "must": s[2] & GPR})
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
    return m.text() + emit_llvm.no_capture("\n".join(extra)) + "\n"


_PLAN = {}
_BOUNDS = {}


def _plan(path):
    if path not in _PLAN:
        with open(path, "rb") as f:
            _PLAN[path] = pickle.load(f)
    return _PLAN[path]


def input_base(plan_path):
    """what every partition's input key starts from: the plan's bytes and the lifter's source files"""
    h = hashlib.sha1(usedforsecurity=False)
    with open(plan_path, "rb") as f:
        for block in iter(lambda: f.read(1 << 24), b""):
            h.update(block)
    h.update(b"verify" if os.environ.get("HOOKSHOT_LIFT_VERIFY") else b"")   # (lift_part reads it)
    here = os.path.dirname(os.path.abspath(__file__))
    for name in sorted(os.listdir(here)):
        if name.endswith(".py"):
            with open(os.path.join(here, name), "rb") as f:
                h.update(name.encode() + f.read())
    return h.hexdigest().encode()


def prepare(gen, out, jobs, opt, hot_opt, march_flags, cache):
    """(the C files to compile, the objects to link, a report line); see the module's description"""
    t0 = time.time()
    clang = llvm.check("clang")
    plan_path = os.path.join(os.path.dirname(gen), "plan.pickle")
    if not os.path.exists(plan_path):
        raise SystemExit("build: no plan %s: lift the game first (scripts/relift.sh)" % plan_path)
    plan = finish_summaries(plan_path, _plan(plan_path))
    if os.environ.get("HOOKSHOT_LIFT_VERIFY") and not plan.get("summaries"):
        raise SystemExit("build: HOOKSHOT_LIFT_VERIFY needs summaries in the plan: lift again")
    chosen = set(plan["bodies"])
    hot = set(plan.get("hot", ()))
    import entry_hooks   # (host/*.c's ENTRY_HOOKs: a call at the function's entry, emit_llvm entry_hook)
    wanted, missing = entry_hooks.wanted(gen)
    absent = sorted(e for e in wanted if e not in chosen)
    if missing or absent:   # (a hook the build would leave out: its host code dead and nothing saying so)
        raise SystemExit("build: ENTRY_HOOKs with no lifted function: %s" % ", ".join(
            ["%s (not exported)" % m for m in missing] + ["%s at %#x" % (wanted[e], e) for e in absent]))
    if plan.get("entry_hooks") != wanted:   # (the summaries count each hook's reads and writes: they must be these)
        raise SystemExit("build: the ENTRY_HOOKs differ from those the lift's summaries count: lift again "
                         "(scripts/relift.sh)")
    hooks = wanted
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
    host_effects.check(hle, hooks)
    base = input_base(plan_path) + repr(sorted(hooks.items())).encode() + repr(sorted(hle.items())).encode()
    flags, pres, keys, objs_k = [], [], [], []
    for k, (tag, names) in enumerate(parts):
        level = hot_opt if tag == "h" else opt
        fl = march_flags + ["-" + level, "-g0", "-Wno-everything", "-ffunction-sections",
                            "-mllvm", "-rotation-max-header-size=0"] + DSE_FLAGS
        pre = opt_argv(clang, level, march_flags)
        flags.append(fl)
        pres.append(pre)
        keys.append(hashlib.sha1(base + (" ".join(fl + (pre or [])) + "\n" + ",".join("%x" % e for e in names)).encode(),
                                 usedforsecurity=False).hexdigest())
        objs_k.append(os.path.join(obj_dir, "lift_%s%03d.o" % (tag, k)))
    todo = []
    for k in range(len(parts)):
        hit = os.path.join(cache, "liftin_%s.o" % keys[k])
        if os.path.exists(hit):
            if os.path.exists(objs_k[k]):
                os.remove(objs_k[k])
            os.link(hit, objs_k[k])
        else:
            todo.append(k)
    with concurrent.futures.ProcessPoolExecutor(min(jobs, 16)) as ex:
        texts = list(ex.map(lift_part, [(plan_path, parts[k][1], hooks) for k in todo])) if todo else []
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
                os.link(obj, hit + ".tmp")
                os.replace(hit + ".tmp", hit)
            except OSError:
                pass
    n_insn = sum(len(plan["bodies"][e][0]) for e in chosen)
    line = ("lift: %d functions (%d instructions; %d of them hot) in %d partitions (%d from the cache by their inputs); "
            "lifted in %.0fs, compiled in %.0fs" % (len(chosen), n_insn, len(chosen & hot), len(parts),
                                                    len(parts) - len(todo), t_lift, time.time() - t1))
    if failed:
        raise SystemExit("lift: %d partitions failed to compile:\n%s" % (len(failed), failed[0][1]))
    print(bounds.call_report(plan, bounds.call_bounds(plan, hooks, hle), chosen), flush=True)
    return srcs, objs, line
