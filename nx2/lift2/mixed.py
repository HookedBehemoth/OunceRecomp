"""The mixed build: functions lift 2 can take come from it, the rest from the present lifter (milestone 5).

HOOKSHOT_LIFT2=all-safe | <file of guest addresses, one per line (hex)>; unset: the build is the present lifter's alone.
HOOKSHOT_LIFT2_VERIFY=1 (at build time, milestone 14): every function with a full summary is checked at run time
(host/lift2_verify.c): its generic entry wraps its body in checks of the summary.
HOOKSHOT_LIFT2_CLASSES=int[,fp,neon] (at lift time): the instruction classes (sem.CLASS) a function may use to be
taken; int alone by default (FP/NEON functions need the runtime's FP mode first).

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

from . import build_ir, emit_llvm, llvm

EDITED = re.compile(r"\b(entry_[0-9a-fx]+|splice_[0-9a-fx]+|hos_\w+|recomp_sj_\w+)\s*\(|\bhos_\w+\b")
DEF = re.compile(r"^void (f_[0-9a-f]{8})\(Ctx \*__restrict A64_NOESC c\) \{", re.M)
PART_INSNS = 120000


def callee_fn(plt):
    def callee(t):
        seen = 0
        while t in plt and seen < 4:
            t = plt[t][0]
            seen += 1
        return t
    return callee


def write_plan(L, img, path):
    """the discovery's facts and the instructions of every function lift 2 can take"""
    t0 = time.time()
    no_call = set(getattr(L, "setjmp_owner", {}) or ())
    classes = os.environ.get("HOOKSHOT_LIFT2_CLASSES", "int").split(",")
    prog = build_ir.Program(entries=L.entries, jt=L.jt, callee=L.callee, plt=L.plt, no_call=no_call, classes=classes)
    after = set(getattr(L, "after", {}) or ())
    hooks = set(L.hooks)
    bodies, why = {}, {}
    for e, (body, _) in L.funcs.items():
        if e in hooks:
            why["hooked"] = why.get("hooked", 0) + 1
            continue
        pcs = sorted(body)
        if after.intersection(pcs):
            why["C added after an instruction (oot_wide, oot_jitter, oot_occlusion)"] = why.get(
                "C added after an instruction (oot_wide, oot_jitter, oot_occlusion)", 0) + 1
            continue
        r = build_ir.scope(e, pcs, img.word, prog)
        if r is not None:
            k = re.sub(r" \(.*| at .*| [0-9a-f]{8}.*|0x[0-9a-f]+", "", r)
            why[k] = why.get(k, 0) + 1
            continue
        bodies[e] = (array("Q", pcs), array("I", (img.word(pc) for pc in pcs)))
    # milestone 13: the whole program's summaries (every function, every class: callees of lift 2's functions too)
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
    print("summaries: %(functions)d functions, %(sccs)d components (largest %(largest scc)d), %(iterations)d "
          "analyses; decode and graph %(build s).0fs, fixed point %(fixed point s).0fs, results %(results s).0fs" % stats)
    print("summaries: %.0fs in all, peak memory %d MB (one process)" % (
        time.time() - t1, resource.getrusage(resource.RUSAGE_SELF).ru_maxrss >> 10), flush=True)
    packed = {e: (s.inputs, s.clobbers, s.mustdef, s.preserved, s.results, s.returns, tuple(s.demote), s.unknown,
                  s.frame, s.pairs, s.boundary, s.fp, s.writes_fpcr) for e, s in sums.items()}
    plan = {"summaries": packed, "entries": sorted(L.entries), "jt": dict(L.jt), "plt": dict(L.plt), "no_call": sorted(no_call),
            "hooks": sorted(hooks), "bodies": bodies, "why": why,
            "insns": {e: len(b) for e, (b, _) in L.funcs.items()}}
    with open(path + ".tmp", "wb") as f:
        pickle.dump(plan, f, protocol=pickle.HIGHEST_PROTOCOL)
    os.replace(path + ".tmp", path)
    n = sum(len(p) for p, _ in bodies.values())
    print("lift2 plan: %d of %d functions (%d instructions) liftable by lift 2 (%.0fs); passed over: %s" % (
        len(bodies), len(L.funcs), n, time.time() - t0,
        ", ".join("%s %d" % kv for kv in sorted(why.items(), key=lambda kv: -kv[1])[:10])), flush=True)


def edited_functions(gen):
    """functions whose lifted C carries a host edit, and the hot set (the functions of code_hot*.c)"""
    edited, hot = set(), set()
    for name in sorted(os.listdir(gen)):
        if not (name.startswith("code_") and name.endswith(".c")):
            continue
        text = open(os.path.join(gen, name)).read()
        defs = list(DEF.finditer(text))
        for k, m in enumerate(defs):
            body = text[m.end():defs[k + 1].start() if k + 1 < len(defs) else len(text)]
            e = int(m.group(1)[2:], 16)
            if EDITED.search(body):
                edited.add(e)
            if name.startswith("code_hot"):
                hot.add(e)
    return edited, hot


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
    plan_path, names, chosen, hot = args
    plan = _plan(plan_path)
    plt = plan["plt"]
    verify = bool(os.environ.get("HOOKSHOT_LIFT2_VERIFY"))
    symbol = lambda e: ("g_%08x" if e in chosen else "f_%08x") % e
    # (verification: a tail call goes to the callee's body, past its checks, so that a loop of tail calls does not
    # grow the host stack; the caller's own checks cover the callee's effects)
    def tail_symbol(e):
        return "body_%08x" % e if verify and e in chosen and verified(plan, e) else symbol(e)
    prog = build_ir.Program(entries=set(plan["entries"]), jt=plan["jt"], callee=callee_fn(plt), plt=plt,
                            no_call=plan["no_call"], symbol=symbol, tail_symbol=tail_symbol)
    m = emit_llvm.Module()
    extra = []
    for e in names:
        pcs, words = plan["bodies"][e]
        w = dict(zip(pcs, words))
        fn = build_ir.build(e, list(pcs), w.__getitem__, prog)
        if verify and verified(plan, e):
            fn.name = "body_%08x" % e
            s = plan["summaries"][e]
            extra.append(VERIFY_WRAPPER % {"a": e, "inputs": s[0] & GPR, "may": s[1] & GPR, "res": s[4] & GPR,
                                           "pres": s[3] & GPR, "flags": 1 if s[5] else 0})
        m.add(fn)
        extra.append("define void @f_%08x(ptr %%ctx) {\n  musttail call void @g_%08x(ptr %%ctx)\n  ret void\n}" % (e, e))
    if verify:
        extra.append("declare void @lift2_check_enter(ptr, ptr, ptr)\ndeclare void @lift2_check_exit(ptr, ptr, ptr)")
    return m.text() + "\n".join(extra) + "\n"


_PLAN = {}


def _plan(path):
    if path not in _PLAN:
        with open(path, "rb") as f:
            _PLAN[path] = pickle.load(f)
    return _PLAN[path]


def prepare(spec, gen, out, srcs, jobs, opt, hot_opt, march_flags, cache):
    """(the code files to compile, the lift 2 objects to link, a report line); see the module's description"""
    t0 = time.time()
    clang = llvm.check("clang")
    plan_path = os.path.join(os.path.dirname(gen), "lift2_plan.pickle")
    plan = _plan(plan_path)
    edited, hot = edited_functions(gen)
    chosen = set(plan["bodies"]) - edited
    if spec != "all-safe":
        want = {int(l.split()[0], 16) for l in open(spec) if l.strip() and not l.startswith("#")}
        chosen &= want
    with open(os.path.join(out, "lift2_functions.txt"), "w") as f:
        f.write("".join("%08x\n" % e for e in sorted(chosen)))
    # the present lifter's files, with the chosen definitions out of the way
    src_dir = os.path.join(out, "lift2_src")
    os.makedirs(src_dir, exist_ok=True)
    new_srcs = [thunked_source(s, chosen, src_dir) if os.path.basename(s).startswith("code_") else s for s in srcs]
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
    with concurrent.futures.ProcessPoolExecutor(min(jobs, 16)) as ex:
        texts = list(ex.map(lift_part, [(plan_path, names, chosen, hot) for _, names in parts]))
    t_lift = time.time() - t0
    cjobs = []
    for k, ((tag, names), text) in enumerate(zip(parts, texts)):
        fl = march_flags + ["-" + (hot_opt if tag == "h" else opt), "-g0", "-Wno-everything", "-ffunction-sections",
                            "-mllvm", "-rotation-max-header-size=0"]
        cjobs.append((clang, text, os.path.join(obj_dir, "lift2_%s%03d.o" % (tag, k)), fl, cache))
    objs, failed, t1 = [], [], time.time()
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for obj, secs, err in ex.map(compile_part, cjobs):
            objs.append(obj)
            if err:
                failed.append((obj, err))
    n_insn = sum(len(plan["bodies"][e][0]) for e in chosen)
    total = sum(plan["insns"].values())
    n_hot = len(chosen & hot)
    line = ("lift2: %d functions (%d instructions, %.1f %% of %d; %d of %d hot functions) in %d partitions; "
            "lifted in %.0fs, compiled in %.0fs; %d kept on the present lifter for host edits" % (
                len(chosen), n_insn, 100.0 * n_insn / max(total, 1), total, n_hot, len(hot), len(parts), t_lift,
                time.time() - t1, len(set(plan["bodies"]) & edited)))
    if failed:
        raise SystemExit("lift2: %d partitions failed to compile:\n%s" % (len(failed), failed[0][1]))
    return new_srcs, objs, line
