"""The build's content keys (nx2/lift2/game.py): what makes a partition compile again and the summaries be computed again.

- source_key: a lifter source without comments and docstrings: editing a comment leaves the lifter's code key as it
  was; editing code changes it (the host's patch declarations are in no lifter source: host/patch.h).
- part_key: a partition's key changes with its own functions' rows, entry hooks and summaries and with the
  call-boundary records of the functions they branch or call to, and with nothing of another partition.
- summary_effects: changes with a row's declared reads or writes, not with its host function's name; with an entry
  hook's declared reads and guest callees, with its presence (a hook added needs no lift), and with an HLE'd
  function's declared reads and clobbers.
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

    def keys(self, plan, rows, hooks, records):
        bound = records.get
        return [game.part_key(plan, [e], hooks, bound, rows) for e in (self.A, self.B)]

    def test_branch_targets(self):
        pcs, words = self.plan()["bodies"][self.A]
        self.assertEqual(game.branch_targets(pcs, words), {self.C, self.A + 12})
        # (a BL to the function itself is a target; a B inside the body is not)
        self.assertEqual(game.branch_targets(array("Q", [0, 4]), array("I", [0x14000001, 0x97ffffff])),
                         {0, 8})

    def test_what_changes_a_key(self):
        plan, rows, hooks, rec = self.plan(), {}, {}, {self.C: ("c",)}
        a, b = self.keys(plan, rows, hooks, rec)
        for change in ("row", "hook", "summary", "callee"):
            p2, r2, h2, c2 = self.plan(), dict(rows), dict(hooks), dict(rec)
            if change == "row":
                r2[self.A] = ["At(...)"]
            elif change == "hook":
                h2[self.A] = "entry_0x00001000"
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
            return game.summary_effects({}, {})
        finally:
            probes.use(*saved)

    def test_effects_not_names(self):
        row = probes.At(0x1000, 0x1004, 0xd503201f, "fn", ("x0",), "x1")
        k = self.effects([row])
        self.assertEqual(self.effects([probes.At(0x1000, 0x1004, 0xd503201f, "other", ("x0",), "x1")]), k)
        self.assertNotEqual(self.effects([probes.At(0x1000, 0x1004, 0xd503201f, "fn", ("x2",), "x1")]), k)
        self.assertNotEqual(self.effects([probes.At(0x1000, 0x1004, 0xd503201f, "fn", ("x0",), "x3")]), k)

    def declared(self, text, hooks):
        before = patch_scan.use(patch_scan.scan_text(text))
        try:
            return game.summary_effects(hooks, {0x2000: "nn_f"})
        finally:
            patch_scan.use(before)

    def test_hooks_and_hle(self):
        hook = "PATCH_HOOK(0x1000, word(0x1)%s) { return 0; }\n"
        named = {0x1000: "entry_0x1000"}
        k = self.declared(hook % "" + "HLE(nn_f) {}", named)
        self.assertNotEqual(self.declared(hook % "" + "HLE(nn_f) {}", {}), k)   # (no hook)
        for text in (hook % ', reads(sp, "t")' + "HLE(nn_f) {}", hook % ", guest(0x3000)" + "HLE(nn_f) {}",
                     hook % "" + 'HLE(nn_f, reads(x29, "t")) {}', hook % "" + 'HLE(nn_f, clobbers("t")) {}'):
            self.assertNotEqual(self.declared(text, named), k, text)
        self.assertEqual(self.declared("/* moved */\n" + hook % "" + "HLE(nn_f) { int body; }", named), k)


if __name__ == "__main__":
    unittest.main()
