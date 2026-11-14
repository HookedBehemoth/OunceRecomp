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


class Owned(unittest.TestCase):
    def test_edit_addresses_found(self):
        with tempfile.TemporaryDirectory() as d:
            got = owned.edit_addresses(os.path.join(d, "gen"))
        # (oot_patches.py's text speed patch names f_80b1e140; its dynamic resolution patch the label L_8082640c)
        self.assertIn(0x80b1e140, got)
        self.assertIn(0x8082640c, got)

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


if __name__ == "__main__":
    unittest.main()
