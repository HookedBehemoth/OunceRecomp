#!/usr/bin/env python3
"""(M4f, M6b) Per-program draw and dispatch counts of one frame, frozen against direct, with a verdict.

    scripts/direct_trace_compare.py <frozen draw_stats_N.txt> <direct log.txt> [--gated drawn|skip] [--all]

The frozen side is the frozen back end's RX_VK_DRAW_STATS file (touch <dir>/draw_stats.now once in play): one line per program
with the draws of one frame. The frozen back end evaluates render conditions on the CPU and leaves a gated draw out when its
condition fails: those draws are not in its count. RX_VK_COND_ALWAYS=1 makes every condition hold (every gated draw drawn
and counted). The direct side is the direct back end's HOS_DBG=drawlog:<present> trace: every draw recorded in the frame, a
gated one marked "drawn gated" (M6b; its condition is evaluated on the GPU, so it is recorded whatever the condition).

--gated drawn (default) compares the frozen counts with every direct draw: right for a frozen run with RX_VK_COND_ALWAYS=1.
--gated skip leaves the direct gated draws out: an upper bound check for a frozen run with conditions evaluated (the
frozen count must then lie between the direct ungated and the direct total for each program).

Verdict lines: per program "same", "frozen more", "direct more" (and, with --gated skip, "within": between the two direct
counts). Exit 1 when any program differs (or lies outside), 0 when all agree.
"""
import argparse
import collections
import re
import sys


def read_frozen(path):
    F = {}
    frame = None
    for line in open(path, errors="replace"):
        h = re.match(r"# frame (\d+): (\d+) draws", line)
        if h:
            frame = int(h.group(1))
        m = re.match(r"(draw|mesh|comp) ([0-9a-f]{16})\s+(\d+) \| (.*)", line)
        if m:
            F[m.group(2)] = (m.group(1), int(m.group(3)), m.group(4).strip())
    return F, frame


def read_direct(path):
    D, G, K, skip, frames = collections.Counter(), collections.Counter(), {}, collections.Counter(), set()
    for line in open(path, errors="replace"):
        if "drawlog" not in line:
            continue
        m = re.search(r"drawlog dispatch .* prog ([0-9a-f]{16})", line)
        if m:
            D[m.group(1)] += 1
            K[m.group(1)] = "comp"
            continue
        m = re.search(r"kind (\d+) rt \d+ (\d+x\d+) c0 (-?\d+): (drawn[a-z+ ]*) prog ([0-9a-f]{16})", line)
        if m:
            D[m.group(5)] += 1
            if "gated" in m.group(4):
                G[m.group(5)] += 1
            K[m.group(5)] = m.group(2) + " k" + m.group(1)
            continue
        m = re.search(r"skipped: ([^;]*);", line)
        if m:
            skip[m.group(1)] += 1
    return D, G, K, skip


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("frozen")
    ap.add_argument("direct")
    ap.add_argument("--gated", choices=("drawn", "skip"), default="drawn")
    ap.add_argument("--all", action="store_true", help="every program, the agreeing ones too")
    a = ap.parse_args()
    F, frame = read_frozen(a.frozen)
    D, G, K, skip = read_direct(a.direct)
    if not F or not D:
        print("no %s" % ("frozen counts" if not F else "direct drawlog lines"))
        return 2
    print("direct skipped draws in the frame:", dict(skip) if skip else "none")
    fd = {k for k, v in F.items() if v[0] != "comp"}
    dd = {k for k in D if K[k] != "comp"}
    fc = {k for k, v in F.items() if v[0] == "comp"}
    dc = {k for k in D if K[k] == "comp"}
    tf = sum(F[k][1] for k in fd)
    td, tg = sum(D[k] for k in dd), sum(G[k] for k in dd)
    print("frozen frame %s: %d draws of %d programs, %d dispatches of %d programs" % (frame, tf, len(fd), sum(F[k][1] for k in fc), len(fc)))
    print("direct: %d draws of %d programs (%d of them gated: recorded, the condition evaluated on the GPU; %d ungated), %d dispatches "
          "of %d programs" % (td, len(dd), tg, td - tg, sum(D[k] for k in dc), len(dc)))
    rows, bad = [], 0
    for k in sorted(fd | dd, key=lambda k: -(max(F.get(k, ("", 0, ""))[1], D.get(k, 0)))):
        f = F[k][1] if k in F else 0
        d_all, d_un = D.get(k, 0), D.get(k, 0) - G.get(k, 0)
        if a.gated == "drawn":
            st = "same" if f == d_all else "frozen more" if f > d_all else "direct more"
        else:
            st = "within" if d_un <= f <= d_all else "frozen more" if f > d_all else "direct more"
        if st not in ("same", "within"):
            bad += 1
        if st not in ("same", "within") or a.all:
            rows.append("  %-12s %016s frozen %4d direct %4d (gated %d) %s | %s" % (st, k, f, d_all, G.get(k, 0), K.get(k, F.get(k, ("",))[0]),
                                                                              F[k][2][:70] if k in F else ""))
    print("draws per program (%s): %d programs, %d differ" % ("direct gated draws counted" if a.gated == "drawn" else
                                                              "frozen between direct ungated and direct total", len(fd | dd), bad))
    print("\n".join(rows))
    cb = sorted(fc ^ dc)
    if cb:
        print("dispatch programs on one side only: %s" % " ".join("%s(%s)" % (k, "frozen" if k in fc else "direct") for k in cb))
    bad += len(cb)
    print("verdict: %s (%d differences)" % ("FAIL" if bad else "PASS", bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
