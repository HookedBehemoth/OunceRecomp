"""Context stores no reader can read (nx2/lift2/build_ir.py omit_stores), on hand-built IR, without the dump.

A dead temporary before a BL to a callee whose input mask lacks it is not stored; the same before a BLR is; an
argument the callee reads is; a register the caller reloads after the call is, unless the callee writes it on every
path; at a return only the results, the callee-saved registers and SP stay (everything without a summary of its own).
The verification build puts the site poison before the BL (host/lift2_verify.c reports a callee that reads it:
tests/test_lift2_verify.py), and the module compiles.

python -I -B tests/test_lift2_omit.py
"""
import os
import re
import subprocess
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
from lift2 import bounds, build_ir, emit_llvm, ir, llvm, summary  # noqa: E402

CALLEE, ME = 0x80001000, 0x80000000
B = summary.BIT


def mask(*regs):
    return sum(B[r] for r in regs)


def build(steps, reads, verify=False):
    """a one-block function of ME: steps ("set", reg) / ("bl",) / ("blr",) / ("get", reg) / ("ctx", regs or None),
    then a return; reads: the call-boundary records ({address: (inputs, must-defs, results)}, may-writes everything;
    a missing one bounds.unknown)"""
    def bound(e):
        r = reads.get(e)
        if r is None:
            return bounds.unknown("unknown", "g_%08x" % e)
        return bounds.Bound("lifted", r[0], summary.ALL, 0, r[1], r[2], False, "g_%08x" % e, "g_%08x" % e)
    fn = ir.Function(ME)
    b = ir.Builder(fn)
    blk = fn.block(ME)
    b.at(blk)
    sink = []
    for k, s in enumerate(steps):
        if s[0] == "set":
            b.set(s[1], b.const(k + 1, ir.VAR_TYPE[s[1]]))
        elif s[0] == "bl":
            b.set("x30", b.const(ME + 4 * k + 4))
            b.op("gcall", "void", sym="g_%08x" % CALLEE, callee=CALLEE, pc=ME + 4 * k)
            b.after_call()
        elif s[0] == "blr":
            b.set("x30", b.const(ME + 4 * k + 4))
            b.op("icall", "void", b.const(CALLEE))
            b.after_call()
        elif s[0] == "get":
            sink.append(b.get(s[1]))
        elif s[0] == "ctx":   # (a call taking the context; regs: the register fields it reads, as a template says)
            b.op("call", "void", fn="t_ctx", ctx=True, **({} if s[1] is None else {"regs": s[1]}))
    for v in sink:   # (the values read: stored to guest memory, so that they are used)
        b.op("store", "void", b.const(0x1000), v)
    blk.term = ("ret",)
    ir.finish(fn)
    prog = build_ir.Program(bound=bound, omit=True, verify=verify)
    build_ir.omit_stores(fn, prog)
    return fn


def stored(fn):
    """the registers written through, in order"""
    return [v.attrs["var"] for blk in fn.blocks.values() for v in blk.ops if v.op == "writeback"]


class Omit(unittest.TestCase):
    def test_dead_temporary_at_bl(self):
        # (x9 written before a call of a callee reading x0 only, written again after it)
        fn = build([("set", "x9"), ("set", "x0"), ("bl",), ("set", "x9")], {CALLEE: (mask("x0"), 0, mask("x0"))})
        self.assertEqual(stored(fn).count("x9"), 1)   # (the second; the function's own return reads everything)
        self.assertIn("x0", stored(fn))               # (an argument the callee reads)

    def test_blr_reads_everything(self):
        fn = build([("set", "x9"), ("set", "x0"), ("blr",), ("set", "x9")], {CALLEE: (mask("x0"), 0, mask("x0"))})
        self.assertEqual(stored(fn).count("x9"), 2)

    def test_unknown_callee_reads_everything(self):
        fn = build([("set", "x9"), ("bl",), ("set", "x9")], {})
        self.assertEqual(stored(fn).count("x9"), 2)

    def test_context_calls(self):
        # (a call taking the context reads every register unless its template says which: the runtime's helpers)
        fn = build([("set", "x9"), ("ctx", None), ("set", "x9")], {ME: (0, 0, 0)})
        self.assertEqual(stored(fn).count("x9"), 1)
        fn = build([("set", "x9"), ("ctx", 0), ("set", "x9")], {ME: (0, 0, 0)})
        self.assertEqual(stored(fn).count("x9"), 0)
        # (every template's call taking the context says what it reads: a new one without it costs speed only, but
        # each of these reads none of the context's registers, rt/a64_rt.c recomp_*, host/hos_fp.c a64_fp*)
        for name in ("sem_int.py", "sem_fp.py", "sem_neon.py", "sem_crypto.py"):
            src = open(os.path.join(ROOT, "nx2", "lift2", name)).read()
            for line in re.findall(r"^.*ctx=True.*$", src, re.M):
                self.assertIn("regs=0", line, (name, line))

    def test_reloaded_after_the_call(self):
        # (x2 is not the callee's input, but the caller loads it after the call: the callee may leave it unchanged)
        fn = build([("set", "x2"), ("bl",), ("get", "x2"), ("set", "x2")], {CALLEE: (mask("x0"), 0, mask("x0"))})
        self.assertEqual(stored(fn).count("x2"), 2)
        # (unless the callee writes x2 on every path)
        fn = build([("set", "x2"), ("bl",), ("get", "x2"), ("set", "x2")], {CALLEE: (mask("x0"), mask("x2"), mask("x2"))})
        self.assertEqual(stored(fn).count("x2"), 1)

    def test_x30_the_callee_does_not_read(self):
        fn = build([("bl",), ("set", "x30")], {CALLEE: (mask("x0"), 0, 0)})
        self.assertEqual(stored(fn).count("x30"), 1)
        fn = build([("bl",), ("set", "x30")], {CALLEE: (mask("x0", "x30"), 0, 0)})   # (a callee saving x30)
        self.assertEqual(stored(fn).count("x30"), 2)

    def test_return(self):
        # (with a summary of its own: results x0, the callee-saved registers and SP stay; a temporary goes)
        reads = {ME: (0, 0, mask("x0"))}
        fn = build([("set", "x0"), ("set", "x5"), ("set", "x19"), ("set", "sp"), ("set", "v8")], reads)
        self.assertEqual(stored(fn), ["x0", "x19", "sp", "v8"])
        fn = build([("set", "x0"), ("set", "x5"), ("set", "x19")], {})
        self.assertEqual(stored(fn), ["x0", "x5", "x19"])
        # (overwritten before any reader)
        fn = build([("set", "x19"), ("set", "x19")], reads)
        self.assertEqual(stored(fn), ["x19"])

    def test_verification_poison(self):
        reads = {CALLEE: (mask("x0"), 0, mask("x0"))}
        fn = build([("set", "x9"), ("set", "x0"), ("bl",), ("set", "x9")], reads, verify=True)
        ops = [v for blk in fn.blocks.values() for v in blk.ops]
        k = next(i for i, v in enumerate(ops) if v.op == "gcall")
        self.assertEqual(ops[k - 1].op, "sitepoison")
        m = ops[k - 1].attrs["mask"]
        self.assertTrue(m & B["x9"] and not m & B["x0"] and not m & B["sp"])
        self.assertFalse(m & B["x19"])   # (live at the return)
        mod = emit_llvm.Module()
        mod.add(fn)
        text = mod.text()
        pv = (build_ir.SITE_POISON << 48 | 9 << 40 | ME + 8) - (1 << 64)
        self.assertIn("store i64 %d, ptr %%cx.x9" % pv, text)
        r = subprocess.run([llvm.check("clang"), "-O1", "-x", "ir", "-", "-c", "-o", os.devnull], input=text,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(re.search(r"store i64 1, ptr %cx.x9", text))   # (the dead store itself is gone)


if __name__ == "__main__":
    unittest.main()
