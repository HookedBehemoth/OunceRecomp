"""The golden of the Rust lifter's phase 1: the Python lifter's IR text of every partition (game.lift_part, verify off),
zstd-compressed, with each function's byte range and sha1 (ounce/lift/EXPORT.md, "Golden"). Made from the game: it
stays under work/.

    work/venv/bin/python -I ounce/lift/tools/golden.py --game games/oot [--jobs N] [--only lift_c000,...] work/ounce/golden

Workers are forked from a process that holds the plan (copy on write: one plan's memory, not one a worker).
"""
import hashlib
import json
import multiprocessing
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "nx2"))

DEFINE = re.compile(rb"^define [^@\n]*@([\w.$]+)\(", re.M)
_STATE = {}


def index(text):
    """{name: [start, end, sha1]} of every function the IR text defines: its define line to its closing "}" line"""
    out = {}
    for m in DEFINE.finditer(text):
        end = text.index(b"\n}\n", m.start()) + 3
        out[m.group(1).decode()] = [m.start(), end, hashlib.sha1(text[m.start():end]).hexdigest()]
    return out


def _lift(k):
    from lift2 import game
    tag, names = _STATE["parts"][k]
    t = time.time()
    text = game.lift_part((_STATE["plan_path"], names, _STATE["funcs"]))
    return k, text.encode(), time.time() - t


def main():
    import argparse
    from compression import zstd
    import lift_export
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--game", required=True)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--only", default="", help="partition names, comma-separated (default all)")
    ap.add_argument("--force", action="store_true", help="made again even if its stamp matches the inputs")
    ap.add_argument("--no-index", action="store_true", help="the IR only (a determinism run)")
    ap.add_argument("out")
    a = ap.parse_args()
    if os.environ.get("HOOKSHOT_LIFT_VERIFY"):
        raise SystemExit("golden: the golden is of the build without HOOKSHOT_LIFT_VERIFY")
    t0 = time.time()
    plan_path, plan, gen, funcs, parts = lift_export.setup(a.game)
    key = lift_export.input_key(plan, gen, funcs)
    old = os.path.join(a.out, "meta.json")
    if not a.only and not a.force and os.path.exists(old) and json.load(open(old)).get("input_key") == key:
        print("golden: %s is of these inputs (input_key %s): kept (--force to make it again)" % (a.out, key[:12]))
        return
    _STATE.update(plan_path=plan_path, funcs=funcs, parts=parts)
    names = [lift_export.part_name(tag, k) for k, (tag, _) in enumerate(parts)]
    want = [k for k, n in enumerate(names) if not a.only or n in a.only.split(",")]
    os.makedirs(a.out, exist_ok=True)
    t1 = time.time()
    secs = {}
    with multiprocessing.get_context("fork").Pool(a.jobs) as pool:
        for k, text, s in pool.imap_unordered(_lift, want):
            secs[names[k]] = round(s, 1)
            base = os.path.join(a.out, names[k])
            with open(base + ".ll.zst.tmp", "wb") as f:
                f.write(zstd.compress(text, level=9))
            os.replace(base + ".ll.zst.tmp", base + ".ll.zst")
            if not a.no_index:
                with open(base + ".idx.json", "w") as f:
                    json.dump(index(text), f, separators=(",", ":"))
            print("golden: %s %.1f MB in %.0fs" % (names[k], len(text) / 1e6, s), flush=True)
    from lift2 import game
    meta = {"commit": lift_export.commit(), "input_key": key, "code_key": game.code_key(), "facts_key": game.facts_key(plan),
            "partitions": [names[k] for k in want], "seconds": secs, "jobs": a.jobs,
            "wall seconds": round(time.time() - t1)}
    with open(os.path.join(a.out, "meta.json"), "w") as f:
        json.dump(meta, f, indent=1)
    print("golden: %d partitions in %s (setup %.0fs, lift %.0fs with %d jobs)" % (
        len(want), a.out, t1 - t0, time.time() - t1, a.jobs), flush=True)


if __name__ == "__main__":
    main()
