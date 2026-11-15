"""Rust partitions against the golden (ounce/lift/EXPORT.md, "Golden"): per partition equal, different or missing, and
for a different one the first differing function (in the golden's order) as a short unified diff of that function.

    work/venv/bin/python -I ounce/lift/tools/compare.py [--golden work/ounce/golden] [--lines 60] <dir of <name>.ll>

Exit status 0 when every golden partition is equal (a partition absent from the directory is "missing" and counts as
a difference unless --present-only).
"""
import argparse
import difflib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from golden import index   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


def first_difference(gold, gidx, rust):
    """(what, unified diff lines) of the first difference between two IR texts: a function in the golden's order that
    differs or is missing, else one the Rust text defines beyond the golden, else the text outside functions"""
    ridx = index(rust)
    for name, (s, e, sha) in sorted(gidx.items(), key=lambda kv: kv[1][0]):
        r = ridx.get(name)
        if r is None:
            return "%s: not defined by the Rust text" % name, []
        if r[2] != sha:
            a = gold[s:e].decode(errors="replace").splitlines(keepends=True)
            b = rust[r[0]:r[1]].decode(errors="replace").splitlines(keepends=True)
            return "%s differs" % name, list(difflib.unified_diff(a, b, "golden/" + name, "rust/" + name, n=2))
    extra = sorted(set(ridx) - set(gidx))
    if extra:
        return "%s: defined by the Rust text, not the golden (%d such)" % (extra[0], len(extra)), []

    def outside(text, idx):
        out, at = [], 0
        for s, e, _ in sorted(idx.values()):
            out.append(text[at:s])
            at = e
        out.append(text[at:])
        return b"".join(out).decode(errors="replace").splitlines(keepends=True)
    return "outside the functions (declarations, header, trailer)", list(difflib.unified_diff(
        outside(gold, gidx), outside(rust, ridx), "golden", "rust", n=1))


def main():
    from compression import zstd
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--golden", default=os.path.join(ROOT, "work", "ounce", "golden"))
    ap.add_argument("--lines", type=int, default=60, help="diff lines shown per partition")
    ap.add_argument("--present-only", action="store_true", help="compare only the partitions the directory has")
    ap.add_argument("--all", action="store_true", help="a diff for every differing partition (default: the first)")
    ap.add_argument("rust")
    a = ap.parse_args()
    meta = json.load(open(os.path.join(a.golden, "meta.json")))
    print("golden: %s (commit %s)" % (a.golden, meta["commit"]))
    counts = {"equal": 0, "different": 0, "missing": 0}
    shown = 0
    for name in meta["partitions"]:
        path = os.path.join(a.rust, name + ".ll")
        if not os.path.exists(path):
            if not a.present_only:
                counts["missing"] += 1
                print("%s: missing" % name)
            continue
        rust = open(path, "rb").read()
        gold = zstd.decompress(open(os.path.join(a.golden, name + ".ll.zst"), "rb").read())
        if rust == gold:
            counts["equal"] += 1
            print("%s: equal" % name)
            continue
        counts["different"] += 1
        gidx = json.load(open(os.path.join(a.golden, name + ".idx.json")))
        what, diff = first_difference(gold, gidx, rust)
        print("%s: different, first: %s" % (name, what))
        if shown == 0 or a.all:
            shown += 1
            sys.stdout.write("".join(diff[:a.lines]))
            if len(diff) > a.lines:
                print("... (%d more diff lines)" % (len(diff) - a.lines))
    print("compare: %(equal)d equal, %(different)d different, %(missing)d missing" % counts)
    return 0 if counts["different"] == 0 and counts["missing"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
