"""ounce/lift/data/horizon.toml (the Rust lifter's platform facts) holds what nx2/horizon holds: the setjmp names, the
noreturn seeds and the OS-facing namespaces. No game is used.

python -I -B tests/test_horizon_data.py
"""
import os
import sys
import tomllib
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
import horizon  # noqa: E402


class HorizonData(unittest.TestCase):
    def test_equal(self):
        with open(os.path.join(ROOT, "ounce", "lift", "data", "horizon.toml"), "rb") as f:
            d = tomllib.load(f)
        self.assertEqual(tuple(d["setjmp_names"]), horizon.SETJMP_NAMES)
        self.assertEqual(tuple(d["noreturn_names"]), horizon.NORETURN_NAMES)
        self.assertEqual(tuple(d["noreturn_parts"]), horizon.NORETURN_PARTS)
        self.assertEqual(set(d["os_namespaces"]), set(horizon.OS_NAMESPACES))
        self.assertEqual(d["os_namespaces"], sorted(d["os_namespaces"]))


if __name__ == "__main__":
    unittest.main()
