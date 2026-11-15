"""The build's content keys (nx2/lift2/game.py): what makes a partition compile again and the summaries be computed again.

- source_key: a lifter source without comments and docstrings: editing a comment leaves the lifter's code key as it
  was; editing code changes it (the host's patch declarations are in no lifter source: host/patch.h).
- part_key: a partition's key changes with its own functions' rows, PATCH_FUNCs and summaries and with the
  call-boundary records of the functions they branch or call to, and with nothing of another partition.
- summary_effects: changes with a row's declared reads or writes, not with its host function's name; with a
  PATCH_FUNC's operands, result, orig and guest callees, with its presence (a function replaced needs no lift), not
  with its body or its place; and with an HLE'd function's declared reads and clobbers.
No game is used.

python -I -B tests/test_lift2_keys.py
"""
import os
import sys
import unittest
from array import array

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
import gamedesc  # noqa: E402
gamedesc.use(os.path.join(ROOT, "games", "oot"))   # (the game whose host files and patches these read)
from lift2 import game, patch_scan, probes, summary  # noqa: E402

HERE = os.path.join(ROOT, "nx2", "lift2")


def text(name):
    with open(os.path.join(HERE, name), encoding="utf-8") as f:
        return f.read()


class SourceKey(unittest.TestCase):
    def test_comments_left_out(self):
        src = text("probes.py")
        key = game.source_key("probes.py", src)
        self.assertEqual(game.source_key("probes.py", src.replace("# the registers a row may write", "# (edited)")),
                         key)
        code = "        self.before = before   #"
        self.assertIn(code, src)
        self.assertNotEqual(game.source_key("probes.py", src.replace(code, "        self.before = not before   #")),
                            key)

    def test_other_sources_keep_their_code(self):
        src = text("bounds.py")
        self.assertNotEqual(game.source_key("bounds.py", src.replace("KEEP_ALL = sum(", "KEEP_ALL = 1 + sum(")),
                            game.source_key("bounds.py", src))


class PartKey(unittest.TestCase):
    A, B, C = 0x1000, 0x2000, 0x3000

    def plan(self):
        bl_c = 0x94000000 | ((self.C - (self.A + 4)) // 4 & 0x3FFFFFF)
        return {"plt": {}, "summaries": {self.A: ("a",), self.B: ("b",)},
                "bodies": {self.A: (array("Q", [self.A, self.A + 4, self.A + 8]), array("I", [0xd503201f, bl_c, 0xd65f03c0])),
                           self.B: (array("Q", [self.B, self.B + 4]), array("I", [0xd503201f, 0xd65f03c0]))}}

    def keys(self, plan, rows, funcs, records):
        bound = records.get
        return [game.part_key(plan, [e], funcs, bound, rows) for e in (self.A, self.B)]

    def test_branch_targets(self):
        pcs, words = self.plan()["bodies"][self.A]
        self.assertEqual(game.branch_targets(pcs, words), {self.C, self.A + 12})
        # (a BL to the function itself is a target; a B inside the body is not)
        self.assertEqual(game.branch_targets(array("Q", [0, 4]), array("I", [0x14000001, 0x97ffffff])),
                         {0, 8})

    def test_what_changes_a_key(self):
        plan, rows, funcs, rec = self.plan(), {}, {}, {self.C: ("c",)}
        a, b = self.keys(plan, rows, funcs, rec)
        for change in ("row", "func", "summary", "callee"):
            p2, r2, h2, c2 = self.plan(), dict(rows), dict(funcs), dict(rec)
            if change == "row":
                r2[self.A] = ["At(...)"]
            elif change == "func":
                h2[self.A] = patch_scan.scan_text("PATCH_FUNC(0x1000, word(1), body(2), aapcs, none, f) {}").funcs[0]
            elif change == "summary":
                p2["summaries"][self.A] = ("a2",)
            else:
                c2[self.C] = ("c2",)
            a2, b2 = self.keys(p2, r2, h2, c2)
            self.assertNotEqual(a2, a, change)
            self.assertEqual(b2, b, change)


class SummaryEffects(unittest.TestCase):
    def tearDown(self):
        summary._ROWS = None

    def effects(self, rows):
        saved = probes.use([], rows)
        try:
            return game.summary_effects({}, {})   # (no HLE, no PATCH_FUNC)
        finally:
            probes.use(*saved)

    def test_effects_not_names(self):
        row = probes.At(0x1000, 0x1004, 0xd503201f, "fn", ("x0",), "x1")
        k = self.effects([row])
        self.assertEqual(self.effects([probes.At(0x1000, 0x1004, 0xd503201f, "other", ("x0",), "x1")]), k)
        self.assertNotEqual(self.effects([probes.At(0x1000, 0x1004, 0xd503201f, "fn", ("x2",), "x1")]), k)
        self.assertNotEqual(self.effects([probes.At(0x1000, 0x1004, 0xd503201f, "fn", ("x0",), "x3")]), k)

    def declared(self, text):
        before = patch_scan.use(patch_scan.scan_text(text))
        try:
            fs = {f.addr: f for f in patch_scan.patches().funcs}
            return game.summary_effects({0x2000: "nn_f"}, summary.func_facts(fs))
        finally:
            patch_scan.use(before)

    def test_funcs_and_hle(self):
        func = "PATCH_FUNC(0x1000, word(0x1), body(0x2), aapcs, x0, f%s) { return 0; }\n"
        k = self.declared(func % "" + "HLE(nn_f) {}")
        self.assertNotEqual(self.declared("HLE(nn_f) {}"), k)   # (none)
        for text in (func % ", x1" + "HLE(nn_f) {}", func % ", orig" + "HLE(nn_f) {}",
                     func % ", guest(0x3000), sp" + "HLE(nn_f) {}", (func % "").replace("x0, f", "none, f") + "HLE(nn_f) {}",
                     func % "" + 'HLE(nn_f, reads(x29, "t")) {}', func % "" + 'HLE(nn_f, clobbers("t")) {}'):
            self.assertNotEqual(self.declared(text), k, text)
        self.assertEqual(self.declared("/* moved */\n" + (func % "").replace("body(0x2)", "body(0x3)").replace(
            "f) {", "g) {") + "HLE(nn_f) { int body; }"), k)   # (the body hash and the C name are checked, not counted)


class PlanComplete(unittest.TestCase):
    def test_no_entry_dropped(self):
        """write_plan: every entry has a body or is an HLE'd function; one without either stops the lift"""
        game.check_complete({1, 2, 3}, {1: None, 2: None}, {3})
        with self.assertRaises(SystemExit) as e:
            game.check_complete({1, 2, 3, 0x80001000}, {1: None, 2: None}, {3})
        self.assertIn("f_80001000", str(e.exception))


if __name__ == "__main__":
    unittest.main()
