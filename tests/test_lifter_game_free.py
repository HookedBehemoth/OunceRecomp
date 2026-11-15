"""The lifter (nx2/lift2) holds no fact of one game: its code names no game address, no game function's label and no
game symbol. A game's facts are in its description (games/<game>/game.py, nx2/gamedesc.py) and its patches are
declared in the host's C (host/patch.h, read by nx2/lift2/patch_scan.py).

Checked on the code's tokens (comments and docstrings are documentation and left out):
- a hexadecimal literal of 8 digits from 0x80000000 to 0x8fffffff (the game image's addresses) is one of ALLOWED, each
  an encoding or a constant of the architecture, with its reason;
- no string holds a lifted function's name (f_8xxxxxxx, entry_0x8xxxxxxx) or a guest address of that range;
- no name or string is a label of a game's labels.txt (the game's functions, as this project names them);
- a mangled C++ symbol in a string is the SDK's or the C++ runtime's (nn::, std::), never the game's.
No game is used.

python -I -B tests/test_lifter_game_free.py
"""
import ast
import glob
import io
import os
import re
import tokenize
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIFTER = os.path.join(ROOT, "nx2", "lift2")

# value: why it is in the lifter
ALLOWED = {
    0x80000000: "bit 31 of an instruction word (the BL opcode bit, the sign of a 32-bit value)",
    0x8B000000: "the encoding of ADD (shifted register)",
    0x8B200000: "the encoding of ADD (extended register)",
    0x82F63B78: "the CRC32C polynomial (reversed)",
    0x8444C004: "the FPCR value dynarmic's configuration reports",
}
ADDR = re.compile(r"\b(?:f_|body_|entry_0x|0x)8[0-9a-fA-F]{7}\b")
SDK = re.compile(r"^_Z(?:N|NK)?(?:2nn|St|NSt)")


def labels():
    out = set()
    for path in glob.glob(os.path.join(ROOT, "games", "*", "labels.txt")):
        with open(path, encoding="utf-8") as f:
            for line in f:
                w = line.split("#", 1)[0].split()
                if len(w) >= 2:
                    out.add(w[1])
    return out


def code_tokens(path):
    """(kind, text, line) of a source's tokens outside comments and docstrings"""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    docs = set()
    for n in ast.walk(ast.parse(text)):
        body = getattr(n, "body", None)
        if isinstance(body, list) and body and isinstance(body[0], ast.Expr) and \
                isinstance(body[0].value, ast.Constant) and isinstance(body[0].value.value, str):
            docs.add(body[0].value.lineno)
    for t in tokenize.generate_tokens(io.StringIO(text).readline):
        if t.type == tokenize.COMMENT or (t.type == tokenize.STRING and t.start[0] in docs):
            continue
        yield t.type, t.string, t.start[0]


class GameFree(unittest.TestCase):
    def test_lifter(self):
        names = labels()
        self.assertGreater(len(names), 500)
        bad, seen = [], set()
        files = sorted(glob.glob(os.path.join(LIFTER, "*.py")))
        self.assertGreater(len(files), 20)
        for path in files:
            rel = os.path.relpath(path, ROOT)
            for kind, s, line in code_tokens(path):
                where = "%s:%d" % (rel, line)
                if kind == tokenize.NUMBER and re.fullmatch(r"0[xX]8[0-9a-fA-F]{7}", s):
                    v = int(s, 16)
                    seen.add(v)
                    if v not in ALLOWED:
                        bad.append("%s: a game address %s" % (where, s))
                elif kind == tokenize.STRING:
                    if ADDR.search(s):
                        bad.append("%s: a game address in %s" % (where, s[:60]))
                    for w in re.findall(r"[A-Za-z_]\w*", s):
                        if w in names:
                            bad.append("%s: the game label %s" % (where, w))
                        if w.startswith("_Z") and not SDK.match(w):
                            bad.append("%s: a game symbol %s" % (where, w))
                elif kind == tokenize.NAME and s in names:
                    bad.append("%s: the game label %s" % (where, s))
        self.assertFalse(bad, "\n".join(bad))
        self.assertFalse(set(ALLOWED) - seen, "allowed but no longer used: %s" % sorted(map(hex, set(ALLOWED) - seen)))


if __name__ == "__main__":
    unittest.main()
