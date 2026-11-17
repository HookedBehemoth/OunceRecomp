"""The host's patch declarations as the lifter reads them (`ounce-lift scan`, no dump), for the tests of the host's
patches: rows (At rows and Value probes), the replaced functions, the HLE'd functions' declared effects.

    import lift_scan; S = lift_scan.load()   (None without cargo: the caller skips)
"""
import json
import os
import shutil
import subprocess
import tempfile
from types import SimpleNamespace

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CRATE = os.path.join(ROOT, "ounce", "lift")


def binary():
    """ounce-lift, built (cargo build --release); None without cargo"""
    cargo = shutil.which("cargo") or os.path.expanduser("~/.cargo/bin/cargo")
    if not os.path.exists(cargo):
        return None
    subprocess.run([cargo, "build", "--release", "--quiet", "--manifest-path", os.path.join(CRATE, "Cargo.toml")], check=True)
    return os.path.join(ROOT, "ounce", "target", "release", "ounce-lift")


class Scan:
    def __init__(self, data):
        def row(r):
            return SimpleNamespace(pc=r["pc"], word=r["word"], fn=r["fn"], args=tuple(r["args"]), result=r["result"],
                                   io=tuple(r["io"]), guest=r["guest"], mode=r["mode"], where=tuple(r["where"]))
        self.AT = [row(r) for r in data["rows"] if not r["value"]]
        self.PROBES = [row(r) for r in data["rows"] if r["value"]]
        self.funcs = [SimpleNamespace(name=f["name"], fn=f["fn"], where=tuple(f["where"]), guest=f["guest"],
                                      reads=(lambda r=frozenset(f["reads"]): set(r))) for f in data["funcs"]]
        self._effects = {}
        for kind in ("nv", "declared", "hle"):   # (effects(): the HLE's first, then HLE_EFFECTS, then NV)
            for sym, r in data[kind].items():
                self._effects[sym] = SimpleNamespace(reads=tuple(r["reads"]), clobbers=r["clobbers"], where=tuple(r["where"]))

    def effects(self, symbol):
        """the declaration of a replaced function (its HLE, HLE_EFFECTS or NV), or None"""
        return self._effects.get(symbol)


def load():
    exe = binary()
    if exe is None:
        return None
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "scan.json")
        subprocess.run([exe, "scan", "--root", ROOT, "--game", os.path.join(ROOT, "games", "oot"), out], check=True)
        with open(out) as f:
            return Scan(json.load(f))


def rt64(folder):
    """the guest model (a64.h, a64_rt.c) written into folder (`ounce-lift rt64`); its path"""
    subprocess.run([binary(), "rt64", folder, "--root", ROOT], check=True)
    return folder
