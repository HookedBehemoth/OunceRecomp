"""The mixed build: functions lift 2 can take come from it, the rest from the present lifter (milestone 5).

HOOKSHOT_LIFT2=all-safe (the default) | <file of guest addresses, one per line (hex)> | 0 (the present lifter's build
alone).
HOOKSHOT_LIFT2_VERIFY=1 (at lift and build time, milestone 14): every function with a full summary is checked at run
time (host/lift2_verify.c): its generic entry wraps its body in checks of the summary. The lift computes the summaries
(milestone 13) only with it or with HOOKSHOT_LIFT2_SUMMARIES=1: the plan's "summaries" is empty otherwise.
HOOKSHOT_LIFT2_CLASSES=int,fp,neon,crypto (the default; at lift time): the instruction classes (sem.CLASS) a function may use to
be taken.

At lift time (nx2/lift.py, the variable set) write_plan() keeps what lift 2 needs from the discovery: the entries,
jump tables, PLT stubs, setjmp owners, hooked addresses, and the instructions of every function lift 2 can take
(build_ir.scope), in <work>/sa/lift2_plan.pickle.

At build time (nx2/build.py, the variable set) prepare():
- chooses the functions: the plan's, minus every function whose lifted C carries a host edit (an ENTRY_HOOK, a
  SPLICE_AFTER, a game patch of nx2/oot_patches.py, oot_wide.py, oot_jitter.py, oot_occlusion.py: their C names an
  `entry_`, `splice_` or `hos_` symbol, or setjmp's `recomp_sj`), minus the functions not in the use file;
- writes copies of the present lifter's code files in which those functions' definitions are made static and unused
  (`lift1_f_<addr>`), so that `f_<addr>` resolves to lift 2's thunk;
- lifts the chosen functions into partitions by guest address, each `g_<addr>` plus a thunk `f_<addr>` (a musttail
  call of `g_<addr>`: the present lifter's callers, the function table and the hooks keep calling `f_<addr>`), and
  streams each partition's text into `clang -x ir -` (no IR on disk); objects are cached by the text's hash;
- returns the code files to compile instead and the extra objects to link.
The hot set (functions of code_hot*.c) goes into its own partitions, compiled at the hot level (-O2) without the
profile (the present PGO profile is clang's front-end instrumentation, which IR input cannot use).
"""
import concurrent.futures
import hashlib
import os
import pickle
import re
import subprocess
import time
from array import array

from . import build_ir, emit_llvm, enc, llvm, probes

# (an entry hook's line, nx2/entry_hooks.py: lift 2 makes the same call at its generic entry, emit_llvm entry_hook)
HOOK_LINE = re.compile(r"    if \((entry_\w+)\(c\)\) return;   /\* \(nx2/entry_hooks\.py\) \*/\n")
# (recomp_sj_*: the present lifter's block around a call of a setjmp owner, which lift 2 makes as hos_sj_call)
EDITED = re.compile(r"\b(entry_[0-9a-fx]+|splice_[0-9a-fx]+|hos_\w+)\s*\(|\bhos_\w+\b")
DEF = re.compile(r"^void (f_[0-9a-f]{8})\(Ctx \*__restrict A64_NOESC c\) \{", re.M)
PART_INSNS = 120000


def spec():
    """HOOKSHOT_LIFT2: "all-safe" when unset, None for 0 (the present lifter alone)"""
    v = os.environ.get("HOOKSHOT_LIFT2", "all-safe")
    return None if v in ("", "0") else v


def callee_fn(plt):
    def callee(t):
        seen = 0
        while t in plt and seen < 4:
            t = plt[t][0]
            seen += 1
        return t
    return callee


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


def summaries(L, img, path, prog, hooks):
    """milestone 13's summaries of every function (every class: callees of lift 2's functions too), packed for the
    plan"""
    import resource
    from . import summary
    t1 = time.time()
    names = getattr(img, "names", {}) or {}
    sums, F, stats = summary.summarise({e: sorted(b) for e, (b, _) in L.funcs.items()}, prog, hooks, img.word,
                                       names)
    hot_path = os.environ.get("A64_HOT_FUNCS") or os.path.join(os.path.dirname(os.path.dirname(path)), "..",
                                                               "hot_funcs_play.txt")
    hot = []
    if os.path.exists(hot_path):
        hot = [int(l.strip()[2:], 16) for l in open(hot_path) if l.strip().startswith("f_")]
    summary.report(sums, F, hot)
    for a in os.environ.get("LIFT2_SUMMARY_DEBUG", "").split(","):   # (a function's call sites and their callees)
        if a:
            summary.explain(int(a, 16), sums, F, hooks)
    print("summaries: %(functions)d functions, %(sccs)d components (largest %(largest scc)d), %(iterations)d "
          "analyses; decode and graph %(build s).0fs, fixed point %(fixed point s).0fs, results %(results s).0fs" % stats)
    print("summaries: %.0fs in all, peak memory %d MB (one process)" % (
        time.time() - t1, resource.getrusage(resource.RUSAGE_SELF).ru_maxrss >> 10), flush=True)
    packed = {e: (s.inputs, s.clobbers, s.mustdef, s.preserved, s.results, s.returns, tuple(s.demote), s.unknown,
                  s.frame, s.pairs, s.boundary, s.fp, s.writes_fpcr) for e, s in sums.items()}
    return packed


def write_plan(L, img, path):
    """the discovery's facts and the instructions of every function lift 2 can take; returns those bodies"""
    t0 = time.time()
    no_call = set(getattr(L, "setjmp_owner", {}) or ())
    classes = os.environ.get("HOOKSHOT_LIFT2_CLASSES", "int,fp,neon,crypto").split(",")
    prog = build_ir.Program(entries=L.entries, jt=L.jt, callee=L.callee, plt=L.plt, no_call=no_call, classes=classes)
    # (a BL to a setjmp owner is lifted as a call of hos_sj_call, host/hos_setjmp.c: scope need not refuse it)
    scope_prog = build_ir.Program(entries=L.entries, jt=L.jt, callee=L.callee, plt=L.plt, classes=classes)
    sj_rets = {e: tuple((list(r) + [0, 0, 0, 0])[:4]) for e, r in (getattr(L, "setjmp_owner", {}) or {}).items()}
    after_c = dict(getattr(L, "after", {}) or {})   # (C after an instruction: oot_wide, oot_jitter, oot_occlusion)
    hooks = set(L.hooks)
    bodies, why = {}, {}
    for e, (body, _) in L.funcs.items():
        if e in hooks:
            why["hooked"] = why.get("hooked", 0) + 1
            continue
        pcs = sorted(body)
        if any(after_c.get(pc) and enc.decode(img.word(pc)) is not None and not probes.is_call(img.word(pc)) and
               enc.decode(img.word(pc)).entry.template in build_ir.TERMINATORS for pc in pcs):
            why["C added after a branch"] = why.get("C added after a branch", 0) + 1   # (after a call: Program.after)
            continue
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
        if r is not None:
            k = re.sub(r" \(.*| at .*| [0-9a-f]{8}.*|0x[0-9a-f]+", "", r)
            why[k] = why.get(k, 0) + 1
            continue
        bodies[e] = (array("Q", pcs), array("I", (img.word(pc) for pc in pcs)))
    # milestone 13: the whole program's summaries, only when something uses them (the verification build, or
    # HOOKSHOT_LIFT2_SUMMARIES=1): they peak at about 9 GB in this process, on top of the present lifter's state
    packed = {}
    if os.environ.get("HOOKSHOT_LIFT2_VERIFY") or os.environ.get("HOOKSHOT_LIFT2_SUMMARIES"):
        packed = summaries(L, img, path, prog, hooks)
        import gc
        gc.collect()   # (the analysis' graph is garbage now; the C is written next, in this process)
    plan = {"summaries": packed, "entries": sorted(L.entries), "jt": dict(L.jt), "plt": dict(L.plt), "no_call": sorted(no_call), "sj_rets": sj_rets, "after_c": after_c,
            "hooks": sorted(hooks), "bodies": bodies, "why": why,
            "insns": {e: len(b) for e, (b, _) in L.funcs.items()}}
    with open(path + ".tmp", "wb") as f:
        pickle.dump(plan, f, protocol=pickle.HIGHEST_PROTOCOL)
    os.replace(path + ".tmp", path)
    n = sum(len(p) for p, _ in bodies.values())
    print("lift2 plan: %d of %d functions (%d instructions) liftable by lift 2 (%.0fs); passed over: %s" % (
        len(bodies), len(L.funcs), n, time.time() - t0,
        ", ".join("%s %d" % kv for kv in sorted(why.items(), key=lambda kv: -kv[1])[:10])), flush=True)
    return bodies


def edited_functions(gen, after_texts=()):
    """functions whose lifted C carries a host edit other than an entry hook, the hot set (the functions of
    code_hot*.c), and the entry hooks (function -> the hook's C name). after_texts: the C put after instructions
    (Lifter.after, the plan's after_c), which lift 2 makes with probes.after_c: not an edit"""
    edited, hot, hooks = set(), set(), {}
    for name in sorted(os.listdir(gen)):
        if not (name.startswith("code_") and name.endswith(".c")):
            continue
        text = open(os.path.join(gen, name)).read()
        defs = list(DEF.finditer(text))
        for k, m in enumerate(defs):
            body = text[m.end():defs[k + 1].start() if k + 1 < len(defs) else len(text)]
            e = int(m.group(1)[2:], 16)
            if e in probes.BY_FUNC:   # (the present lifter's patch, which lift 2 makes as a probe: probes.py)
                body = "".join(l for l in body.splitlines(True) if not any(mk in l for mk in probes.MARKS))
            if e in probes.SPLICE_AT or e in probes.SPLICE_BEFORE:   # (its SPLICE_AFTER, which lift 2 calls by address)
                body = body.replace("    splice_0x%08x(c);   /* (nx2/entry_hooks.py) */\n" % e, "")
            for t in after_texts:   # (C after an instruction: probes.after_c)
                if t in body:
                    body = body.replace(t, "")
            hook = HOOK_LINE.search(body)
            if hook:
                hooks[e] = hook.group(1)
                body = HOOK_LINE.sub("", body)
            if EDITED.search(body):
                edited.add(e)
            if name.startswith("code_hot"):
                hot.add(e)
    return edited, hot, hooks


def thunked_source(src, chosen, out_dir):
    """a copy of a code file whose chosen functions are static and unused (lift 2's f_<addr> thunks stand in), or the
    file itself when it has none of them"""
    text = open(src).read()
    hit = [False]

    def sub(m):
        if int(m.group(1)[2:], 16) in chosen:
            hit[0] = True
            return "static __attribute__((unused)) void lift1_%s(Ctx *__restrict A64_NOESC c) {" % m.group(1)
        return m.group(0)
    new = DEF.sub(sub, text)
    if not hit[0]:
        return src
    dst = os.path.join(out_dir, os.path.basename(src))
    if not os.path.exists(dst) or open(dst).read() != new:
        with open(dst, "w", newline="\n") as f:
            f.write(new)
    return dst


def compile_part(job):
    """one partition: IR text streamed into clang, the object cached by the text's hash"""
    clang, text, obj, fl, cache = job
    key = hashlib.sha1((" ".join(fl) + "\n").encode() + text.encode(), usedforsecurity=False).hexdigest()
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
    r = subprocess.run([clang] + fl + ["-x", "ir", "-", "-c", "-o", obj], input=text, capture_output=True, text=True)
    if r.returncode:
        return obj, time.time() - t, (r.stderr or r.stdout)[-3000:]
    open(stamp, "w").write(key)
    try:
        os.link(obj, cached + ".tmp")
        os.replace(cached + ".tmp", cached)
    except OSError:
        pass
    return obj, time.time() - t, None


VERIFY_WRAPPER = """@sum_%(a)08x = private constant [6 x i64] [i64 %(inputs)d, i64 %(may)d, i64 %(res)d, i64 %(pres)d, i64 %(flags)d, i64 %(a)d]
define void @g_%(a)08x(ptr %%ctx) {
  %%fr = alloca [32 x i64], align 8
  call void @lift2_check_enter(ptr %%ctx, ptr @sum_%(a)08x, ptr %%fr)
  call void @body_%(a)08x(ptr %%ctx)
  call void @lift2_check_exit(ptr %%ctx, ptr @sum_%(a)08x, ptr %%fr)
  ret void
}"""
GPR = (1 << 32) - 1


def verified(plan, e):
    """a function the verification build checks: a full summary (no demotion)"""
    s = plan.get("summaries", {}).get(e)
    return s is not None and not s[6] and not s[7]


def lift_part(args):
    """(lifted in a worker process) the IR text of one partition"""
    plan_path, names, chosen, hot, hooks = args
    plan = _plan(plan_path)
    plt = plan["plt"]
    verify = bool(os.environ.get("HOOKSHOT_LIFT2_VERIFY"))
    base_symbol = lambda e: ("g_%08x" if e in chosen else "f_%08x") % e
    sj = plan.get("sj_rets", {})
    # (a call of a setjmp owner: hos_sj_call through a per-owner stub, as the present lifter's recomp_sj_* block)
    symbol = lambda e: "sjcall_%08x" % e if e in sj else base_symbol(e)
    # (verification: a tail call goes to the callee's body, past its checks, so that a loop of tail calls does not
    # grow the host stack; the caller's own checks cover the callee's effects)
    def tail_symbol(e):
        return "body_%08x" % e if verify and e in chosen and verified(plan, e) else base_symbol(e)
    prog = build_ir.Program(entries=set(plan["entries"]), jt=plan["jt"], callee=callee_fn(plt), plt=plt,
                            symbol=symbol, tail_symbol=tail_symbol)
    m = emit_llvm.Module()
    extra = []
    for e in names:
        pcs, words = plan["bodies"][e]
        w = dict(zip(pcs, words))
        prog.probes = probes.find(e, pcs, w.__getitem__)
        prog.before = probes.splice_before(e, pcs, w.__getitem__)
        # (after an instruction, in this order: the At probes, the C after it, the SPLICE_AT splices)
        prog.after = probes.merge(probes.at(e, pcs, w.__getitem__),
                                  probes.after_c(plan.get("after_c", {}), pcs, w.__getitem__),
                                  probes.after_c(probes.splice_texts(e, w.__getitem__), pcs, w.__getitem__))
        fn = build_ir.build(e, list(pcs), w.__getitem__, prog)
        fn.entry_hook = hooks.get(e)
        if verify and verified(plan, e):
            fn.name = "body_%08x" % e
            s = plan["summaries"][e]
            extra.append(VERIFY_WRAPPER % {"a": e, "inputs": s[0] & GPR, "may": s[1] & GPR, "res": s[4] & GPR,
                                           "pres": s[3] & GPR, "flags": 1 if s[5] else 0})
        m.add(fn)
        m.defined = m.defined | {"g_%08x" % e}   # (the wrapper defines g_; drop declarations of it)
        extra.append("define void @f_%08x(ptr %%ctx) {\n  musttail call void @g_%08x(ptr %%ctx)\n  ret void\n}" % (e, e))
    if verify:
        extra.append("declare void @lift2_check_enter(ptr, ptr, ptr)\ndeclare void @lift2_check_exit(ptr, ptr, ptr)")
    stubs = sorted(int(d.split("@sjcall_")[1][:8], 16) for d in m.decls if d.startswith("declare void @sjcall_"))
    if stubs:
        m.defined = m.defined | {"sjcall_%08x" % e for e in stubs}
        extra.append("declare void @hos_sj_call(ptr, ptr, i64, i64, i64, i64)")
        for e in stubs:
            t = base_symbol(e)
            if t not in m.defined:
                m.decls.add("declare void @%s(ptr)" % t)
            extra.append("define internal void @sjcall_%08x(ptr %%ctx) {\n  call void @hos_sj_call(ptr %%ctx, ptr @%s, "
                         "i64 %d, i64 %d, i64 %d, i64 %d)\n  ret void\n}" % ((e, t) + sj[e]))
    return m.text() + "\n".join(extra) + "\n"


_PLAN = {}


def _plan(path):
    if path not in _PLAN:
        with open(path, "rb") as f:
            _PLAN[path] = pickle.load(f)
    return _PLAN[path]


def input_base(plan_path, chosen):
    """what every partition's input key starts from: the plan's bytes, the chosen set and lift 2's source files"""
    h = hashlib.sha1(usedforsecurity=False)
    with open(plan_path, "rb") as f:
        for block in iter(lambda: f.read(1 << 24), b""):
            h.update(block)
    h.update(",".join("%x" % e for e in sorted(chosen)).encode())
    h.update(b"verify" if os.environ.get("HOOKSHOT_LIFT2_VERIFY") else b"")   # (lift_part reads it)
    here = os.path.dirname(os.path.abspath(__file__))
    for name in sorted(os.listdir(here)):
        if name.endswith(".py"):
            with open(os.path.join(here, name), "rb") as f:
                h.update(name.encode() + f.read())
    return h.hexdigest().encode()


def prepare(spec, gen, out, srcs, jobs, opt, hot_opt, march_flags, cache):
    """(the code files to compile, the lift 2 objects to link, a report line); see the module's description"""
    t0 = time.time()
    clang = llvm.check("clang")
    plan_path = os.path.join(os.path.dirname(gen), "lift2_plan.pickle")
    if not os.path.exists(plan_path):
        raise SystemExit("lift2: no plan %s: the lift predates the mixed build's default; lift again "
                         "(scripts/relift.sh), or build the present lifter's game alone with HOOKSHOT_LIFT2=0" % plan_path)
    plan = _plan(plan_path)
    if os.environ.get("HOOKSHOT_LIFT2_VERIFY") and not plan.get("summaries"):
        raise SystemExit("lift2: HOOKSHOT_LIFT2_VERIFY needs summaries in the plan: lift again with it set")
    edited, hot, hooks = edited_functions(gen, tuple(set(plan.get("after_c", {}).values())))
    chosen = set(plan["bodies"]) - edited
    from . import owned
    own = owned.load(os.path.dirname(gen))
    if own is not None:   # (the present lifter wrote only its own functions: lift 2 takes every other one)
        chosen = set(plan["bodies"]) - own[0]
        hot = own[2]
        import entry_hooks   # (the entry hooks of functions without present-lifter C: from host/*.c itself)
        hooks = {e: n for e, n in entry_hooks.wanted(gen)[0].items() if e in chosen or e in hooks}
    if spec != "all-safe":
        want = {int(l.split()[0], 16) for l in open(spec) if l.strip() and not l.startswith("#")}
        chosen &= want
    lost = sorted(probes.AT_FUNCS - chosen)
    if lost and own is not None:   # (the present lifter does not make the At probes: such a function would lose its patch)
        raise SystemExit("lift2: the At probes' functions %s are not lift 2's" % ", ".join("f_%08x" % e for e in lost))
    with open(os.path.join(out, "lift2_functions.txt"), "w") as f:
        f.write("".join("%08x\n" % e for e in sorted(chosen)))
    # the present lifter's files, with the chosen definitions out of the way
    src_dir = os.path.join(out, "lift2_src")
    os.makedirs(src_dir, exist_ok=True)
    new_srcs = [thunked_source(s, chosen, src_dir) if os.path.basename(s).startswith("code_") else s for s in srcs]
    # (the C the present lifter put after an instruction, and the SPLICE_AT calls, as host functions lift 2 calls)
    after = dict(plan.get("after_c", {}))
    after.update(probes.all_splice_texts())
    new_srcs.append(probes.write_after_c(after, src_dir))
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
    # (a partition whose inputs are unchanged, the plan, the chosen set, its functions, lift 2's own source and the
    # flags, is neither lifted nor compiled again: its object comes from the cache by an input key. A host-only
    # rebuild then costs the hashing, not the 2-3 minutes of lifting)
    base = input_base(plan_path, chosen) + repr(sorted(hooks.items())).encode()
    flags, keys, objs_k = [], [], []
    for k, (tag, names) in enumerate(parts):
        fl = march_flags + ["-" + (hot_opt if tag == "h" else opt), "-g0", "-Wno-everything", "-ffunction-sections",
                            "-mllvm", "-rotation-max-header-size=0"]
        flags.append(fl)
        keys.append(hashlib.sha1(base + (" ".join(fl) + "\n" + ",".join("%x" % e for e in names)).encode(),
                                 usedforsecurity=False).hexdigest())
        objs_k.append(os.path.join(obj_dir, "lift2_%s%03d.o" % (tag, k)))
    todo = []
    for k in range(len(parts)):
        hit = os.path.join(cache, "lift2in_%s.o" % keys[k])
        if os.path.exists(hit):
            if os.path.exists(objs_k[k]):
                os.remove(objs_k[k])
            os.link(hit, objs_k[k])
        else:
            todo.append(k)
    with concurrent.futures.ProcessPoolExecutor(min(jobs, 16)) as ex:
        texts = list(ex.map(lift_part, [(plan_path, parts[k][1], chosen, hot, hooks) for k in todo])) if todo else []
    t_lift = time.time() - t0
    cjobs = [(clang, text, objs_k[k], flags[k], cache) for k, text in zip(todo, texts)]
    objs, failed, t1 = [o for k, o in enumerate(objs_k) if k not in set(todo)], [], time.time()
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for (obj, secs, err), k in zip(ex.map(compile_part, cjobs), todo):
            objs.append(obj)
            if err:
                failed.append((obj, err))
                continue
            try:
                hit = os.path.join(cache, "lift2in_%s.o" % keys[k])
                os.link(obj, hit + ".tmp")
                os.replace(hit + ".tmp", hit)
            except OSError:
                pass
    n_insn = sum(len(plan["bodies"][e][0]) for e in chosen)
    total = sum(plan["insns"].values())
    n_hot = len(chosen & hot)
    line = ("lift2: %d functions (%d instructions, %.1f %% of %d; %d of %d hot functions) in %d partitions (%d from "
            "the cache by their inputs); lifted in %.0fs, compiled in %.0fs; %d kept on the present lifter for host "
            "edits" % (len(chosen), n_insn, 100.0 * n_insn / max(total, 1), total, n_hot, len(hot), len(parts),
                       len(parts) - len(todo), t_lift, time.time() - t1, len(set(plan["bodies"]) & edited)))
    if failed:
        raise SystemExit("lift2: %d partitions failed to compile:\n%s" % (len(failed), failed[0][1]))
    return new_srcs, objs, line
