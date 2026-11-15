"""Compare ounce-decode with Python's enc.decode (the reference) on the game's own words.

  python3 -I -B ounce/lift/decode/tools/compare.py game IMAGE_DIR OUTDIR    every distinct word of the image's .text
                                                                          segments: entry, every field and value

It also writes OUTDIR/text_in_order.bin, every .text word in address order (the benchmark's input:
ounce-decode-check bench). OUTDIR holds the words and the Rust output: game-derived, keep it under work/. Prints the differences by class.
"""
import collections
import json
import multiprocessing
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CRATE = os.path.dirname(HERE)
ROOT = os.path.abspath(os.path.join(CRATE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
from lift2 import enc  # noqa: E402

TOOL = os.path.join(CRATE, "target", "release", "ounce-decode-check")
INDEX = {id(e): k for k, e in enumerate(enc.ENTRIES)}


def py_text(words):
    out = []
    for w in words.tolist():
        i = enc.decode(w)
        out.append("None" if i is None else " ".join([i.entry.name] + ["%s=%d" % kv for kv in i.f.items()]))
    enc._cache.clear()
    return out


def parallel(fn, words, procs=6):
    chunks = np.array_split(words, procs * 4)
    with multiprocessing.Pool(procs) as pool:
        parts = pool.map(fn, chunks)
    return parts


def main():
    mode, outdir = sys.argv[1], sys.argv[3]
    assert mode == "game", __doc__
    os.makedirs(outdir, exist_ok=True)
    wpath = os.path.join(outdir, "words.bin")
    img = sys.argv[2]
    meta = json.load(open(os.path.join(img, "image.json")))
    mem = np.fromfile(os.path.join(img, "image.bin"), dtype=np.uint8)
    parts = []
    for m in meta["modules"]:
        for s in m["segments"]:
            if s["name"] == ".text":
                off = s["addr"] - meta["base"]
                parts.append(mem[off:off + s["size"] // 4 * 4].view("<u4"))
    np.concatenate(parts).astype("<u4").tofile(os.path.join(outdir, "text_in_order.bin"))
    words = np.unique(np.concatenate(parts)).astype("<u4")
    words.tofile(wpath)
    tpath = os.path.join(outdir, "rust.txt")
    subprocess.run([TOOL, "text", wpath, tpath], check=True)
    rs = open(tpath).read().splitlines()
    py = [line for part in parallel(py_text, words) for line in part]
    assert len(rs) == len(py) == len(words)
    bad = [k for k in range(len(words)) if rs[k] != py[k]]
    print("%d distinct .text words, %d decoded by Python, %d differ (entry, fields or values)"
          % (len(words), sum(1 for x in py if x != "None"), len(bad)))
    by = collections.Counter((py[k].split()[0], rs[k].split()[0]) for k in bad)
    for (p, r), n in by.most_common(40):
        ex = [(hex(int(words[k])), py[k], rs[k]) for k in bad if py[k].split()[0] == p and rs[k].split()[0] == r][:2]
        print("  python %-16s rust %-16s %8d  e.g. %s" % (p, r, n, ex))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
