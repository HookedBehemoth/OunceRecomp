"""Call boundaries from the whole-program summaries (nx2/lift2/game.py call_bounds, build_ir.py BL), without the dump.

Small functions: c1 keeps x19 and x29 in a frame and calls `keep` (which writes no callee-saved register); c2 the same
calling `clob` (writes x19: its summary is demoted); c3 calls `keep` through BLR; c4 calls `die` (BRK: noreturn) when
x0 is not 0. Checks on the IR: after c1's call no load of x19, x29 or SP from the context; after c2's and c3's the
reload of x19; with `keep` a setjmp owner, or `clob` entry-hooked, the reload too; with `keep` and `clob` HLE'd
(nx2/lift2/host_effects.py: clean unless declared) or `keep` entry-hooked, no reload; with `keep` declared clobbering,
or its hook declared to call `clob`, the reload; a declared name that does not exist stops the build. After c4's call
of `die` a trap and `unreachable`. The program, lifted, compiled and run, returns what the guest code returns.

python -I -B tests/test_lift2_calls.py
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
from lift2 import build_ir, emit_llvm, game, host_effects, llvm, summary, tables  # noqa: E402
import rtgen  # noqa: E402

BASE = 0x80000000
# (clang --target=aarch64-linux-gnu, ld.lld -Ttext=0x80000000)
#  c1 0x00: stp x29, x30, [sp, #-32]!; stp x19, x20, [sp, #16]; mov x29, sp; mov x19, x0; bl keep; add x0, x0, x19;
#           mov x2, sp; sub x1, x29, x2; add x0, x0, x1; ldp x19, x20, [sp, #16]; ldp x29, x30, [sp], #32; ret
#  c2 0x30: the same frame; mov x19, x0; bl clob; add x0, x0, x19; epilogue
#  c3 0x54: the same frame; mov x19, x0; adr x8, keep; blr x8; add x0, x0, x19; epilogue
#  c4 0x7c: stp x29, x30, [sp, #-16]!; mov x29, sp; cbz x0, 1f; bl die; 1: mov x0, #7; ldp x29, x30, [sp], #16; ret
#  keep 0x98: add x0, x0, #1; ret      clob 0xa0: mov x19, #5; add x0, x0, #1; ret      die 0xac: brk #1
CODE = [
    0xa9be7bfd, 0xa90153f3, 0x910003fd, 0xaa0003f3, 0x94000022, 0x8b130000, 0x910003e2, 0xcb0203a1,
    0x8b010000, 0xa94153f3, 0xa8c27bfd, 0xd65f03c0, 0xa9be7bfd, 0xa90153f3, 0x910003fd, 0xaa0003f3,
    0x94000018, 0x8b130000, 0xa94153f3, 0xa8c27bfd, 0xd65f03c0, 0xa9be7bfd, 0xa90153f3, 0x910003fd,
    0xaa0003f3, 0x100001a8, 0xd63f0100, 0x8b130000, 0xa94153f3, 0xa8c27bfd, 0xd65f03c0, 0xa9bf7bfd,
    0x910003fd, 0xb4000040, 0x94000009, 0xd28000e0, 0xa8c17bfd, 0xd65f03c0, 0x91000400, 0xd65f03c0,
    0xd28000b3, 0x91000400, 0xd65f03c0, 0xd4200020]
F = {"c1": 0x00, "c2": 0x30, "c3": 0x54, "c4": 0x7c, "keep": 0x98, "clob": 0xa0, "die": 0xac}
A = {k: BASE + v for k, v in F.items()}


def word(pc):
    return CODE[(pc - BASE) // 4]


def lift(hooks=(), sj=(), hle=None, calls=None):
    """{name: IR function}, the Program and the bodies, with the summaries as a game build has them; hooks: entry-hooked
    functions, hle: {address: symbol} of HLE'd ones, calls: {entry hook name: guest functions it calls}"""
    entries = set(A.values())
    funcs = summary.walk(entries, word)
    prog = build_ir.Program(entries=entries)
    sums, _, _ = summary.summarise(funcs, prog, set(), word)
    hle = hle or {}
    plan = {"summaries": {e: summary.pack(s) for e, s in sums.items()}, "plt": {}, "hooks": sorted(hle),
            "sj_rets": {e: (0, 0, 0, 0) for e in sj}, "after_c": {},
            "bodies": {e: (sorted(pcs), [word(p) for p in sorted(pcs)]) for e, pcs in funcs.items()}}
    entry = {e: "entry_0x%08x" % e for e in hooks}
    saved = dict(host_effects.CALLS)
    host_effects.CALLS.clear()
    host_effects.CALLS.update(calls or {})
    try:
        clean = game.host_clean(plan, entry, hle)
    finally:
        host_effects.CALLS.clear()
        host_effects.CALLS.update(saved)
    keeps, noreturn = game.call_bounds(plan, game.host_edited(plan, entry), clean)
    prog = build_ir.Program(entries=entries, symbol=lambda e: "g_%08x" % e, keeps=keeps, noreturn=noreturn)
    fns = {n: build_ir.build(a, sorted(funcs[a]), word, prog) for n, a in A.items()}
    return fns, funcs


def loads_after_call(fn):
    """the registers the blocks with a call load from the context after it"""
    out = set()
    for blk in fn.blocks.values():
        seen = False
        for v in blk.ops:
            if v.op in ("gcall", "icall"):
                seen = True
            elif seen and v.op == "regload":
                out.add(v.attrs["var"])
    # (and the blocks after them: a successor's parameter of a called block is a load in it, ir.finish)
    return out


class Calls(unittest.TestCase):
    def test_kept(self):
        fns, _ = lift()
        self.assertFalse(loads_after_call(fns["c1"]) & {"x19", "x29", "sp"}, loads_after_call(fns["c1"]))
        self.assertIn("x19", loads_after_call(fns["c2"]))       # (clob writes x19)
        self.assertIn("x19", loads_after_call(fns["c3"]))       # (BLR)

    def test_host_edited(self):
        fns, _ = lift(sj=[A["keep"]])
        self.assertIn("x19", loads_after_call(fns["c1"]))      # (a setjmp owner)
        fns, _ = lift(hooks=[A["clob"]])
        self.assertIn("x19", loads_after_call(fns["c2"]))      # (an entry hook on a body that writes x19)

    def test_host_clean(self):
        # (an HLE'd callee, declared clean by default: x19, x29 and SP stay; even in place of clob, whose guest body
        # the host replaces)
        fns, _ = lift(hle={A["keep"]: "nn_keep", A["clob"]: "nn_clob"})
        for f in ("c1", "c2"):
            self.assertFalse(loads_after_call(fns[f]) & {"x19", "x29", "sp"}, (f, loads_after_call(fns[f])))
        # (an entry hook on a body whose summary preserves them)
        fns, _ = lift(hooks=[A["keep"]])
        self.assertFalse(loads_after_call(fns["c1"]) & {"x19", "x29", "sp"}, loads_after_call(fns["c1"]))
        # (an entry hook that calls guest code writing x19: host_effects.CALLS)
        fns, _ = lift(hooks=[A["keep"]], calls={"entry_0x%08x" % A["keep"]: (A["clob"],)})
        self.assertIn("x19", loads_after_call(fns["c1"]))

    def test_declared_clobbering(self):
        host_effects.CLOBBERS["nn_keep"] = "test"
        try:
            fns, _ = lift(hle={A["keep"]: "nn_keep"})
        finally:
            del host_effects.CLOBBERS["nn_keep"]
        self.assertIn("x19", loads_after_call(fns["c1"]))

    def test_undeclared_name(self):
        names = {e: "nn_" + n for n, e in A.items()}
        hooks = {A["keep"]: "entry_0x%08x" % A["keep"]}
        saved = (dict(host_effects.CLOBBERS), dict(host_effects.CALLS))
        try:
            host_effects.CLOBBERS.clear()
            host_effects.CALLS.clear()
            host_effects.CLOBBERS["nn_clob"] = "test"
            host_effects.CALLS["entry_0x%08x" % A["keep"]] = (A["clob"],)
            host_effects.check(names, hooks)   # (both exist)
            host_effects.CLOBBERS["nn_none"] = "test"
            with self.assertRaises(SystemExit):
                host_effects.check(names, hooks)
            del host_effects.CLOBBERS["nn_none"]
            host_effects.CALLS["entry_0x00000000"] = ()
            with self.assertRaises(SystemExit):
                host_effects.check(names, hooks)
        finally:
            for d, v in zip((host_effects.CLOBBERS, host_effects.CALLS), saved):
                d.clear()
                d.update(v)

    def test_noreturn(self):
        fns, _ = lift()
        self.assertTrue(any(b.term[0] == "noreturn" for b in fns["c4"].blocks.values()))
        m = emit_llvm.Module()
        m.add(fns["c4"])
        text = m.text()
        self.assertRegex(text, r"call void @a64_trap\(ptr %%ctx, i64 %d, i32 %d\)\n  unreachable" % (A["c4"] + 0xc,
                                                                                                   CODE[0x88 // 4]))

    def test_runs(self):
        fns, funcs = lift()
        m = emit_llvm.Module()
        extra = []
        for n, fn in fns.items():
            m.add(fn)
            extra.append("define void @f_%08x(ptr %%ctx) {\n  musttail call void @g_%08x(ptr %%ctx)\n  ret void\n}"
                         % (A[n], A[n]))
        harness = r"""
#include "a64.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
int main(void) {
    static const unsigned int code[] = { %(code)s };
    static const unsigned long long fn[] = { %(fns)s };
    if (mmap(A64_PTR(%(base)#x), 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
             -1, 0) == MAP_FAILED) return 2;
    if (mmap(A64_PTR(0x90000000), 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
             -1, 0) == MAP_FAILED) return 3;
    memcpy(A64_PTR(%(base)#x), code, sizeof code);
    static GuestContext ctx;
    for (int f = 0; f < (int)(sizeof fn / sizeof fn[0]); f++)
        for (int k = 0; k < 3; k++) {
            memset(&ctx, 0, sizeof ctx);
            ctx.x[31] = 0x90008000; ctx.x[0] = f == 3 ? 0 : k; ctx.x[19] = 100;
            a64_icall(&ctx, fn[f]);
            printf("%%d %%d %%llu %%llu %%llu\n", f, k, (unsigned long long)ctx.x[0], (unsigned long long)ctx.x[19],
                   (unsigned long long)ctx.x[31]);
        }
    return 0;
}
"""
        names = ["c1", "c2", "c3", "c4"]
        with tempfile.TemporaryDirectory() as d:
            rt = rtgen.write(os.path.join(d, "rt64"))
            tables.write(d, {A[n]: (funcs[A[n]], None) for n in A}, {})
            with open(os.path.join(d, "code.ll"), "w") as f:
                f.write(m.text() + "\n".join(extra) + "\n")
            with open(os.path.join(d, "main.c"), "w") as f:
                f.write(harness % {"code": ",".join("%#x" % w for w in CODE), "base": BASE,
                                   "fns": ", ".join("%#x" % A[n] for n in names)})
            exe = os.path.join(d, "prog")
            cc = [llvm.check("clang"), "-O1", "-w", "-march=x86-64-v3", "-I" + rt, "-I" + d]
            subprocess.run(cc + [os.path.join(d, "main.c"), os.path.join(rt, "a64_rt.c"),
                                 os.path.join(d, "a64_table.c"), os.path.join(d, "code.ll"), "-o", exe], check=True)
            out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()
        got = {(int(out[i]), int(out[i + 1])): tuple(int(x) for x in out[i + 2:i + 5]) for i in range(0, len(out), 5)}
        for k in range(3):
            # (x0; x19 back at its entry value 100; SP back at its entry value)
            self.assertEqual(got[(0, k)], (2 * k + 1, 100, 0x90008000), "c1(%d)" % k)
            self.assertEqual(got[(1, k)], (k + 6, 100, 0x90008000), "c2(%d)" % k)
            self.assertEqual(got[(2, k)], (2 * k + 1, 100, 0x90008000), "c3(%d)" % k)
            self.assertEqual(got[(3, k)], (7, 100, 0x90008000), "c4(0)")


if __name__ == "__main__":
    unittest.main()
