"""The frame's critical path from a debug.crit trace (runtime/crit.c).

    python -I tools/crit_path.py <run folder> [--marks evsig|present] [--frames N] [--top 25] [--dump F|-2 [--merged]]
    CRIT_GANTT=<frame> [CRIT_RES=0.1] [CRIT_FRAMES=1] python -I tools/crit_path.py <run folder>   (one frame as rows of characters)

The run folder holds crit.bin and crit_threads.txt. Every guest thread's time is running (it holds a virtual core: game
code, the host's NVN layer, the direct back end), blocked (on a condition variable, the kernel's arbiter, a sleep, or
another host wait) or waiting for a virtual core. A frame is the time between two marks: by default the end of the main
thread's CPU event signal with value 1 (this game starts the next frame's jobs right after it; the present runs beside it
on the Presentation Thread and gates nothing), or the presents. The critical path is walked back from the later mark: on
the thread at hand, running time is taken as it is; at a block that a wake from another thread ended (a condition
variable, the arbiter, a virtual core handed over), the path moves to the waker at the moment of the wake; a poll (sleeps
with short looks between) moves to the event's writer (the present's poll) or to the thread that had been running longest
when it ended (the job system's 20 us polls: a heuristic); any other block stays on the thread and counts as waiting.
Prints, averaged over the frames: the path by category (median 10 %, top 1 %, all), the back end's emits and calls on it by
list, the path by thread, its waits, and each thread's own time a frame. A thread that blocks outside the instrumented
waits (no virtual core given up) counts as running in the last table: read it for the game's job threads only.
"""
import argparse
import bisect
import collections
import os
import struct
import sys

import numpy as np

KIND = {0: "block cv", 1: "block arbiter", 2: "sleep", 3: "block host", 4: "vcore wait", 5: "wake",
        10: "be emit", 11: "be list_begin", 12: "be list_end", 13: "be list_call", 14: "be submit", 15: "be present",
        16: "be event_signal", 17: "be sync_wait", 18: "be asset/texels", 19: "be list_released", 20: "present"}
BLOCKS = (0, 1, 2, 3)
SITES = {1: "nvnSyncWait", 2: "nvnQueueFinish", 3: "nvnWindowAcquireTexture", 4: "nvnQueuePresentTexture", 5: "nvnQueueSubmitCommands",
         6: "fw_flush", 7: "thread join", 8: "present poll", 9: "fs open", 10: "fs read", 11: "socket poll", 12: "svc",
         14: "rx_nvn_vmap_changing"}


def load(folder):
    raw = open(os.path.join(folder, "crit.bin"), "rb").read()
    h = struct.unpack_from("<6Q", raw, 0)
    n = h[1]
    dt = np.dtype([("t0", "<u8"), ("t1", "<u8"), ("a", "<u8"), ("b", "<u8"), ("kind", "<u2"), ("thread", "<u2"), ("pad", "<u4")])
    ev = np.frombuffer(raw, dtype=dt, count=n, offset=48)
    tsc0, ns0, tsc1, ns1 = h[2], h[3], h[4], h[5]
    scale = (ns1 - ns0) / float(tsc1 - tsc0) / 1e6   # (ms per tick)
    names = {}
    for line in open(os.path.join(folder, "crit_threads.txt"), encoding="utf-8", errors="replace"):
        i, _, nm = line.rstrip("\n").partition(" ")
        names[int(i)] = nm
    return ev, tsc0, scale, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--marks", default="evsig", help="frame boundaries: evsig (the main thread's CPU event signal of value 1) or present")
    ap.add_argument("--dump", type=int, default=-1, help="print the path of this frame step by step (-2: the median frame)")
    ap.add_argument("--merged", action="store_true", help="with --dump: consecutive steps on one thread merged")
    a = ap.parse_args()
    ev, tsc0, scale, names = load(a.run)
    ms = lambda t: (np.asarray(t, dtype=np.float64) - tsc0) * scale  # noqa: E731
    t0 = ms(ev["t0"]); t1 = ms(ev["t1"])
    kind = ev["kind"]; thr = ev["thread"]; ea = ev["a"]; eb = ev["b"]
    pres = np.where(kind == 20)[0]
    pres = pres[np.argsort(t0[pres])]
    ptimes = t0[pres]
    pthread = int(thr[pres[0]])
    if a.marks == "evsig":
        # (the frame's real boundary in this game: the main thread starts the next frame's jobs right after its CPU
        # event signal with value 1, the end of its frame; the present runs beside it on the Presentation Thread)
        m = np.where((kind == 16) & (eb == 1))[0]
        m = m[np.argsort(t1[m])]
        ptimes = t1[m]
        pthread = int(thr[m[0]])
    nfr = len(ptimes) - 1
    if a.frames:
        nfr = min(nfr, a.frames)
    if a.dump == -2:   # (the frame nearest the median)
        d = np.diff(ptimes[:nfr + 1])
        a.dump = int(np.argmin(np.abs(d - np.median(d))))
    print("%d events, %d threads, %d frames (%.2f ms a frame), frame marks (%s) on %s" % (len(ev), len(names), nfr,
          (ptimes[nfr] - ptimes[0]) / nfr, a.marks, names.get(pthread)))

    # per thread: the not-running segments (blocks and core waits), sorted
    seg = collections.defaultdict(list)    # thread -> [(start, end, kind, a, b)]
    bes = collections.defaultdict(list)    # thread -> [(start, end, kind, inner)]
    wakes = collections.defaultdict(list)  # key -> [(t, thread)]
    # (the innermost list open on a thread when an emit burst or a call starts: its kind and BeginRecording lr)
    LK = {0: "packet", 1: "load", 2: "job", 3: "layer", 4: "gate", 5: "compute", 6: "head"}
    open_l = collections.defaultdict(list)
    in_list = {}
    for i in np.argsort(t0, kind="stable"):
        k = int(kind[i])
        if k == 11:
            open_l[int(thr[i])].append((int(ea[i]), int(eb[i])))
        elif k == 12:
            if open_l[int(thr[i])]:
                open_l[int(thr[i])].pop()
        elif k in (10, 13):
            st = open_l[int(thr[i])]
            in_list[i] = ("%s %#x" % (LK.get(st[-1][0], st[-1][0]), st[-1][1])) if st else "no list"
    for i in range(len(ev)):
        k = int(kind[i])
        if k in BLOCKS or k == 4:
            seg[int(thr[i])].append((t0[i], t1[i], k, int(ea[i]), int(eb[i])))
        elif k == 5:
            wakes[int(ea[i])].append((t0[i], int(thr[i])))
        elif 10 <= k <= 19:
            inner = (int(ea[i]) * scale) if k == 10 else (t1[i] - t0[i])
            bes[int(thr[i])].append((t0[i], t1[i], k, inner, in_list.get(i, "")))
    # a block still open at the window's end: its start is the thread's last core release (no segment ended after it)
    rel = np.where((kind == 5) & (ea >= 0x1000) & (ea < 0x1100))[0]
    last_rel = {}
    for i in rel:
        th = int(thr[i])
        if t0[i] > last_rel.get(th, -1e18):
            last_rel[th] = t0[i]
    for v in seg.values():
        v.sort()
    # core holding: a thread holds a virtual core from the end of its core wait to its next release
    acq_t = collections.defaultdict(list); rel_t = collections.defaultdict(list)
    for i in np.where(kind == 4)[0]:
        acq_t[int(thr[i])].append(t1[i])
    for i in rel:
        rel_t[int(thr[i])].append(t0[i])
    for v in acq_t.values():
        v.sort()
    for v in rel_t.values():
        v.sort()

    def holding_since(th, te):   # (the time it took its core, if it holds one at te; else None)
        a_ = acq_t.get(th); r_ = rel_t.get(th, [])
        if not a_:
            return None
        i = bisect.bisect_right(a_, te) - 1
        if i < 0:
            return None
        j = bisect.bisect_right(r_, te) - 1
        if j >= 0 and r_[j] > a_[i]:
            return None
        return a_[i]
    for th, tr in last_rel.items():
        v = seg[th]
        if not v or tr > v[-1][1] + 1e-6:
            v.append((tr, 1e18, 3, 0, 0))
    for v in bes.values():
        v.sort()
    for v in wakes.values():
        v.sort()
    seg_starts = {th: [s[0] for s in v] for th, v in seg.items()}
    seg_ends = {th: [s[1] for s in v] for th, v in seg.items()}
    be_starts = {th: [s[0] for s in v] for th, v in bes.items()}
    wake_ts = {k: [w[0] for w in v] for k, v in wakes.items()}

    by_list = collections.Counter()   # (emit / call, the list) -> ms, on the path only (reset after the walk)

    def be_in(th, lo, hi, acc, lists=True):   # (back-end time of th inside [lo, hi], by kind)
        v = bes.get(th)
        if not v:
            return
        i = bisect.bisect_left(be_starts[th], lo - 50.0)
        while i < len(v) and v[i][0] < hi:
            s, e, k, inner, li = v[i]
            ov = min(e, hi) - max(s, lo)
            if ov > 0:
                frac = ov / (e - s) if e > s else 1.0
                x = inner * frac if k == 10 else ov
                acc[KIND[k]] += x
                if k in (10, 13) and lists:
                    by_list[KIND[k] + " in " + li] += x
            i += 1

    path = collections.Counter()      # (thread, what) -> ms
    path_be = collections.Counter()   # (thread, be kind) -> ms
    waits = collections.Counter()     # (thread, kind, caller) -> ms
    hops = 0
    steps = []
    per = collections.defaultdict(collections.Counter)   # frame -> category -> ms

    def group(th):
        n = names.get(th, "")
        for p_ in ("sead::MainThread", "ModuleSystemWorker", "BaseProcExecutor", "NavMesh", "Presentation"):
            if n.startswith(p_):
                return p_
        return "other"

    def step(f, th, a0, a1, what):
        if f == a.dump:
            steps.append((a0, a1, names.get(th, th), what))
        if what.startswith("running"):
            per[f]["run " + group(th)] += a1 - a0
        elif "poll" in what:
            per[f]["poll latency"] += a1 - a0
        elif "vcore" in what:
            per[f]["core wait"] += a1 - a0
        else:
            per[f]["other wait"] += a1 - a0
    for f in range(nfr):
        lo, t, th = ptimes[f], ptimes[f + 1], pthread
        guard = 0
        while t > lo and guard < 100000:
            guard += 1
            ends = seg_ends.get(th, [])
            j = bisect.bisect_left(ends, t) - 1   # (the last segment ending before t)
            # a segment containing t (we are inside a block: can happen after a hop to a waker? no: wakers run)
            if j + 1 < len(ends) and seg[th][j + 1][0] < t <= seg[th][j + 1][1]:
                j += 1
            if j < 0:
                step(f, th, lo, t, "running")
                path[(th, "running")] += t - lo
                acc = collections.Counter(); be_in(th, lo, t, acc)
                for k, v in acc.items():
                    path_be[(th, k)] += v
                    per[f]["be " + k[3:]] += v
                break
            s, e, k, ka, kb = seg[th][j]
            e_eff = min(e, t)
            if e_eff < t:   # (running from the segment's end to t)
                r0 = max(e_eff, lo)
                accd = collections.Counter(); be_in(th, r0, t, accd, False)
                step(f, th, r0, t, "running" + ("".join(" %s %.3f" % (k[3:], v) for k, v in accd.most_common(3))))
                path[(th, "running")] += t - r0
                acc = collections.Counter(); be_in(th, r0, t, acc)
                for kk, v in acc.items():
                    path_be[(th, kk)] += v
                    per[f]["be " + kk[3:]] += v
                t = r0
                if t <= lo:
                    break
            # the segment [s, e_eff]: who ended it
            key = None
            if k == 4:
                key = ka
            elif k in (0, 1):
                key = ka
            moved = False
            if key is not None and key in wakes:
                wl = wake_ts[key]
                i = bisect.bisect_right(wl, e_eff) - 1
                while i >= 0 and wl[i] >= s:
                    wt, wth = wakes[key][i]
                    if wth != th:
                        lat = e_eff - max(wt, lo)
                        waits[(th, KIND[k] + " (woken: latency)", "")] += max(0.0, lat)
                        step(f, th, max(wt, lo), e_eff, "%s, woken by %s (latency)" % (KIND[k], names.get(wth, wth)))
                        t, th = max(wt, lo), wth
                        hops += 1
                        moved = True
                        break
                    i -= 1
            if not moved and k == 2:
                # a poll: sleeps with short runs between (the job system's 20 us looks; the present's look at its event).
                # What it waited for is done by another thread: for the present poll the latest event write (a submit or
                # a CPU event signal of the back end), for the others the latest block of another thread (a worker that
                # finished its job), before the poll's end
                cs, jj = s, j
                while jj > 0 and seg[th][jj - 1][2] == 2 and s - seg[th][jj - 1][1] < 0.05 and cs - seg[th][jj - 1][1] < 0.05:
                    jj -= 1
                    cs = seg[th][jj][0]
                best = None
                if kb == 0x80590520:
                    for th2, v in bes.items():
                        if th2 == th:
                            continue
                        i2 = bisect.bisect_right(be_starts[th2], e_eff) - 1
                        while i2 >= 0 and v[i2][1] > cs - 3.0:
                            if v[i2][2] in (14, 16) and v[i2][1] <= e_eff and (best is None or v[i2][1] > best[0]):
                                best = (v[i2][1], th2, "event write")
                                break
                            i2 -= 1
                else:
                    # the thread that was running longest at the poll's end (the job it waited for, or the one before it)
                    te = e_eff - 0.001
                    for th2, v in seg.items():
                        if th2 == th:
                            continue
                        hs = holding_since(th2, te)
                        if hs is None:
                            continue   # (no core: blocked, or waiting for one)
                        i2 = bisect.bisect_right(seg_ends[th2], te) - 1
                        st = max(hs, v[i2][1] if i2 >= 0 else lo)
                        if te - st > 0.05 and (best is None or st < best[3]):
                            best = (te, th2, "a job running", st)
                if best and best[0] > lo:
                    waits[(th, "poll latency (%s)" % best[2], "%#x" % kb)] += e_eff - best[0]
                    step(f, th, best[0], e_eff, "poll %#x, ended after %s on %s (latency)" % (kb, best[2], names.get(best[1], best[1])))
                    t, th = best[0], best[1]
                    hops += 1
                    moved = True
            if not moved:
                w0 = max(s, lo)
                caller = ("%s %#x" % (SITES.get(ka, ka), kb)) if k == 3 else ("%#x" % kb if k in BLOCKS else ("yield/kick %d" % kb if kb else ""))
                waits[(th, KIND[k], caller)] += e_eff - w0
                step(f, th, w0, e_eff, "%s %s" % (KIND[k], caller))
                t = w0

    if steps and a.merged:
        print("\n-- frame %d (%.3f ms), its path from the start, consecutive steps on a thread merged (ms from the frame's start)" % (a.dump, ptimes[a.dump + 1] - ptimes[a.dump]))
        out = []
        for a0, a1, nm, what in reversed(steps):
            cat = "running" if what.startswith("running") else what.split(",")[0].split(" 0x")[0]
            be = sum(float(x) for x in __import__("re").findall(r"(?:emit|submit|list_call|list_begin|list_end|event_signal|list_released) ([\d.]+)", what))
            if out and out[-1][2] == nm and out[-1][3] == cat:
                out[-1][1] = a1; out[-1][4] += be
            else:
                out.append([a0, a1, nm, cat, be])
        small = 0.0
        for a0, a1, nm, cat, be in out:
            if a1 - a0 >= 0.05:
                print("  %7.3f-%7.3f %6.3f  %-26s %-22s back end %.3f" % (a0 - ptimes[a.dump], a1 - ptimes[a.dump], a1 - a0, nm[:26], cat, be))
            else:
                small += a1 - a0
        print("  (steps under 0.05 ms not shown: %.3f ms in all)" % small)
    elif steps:
        print("\n-- frame %d, back from its end (ms from the frame's start)" % a.dump)
        for a0, a1, nm, what in steps:
            print("  %7.3f-%7.3f %6.3f  %-26s %s" % (a0 - ptimes[a.dump], a1 - ptimes[a.dump], a1 - a0, nm[:26], what))
    path_by_list = dict(by_list)
    ft = np.diff(ptimes[:nfr + 1])
    print("\n-- the back end's emits and calls on the path by the list they went into, ms a frame")
    for k_, v_ in sorted(path_by_list.items(), key=lambda kv: -kv[1])[:12]:
        print("  %6.3f  %s" % (v_ / nfr, k_))
    order_f = np.argsort(ft)
    sets = [("median 10%", order_f[int(nfr * 0.45):int(nfr * 0.55)]), ("top 1%", order_f[-max(1, nfr // 100):]), ("all", order_f)]
    cats = sorted({c for d in per.values() for c in d})
    print("\n-- the path by category, ms a frame (back end 'be' is inside the running time)")
    print("  %-22s" % "" + "".join("%12s" % n for n, _ in sets))
    print("  %-22s" % "frame time" + "".join("%12.3f" % ft[ix].mean() for _, ix in sets))
    for c in cats:
        print("  %-22s" % c + "".join("%12.3f" % np.mean([per[i][c] for i in ix]) for _, ix in sets))
    print("  top 1%% frames: %s" % " ".join("%d(%.1f)" % (i, ft[i]) for i in sorted(sets[1][1])))
    tot = (ptimes[nfr] - ptimes[0]) / nfr
    print("\n-- the critical path, ms a frame (sums to %.2f)" % tot)
    by_thread = collections.Counter()
    for (th, _), v in path.items():
        by_thread[th] += v
    for (th, _, _), v in waits.items():
        by_thread[th] += v
    for th, v in by_thread.most_common():
        run = path.get((th, "running"), 0) / nfr
        bsum = sum(x for (t2, _), x in path_be.items() if t2 == th) / nfr
        print("  %-28s %6.3f  (running %6.3f, of it in the back end %6.3f; waiting %6.3f)" % (names.get(th, th)[:28], v / nfr, run, bsum, v / nfr - run))
        for (t2, k), x in sorted(path_be.items(), key=lambda kv: -kv[1]):
            if t2 == th and x / nfr >= 0.005:
                print("      %-26s %6.3f" % (k, x / nfr))
    print("  hops between threads %.1f a frame" % (hops / nfr))
    print("\n-- waits on the path, ms a frame")
    for (th, k, c), v in waits.most_common(a.top):
        if v / nfr >= 0.005:
            print("  %6.3f  %-24s %-18s %s" % (v / nfr, names.get(th, th)[:24], k, c))

    # each thread's own time a frame
    span = ptimes[nfr] - ptimes[0]
    lo, hi = ptimes[0], ptimes[nfr]
    print("\n-- each thread over the window, ms a frame: running, blocked, core waits; back end by call")
    rows = []
    for th in names:
        blk = vcw = 0.0
        for s, e, k, _, _ in seg.get(th, []):
            ov = min(e, hi) - max(s, lo)
            if ov > 0:
                if k == 4:
                    vcw += ov
                else:
                    blk += ov
        acc = collections.Counter(); be_in(th, lo, hi, acc)
        alive = seg.get(th) or bes.get(th)
        if not alive:
            continue
        run = span - blk - vcw
        rows.append((run, th, blk, vcw, acc))
    for run, th, blk, vcw, acc in sorted(rows, key=lambda r: -r[0])[:a.top]:
        be = sum(acc.values())
        top = ", ".join("%s %.3f" % (k[3:], v / nfr) for k, v in acc.most_common(4) if v / nfr >= 0.002)
        print("  %-28s run %6.3f blk %6.3f vcw %6.3f | be %6.3f: %s" % (names[th][:28], run / nfr, blk / nfr, vcw / nfr, be / nfr, top))




def gantt(folder, frame, res=0.1, frames=1):
    """One frame (or a few) as rows of characters per thread: # running, B in the back end, s a poll sleep, . blocked, w a core wait"""
    ev, tsc0, scale, names = load(folder)
    t0 = (ev["t0"].astype(np.float64) - tsc0) * scale
    t1 = (ev["t1"].astype(np.float64) - tsc0) * scale
    kind = ev["kind"]; thr = ev["thread"]
    pres = np.sort(t0[kind == 20])
    lo, hi = pres[frame], pres[frame + frames]
    n = int((hi - lo) / res) + 1
    rows = {}
    order = {"#": 0, "B": 1, "s": 2, ".": 3, "w": 4, "o": 5}
    sel = (t1 >= lo) & (t0 <= hi)
    idx = np.where(sel)[0]
    # block/vcwait/sleep first, back end over it
    for prio in (0, 1):
        for i in idx:
            k = int(kind[i]); th = int(thr[i])
            if prio == 0 and k in (0, 1, 2, 3, 4):
                c = "s" if k == 2 else ("w" if k == 4 else ("o" if k == 3 else "."))
            elif prio == 1 and 10 <= k <= 19:
                c = "B"
            else:
                continue
            r = rows.setdefault(th, ["#"] * n)
            a = max(0, int((t0[i] - lo) / res)); b = min(n, int((t1[i] - lo) / res) + 1)
            for x in range(a, b):
                if prio == 1:
                    if r[x] == "#":
                        r[x] = "B"
                else:
                    r[x] = c
    print("frame %d: %.3f ms; a column %.2f ms; # running, B back end, s poll sleep, . blocked, o host wait, w core wait" % (frame, hi - lo, res))
    for th in sorted(rows, key=lambda t: names.get(t, "")):
        r = "".join(rows[th])
        if r.count("#") + r.count("B") == 0:
            continue
        print("%-22s %s" % (names.get(th, th)[:22], r))


if __name__ == "__main__" and os.environ.get("CRIT_GANTT"):
    gantt(sys.argv[1], int(os.environ["CRIT_GANTT"]), float(os.environ.get("CRIT_RES", "0.1")), int(os.environ.get("CRIT_FRAMES", "1")))
    sys.exit(0)


if __name__ == "__main__":
    sys.exit(main())
