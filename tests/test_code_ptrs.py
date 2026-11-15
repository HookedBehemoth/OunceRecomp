"""Code pointers are host function pointers (nx2/lift2/code_ptrs.py), without the dump.

The census's shapes: ADR of an entry; ADRP + ADD of the same register within 4 instructions, in one block, with no
write of the register between; not a module's base, not an ADRP alone. The lift translates exactly those (a site whose
ADD does not see the page, or an entry's address formed another way, stops it), a BLR calls a host pointer directly and
passes a guest address with its pc to the runtime, a BR not in a table tail-calls a host pointer, and the module
compiles.

python -I -B tests/test_code_ptrs.py
"""
import os
import subprocess
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
from lift2 import build_ir, code_ptrs, emit_llvm, llvm  # noqa: E402

ME, F, G = 0x80000000, 0x80002040, 0x80003000   # (F, G: functions of the table)


def adr(pc, rd, target):
    v = target - pc
    return 0x10000000 | (v & 3) << 29 | ((v >> 2) & 0x7FFFF) << 5 | rd


def adrp(pc, rd, target):
    v = (target >> 12) - (pc >> 12)
    return 0x90000000 | (v & 3) << 29 | ((v >> 2) & 0x7FFFF) << 5 | rd


def add(rd, rn, imm):
    return 0x91000000 | imm << 10 | rn << 5 | rd


MOVZ_X1 = 0xD2800021   # movz x1, #1
STR_X8 = 0xF90003E8    # str x8, [sp]
BLR_X0 = 0xD63F0000
BR_X1 = 0xD61F0020
RET = 0xD65F03C0
CBZ_X2_8 = 0xB4000042   # cbz x2, +8


def body(words):
    return [ME + 4 * k for k in range(len(words))], words


def sites(words, tg=(F, G)):
    pcs, ws = body(words)
    return code_ptrs.sites(pcs, ws, set(tg))


class Census(unittest.TestCase):
    def test_shapes(self):
        s = sites([adr(ME, 0, F), adrp(ME + 4, 8, F), add(1, 8, F & 0xFFF), RET])
        self.assertEqual(s, {ME: (F, None), ME + 8: (F, F & ~0xFFF)})

    def test_store_between_keeps_the_pair(self):
        s = sites([adrp(ME, 8, F), STR_X8, add(1, 8, F & 0xFFF), RET])
        self.assertEqual(s, {ME + 8: (F, F & ~0xFFF)})

    def test_write_between_breaks_the_pair(self):
        s = sites([adrp(ME, 1, F), MOVZ_X1, add(1, 1, F & 0xFFF), RET])
        self.assertEqual(s, {})

    def test_block_between_breaks_the_pair(self):
        # (cbz x2 at ME+4 jumps to ME+12: a block starts between the ADRP and the ADD at ME+12)
        s = sites([adrp(ME, 8, F), CBZ_X2_8, MOVZ_X1, add(0, 8, F & 0xFFF), RET])
        self.assertEqual(s, {})

    def test_not_an_entry_or_an_adrp_alone(self):
        self.assertEqual(sites([adr(ME, 0, F + 4), adrp(ME + 4, 3, G), RET]), {})

    def test_module_base_is_not_a_target(self):
        plan = {"bodies": {ME: None, F: None}, "hooks": [G], "code_excluded": [ME]}
        self.assertEqual(code_ptrs.targets(plan), {F, G})


def lift(words, tg=(F, G)):
    pcs, ws = body(words)
    prog = build_ir.Program(entries={ME, F, G})
    prog.code_targets = set(tg)
    prog.code_sites = code_ptrs.sites(pcs, ws, prog.code_targets)
    fn = build_ir.build(ME, pcs, dict(zip(pcs, ws)).__getitem__, prog)
    fn.name = "f_%08x" % ME
    m = emit_llvm.Module()
    m.add(fn)
    return fn, m.text()


class Lift(unittest.TestCase):
    def test_translated_and_called(self):
        fn, text = lift([adrp(ME, 8, F), add(0, 8, F & 0xFFF), BLR_X0, adr(ME + 12, 1, G), BR_X1])
        self.assertEqual(fn.code_sites, 2)
        self.assertIn("ptrtoint (ptr @f_%08x to i64)" % F, text)
        self.assertIn("ptrtoint (ptr @f_%08x to i64)" % G, text)
        self.assertIn("call void @lift2_icall(ptr %%ctx, i64 ptrtoint (ptr @f_%08x to i64), i64 %d)" % (F, ME + 8), text)
        self.assertIn("call void @a64_icall_guest(ptr %ctx, i64 %t, i64 %site)", text)
        self.assertIn("musttail call void", text)
        self.assertRegex(text, r"phi i64 \[ ptrtoint \(ptr @f_%08x to i64\)" % G)
        self.assertRegex(text, r"call void @a64_ijump_guest\(ptr %%ctx, i64 %%v\d+, i64 %d\)" % (ME + 16))
        r = subprocess.run([llvm.check("clang"), "-O1", "-x", "ir", "-", "-c", "-o", os.devnull], input=text.encode(),
                           capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr.decode()[-2000:])

    def test_other_shape_stops(self):
        # (an entry's address made by an ADD the census does not pair: x9 = the page by an ADD #0 copy, then + lo12)
        with self.assertRaisesRegex(ValueError, "not a code_ptrs.sites shape"):
            lift([adrp(ME, 8, F), add(9, 8, 0), add(0, 9, F & 0xFFF), RET])

    def test_site_without_the_page_stops(self):
        pcs, ws = body([adrp(ME, 8, F), add(0, 8, F & 0xFFF), RET])
        prog = build_ir.Program(entries={ME, F})
        prog.code_targets = {F}
        prog.code_sites = {ME + 4: (F, F & ~0xFFF ^ 0x1000)}   # (a census that disagrees with the code)
        with self.assertRaisesRegex(ValueError, "does not hold the page"):
            build_ir.build(ME, pcs, dict(zip(pcs, ws)).__getitem__, prog)


if __name__ == "__main__":
    unittest.main()
