"""The lifter's output for given functions, to read: the guest code, the IR, the LLVM IR, and optionally the optimised
IR and the x86.

    python -I nx2/lift2/dump.py <guest address or label>... [--tree] [--opt O1|O2] [--asm] [-o <dir>]

For each function, in <dir>/<address>/ (default work/lift2/dump/: game-derived, never committed):
  guest.s       the guest instructions as llvm-mc prints them (asm.py)
  ir.txt        the IR (blocks, parameters, operations)
  lift2.ll      the LLVM IR as emitted (the generic entry, g_<address>)
  lift2.opt.ll  with --opt: after clang's optimiser at that level
  lift2.s       with --asm: the x86
--tree adds every function the given ones call or tail-call directly. Labels come from nx2/oot_labels.txt. The
function's summary (inputs, clobbers, preserved, demotion) is printed when <work>/sa/plan.pickle
holds one.
"""
import argparse
import os
import pickle
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lift2 import enc  # noqa: E402  (first: its tables import asm)
from lift2 import asm, build_ir, emit_llvm, game, llvm, summary  # noqa: E402

IR_FLAGS = ["-march=x86-64-v3", "-g0", "-Wno-everything", "-mllvm", "-rotation-max-header-size=0"]   # (+ game.DSE_FLAGS)


def labels():
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "oot_labels.txt")
    out = {}
    for line in open(path):
        p = line.split("#")[0].split()
        if len(p) >= 2:
            out[p[1]] = int(p[0], 16)
    return out


def ir_text(fn):
    """the lifter's IR as text: a block per guest address, its parameters, operations and terminator"""
    def r(v):
        if v is None:
            return "?"
        return repr(v) if v.op == "const" else "%%%d" % v.id
    out = ["function %s (entry %#x)" % (fn.name, fn.entry)]
    for blk in fn.blocks.values():
        params = ", ".join("%s=%%%d" % (v, p.id) for v, p in blk.params.items())
        out.append("%s(%s):  preds %s" % (blk.name, params, " ".join(p.name for p in blk.preds)))
        for v in blk.ops:
            args = ", ".join(r(a) if hasattr(a, "op") else str(a) for a in v.args)
            attrs = " " + " ".join("%s=%s" % kv for kv in v.attrs.items()) if v.attrs else ""
            lhs = "" if v.ty == "void" else "%%%d:%s = " % (v.id, v.ty)
            out.append("    %s%s %s%s" % (lhs, v.op, args, attrs))
        t = blk.term
        parts = []
        for x in t[1:]:
            if hasattr(x, "name") and hasattr(x, "ops"):
                parts.append(x.name)
            elif hasattr(x, "op"):
                parts.append(r(x))
            elif isinstance(x, list):
                parts.append("[" + ", ".join("%s: %s" % (k, b.name) for k, b in x) + "]")
            else:
                parts.append(str(x))
        out.append("    -> %s %s" % (t[0], " ".join(parts)))
    return "\n".join(out) + "\n"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("functions", nargs="+")
    p.add_argument("--tree", action="store_true")
    p.add_argument("--opt", choices=("O1", "O2"))
    p.add_argument("--asm", action="store_true")
    p.add_argument("-o", dest="out", default=None)
    a = p.parse_args()
    import kit
    import image as eden_image
    from lift2 import reference as TL
    lab = labels()
    want = []
    for f in a.functions:
        want.append(lab[f] if f in lab else int(f.replace("f_", ""), 16))
    out = a.out or os.path.join(kit.WORK, "lift2", "dump")
    clang = llvm.check("clang")
    img = eden_image.EdenImage(kit.need_exefs())
    P = TL.Picker(img)
    L = P.L
    funcs = []
    todo = list(want)
    while todo:
        e = todo.pop(0)
        if e in funcs or e in img.hooks:
            continue
        P.liftable_body(e, True)
        funcs.append(e)
        if a.tree and e in want:
            for k, _ in L.funcs[e][0].values():
                if k[0] in ("bl", "b") and L.callee(k[1]) in L.entries and L.callee(k[1]) not in L.funcs[e][0]:
                    todo.append(L.callee(k[1]))
    plan = None
    plan_path = os.path.join(kit.WORK, "sa", "plan.pickle")
    if os.path.exists(plan_path):
        with open(plan_path, "rb") as f:
            plan = pickle.load(f)
    L.index_tables()   # (over the tables of the functions walked so far)
    bound = None
    if plan and plan.get("summaries"):   # (the call boundaries as the build makes them)
        from lift2 import bounds, host_effects, patch_scan
        gen = os.path.join(kit.WORK, "sa", "gen")
        bound = bounds.call_bounds(plan, patch_scan.entry_hooks(gen)[0], host_effects.hle_names(gen))
    prog = build_ir.Program(entries=L.entries, jt=L.jt, callee=L.callee, plt=L.plt, jt_index=L.jt_index, bound=bound,
                            omit=bound is not None)
    names = {v: k for k, v in lab.items()}
    for e in funcs:
        d = os.path.join(out, "%08x" % e)
        os.makedirs(d, exist_ok=True)
        pcs = sorted(L.funcs[e][0])
        with open(os.path.join(d, "guest.s"), "w") as f:
            for pc in pcs:
                i = enc.decode(img.word(pc))
                f.write("%08x  %08x  %s\n" % (pc, img.word(pc), asm.render(i, pc) if i else "(not decoded)"))
        head = "f_%08x %s: %d instructions" % (e, names.get(e, ""), len(pcs))
        why = build_ir.scope(e, pcs, img.word, prog)
        if plan and e in plan.get("summaries", {}):
            s = plan["summaries"][e]
            g = lambda m: ",".join(summary.mask_names(m & 0xFFFFFFFF)) or "-"
            head += "\n  summary: inputs %s | clobbers %s | preserved %s | results %s | %s%s" % (
                g(s[0]), g(s[1]), g(s[3]), g(s[4]), "returns" if s[5] else "noreturn",
                (" | demoted: " + ", ".join(s[6])) if s[6] else " | full")
        if why is not None:
            print(head + "\n  the lifter cannot take it: %s (guest.s written)" % why)
            continue
        fn = build_ir.build(e, pcs, img.word, prog)
        with open(os.path.join(d, "ir.txt"), "w") as f:
            f.write(ir_text(fn))
        m = emit_llvm.Module()
        m.add(fn)
        ll = os.path.join(d, "lift2.ll")
        with open(ll, "w") as f:
            f.write(m.text())
        text = m.text()
        level = a.opt or "O1"
        pre = game.opt_argv(clang, level, IR_FLAGS[:1])   # (as the build: -O1 through opt with DSE)
        fl = ["-" + level] + IR_FLAGS + game.DSE_FLAGS
        if a.opt:
            r = game.compile_ir(clang, text, os.path.join(d, "lift2.opt.ll"), fl, pre, ("-S", "-emit-llvm"))
            if r.returncode:
                raise SystemExit(r.stderr.decode(errors="replace"))
        if a.asm:
            obj = os.path.join(d, "lift2.o")
            r = game.compile_ir(clang, text, obj, fl, pre)
            if r.returncode:
                raise SystemExit(r.stderr.decode(errors="replace"))
            with open(os.path.join(d, "lift2.s"), "w") as f:
                f.write(subprocess.run(["objdump", "-d", "--no-show-raw-insn", "-M", "intel", obj],
                                       capture_output=True, text=True).stdout)
            os.remove(obj)
        print(head + "\n  written to %s" % d)


if __name__ == "__main__":
    main()
