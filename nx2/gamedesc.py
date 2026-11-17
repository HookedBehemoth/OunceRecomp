"""The game description: what the lift and the build take from one game (games/<game>/game.py, docs/lifter.md "The
game boundary"). The lifter (nx2/lift2) holds no fact of one game; everything it needs of the game is here or in the
game's patch declarations (host/patch.h).

A description is a Python file of data and constructor calls: `GAME = Game(...)`. nx2/lift.py and the RE tools take
exactly one (--game games/<game>); use() makes it the process's description, and its path goes to the worker
processes and the summary job in HOOKSHOT_GAME (set by use(), the transport of the one passed, never a default).
current() without one stops.

Fields:
  name          the folder's name
  identity      Identity(build_id, main_sha256): the main module's build id (the NSO header's) and the SHA-256 of
                exefs/main. The lift checks the dump before it reads anything else (check_exefs) and records it
                (record); the build checks the record (check_lift). A mismatch stops with both values.
  labels        guest address -> label file, beside game.py (nx2/label_syms.py, nx2/lift2/dump.py, abi_census.py)
  hot           the hot list's file, beside game.py (docs/lifter.md, "Game description files")
  hot_order     the link order's file, beside game.py (the same section)
  host          the host's C files (in host/) the build compiles and the patch scan reads
  local_continuations   {pc: word}: `adr xN, <label>` ... `br xN` inside one function, which the code pointer audit
                accepts (nx2/lift2/code_ptrs.py)
"""
import hashlib
import importlib.util
import json
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, "host")
ENV = "HOOKSHOT_GAME"


class Identity:
    def __init__(self, build_id, main_sha256):
        self.build_id, self.main_sha256 = build_id.lower(), main_sha256.lower()

    def as_dict(self):
        return {"build_id": self.build_id, "main_sha256": self.main_sha256}


class Game:
    def __init__(self, name, identity, labels, hot, host, local_continuations=None, hot_order=None):
        self.name, self.identity, self.hot, self.hot_order = name, identity, hot, hot_order
        self._labels, self.host = labels, tuple(host)
        self.local_continuations = dict(local_continuations or {})
        self.folder = None   # (load sets it)

    @property
    def labels(self):
        """the label file's path"""
        return os.path.join(self.folder, self._labels)

    def host_sources(self):
        """the host's C files, as paths, in the build's order"""
        return [os.path.join(HOST, f) for f in self.host]

    def hot_path(self):
        """the hot list's file, beside game.py; SystemExit when it does not exist"""
        p = os.path.join(self.folder, self.hot)
        if not os.path.exists(p):
            raise SystemExit("game %s: no hot list %s (the description's hot, beside game.py)" % (self.name, p))
        return p

    def hot_list(self):
        """the hot list's guest addresses, hottest first; SystemExit when the file is missing or malformed"""
        return self._addresses(self.hot_path(), b"HOTF", "hot list")

    def link_order(self):
        """the link order's guest addresses (hot_order), [] when the description names none; SystemExit when the file is
        missing or malformed"""
        if not self.hot_order:
            return []
        p = os.path.join(self.folder, self.hot_order)
        if not os.path.exists(p):
            raise SystemExit("game %s: no link order %s (the description's hot_order, beside game.py)" % (self.name, p))
        return self._addresses(p, b"HOTO", "link order")

    def _addresses(self, p, sig, what):
        with open(p, "rb") as f:
            d = f.read()
        n = struct.unpack_from("<I", d, 4)[0] if len(d) >= 8 and d[:4] == sig else -1
        if n < 0 or len(d) != 8 + 4 * n:
            raise SystemExit("game %s: %s is not a %s (\"%s\", u32 count, count u32 addresses)" % (self.name, p, what, sig.decode()))
        return list(struct.unpack_from("<%dI" % n, d, 8))

    def check_exefs(self, exefs):
        """SystemExit unless exefs/main is this game's (its build id, then its SHA-256); returns the Identity seen"""
        path = os.path.join(exefs, "main")
        with open(path, "rb") as f:
            data = f.read()
        if data[:4] != b"NSO0":
            raise SystemExit("game %s: %s is not an NSO" % (self.name, path))
        seen = Identity(data[0x40:0x54].hex(), hashlib.sha256(data).hexdigest())
        for what in ("build_id", "main_sha256"):
            if getattr(seen, what) != getattr(self.identity, what):
                raise SystemExit("game %s: the dump is another game or version: main's %s is %s, the description's %s"
                                 % (self.name, what, getattr(seen, what), getattr(self.identity, what)))
        return seen

    def record(self, folder):
        """the lift's record of the game it lifted (folder/game.json), for the build's check"""
        with open(os.path.join(folder, "game.json"), "w") as f:
            json.dump({"name": self.name, **self.identity.as_dict()}, f)

    def check_lift(self, folder):
        """SystemExit unless the lift in folder is of this game (its game.json)"""
        path = os.path.join(folder, "game.json")
        if not os.path.exists(path):
            raise SystemExit("game %s: %s has no record of the game it lifted: lift again (scripts/relift.sh)"
                             % (self.name, folder))
        with open(path) as f:
            seen = json.load(f)
        want = {"name": self.name, **self.identity.as_dict()}
        for k, v in want.items():
            if seen.get(k) != v:
                raise SystemExit("game %s: the lift in %s is of another game or version: its %s is %s, the "
                                 "description's %s" % (self.name, folder, k, seen.get(k), v))


def load(path):
    """the Game of games/<game> (or its game.py)"""
    p = os.path.abspath(path)
    if os.path.isdir(p):
        p = os.path.join(p, "game.py")
    if not os.path.exists(p):
        raise SystemExit("no game description at %s" % path)
    spec = importlib.util.spec_from_file_location("game_description_%s" % os.path.basename(os.path.dirname(p)), p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    g = getattr(mod, "GAME", None)
    if not isinstance(g, Game):
        raise SystemExit("%s defines no GAME = Game(...)" % p)
    g.folder = os.path.dirname(p)
    g.path = p
    return g


_CURRENT = None


def use(path):
    """the Game at path, as this process's description and its workers' (HOOKSHOT_GAME)"""
    global _CURRENT
    _CURRENT = load(path)
    os.environ[ENV] = _CURRENT.path
    return _CURRENT


def current():
    """the process's description (use(), or the one the parent process passed in HOOKSHOT_GAME); SystemExit if none"""
    global _CURRENT
    if _CURRENT is None:
        p = os.environ.get(ENV)
        if not p:
            raise SystemExit("no game description: nx2/lift.py takes --game games/<game>")
        _CURRENT = load(p)
    return _CURRENT


def add_argument(parser, required=True):
    parser.add_argument("--game", required=required, help="the game description, games/<game> (nx2/gamedesc.py)")
