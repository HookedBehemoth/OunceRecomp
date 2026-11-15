"""The present lifter's share of the mixed build (nx2/lift2/owned.py): which functions it keeps, from the edits'
sources, and the file the lift hands the build. No dump.

python -I -B tests/test_lift2_owned.py
"""
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
from lift2 import owned  # noqa: E402


def owned_splices():
    import entry_hooks
    return entry_hooks.wanted_splices()


class Owned(unittest.TestCase):
    def test_edit_addresses_found(self):
        """the addresses a patch source names (heads, labels, constants) but those lift 2 makes as probes"""
        real = owned.PATCH_SOURCES
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "patches.py")
            with open(src, "w") as f:
                f.write('HEAD = "void f_80b1e140(Ctx *c) {"\nLABEL = "L_8082640c:;"\nRET = 0x83b7b440\n'
                        '# f_80a87e50 (a WHOLE_SITES probe: lift 2 makes it)\n')
            try:
                owned.PATCH_SOURCES = (src,)   # (absolute: joined to nx2/ it stays itself)
                got = owned.edit_addresses(os.path.join(d, "gen"))
            finally:
                owned.PATCH_SOURCES = real
        self.assertTrue({0x80b1e140, 0x8082640c, 0x83b7b440} <= got)
        self.assertNotIn(0x80a87e50, got)
        # (the game's own sources now: the frame-rate and resolution patches are lift 2's At probes,
        # and the parked ones, nx2/oot_parked.py, are applied by nothing: no present-lifter edit is left in them)
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(owned.edit_addresses(os.path.join(d, "gen")) - set(owned_splices()), set())

    def test_compute(self):
        funcs = {0x80000000: [0x80000000, 0x80000004], 0x80000100: [0x80000100, 0x80000104],
                 0x80000200: [0x80000200], 0x80000300: [0x80000300]}
        bodies = {0x80000000, 0x80000100, 0x80000200}   # (0x80000300: lift 2 cannot take it)
        real = owned.edit_addresses
        try:
            owned.edit_addresses = lambda gen: {0x80000108}   # (a return address: the call before it is 0x80000104)
            own, edited = owned.compute(funcs, bodies, "gen")
            self.assertEqual(edited, {0x80000100})
            self.assertEqual(own, {0x80000100, 0x80000300})
            own, _ = owned.compute(funcs, bodies, "gen", listed={0x80000000})
            self.assertEqual(own, {0x80000100, 0x80000200, 0x80000300})
        finally:
            owned.edit_addresses = real

    def test_save_load(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(owned.load(d))
            owned.save(d, {0x80000100}, {0x80000100}, {"f_80000000"})
            self.assertEqual(owned.load(d), ({0x80000100}, {0x80000100}, {0x80000000}))
        self.assertEqual(owned.check({1, 2}, {2}), [1])


class Traps(unittest.TestCase):
    def test_traps_compile(self):
        # (HOOKSHOT_LIFT1=0 with HOOKSHOT_LIFT1_TRAP=1: each owned function a trap, against the generated a64.h)
        import subprocess
        import rtgen
        from lift2 import llvm
        with tempfile.TemporaryDirectory() as d:
            rt = rtgen.write(os.path.join(d, "rt64"))
            owned.write_traps(d, {0x80001000, 0x80002000})
            r = subprocess.run([llvm.check("clang"), "-fsyntax-only", "-w", "-I" + rt,
                                os.path.join(d, owned.TRAPS)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-1500:])
            owned.write_traps(d, set())   # (none owned: no file)
            self.assertFalse(os.path.exists(os.path.join(d, owned.TRAPS)))


if __name__ == "__main__":
    unittest.main()
