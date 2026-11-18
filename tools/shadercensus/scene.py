"""A scene's GPU work by program: HOS_DBG=pstats's per-command statistics joined with the census's static cost per unit.

    work/venv/bin/python -I tools/shadercensus/scene.py <run dir> --census <stages.jsonl> [--clock 2.3]
                                                       [--simds 160] [--out <dir>]

<run dir> is a run of the game with HOS_DBG=pstats:<present>,drawlog:<present> (pstats.csv and the
drawlog lines in log.txt). For each command: the vertex stage's cost a vertex x its vertex invocations, the fragment stage's
cost a pixel x its fragment invocations, the task stage's cost a workgroup x its workgroups (compute invocations / threads),
a dispatch's cost an invocation x its compute invocations (mesh stages are not costed: RADV gives no mesh invocation
statistics on this GPU). Costs are
ACO's estimates in SIMD cycles (tools/shadercensus/report.py); a millisecond estimate divides them by the GPU's SIMDs (80 CUs
x 2 SIMD32) and clock. Prints Markdown tables; writes <out>/programs.csv (local: keys and counts, and the texture names the
drawlog gave, which are the game's).
"""
import argparse
import collections
import csv
import json
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import report  # noqa: E402  (its cost unit)


def census_costs(path):
    by_key = {}
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            if "key" not in r or "nvn_stage" not in r:
                continue
            e = report.exe_of(r)
            if not e:
                continue
            c1, c2 = report.unit_cost(r, e)
            wg = r.get("workgroup") or [1, 1, 1]
            by_key[(r["nvn_stage"], r["key"])] = {"unit": c1, "unit2": c2, "threads": max(1, wg[0] * wg[1] * wg[2]), "row": r, "exe": e}
    return by_key


def drawlog_names(log):
    """Per program: the texture names its draws sampled (drawlog), the passes"""
    names = collections.defaultdict(collections.Counter)
    passes = collections.defaultdict(collections.Counter)
    with open(log, errors="replace") as f:
        for line in f:
            if "direct: drawlog lr" not in line:
                continue
            m = re.search(r"kind (\d+) rt \d+ (\d+x\d+) c0 (-?\d+): .*? prog ([0-9a-f]{16})", line)
            if not m:
                continue
            key = m[4]
            passes[key]["%s kind %s fmt %s" % (m[2], m[1], m[3])] += 1
            for t in re.findall(r"entries '([^']*)'", line):
                names[key][t] += 1
    return names, passes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--census", required=True)
    ap.add_argument("--clock", type=float, default=2.3)
    ap.add_argument("--simds", type=int, default=160)
    ap.add_argument("--out")
    ap.add_argument("--top", type=int, default=20)
    a = ap.parse_args()
    cost = census_costs(a.census)
    names, passes = drawlog_names(os.path.join(a.run, "log.txt"))
    rows = list(csv.DictReader(open(os.path.join(a.run, "pstats.csv"))))
    cyc_ms = 1.0 / (a.simds * a.clock * 1e6)   # (SIMD cycles -> ms of the whole GPU)
    P = collections.defaultdict(lambda: collections.Counter())
    info = {}
    stages_used = set()
    missing = collections.Counter()
    unavailable = 0
    for r in rows:
        if r["available"] != "1":
            unavailable += 1
            continue
        what = r["what"]
        key = r["key"]
        p = P[key]
        p["commands"] += 1
        if "t_begin_ns" in r and float(r["t_begin_ns"]) > 0:   # (the time the command added to the queue's timeline)
            p["measured_ns"] += float(r["t_end_ns"]) - float(r["t_begin_ns"])
        vs, fs = int(r["vs_invocations"]), int(r["fs_invocations"])
        cs = int(r["cs_invocations"])
        info.setdefault(key, {"what": collections.Counter(), "codes": (r["code0"], r["code1"], r["code2"]), "pass": collections.Counter()})
        info[key]["what"][what.split()[0] if what.startswith("draw") else what] += 1
        info[key]["pass"]["%sx%s k%s" % (r["w"], r["h"], r["kind"])] += 1
        if what.startswith("dispatch"):
            c = cost.get((5, r["code0"]))
            stages_used.add((5, r["code0"]))
            p["cs_inv"] += cs
            if c:
                p["cycles_cs"] += c["unit"] * cs
            else:
                missing[(5, r["code0"])] += 1
            continue
        if what.startswith("task step"):
            c = cost.get((6, r["code2"]))
            stages_used.add((6, r["code2"]))
            p["cs_inv"] += cs
            if c:
                p["task_groups"] += cs / c["threads"]
                p["cycles_task"] += c["unit"] * cs / c["threads"]
            else:
                missing[(6, r["code2"])] += 1
            continue
        mesh = what.startswith("mesh") or what.startswith("task+mesh")
        if mesh:
            stages_used.add((7, r["code0"]))
        else:
            stages_used.add((0, r["code0"]))
            c = cost.get((0, r["code0"]))
            p["vs_inv"] += vs
            p["ia_vertices"] += int(r["ia_vertices"])
            if c:
                p["cycles_vs"] += c["unit"] * vs
            else:
                missing[(0, r["code0"])] += 1
        if r["code1"] != "0" * 16:
            stages_used.add((1, r["code1"]))
            c = cost.get((1, r["code1"]))
            p["fs_inv"] += fs
            if c:
                p["cycles_fs"] += c["unit"] * fs
            else:
                missing[(1, r["code1"])] += 1
        p["clip_prims"] += int(r["clip_primitives"])
    tot = collections.Counter()
    for p in P.values():
        for k in ("cycles_vs", "cycles_fs", "cycles_cs", "cycles_task", "cycles_ms", "vs_inv", "fs_inv", "cs_inv", "commands", "mesh_groups", "measured_ns"):
            tot[k] += p[k]
    total_cycles = sum(tot[k] for k in ("cycles_vs", "cycles_fs", "cycles_cs", "cycles_task", "cycles_ms"))
    print("Commands measured: %d (%d without results). Programs: %d. Stages used: %s." % (
        len(rows), unavailable, len(P), dict(collections.Counter({0: "vertex", 1: "fragment", 5: "compute", 6: "task", 7: "mesh"}[k] for k, _ in stages_used))))
    print("Invocations: vertex %d, fragment %d, compute %d." % (tot["vs_inv"], tot["fs_inv"], tot["cs_inv"]))
    print("Estimated GPU time (ACO cycles / (%d SIMDs x %.2f GHz)): total %.2f ms; vertex %.2f, fragment %.2f, mesh %.2f, task %.2f, compute %.2f." % (
        a.simds, a.clock, total_cycles * cyc_ms, tot["cycles_vs"] * cyc_ms, tot["cycles_fs"] * cyc_ms, tot["cycles_ms"] * cyc_ms,
        tot["cycles_task"] * cyc_ms, tot["cycles_cs"] * cyc_ms))
    if tot["measured_ns"]:
        print("Measured (timestamps around each command: the time it added to the queue's timeline): %.2f ms in all; estimate / measured %.2f." % (
            tot["measured_ns"] / 1e6, total_cycles * cyc_ms / (tot["measured_ns"] / 1e6)))
    if missing:
        print("Stages not in the census (no cost): %d, %d commands" % (len(missing), sum(missing.values())))
    measured = tot["measured_ns"] > 0
    if measured:
        order = sorted(P.items(), key=lambda kv: -kv[1]["measured_ns"])
    else:
        order = sorted(P.items(), key=lambda kv: -sum(kv[1][k] for k in ("cycles_vs", "cycles_fs", "cycles_cs", "cycles_task", "cycles_ms")))
    print("\n| # | Program | Kind | Commands | Pass(es) | Vertex inv | Fragment inv (Mpx) | Mesh wg | CS inv | VS cyc/vtx | FS cyc/px | Est. ms | Est. share | Measured ms | Measured share | Textures (drawlog) |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    rows_out = []
    for i, (key, p) in enumerate(order):
        cyc = sum(p[k] for k in ("cycles_vs", "cycles_fs", "cycles_cs", "cycles_task", "cycles_ms"))
        inf = info[key]
        codes = inf["codes"]
        vsc = cost.get((0, codes[0]), {}).get("unit")
        fsc = cost.get((1, codes[1]), {}).get("unit")
        tex = ", ".join(n for n, _ in names[key].most_common(4))
        row = {"rank": i + 1, "key": key, "kind": "/".join(inf["what"]), "commands": p["commands"], "passes": "; ".join(x for x, _ in inf["pass"].most_common(2)),
               "vs_inv": p["vs_inv"], "fs_inv": p["fs_inv"], "mesh_groups": p["mesh_groups"], "cs_inv": p["cs_inv"], "code0": codes[0], "code1": codes[1],
               "code2": codes[2], "vs_cyc": vsc, "fs_cyc": fsc, "ms": cyc * cyc_ms, "share": cyc / total_cycles if total_cycles else 0,
               "ms_vs": p["cycles_vs"] * cyc_ms, "ms_fs": p["cycles_fs"] * cyc_ms, "ms_mesh": p["cycles_ms"] * cyc_ms, "ms_task": p["cycles_task"] * cyc_ms,
               "ms_cs": p["cycles_cs"] * cyc_ms, "measured_ms": p["measured_ns"] / 1e6,
               "measured_share": p["measured_ns"] / tot["measured_ns"] if tot["measured_ns"] else 0, "textures": tex}
        rows_out.append(row)
        if i < a.top:
            print("| %d | `%s` | %s | %d | %s | %d | %.2f | %d | %d | %s | %s | %.3f | %.1f%% | %.3f | %.1f%% | %s |" % (
                i + 1, key, row["kind"], p["commands"], row["passes"], p["vs_inv"], p["fs_inv"] / 1e6, p["mesh_groups"], p["cs_inv"],
                "%.1f" % vsc if vsc is not None else "-", "%.1f" % fsc if fsc is not None else "-", row["ms"], 100 * row["share"],
                row["measured_ms"], 100 * row["measured_share"], tex[:60]))
    # (by stage: the fragment and the other stages' shares across programs)
    st = collections.defaultdict(float)
    stm = collections.defaultdict(float)
    for key, p in P.items():
        codes = info[key]["codes"]
        est = sum(p[k] for k in ("cycles_vs", "cycles_fs", "cycles_cs", "cycles_task", "cycles_ms"))
        for kind, code, k in (("fragment", codes[1], "cycles_fs"), ("vertex", codes[0], "cycles_vs"), ("mesh", codes[0], "cycles_ms"),
                              ("task", codes[2], "cycles_task"), ("compute", codes[0], "cycles_cs")):
            if p[k] and est:
                stm[(kind, code)] += p["measured_ns"] * p[k] / est
        if p["cycles_fs"]:
            st[("fragment", codes[1])] += p["cycles_fs"]
        if p["cycles_vs"]:
            st[("vertex", codes[0])] += p["cycles_vs"]
        if p["cycles_ms"]:
            st[("mesh", codes[0])] += p["cycles_ms"]
        if p["cycles_task"]:
            st[("task", codes[2])] += p["cycles_task"]
        if p["cycles_cs"]:
            st[("compute", codes[0])] += p["cycles_cs"]
    print("\nTop stages (a stage summed over the programs using it; measured: its programs' measured time split by the estimate's "
          "shares within each program):\n")
    print("| # | Stage | Kind | Est. ms | Est. share | Measured ms | Measured share |")
    print("|---|---|---|---|---|---|---|")
    keyf = (lambda kv: -stm.get(kv[0], 0)) if measured else (lambda kv: -kv[1])
    for i, ((kind, code), cyc) in enumerate(sorted(st.items(), key=keyf)[: a.top]):
        m = stm.get((kind, code), 0)
        print("| %d | `%s` | %s | %.3f | %.1f%% | %.3f | %.1f%% |" % (i + 1, code, kind, cyc * cyc_ms, 100 * cyc / total_cycles, m / 1e6,
                                                         100 * m / tot["measured_ns"] if tot["measured_ns"] else 0))
    if a.out:
        with open(os.path.join(a.out, "stages_share.csv"), "w") as f:
            f.write("kind,code,est_ms,measured_ms\n")
            for (kind, code), cyc in st.items():
                f.write("%s,%s,%.5f,%.5f\n" % (kind, code, cyc * cyc_ms, stm.get((kind, code), 0) / 1e6))
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, "programs.csv"), "w", newline="") as f:
            w = csv.DictWriter(f, list(rows_out[0].keys()))
            w.writeheader()
            w.writerows(rows_out)
        with open(os.path.join(a.out, "stages_used.txt"), "w") as f:
            for k, code in sorted(stages_used):
                f.write("%d %s\n" % (k, code))


if __name__ == "__main__":
    sys.exit(main())
