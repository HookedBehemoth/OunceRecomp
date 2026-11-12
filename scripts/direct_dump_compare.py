#!/usr/bin/env python3
"""Line up a frozen back end's frame dump (RX_VK_DUMP=<present>, gpu/vk/vk_dump.cpp) with a direct back end's
(HOS_DBG=dump:<present>, gpu/direct/direct_dump.cpp), pass by pass (docs/direct/m2-interface.md, M2a).

Both write one log line per dumped target, "vk: dump NNN <what>: tex <gpu> format <VkFormat> <w>x<h>: ...". The
passes' targets ("pass c<i> ..." and "pass depth ...") are taken from each log in order, each as (kind, format, size),
and aligned by their order (difflib); dispatch and sampled-texture items of the frozen dump are left out.

    scripts/direct_dump_compare.py <frozen run>/log.txt <direct run>/log.txt [--all]

Prints a summary (targets per side, aligned, only in one side, and per (kind, format, size) the counts of both) and,
with --all, every aligned row.
"""
import argparse
import collections
import difflib
import re
import sys

LINE = re.compile(r"vk: dump (\d+) (pass (c\d+|depth)[^:]*): tex (0x[0-9a-f]+) format (\d+) (\d+)x(\d+):(.*)")
NAMES = {37: "RGBA8", 43: "RGBA8_SRGB", 44: "BGRA8", 64: "A2B10G10R10", 76: "R16F", 83: "RG16F", 97: "RGBA16F",
         98: "R32UI", 100: "R32F", 109: "RGBA32F", 122: "B10G11R11F", 124: "D16", 126: "D32F", 129: "S8",
         130: "D32F_S8"}


def read(path):
    """The pass targets, and the frames the dump holds (the frozen back end logs a line a frame while it dumps; the
    direct one names its count)"""
    out, frames = [], 0
    with open(path, errors="replace") as f:
        for line in f:
            d = re.search(r"\(direct: (\d+) frames", line)
            if d:
                frames = int(d.group(1))
            elif "vk: dump of the frame after present" in line:
                frames += 1
            m = LINE.search(line)
            if not m:
                continue
            seq, what, slot, gpu, fmt, w, h, rest = m.groups()
            kind = "depth" if slot == "depth" else "colour"
            out.append({"seq": int(seq), "what": what, "kind": kind, "slot": 99 if slot == "depth" else int(slot[1:]), "gpu": gpu, "format": int(fmt), "w": int(w),
                        "h": int(h), "rest": rest.strip()})
    return out, max(1, frames)


def passes(items):
    """Items grouped into passes (a pass's targets come in slot order, depth last), and runs of passes over the same
    targets (the frozen back end ends and begins a rendering again inside one SetRenderTargets) merged into one"""
    out = []
    for it in items:
        if not out or it["slot"] <= out[-1][-1]["slot"]:
            out.append([])
        out[-1].append(it)
    merged = []
    for p in out:
        key = tuple((t["slot"], t["gpu"]) + sig(t) for t in p)
        if merged and merged[-1][0] == key:
            merged[-1][1] += 1
            continue
        merged.append([key, 1, p])
    return merged


def psig(p):
    return tuple(sig(t) for t in p[2])


def pshow(s):
    return " + ".join(show(t) for t in s)


def sig(it):
    return (it["kind"], it["format"], it["w"], it["h"])


def show(s):
    k, f, w, h = s
    return "%s %s %dx%d" % (k, NAMES.get(f, str(f)), w, h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frozen")
    ap.add_argument("direct")
    ap.add_argument("--all", action="store_true", help="every aligned row")
    ap.add_argument("--frames", type=int, nargs=2, metavar=("FROZEN", "DIRECT"), help="the frames each dump holds, if the logs do not say")
    a = ap.parse_args()
    (fz, ff), (dr, df) = read(a.frozen), read(a.direct)
    if a.frames:
        ff, df = a.frames
    print("frames dumped: frozen %d, direct %d (counts below are a frame)" % (ff, df))
    if not fz or not dr:
        print("no pass targets in %s" % (a.frozen if not fz else a.direct))
        return 1
    fs, ds = [sig(x) for x in fz], [sig(x) for x in dr]
    sm = difflib.SequenceMatcher(None, fs, ds, autojunk=False)
    rows, matched, only_f, only_d = [], 0, 0, 0
    for op, i0, i1, j0, j1 in sm.get_opcodes():
        if op == "equal":
            matched += i1 - i0
            rows += [(fz[i], dr[j], "=") for i, j in zip(range(i0, i1), range(j0, j1))]
            continue
        only_f += i1 - i0
        only_d += j1 - j0
        for k in range(max(i1 - i0, j1 - j0)):
            rows.append((fz[i0 + k] if i0 + k < i1 else None, dr[j0 + k] if j0 + k < j1 else None, "x"))
    print("pass targets: frozen %d, direct %d; aligned %d, only frozen %d, only direct %d" % (len(fz), len(dr), matched, only_f, only_d))
    cf, cd = collections.Counter(fs), collections.Counter(ds)
    print("%-34s %7s %7s" % ("target (kind format size)", "frozen", "direct"))
    for s in sorted(set(cf) | set(cd), key=lambda s: (-max(cf[s], cd[s]), s)):
        mark = "" if cf[s] == cd[s] else "  <-"
        print("%-34s %7d %7d%s" % (show(s), cf[s], cd[s], mark))
    same = sum(min(cf[s], cd[s]) for s in cf)
    print("by signature: %d of %d frozen targets have a direct one of the same kind, format and size" % (same, len(fz)))
    pf, pd = passes(fz), passes(dr)
    psf, psd = [psig(p) for p in pf], [psig(p) for p in pd]
    sm = prow = None
    sm = difflib.SequenceMatcher(None, psf, psd, autojunk=False)
    pm = sum(i1 - i0 for op, i0, i1, j0, j1 in sm.get_opcodes() if op == "equal")
    print("passes (runs over the same targets merged): frozen %d (from %d renderings), direct %d (from %d); aligned %d" %
          (len(pf), sum(p[1] for p in pf), len(pd), sum(p[1] for p in pd), pm))
    pcf, pcd = collections.Counter(psf), collections.Counter(psd)
    kinds = sorted(set(pcf) | set(pcd), key=lambda s: (-max(pcf[s] / ff, pcd[s] / df), s))
    same = [s for s in kinds if abs(pcf[s] / ff - pcd[s] / df) < 1e-9]
    print("distinct passes (by their targets' kinds, formats and sizes): frozen %d, direct %d, in both %d; with the same "
          "count a frame %d" % (len(pcf), len(pcd), len(set(pcf) & set(pcd)), len(same)))
    print("  frozen  direct  a frame, where they differ:")
    for s in kinds:
        if s not in same:
            print("  %6.2f  %6.2f  %s" % (pcf[s] / ff, pcd[s] / df, pshow(s)))
    if a.all:
        print("passes, aligned (= same targets, x not):")
        for op, i0, i1, j0, j1 in sm.get_opcodes():
            for k in range(max(i1 - i0, j1 - j0)):
                f = pf[i0 + k] if i0 + k < i1 else None
                d = pd[j0 + k] if j0 + k < j1 else None
                print("%s %-60s | %s" % ("=" if op == "equal" else "x",
                                         "%03d x%d %s" % (f[2][0]["seq"], f[1], pshow(psig(f))) if f else "",
                                         "%03d x%d %s" % (d[2][0]["seq"], d[1], pshow(psig(d))) if d else ""))
    if a.all:
        for f, d, m in rows:
            print("%s %-44s | %s" % (m, "%03d %s" % (f["seq"], show(sig(f))) if f else "", "%03d %s" % (d["seq"], show(sig(d))) if d else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
