"""A lift that was stopped before its end is never built from (nx2/lift_mark.py), and play-refresh lifts again when the
lift's inputs changed.

python -I -B tests/test_lift_mark.py
"""
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
import gamedesc  # noqa: E402
import lift_mark  # noqa: E402

GAME = '''from gamedesc import Game, Identity
GAME = Game(name="t", identity=Identity(build_id="00", main_sha256="00"), labels="labels.txt", hot="hot.txt",
            host=("a.c",))
'''


def read(*p):
    with open(os.path.join(ROOT, *p), encoding="utf-8") as f:
        return f.read()


class Marks(unittest.TestCase):
    def test_begin_done_check(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(lift_mark.check(d))   # (lifted before the marks: built as before)
            no_game = mock.patch.object(lift_mark, "default_inputs", lambda root: (None, None, None))   # (no game here)
            no_game.start()
            self.addCleanup(no_game.stop)
            lift_mark.begin(d)
            self.assertIn("incomplete", lift_mark.check(d))
            with mock.patch.dict(os.environ, {"HOOKSHOT_LIFT_VERIFY": "1"}):
                lift_mark.done(d)
            self.assertIsNone(lift_mark.check(d))
            with open(os.path.join(d, lift_mark.OK)) as f:
                ok = f.read()
            self.assertTrue(ok.startswith("commit "))
            self.assertIn("HOOKSHOT_LIFT_VERIFY=1", ok)
            lift_mark.begin(d)   # (a new lift: the old OK goes)
            self.assertFalse(os.path.exists(os.path.join(d, lift_mark.OK)))
            self.assertIsNotNone(lift_mark.check(d))

    def test_callers(self):
        lift = read("nx2", "lift.py")
        self.assertLess(lift.index("lift_mark.begin(out_dir)"), lift.index("meta = img.write(img_dir)"),
                        "the mark goes up before the lift writes anything")
        self.assertLess(lift.index("tables.write("), lift.rindex("lift_mark.done(out_dir)"), "and comes down last")
        self.assertIn("lift_mark.check(", read("nx2", "build.py"))
        refresh = read("scripts", "play-refresh.sh")
        self.assertLess(refresh.index("LIFT_INCOMPLETE"), refresh.index("nx2/build.py"),
                        "play-refresh refuses before it touches the lift")


class Inputs(unittest.TestCase):
    """play-refresh lifts again when the lift's inputs changed, and keeps the fast path for a host-only change"""

    def tree(self, d):
        files = {"third_party/a64-kit/rt/a64.h": "runtime", "nx2/lift.py": "lift", "nx2/lift2/game.py": "game",
                 "nx2/build.py": "build", "nx2/oot_wide.py": "patches",
                 "host/a.c": "int x;\nHLE(nn_a) {}\nPATCH_FUNC(0x80001000, word(0x1), body(0x2), aapcs, none, f) {}\n",
                 "games/t/game.py": GAME}
        for rel, text in files.items():
            os.makedirs(os.path.dirname(os.path.join(d, rel)), exist_ok=True)
            with open(os.path.join(d, rel), "w") as f:
                f.write(text)

    def edit(self, d, rel, old, new):
        p = os.path.join(d, rel)
        with open(p) as f:
            t = f.read()
        with open(p, "w") as f:
            f.write(t.replace(old, new))

    def test_decision(self):
        with tempfile.TemporaryDirectory() as d:
            self.tree(d)
            hot = os.path.join(d, "hot.txt")
            with open(hot, "w") as f:
                f.write("f_80001000\n")
            env = {"HOOKSHOT_LIFT_VERIFY": "1"}

            def h(env=env):
                return lift_mark.inputs_hash(d, env, hot, None, gamedesc.load(os.path.join(d, "games", "t")))
            base = h()
            self.assertEqual(h(), base)
            for rel, old, new, changes in (
                    ("host/a.c", "int x;", "int y;", False),                     # (host code: the fast path)
                    ("nx2/build.py", "build", "build2", False),                  # (the build, not the lift)
                    ("host/a.c", "0x80001000", "0x80002000", False),             # (a PATCH_FUNC: the build's)
                    ("host/a.c", "HLE(nn_a)", "HLE(nn_b)", True),                # (a function the lift hooks)
                    ("nx2/lift.py", "lift", "lift2", True),
                    ("third_party/a64-kit/rt/a64.h", "runtime", "runtime2", True),
                    ("nx2/lift2/game.py", "game", "game2", True),
                    ("nx2/oot_wide.py", "patches", "patches2", True),
                    ("games/t/game.py", '"a.c",', '"a.c", "b.c",', True)):    # (the game description)
                if rel == "games/t/game.py":
                    with open(os.path.join(d, "host", "b.c"), "w") as f:
                        f.write("int z;\n")
                self.edit(d, rel, old, new)
                now = h()
                self.assertEqual(now != base, changes, rel)
                base = now
            with open(hot, "a") as f:
                f.write("f_80002000\n")
            self.assertNotEqual(h(), base)
            self.assertNotEqual(h({"HOOKSHOT_LIFT_VERIFY": "0"}), h())

    def test_stale(self):
        with tempfile.TemporaryDirectory() as d:
            self.tree(d)
            folder = os.path.join(d, "sa")
            os.makedirs(folder)
            self.assertIn("records no inputs", lift_mark.stale(folder, d, {}))
            with open(os.path.join(folder, lift_mark.OK), "w") as f:
                f.write("commit abc\ninputs %s\n" % lift_mark.inputs_hash(d, {}))
            self.assertIsNone(lift_mark.stale(folder, d, {}))
            self.edit(d, "nx2/lift.py", "lift", "lifted")
            self.assertIn("inputs changed", lift_mark.stale(folder, d, {}))

    def test_refresh_uses_it(self):
        refresh = read("scripts", "play-refresh.sh")
        self.assertIn("nx2/lift_mark.py stale work/sa --game", refresh)
        self.assertLess(refresh.index("lift_if_stale &&"), refresh.index("nx2/build.py"), "the lift before the build")
        self.assertIn("heavy.lock", refresh)
        self.assertIn("heavy.lock", read("scripts", "relift.sh"))


if __name__ == "__main__":
    unittest.main()
