"""A lift that was stopped before its end is never built from (nx2/lift_mark.py), and the lift's emit sizes its forked
workers by the memory left (nx2/chunks.py emit_jobs), the cgroup's limit included.

python -I -B tests/test_lift_mark.py
"""
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
import kit  # noqa: E402,F401  (a64-kit on the path, for chunks)
import lift_mark  # noqa: E402


def read(*p):
    with open(os.path.join(ROOT, *p), encoding="utf-8") as f:
        return f.read()


class Marks(unittest.TestCase):
    def test_begin_done_check(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(lift_mark.check(d))   # (lifted before the marks: built as before)
            lift_mark.begin(d)
            self.assertIn("incomplete", lift_mark.check(d))
            with mock.patch.dict(os.environ, {"HOOKSHOT_LIFT2": "all-safe"}):
                lift_mark.done(d)
            self.assertIsNone(lift_mark.check(d))
            with open(os.path.join(d, lift_mark.OK)) as f:
                ok = f.read()
            self.assertTrue(ok.startswith("commit "))
            self.assertIn("HOOKSHOT_LIFT2=all-safe", ok)
            lift_mark.begin(d)   # (a new lift: the old OK goes)
            self.assertFalse(os.path.exists(os.path.join(d, lift_mark.OK)))
            self.assertIsNotNone(lift_mark.check(d))

    def test_callers(self):
        lift = read("nx2", "lift.py")
        self.assertLess(lift.index("lift_mark.begin(out_dir)"), lift.index("meta = img.write(img_dir)"),
                        "the mark goes up before the lift writes anything")
        self.assertLess(lift.index("chunks.write("), lift.index("lift_mark.done(out_dir)"), "and comes down last")
        self.assertIn("lift_mark.check(", read("nx2", "build.py"))
        refresh = read("scripts", "play-refresh.sh")
        self.assertLess(refresh.index("LIFT_INCOMPLETE"), refresh.index("oot_patches.apply"),
                        "play-refresh refuses before it touches the lift")


class EmitJobs(unittest.TestCase):
    def jobs(self, avail, cgroup, cores=16):
        import chunks
        with mock.patch.object(chunks, "_available_gb", return_value=avail), \
                mock.patch.object(chunks, "_cgroup_free_gb", return_value=cgroup), \
                mock.patch.object(os, "sched_getaffinity", return_value=set(range(cores))), \
                mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("HOOKSHOT_LIFT_JOBS", None)
            return chunks.emit_jobs()

    def test_sized_by_memory(self):
        self.assertEqual(self.jobs(23.0, None), int((23.0 - 2.0) // 3.7))
        self.assertEqual(self.jobs(23.0, 8.0), 1)    # (a capped scope with 8 GB left: one worker)
        self.assertEqual(self.jobs(100.0, None, cores=4), 4)
        self.assertEqual(self.jobs(1.0, None), 1)
        with mock.patch.dict(os.environ, {"HOOKSHOT_LIFT_JOBS": "3"}):
            import chunks
            self.assertEqual(chunks.emit_jobs(), 3)


if __name__ == "__main__":
    unittest.main()
