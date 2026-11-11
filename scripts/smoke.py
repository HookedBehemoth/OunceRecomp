#!/usr/bin/env python3
"""Smoke check of one game run (scripts/smoke.sh): did anything blow up? Prints a short verdict, never the log.

  smoke.py <run dir> <golden dir> [--golden]

Checks, against the golden dir (work/smoke/<scene>/: game-derived, never committed):
  - fatal signs in log.txt (device lost, Vulkan errors, crashes, traps) and a run that stopped
    presenting;
  - pictures (from present 900 on, SMOKE_SKIP): a 64-bit DCT perceptual hash of each screenshot vs the golden run's shot of the same present number
    (Hamming distance; two near-black shots count as equal, one black and one not as a failure);
  - rough performance and memory: steady fps and replay times from the renderer's own log lines, peak RSS, the least
    free memory and the most shared memory (layer 0's guest window) the sampler saw; shown beside the golden run's;
  - warnings and errors: log lines that look like one, normalised (numbers and addresses masked), minus the known ones
    (known.txt: one normalised line per entry, or "re:<regex>"; '#' comments). New groups are printed, counted.
--golden: adds this run to the golden set (hashes, numbers, and every warning group as known); GOLDEN=reset starts
  the set anew. Record 2-3 golden runs for a scene that moves while standing (Kokiri's idle pose).
Exit status 0 = PASS, 1 = FAIL (fatal, pictures, a stall, or new error groups; new warnings alone do not fail)."""
import json
import os
import re
import statistics
import sys

import numpy as np
from PIL import Image

FATAL = re.compile(r"device lost|DEVICE_LOST|VK_ERROR_|segmentation fault|SIGSEGV|SIGABRT|"
                   r"a64_trap|unhandled exception|stack overflow|terminate called|Assertion .* failed", re.I)
WARN = re.compile(r"\b(warn(ing)?|error|fail(ed|s|ure)?|assert|abort|unhandled|unimplemented|stub(bed)?|timeout|"
                  r"timed out|invalid|mismatch(es)?|stale|lost)\b", re.I)
ERRISH = re.compile(r"\b(error|assert|abort|unhandled|lost)\b", re.I)
HASH_BAD = 12      # of 64 bits
SKIP_BEFORE = int(os.environ.get("SMOKE_SKIP", "900"))   # (presents: shots during the load differ run to run)
DARK = 8.0         # mean luminance below which a shot counts as black


def norm(line):
    line = re.sub(r"^\[[^\]]*\]\s*", "", line.rstrip("\n"))
    line = re.sub(r"0x[0-9a-fA-F]+", "#", line)
    line = re.sub(r"\b[0-9a-f]{8,}\b", "#", line)
    return re.sub(r"\d+(\.\d+)?", "N", line)[:200]


_dct = None


def phash(path):
    global _dct
    im = Image.open(path).convert("L").resize((32, 32), Image.LANCZOS)
    a = np.asarray(im, dtype=np.float64)
    if _dct is None:
        n = np.arange(32)
        _dct = np.cos(np.pi * (2 * n[None, :] + 1) * n[:, None] / 64)
    d = (_dct @ a @ _dct.T)[:8, :8].flatten()
    med = np.median(d[1:])
    return int("".join("1" if v > med else "0" for v in d), 2), float(a.mean())


def shots(run):
    out = {}
    sd = os.path.join(run, "shots")
    for f in sorted(os.listdir(sd)) if os.path.isdir(sd) else []:
        m = re.match(r"vk_(\d+)\.png$", f)
        if m:
            try:
                out[int(m.group(1))] = phash(os.path.join(sd, f))
            except Exception:
                out[int(m.group(1))] = (None, 0.0)
    return out


def numbers(run):
    log = os.path.join(run, "log.txt")
    fps, submit, draw, presents = [], [], [], 0
    for line in open(log, errors="replace") if os.path.exists(log) else []:
        m = re.search(r"vk: (\d+) presents, ([\d.]+) fps", line)
        if m:
            presents = int(m.group(1))
            fps.append(float(m.group(2)))
        m = re.search(r"replay time \(ms\): submit ([\d.]+).*?draw ([\d.]+)", line)
        if m:
            submit.append(float(m.group(1)))
            draw.append(float(m.group(2)))
    tail = lambda v: round(statistics.median(v[-4:]), 2) if v else None   # (steady: the last few lines)
    rss = 0
    tl = os.path.join(run, "timeline.txt")
    for line in open(tl) if os.path.exists(tl) else []:
        p = line.split()
        if len(p) >= 3:
            rss = max(rss, int(p[2]))
    avail, shm = None, 0
    mf = os.path.join(run, "mem.txt")
    for line in open(mf) if os.path.exists(mf) else []:
        p = line.split()
        if len(p) >= 2:
            a, s = int(p[0]), int(p[1])
            avail = a if avail is None else min(avail, a)
            shm = max(shm, s)
    return {"presents": presents, "fps": tail(fps), "submit_ms": tail(submit), "draw_ms": tail(draw),
            "rss_mb": rss, "min_avail_mb": avail, "max_shmem_mb": shm}


def groups(run):
    g, ex, fatal = {}, {}, []
    log = os.path.join(run, "log.txt")
    for line in open(log, errors="replace") if os.path.exists(log) else []:
        if FATAL.search(line) and len(fatal) < 5:
            fatal.append(line.strip()[:200])
        if WARN.search(line):
            k = norm(line)
            g[k] = g.get(k, 0) + 1
            ex.setdefault(k, line.strip()[:200])
    return g, ex, fatal


def load_known(gd):
    exact, res = set(), []
    p = os.path.join(gd, "known.txt")
    for line in open(p) if os.path.exists(p) else []:
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        if line.startswith("re:"):
            res.append(re.compile(line[3:]))
        else:
            exact.add(line)
    return lambda k: k in exact or any(r.search(k) for r in res)


def main():
    run, gd = sys.argv[1], sys.argv[2]
    golden = "--golden" in sys.argv
    os.makedirs(gd, exist_ok=True)
    num = numbers(run)
    sh = shots(run)
    g, ex, fatal = groups(run)
    status = open(os.path.join(run, "run.out")).read().strip().splitlines()[-1:] if os.path.exists(
        os.path.join(run, "run.out")) else []
    if golden:
        gp = os.path.join(gd, "golden.json")
        runs = []
        if os.path.exists(gp) and os.environ.get("GOLDEN") != "reset":
            old = json.load(open(gp))
            runs = old.get("runs", [old] if "shots" in old else [])
        runs.append({"numbers": num, "shots": {str(k): [h, m] for k, (h, m) in sh.items()}})
        json.dump({"runs": runs}, open(gp, "w"), indent=1)
        with open(os.path.join(gd, "known.txt"), "a") as f:
            f.write("# from golden run %s\n" % os.path.basename(run))
            known = load_known(gd)
            for k in sorted(g):
                if not known(k):
                    f.write(k + "\n")
        print("golden recorded (%d golden run(s) now): %d shots, %d warning groups as known; %s" % (
            len(runs), len(sh), len(g), json.dumps(num)))
        if fatal:
            print("NOTE: the golden run has fatal lines:\n  " + "\n  ".join(fatal))
        return 0
    gj = json.load(open(os.path.join(gd, "golden.json"))) if os.path.exists(os.path.join(gd, "golden.json")) else None
    gruns = (gj.get("runs", [gj] if "shots" in gj else []) if gj else [])
    gj = gruns[-1] if gruns else None
    fails = []
    print("run: %s" % (status[0] if status else os.path.basename(run)))
    if fatal:
        fails.append("fatal")
        print("FATAL (%d shown):\n  %s" % (len(fatal), "\n  ".join(fatal)))
    # pictures
    if gj:
        # (several golden runs: a shot passes when it is close to any of them, so a scene's own run-to-run motion,
        # e.g. an idle pose's clock, is in the golden set rather than a failure)
        gsets = [{int(k): v for k, v in r["shots"].items()} for r in gruns]
        gs = gsets[-1]
        common = sorted(p for p in set(gs) & set(sh) if p >= SKIP_BEFORE)
        bad, worst = [], (0, None)
        for p in common:
            h, m = sh[p]
            best = None
            for gset in gsets:
                if p not in gset:
                    continue
                gh, gm = gset[p]
                if h is None or gh is None:
                    why = (99, "unreadable")
                elif m < DARK and gm < DARK:
                    why = (0, None)
                elif (m < DARK) != (gm < DARK):
                    why = (98, "black vs not (mean %.0f vs %.0f)" % (m, gm))
                else:
                    d = bin(h ^ gh).count("1")
                    why = (d, "hash distance %d" % d if d > HASH_BAD else None)
                best = why if best is None or why[0] < best[0] else best
            if best[0] < 98 and (worst[1] is None or best[0] > worst[0]):
                worst = (best[0], p)
            if best[1]:
                bad.append((p, best[1]))
        missing = sorted(p for p in set(gs) - set(sh) if p >= SKIP_BEFORE)
        print("pictures: %d compared against %d golden run(s), worst distance %d (present %s), %d bad%s" % (
            len(common), len(gsets), worst[0], worst[1], len(bad), ", %d golden shots missing" % len(missing) if missing else ""))
        for p, why in bad[:6]:
            print("  present %d: %s" % (p, why))
        if bad:
            fails.append("pictures")
        if missing and len(missing) > len(gs) // 4:
            fails.append("stall (few shots)")
    else:
        print("pictures: no golden run (scripts/smoke.sh with GOLDEN=1)")
    # numbers
    gn = gj["numbers"] if gj else {}
    cols = ["presents", "fps", "submit_ms", "draw_ms", "rss_mb", "min_avail_mb", "max_shmem_mb"]
    print("numbers:  " + "  ".join("%s %s%s" % (c, num.get(c), " (golden %s)" % gn.get(c) if gn else "") for c in cols))
    if gn and gn.get("presents") and num["presents"] < 0.6 * gn["presents"]:
        fails.append("stall (presents)")
    # warnings
    known = load_known(gd)
    new = sorted(((n, k) for k, n in g.items() if not known(k)), reverse=True)
    newerr = [x for x in new if ERRISH.search(x[1])]
    print("log: %d warning/error groups, %d known, %d new (%d error-like)" % (len(g), len(g) - len(new), len(new),
                                                                          len(newerr)))
    for n, k in new[:12]:
        print("  %5d x %s" % (n, ex[k]))
    if newerr:
        fails.append("new errors")
    print("VERDICT: %s" % ("FAIL (" + ", ".join(fails) + ")" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
