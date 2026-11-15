"""Lift 2's side of the runtime (milestone 19, step 3): the tables nx2/lift2/tables.py writes are the present lifter's
byte for byte, and the generated context header (nx2/rtgen.py's a64.h) compiles without the present lifter's
headers (a64_suyu.h, a64_fpx.h), as a build without present-lifter C has none. No dump.

python -I -B tests/test_lift2_tables.py
"""
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
sys.path.insert(0, os.path.join(ROOT, "third_party", "a64-kit", "lift"))
from lift2 import llvm, tables  # noqa: E402

FUNCS = {0x80001000: None, 0x80000100: None, 0x80002000: None}
HOOKS = {0x80000400: "hle_memcpy", 0x80000800: "stub_nn_foo", 0x80000900: "hle_memcpy"}


class Tables(unittest.TestCase):
    def test_same_as_the_present_lifter(self):
        try:
            import a64_lift
        except Exception as e:   # (the present lifter needs its translator library; gone after milestone 19)
            self.skipTest("the present lifter does not load here: %s" % e)

        class Img:
            entries, hooks, names = set(FUNCS), dict(HOOKS), {}
        L = a64_lift.Lifter(Img())
        L.funcs = dict(FUNCS)
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            L._write_table(a)
            tables.write(b, FUNCS, HOOKS)
            for name in ("a64_table.c", "a64_hooks.c"):
                with open(os.path.join(a, name), "rb") as f1, open(os.path.join(b, name), "rb") as f2:
                    self.assertEqual(f1.read(), f2.read(), name)

    def test_tables_compile_without_present_lifter_headers(self):
        import rtgen
        with tempfile.TemporaryDirectory() as d:
            rt = rtgen.write(os.path.join(d, "rt64"))
            tables.write(d, FUNCS, HOOKS)
            for name in ("a64_table.c", "a64_hooks.c"):
                r = subprocess.run([llvm.check("clang"), "-fsyntax-only", "-w", "-I" + rt, "-I" + d,
                                    os.path.join(d, name)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr[-2000:])


if __name__ == "__main__":
    unittest.main()
