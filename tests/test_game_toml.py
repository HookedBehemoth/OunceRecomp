"""games/<game>/game.toml holds the same data as games/<game>/game.py (nx2/gamedesc.py) until game.py is dropped:
every field is compared. Needs no dump.
python -I -B tests/test_game_toml.py
"""
import os
import sys
import tomllib
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
import gamedesc  # noqa: E402

GAMES = os.path.join(ROOT, "games")


class GameToml(unittest.TestCase):
    def test_every_game_matches(self):
        names = sorted(n for n in os.listdir(GAMES) if os.path.exists(os.path.join(GAMES, n, "game.toml")))
        self.assertIn("oot", names)
        for name in names:
            with self.subTest(game=name):
                with open(os.path.join(GAMES, name, "game.toml"), "rb") as f:
                    t = tomllib.load(f)
                g = gamedesc.load(os.path.join(GAMES, name))
                self.assertEqual(set(t), {"name", "labels", "hot", "host", "identity", "local_continuations"})
                self.assertEqual(t["name"], g.name)
                self.assertEqual(t["labels"], g._labels)
                self.assertEqual(t["hot"], g.hot)
                self.assertEqual(t["host"], list(g.host))
                self.assertEqual(set(t["identity"]), {"build_id", "main_sha256"})
                self.assertEqual(t["identity"]["build_id"], g.identity.build_id)
                self.assertEqual(t["identity"]["main_sha256"], g.identity.main_sha256)
                cont = {}
                for row in t["local_continuations"]:
                    self.assertEqual(set(row), {"pc", "word"})
                    self.assertNotIn(row["pc"], cont)
                    cont[row["pc"]] = row["word"]
                self.assertEqual(cont, g.local_continuations)


if __name__ == "__main__":
    unittest.main()
