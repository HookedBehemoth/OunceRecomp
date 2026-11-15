"""A lifted program with no present lifter in the path (milestone 19, step 4): lift 2's discovery, IR and LLVM emission,
its runtime tables (nx2/lift2/tables.py) and the generated runtime (nx2/rtgen.py: a64.h, a64_rt.c), compiled and run.
The guest calls through a64_icall and a64_lookup, a jump table, a PAC return, a tail call and a PLT stub; nothing of
third_party/a64-kit/lift is imported (checked). No dump.

python -I -B tests/test_lift2_nolift1.py
"""
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from lift2 import build_ir, discover, emit_llvm, llvm, tables  # noqa: E402
import rtgen  # noqa: E402
import test_lift2_discover as T  # noqa: E402  (the synthetic image)

HARNESS = r"""
#include "a64.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
void a64_poll_cur(void) {}
int main(void) {
    static const unsigned char mem[] = { %(mem)s };
    if (mmap(A64_PTR(%(base)#x), 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
             -1, 0) == MAP_FAILED) return 2;
    if (mmap(A64_PTR(0x90000000), 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
             -1, 0) == MAP_FAILED) return 3;
    memcpy(A64_PTR(%(base)#x), mem, sizeof mem);
    static GuestContext ctx;
    for (int k = 0; k < 4; k++) {
        GuestContext *c = &ctx;
        memset(c, 0, sizeof *c);
        c->x[31] = 0x90008000; c->x[0] = k; c->x[1] = 40;
        a64_icall(c, %(entry)#x);   /* (f: a call of g, then RETAB) */
        printf("f %%d %%llu\n", k, (unsigned long long)c->x[0]);
        memset(c, 0, sizeof *c);
        c->x[31] = 0x90008000; c->x[0] = k; c->x[1] = 40;
        a64_icall(c, %(stub)#x);    /* (the PLT stub: g through the GOT) */
        printf("s %%d %%llu\n", k, (unsigned long long)c->x[0]);
    }
    return 0;
}
"""


class NoLift1(unittest.TestCase):
    def test_runs_without_the_present_lifter(self):
        img = T.Image()
        D = discover.Discovery(img)
        D.discover()
        prog = build_ir.Program(entries=D.entries, jt=D.jt, callee=D.callee, plt=D.plt,
                                symbol=lambda e: "g_%08x" % e)
        m = emit_llvm.Module()
        extra = []
        for e in sorted(D.funcs):
            fn = build_ir.build(e, sorted(D.funcs[e][0]), img.word, prog)
            m.add(fn)
            extra.append("define void @f_%08x(ptr %%ctx) {\n  musttail call void @g_%08x(ptr %%ctx)\n  ret void\n}"
                         % (e, e))
        self.assertFalse([n for n in sys.modules if n in ("a64_lift", "a64emit", "a64_rtgen", "insn", "lift_fixes")])
        with tempfile.TemporaryDirectory() as d:
            rt = rtgen.write(os.path.join(d, "rt64"))
            tables.write(d, D.funcs, {})
            with open(os.path.join(d, "code.ll"), "w") as f:
                f.write(m.text() + "\n".join(extra) + "\n")
            with open(os.path.join(d, "main.c"), "w") as f:
                f.write(HARNESS % {"mem": ",".join(str(b) for b in img.mem[:0x1010]), "base": T.BASE,
                                   "entry": T.BASE, "stub": T.BASE + 0x48})
            exe = os.path.join(d, "prog")
            cc = [llvm.check("clang"), "-O1", "-w", "-march=x86-64-v3", "-I" + rt, "-I" + d]
            subprocess.run(cc + [os.path.join(d, "main.c"), os.path.join(rt, "a64_rt.c"),
                                 os.path.join(d, "a64_table.c"), os.path.join(d, "code.ll"), "-o", exe], check=True)
            out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()
        got = {(out[i], int(out[i + 1])): int(out[i + 2]) for i in range(0, len(out), 3)}
        # (g: index 0 and 2 return 0, 1 returns 1, above 2 the bound's default returns 1)
        for k, want in ((0, 0), (1, 1), (2, 0), (3, 1)):
            self.assertEqual(got[("f", k)], want, "f(%d)" % k)
            self.assertEqual(got[("s", k)], want, "stub(%d)" % k)


if __name__ == "__main__":
    unittest.main()
