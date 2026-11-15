"""The patch tools (nx2/patch.py) on a small plan of their own: site (the paths into an instruction, its liveness, the
declaration to paste and the build's verdict on it), check (every error the build stops on, with file:line), report and
the summaries' difference; summary.live_out against liveness. No game is used.

python -I -B tests/test_patch_tools.py
"""
import io
import os
import sys
import tempfile
import unittest
from array import array

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
from lift2 import build_ir, patch_scan, probes, summary  # noqa: E402
import gamedesc  # noqa: E402
import patch  # noqa: E402
gamedesc.use(os.path.join(ROOT, "games", "oot"))   # (the host's declarations patch_scan.use sets aside)

F, C = 0x80001000, 0x80002000
WORDS = {
    F: [0xb4000040,                                    # cbz x0, +8
        0x8b020020,                                    # add x0, x1, x2
        0x94000000 | ((C - (F + 8)) // 4 & 0x3FFFFFF),  # bl C
        0xd65f03c0],                                   # ret
    C: [0x8b010000,                                    # add x0, x0, x1
        0xd65f03c0],                                   # ret
}
DECLS = '''PATCH_AT(0x80001004, word(0x8b020020), after, x0, t_after, x0, x1) { return x0 + x1; }
PATCH_AT(0x80001008, word(0x940003fe), before, none, t_before, x0) {}
PATCH_AT(0x80001004, word(0x8b020021), after, none, t_word, x1) {}
PATCH_AT(0x80003000, word(0xd503201f), after, none, t_nowhere, x1) {}
PATCH_AT(0x80001004, word(0x8b020020), after, x19, t_x19, x1) { return x1; }
'''


def plan_of(ps):
    bodies = {e: (array("Q", [e + 4 * k for k in range(len(w))]), array("I", w)) for e, w in WORDS.items()}
    word = lambda pc: WORDS[pc & ~0xFFF][(pc & 0xFFF) // 4]
    old = patch_scan.use(ps)
    try:
        probes._SET = None
        summary._ROWS = None
        sums, _, _ = summary.summarise({e: list(b[0]) for e, b in bodies.items()},
                                       build_ir.Program(entries=set(WORDS)), set(), word)
    finally:
        patch_scan.use(old)
        probes._SET = None
        summary._ROWS = None
    return {"plt": {}, "entries": sorted(WORDS), "jt": {}, "bodies": bodies, "hle": {}, "hooks": [],
            "summaries": {e: summary.pack(s) for e, s in sums.items()}}


class Tools(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.gen = os.path.join(self.tmp.name, "gen")
        self.ps = patch_scan.scan_text(DECLS, "host/t.c")
        self.old = patch_scan.use(self.ps)
        probes._SET = None
        summary._ROWS = None
        self.P = patch.Plan(plan_of(patch_scan.scan_text("")), self.gen, {F: "t_function"}, self.ps)

    def tearDown(self):
        patch_scan.use(self.old)
        probes._SET = None
        summary._ROWS = None
        self.tmp.cleanup()

    def run_site(self, pc):
        out = []
        patch.site(self.P, pc, out.append)
        return "\n".join(out)

    def test_site(self):
        t = self.run_site(0x80001008)
        self.assertIn("in f_80001000 (t_function)", t)
        self.assertIn("a branch at 0x80001000", t)
        self.assertIn("(to f_80002000)", t)
        self.assertIn("instead, x0, my_fn, x0, x1)", t)   # (the callee's reads and result)
        self.assertIn("host/t.c:2 PATCH_AT 0x80001008 before t_before  <- this instruction", t)
        t = self.run_site(0x80001004)
        self.assertIn("paths into it other than from the instruction before: none", t)
        self.assertIn("live in:  x1-x2", t)   # (the add reads x1, x2; the row t_after its x0)
        self.assertIn("live out: x0-x1", t)
        self.assertIn("PATCH_AT(0x80001004, word(0x8b020020), after, x0, my_fn, x0, x1, x2) {", t)
        self.assertIn("generates: uint64_t my_fn(uint64_t x0, uint64_t x1, uint64_t x2)", t)
        self.assertIn("the build's checks: it passes", t)
        t = self.run_site(F)   # (at an entry: the function replaced, its body hash, the argument registers it reads)
        self.assertIn("PATCH_FUNC(0x80001000, word(0xb4000040), body(%#018x), aapcs, x0, my_fn, orig, x0, x1, x2) {"
                      % patch_scan.body_hash(WORDS[F]), t)

    def test_check(self):
        errs = ["%s:%d: %s" % (w[0], w[1], m) for w, m in patch.errors(self.P)]
        self.assertEqual(len(errs), 4, errs)
        self.assertTrue(errs[0].startswith("host/t.c:2: ") and "a branch target" in errs[0], errs[0])
        self.assertTrue(errs[1].startswith("host/t.c:3: ") and "another game version" in errs[1], errs[1])
        self.assertTrue(errs[2].startswith("host/t.c:4: ") and "no lifted function" in errs[2], errs[2])
        self.assertTrue(errs[3].startswith("host/t.c:5: ") and "x19" in errs[3], errs[3])
        self.assertEqual(patch.errors(self.P, ("host/t.c", 1)), [])

    def test_report(self):
        out = []
        lines = patch.report(self.P, out.append)
        first = [l for l in lines if l[3] == ("host/t.c", 1)][0]
        self.assertIn("reads -, writes - beyond the instruction", first[7])   # (the row reads the add's x0 and x1)

    def test_diff(self):
        before = dict(self.P.plan["summaries"])
        after = dict(before)
        s = list(after[C])
        s[0] |= summary.BIT["x5"]
        after[C] = tuple(s)
        out = []
        n, _, sites = patch.diff(self.P, before, after, {}, {}, {F: [("call", C)]}, self.ps, out.append)
        self.assertEqual((n, sites), (1, 1))
        self.assertIn("inputs +x5", "\n".join(out))

    def test_live_out(self):
        fn = self.P.func(F)
        li = summary.liveness(fn, self.P.sums, self.P.host)
        lo = summary.live_out(fn, self.P.sums, self.P.host, li)
        for k, kind in enumerate(fn.kind):
            if kind == "op":   # (live-in = what it reads, and what is live after it that it does not write)
                self.assertEqual(li[k], fn.use[k] | (lo[k] & ~fn.dfn[k]))


if __name__ == "__main__":
    unittest.main()
