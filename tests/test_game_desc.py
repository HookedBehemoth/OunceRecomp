"""The game description (nx2/gamedesc.py, games/<game>/game.py): what the lift and the build take from one game.

- games/oot loads; its host files exist, each once, and every host/*.c is some game's;
- the identity: a dump whose main has another build id, or the same build id and other bytes, stops with both values
  (check_exefs); a lift recorded for another game or version, or none, stops the build (check_lift);
- the hot list is one file beside the description; a missing one stops and names it.
No game is used.

python -I -B tests/test_game_desc.py
"""
import glob
import hashlib
import json
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
import gamedesc  # noqa: E402

BUILD_ID = "11" * 20
DESC = '''from gamedesc import Game, Identity
GAME = Game(name="fake", identity=Identity(build_id="%s", main_sha256="%s"), labels="labels.txt",
            hot="hot.txt", host=("hos_main.c",))
'''


def nso(build_id, tail=b""):
    """an NSO's first bytes: the magic and the module id (all check_exefs reads), then tail"""
    return b"NSO0" + bytes(0x3c) + bytes.fromhex(build_id) + bytes(12) + tail


class Description(unittest.TestCase):
    def test_oot(self):
        g = gamedesc.load(os.path.join(ROOT, "games", "oot"))
        self.assertEqual(g.name, "oot")
        self.assertTrue(os.path.exists(g.labels))
        self.assertEqual(len(set(g.host)), len(g.host))
        for p in g.host_sources():
            self.assertTrue(os.path.exists(p), p)
        listed = set()
        for path in glob.glob(os.path.join(ROOT, "games", "*")):
            listed |= set(gamedesc.load(path).host)
        unlisted = sorted(os.path.basename(p) for p in glob.glob(os.path.join(ROOT, "host", "*.c"))
                          if os.path.basename(p) not in listed)
        self.assertFalse(unlisted, "host files no game compiles: %s" % unlisted)

    def fake(self, d, build_id=BUILD_ID, sha=None):
        exefs = os.path.join(d, "exefs")
        os.makedirs(exefs, exist_ok=True)
        data = nso(BUILD_ID, b"code")
        with open(os.path.join(exefs, "main"), "wb") as f:
            f.write(data)
        game = os.path.join(d, "games", "fake")
        os.makedirs(game, exist_ok=True)
        with open(os.path.join(game, "game.py"), "w") as f:
            f.write(DESC % (build_id, sha or hashlib.sha256(data).hexdigest()))
        return exefs, game

    def test_identity(self):
        with tempfile.TemporaryDirectory() as d:
            exefs, game = self.fake(d)
            g = gamedesc.load(game)
            self.assertEqual(g.check_exefs(exefs).build_id, BUILD_ID)
            _, game = self.fake(d, build_id="22" * 20)
            with self.assertRaises(SystemExit) as e:
                gamedesc.load(game).check_exefs(exefs)
            self.assertIn("build_id is %s, the description's %s" % (BUILD_ID, "22" * 20), str(e.exception))
            _, game = self.fake(d, sha="ab" * 32)
            with self.assertRaises(SystemExit) as e:
                gamedesc.load(game).check_exefs(exefs)
            self.assertIn("main_sha256 is", str(e.exception))
            self.assertIn("the description's " + "ab" * 32, str(e.exception))
            # (the lift's record, the build's check)
            sa = os.path.join(d, "sa")
            os.makedirs(sa)
            with self.assertRaises(SystemExit) as e:
                g.check_lift(sa)
            self.assertIn("no record of the game it lifted", str(e.exception))
            g.record(sa)
            g.check_lift(sa)
            with open(os.path.join(sa, "game.json")) as f:
                rec = json.load(f)
            rec["build_id"] = "33" * 20
            with open(os.path.join(sa, "game.json"), "w") as f:
                json.dump(rec, f)
            with self.assertRaises(SystemExit) as e:
                g.check_lift(sa)
            self.assertIn("build_id is %s, the description's %s" % ("33" * 20, BUILD_ID), str(e.exception))

    def test_hot(self):
        with tempfile.TemporaryDirectory() as d:
            _, game = self.fake(d)
            g = gamedesc.load(game)
            with self.assertRaises(SystemExit) as e:
                g.hot_path()
            self.assertIn("no hot list " + os.path.join(game, "hot.txt"), str(e.exception))
            for bad in (b"HOTX\0\0\0\0", b"HOTF\x01\0\0\0", b"HOTF\x01\0\0\0\x01\0\0\0\0"):
                with open(os.path.join(game, "hot.txt"), "wb") as f:
                    f.write(bad)
                with self.assertRaises(SystemExit) as e:
                    g.hot_list()
                self.assertIn("is not a hot list", str(e.exception))
            with open(os.path.join(game, "hot.txt"), "wb") as f:
                f.write(b"HOTF\x02\0\0\0\x40\xab\x04\x80\x80\xbb\x09\x80")
            self.assertEqual(g.hot_list(), [0x8004ab40, 0x8009bb80])


if __name__ == "__main__":
    unittest.main()
