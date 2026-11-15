"""Lifter 2's whole-program summaries (nx2/lift2/summary.py) on synthetic programs: a leaf, a caller with a frame,
a function that clobbers x19, a jmp_buf-style capture and its caller, recursion, mutual recursion through tail
calls, a tail call, a function that never returns and its caller, and flags read at entry.

python -I -B tests/test_lift2_summary.py
"""
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
import gamedesc  # noqa: E402
gamedesc.use(os.path.join(ROOT, "games", "oot"))   # (the game whose host files and patches these read)
from lift2 import build_ir, summary as S  # noqa: E402

BASE = 0x1000
# llvm-mc of the program in the comments (offsets from BASE)
WORDS = [0x8b010000, 0xd65f03c0, 0xa9be7bfd, 0xa90153f3, 0x910003fd, 0xaa0003f3, 0x97fffffa, 0x8b130000, 0xa94153f3,
         0xa8c27bfd, 0xd65f03c0, 0xd2800033, 0xd65f03c0, 0xa9005013, 0x910003e1, 0xf9003001, 0xa905781d, 0xd2800000,
         0xd65f03c0, 0xa9be7bfd, 0xf9000bf3, 0xaa0003f3, 0xb4000080, 0xd1000400, 0x97fffffb, 0x8b130000, 0xf9400bf3,
         0xa8c27bfd, 0xd65f03c0, 0xb4000060, 0xd1000400, 0x14000003, 0xd2800020, 0xd65f03c0, 0xb4000060, 0xd1000400,
         0x17fffff9, 0xd2800000, 0xd65f03c0, 0x91000421, 0x17ffffd8, 0xd4200000, 0xa9bf7bfd, 0x97fffffe, 0x54000040,
         0xd2800020, 0xd65f03c0, 0xa9bf7bfd, 0x97ffffdd, 0xa8c17bfd, 0xd65f03c0]
ENTRIES = {  # leaf: add x0, x0, x1; ret
    "leaf": 0x00,
    "caller": 0x08,   # stp x29, x30, [sp, #-32]!; stp x19, x20, [sp, #16]; mov x29, sp; mov x19, x0; bl leaf;
                      # add x0, x0, x19; ldp x19, x20, [sp, #16]; ldp x29, x30, [sp], #32; ret
    "clob": 0x2c,     # mov x19, #1; ret
    "jbuf": 0x34,     # stp x19, x20, [x0]; mov x1, sp; str x1, [x0, #96]; stp x29, x30, [x0, #80]; mov x0, #0; ret
    "rec": 0x4c,      # a recursive sum keeping n in x19 (saved and restored)
    "even": 0x74,     # even(n) = n == 0 ? 1 : odd(n - 1), by tail calls
    "odd": 0x88,      # odd(n) = n == 0 ? 0 : even(n - 1)
    "tail": 0x9c,     # add x1, x1, #1; b leaf
    "nr": 0xa4,       # brk #0
    "callnr": 0xa8,   # stp x29, x30, [sp, #-16]!; bl nr  (the body ends: nr does not return)
    "flagin": 0xb0,   # b.eq 1f; mov x0, #1; 1: ret
    "usejb": 0xbc,    # stp x29, x30, [sp, #-16]!; bl jbuf; ldp x29, x30, [sp], #16; ret
}


def regs(m, prefix="x"):
    return {v for v in S.mask_names(m) if v.startswith(prefix) and v[1:].isdigit()}


class Summaries(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        word = lambda pc: WORDS[(pc - BASE) // 4]
        entries = {BASE + o for o in ENTRIES.values()}
        funcs = S.walk(entries, word)
        prog = build_ir.Program(entries=entries)
        cls.sums, _, _ = S.summarise(funcs, prog, set(), word)
        cls.s = {n: cls.sums[BASE + o] for n, o in ENTRIES.items()}

    def test_leaf(self):
        s = self.s["leaf"]
        self.assertEqual(regs(s.inputs), {"x0", "x1"})
        self.assertEqual(regs(s.clobbers), {"x0"})
        self.assertEqual(s.preserved, S.CALLEE_SAVED)
        self.assertEqual(s.demote, [])
        self.assertTrue(s.returns)
        self.assertIn("x0", regs(s.results))

    def test_caller_frame(self):
        s = self.s["caller"]
        self.assertEqual(s.demote, [])
        self.assertTrue(s.preserved & S.BIT["x19"] and s.preserved & S.BIT["x20"] and s.preserved & S.SP)
        self.assertEqual(s.pairs, 3)   # (x19, x20, x29: saved and restored, nothing else reads the slots)
        self.assertEqual(s.frame, 32)
        self.assertTrue({"x0", "x1"} <= regs(s.inputs))

    def test_clobbers_x19(self):
        self.assertIn("cs_clobber", self.s["clob"].demote)

    def test_jmp_buf_style(self):
        self.assertIn("save_ptr", self.s["jbuf"].demote)
        self.assertTrue(self.s["jbuf"].unknown)
        self.assertIn("unknown callee", self.s["usejb"].demote)

    def test_recursion(self):
        s = self.s["rec"]
        self.assertEqual(s.demote, [])
        self.assertTrue(s.returns)
        self.assertTrue(s.preserved & S.BIT["x19"])
        self.assertIn("x0", regs(s.inputs))

    def test_mutual_recursion_by_tail_calls(self):
        for n in ("even", "odd"):
            s = self.s[n]
            self.assertEqual(s.demote, [], n)
            self.assertTrue(s.returns, n)
            self.assertEqual(regs(s.inputs), {"x0"}, n)
            self.assertEqual(regs(s.clobbers), {"x0"}, n)

    def test_tail_call(self):
        s = self.s["tail"]
        self.assertEqual(regs(s.inputs), {"x0", "x1"})
        self.assertEqual(regs(s.clobbers), {"x0", "x1"})

    def test_noreturn(self):
        self.assertFalse(self.s["nr"].returns)
        self.assertFalse(self.s["callnr"].returns)
        self.assertNotIn("dangling_bl", self.s["callnr"].demote)
        self.assertNotIn("fallthrough_entry", self.s["flagin"].demote)   # (callnr's BL never comes back)

    def test_parallel_matches(self):
        word = lambda pc: WORDS[(pc - BASE) // 4]
        entries = {BASE + o for o in ENTRIES.values()}
        funcs = S.walk(entries, word)
        par, _, _ = S.summarise_parallel(funcs, build_ir.Program(entries=entries), set(), word, jobs=2)
        for e, s in self.sums.items():
            self.assertEqual((s.key(), s.demote, s.results), (par[e].key(), par[e].demote, par[e].results), "%x" % e)

    def test_flags_at_entry(self):
        self.assertIn("flags_in", self.s["flagin"].demote)


if __name__ == "__main__":
    unittest.main()
