"""nx2/build.py's compile estimates (HOOKSHOT_TIMES=<build_times.json>: seconds per module from earlier builds), its plan of
what a cache already holds.

Nothing here runs a compiler or needs the game.

python -I -B tests/test_build_times.py
"""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "nx2"), str(ROOT / "third_party" / "a64-kit" / "tools")]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


build = load("build_times", ROOT / "nx2" / "build.py")


class EstimateTests(unittest.TestCase):
    def test_no_files_left_is_no_time(self):
        self.assertEqual(build.eta_left([], 8), 0.0)

    def test_the_work_spreads_over_the_compilers(self):
        self.assertEqual(build.eta_left([100.0, 100.0, 100.0, 100.0], 4), 100.0)
        self.assertEqual(build.eta_left([100.0, 100.0], 1), 200.0)

    def test_the_longest_file_sets_the_floor(self):
        self.assertEqual(build.eta_left([600.0, 60.0], 8), 600.0)

    def test_measured_seconds_are_used_as_they_are(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "code_003.c"
            src.write_bytes(b"x" * 1024)
            jobs = [("cc", str(src), str(Path(d) / "code_003.o"), [], b"")]
            self.assertEqual(build.per_file_seconds(jobs, {"code_003.c": 42.0}), [42.0])

    def test_an_unmeasured_module_follows_the_measured_rate(self):
        with tempfile.TemporaryDirectory() as d:
            known = Path(d) / "code_001.c"
            known.write_bytes(b"x" * 1024)
            new = Path(d) / "code_002.c"
            new.write_bytes(b"x" * 2048)
            jobs = [("cc", str(known), "", [], b""), ("cc", str(new), "", [], b"")]
            self.assertEqual(build.per_file_seconds(jobs, {"code_001.c": 10.0}), [10.0, 20.0])

    def test_a_first_build_still_has_an_estimate_for_every_module(self):
        with tempfile.TemporaryDirectory() as d:
            jobs = []
            for name in ("code_hot.c", "code_001.c", "host_main.c"):
                src = Path(d) / name
                src.write_bytes(b"x" * (1 << 20))
                jobs.append(("cc", str(src), "", [], b""))
            seconds = build.per_file_seconds(jobs, {})
            self.assertEqual(len(seconds), 3)
            self.assertTrue(all(seconds))
            self.assertGreater(seconds[0], seconds[1])   # (the hot file's rate is the highest)

    def test_a_broken_record_is_no_estimate(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "build_times.json"
            self.assertEqual(build.times_from(str(path)), {})
            path.write_text("{not json", encoding="utf-8")
            self.assertEqual(build.times_from(str(path)), {})
            path.write_text('{"seconds": {"code_001.c": 12.5, "odd": "x"}}', encoding="utf-8")
            self.assertEqual(build.times_from(str(path)), {"code_001.c": 12.5})

    def test_the_plan_leaves_out_what_a_cache_already_holds(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "code_001.c"
            src.write_text("int f(void) { return 0; }\n", encoding="utf-8")
            obj = Path(d) / "code_001.o"
            job = ("cc", str(src), str(obj), ["-O1"], b"headers")
            work, keys = build.plan_jobs([job])
            self.assertEqual(work, [job])
            key = keys[(str(src), str(obj))]
            obj.write_bytes(b"")
            Path(str(obj) + ".sha1").write_text(key, encoding="utf-8")
            self.assertEqual(build.plan_jobs([job])[0], [])


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
