"""The runtime's tables (nx2/lift2/tables.py: a64_table.c, the function table a64_lookup searches; a64_hooks.c, the
hooked addresses) compile against the generated context header (nx2/rtgen.py's a64.h). No dump.

python -I -B tests/test_lift2_tables.py
"""
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
from lift2 import llvm, tables  # noqa: E402

FUNCS = {0x80001000: None, 0x80000100: None, 0x80002000: None}
HOOKS = {0x80000400: "hle_memcpy", 0x80000800: "stub_nn_foo", 0x80000900: "hle_memcpy"}


class Tables(unittest.TestCase):
    def test_tables_compile(self):
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
