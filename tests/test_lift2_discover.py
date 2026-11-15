"""Lift 2's discovery (nx2/lift2/discover.py) against the present lifter's (third_party/a64-kit/lift/a64_lift.py with
nx2/insn.py and nx2/lift_fixes.py installed, as nx2/lift.py runs it) on a synthetic image: calls, a tail call, a
jump table, a RETAB, a PLT stub. No dump.

python -I -B tests/test_lift2_discover.py
"""
import os
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
sys.path.insert(0, os.path.join(ROOT, "third_party", "a64-kit", "lift"))
from lift2 import discover  # noqa: E402

BASE = 0x80000000
# code (offsets from BASE; llvm-mc, branches by hand):
#  0x00 f:     stp x29, x30, [sp, #-16]!; bl g; ldp x29, x30, [sp], #16; retab
#  0x10 g:     cmp w0, #2; b.hi 0x38; adr x9, 0x30; adrp x8, BASE + 0x1000; add x8, x8, #0; ldrb w10, [x8, w0, uxtw];
#              add x9, x9, x10, lsl #2; br x9      (a byte table: cases 0x30, 0x38, 0x30)
#  0x30        mov x0, #0; ret; mov x0, #1; ret
#  0x40 t:     add x1, x1, #1; b g                 (a tail call)
#  0x48 stub:  adrp x16, BASE + 0x1000; ldr x17, [x16, #8]; add x16, x16, #8; br x17   (a PLT stub to g)
CODE = [0xa9bf7bfd, 0x94000003, 0xa8c17bfd, 0xd65f0fff,
        0x7100081f, 0x54000128, 0x100000c9, 0xb0000008, 0x91000108, 0x3860490a, 0x8b0a0929, 0xd61f0120,
        0xd2800000, 0xd65f03c0, 0xd2800020, 0xd65f03c0,
        0x91000421, 0x17fffff3,
        0xb0000010, 0xf9400611, 0x91002210, 0xd61f0220]
ENTRIES = {BASE, BASE + 0x10, BASE + 0x40, BASE + 0x48}


class Image:
    def __init__(self):
        self.mem = bytearray(0x3000)
        for k, w in enumerate(CODE):
            struct.pack_into("<I", self.mem, 4 * k, w)
        # the byte table at BASE + 0x1000, entries (target - 0x30) / 4; the stub's GOT slot at BASE + 0x1008
        struct.pack_into("<BBB", self.mem, 0x1000, 0, 2, 0)
        struct.pack_into("<Q", self.mem, 0x1008, BASE + 0x10)
        self.entries, self.hooks, self.names = set(ENTRIES), {}, {}
        self.eh_starts = sorted(ENTRIES)

    def in_code(self, a):
        return BASE <= a < BASE + 4 * len(CODE)

    def word(self, a):
        return struct.unpack_from("<I", self.mem, a - BASE)[0]

    def read(self, a, n):
        if not BASE <= a <= BASE + len(self.mem) - n:
            raise ValueError(a)
        return bytes(self.mem[a - BASE:a - BASE + n])


class Discover(unittest.TestCase):
    def test_same_as_the_present_lifter(self):
        try:
            import a64_lift
            import insn
            import lift_fixes
        except Exception as e:   # (the present lifter needs its translator library; gone after milestone 19)
            self.skipTest("the present lifter does not load here: %s" % e)
        insn.install()
        lift_fixes.install()
        L = a64_lift.Lifter(Image())
        L.discover()
        D = discover.Discovery(Image())
        D.discover()
        self.assertTrue(discover.compare(L, D))

    def test_facts(self):
        D = discover.Discovery(Image())
        D.discover()
        f = D.funcs[BASE][0]
        self.assertEqual(f[BASE + 0xc][0], ("ret", 30))   # (RETAB is a return)
        self.assertIn(BASE + 0x2c, D.jt)                   # (the BR's table)
        self.assertEqual(sorted(set(D.jt[BASE + 0x2c])), [BASE + 0x30, BASE + 0x38])
        self.assertEqual(D.plt.get(BASE + 0x48), (BASE + 0x10, BASE + 0x1008))
        self.assertEqual(D.callee(BASE + 0x48), BASE + 0x10)


if __name__ == "__main__":
    unittest.main()
