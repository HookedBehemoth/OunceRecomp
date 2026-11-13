"""The shader census's aggregates: work/.../stages.jsonl (tools/shadercensus/census.py) into a table and summaries.

    work/venv/bin/python -I tools/shadercensus/report.py <stages.jsonl> --out <dir>

Writes <dir>/stages.csv (a row a stage: the machine-readable table, local: it is made from the game's code) and prints the
aggregates and top lists as Markdown (code keys, counts and our own descriptions only). The cost unit of each stage kind is
ACO's "Inverse Throughput" (estimated busy cycles of one wave, RADV's pipeline executable statistics; ACO weights blocks in
loops by 8 / 4 / 2 by depth, uniform branches by 1/2, divergent ones by 3/4) turned into SIMD cycles per unit:
  fragment  per pixel shaded       IT / subgroup lanes
  vertex    per vertex             IT / subgroup lanes
  mesh      per mesh workgroup     IT x waves a workgroup (and per emitted vertex: that / max vertices)
  task      per task workgroup     IT x waves a workgroup
  compute   per invocation         IT / subgroup lanes (and per workgroup: IT x waves a workgroup)
"""
import argparse
import collections
import csv
import json
import math
import os
import statistics
import sys

KIND = {0: "vertex", 1: "fragment", 5: "compute", 6: "task", 7: "mesh"}
VKBIT = {0: 1, 1: 16, 5: 32, 6: 32, 7: 128}


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            rows.append(json.loads(line))
    return rows


def exe_of(r):
    """The RADV executable of the stage itself (a pipeline holds its partner's too)"""
    bit = VKBIT.get(r.get("nvn_stage"), 0)
    for e in r.get("radv", []) or []:
        if e.get("stages", 0) & bit:
            return e
    return None


def unit_cost(r, e):
    """SIMD cycles per cost unit (see the module's text), and per secondary unit"""
    it = e.get("Inverse Throughput", 0)
    lanes = e.get("subgroup", 32) or 32
    k = r["nvn_stage"]
    wg = r.get("workgroup") or [1, 1, 1]
    threads = max(1, wg[0] * wg[1] * wg[2])
    waves = math.ceil(threads / lanes)
    if k in (0, 1):
        return it / lanes, None
    if k == 7:
        per_wg = it * waves
        return per_wg, per_wg / max(1, r.get("max_vertices", 1) or 1)
    if k == 6:
        return it * waves, None
    if k == 5:
        return it / lanes, it * waves
    return it, None


def q(vals, p):
    if not vals:
        return 0
    v = sorted(vals)
    return v[min(len(v) - 1, int(p * len(v)))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stages")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    rows = load(a.stages)
    cols = ["index", "key", "kind", "programs", "native", "ir", "spirv", "error", "workgroup", "shared", "local_mem", "in_lines", "in_comp",
            "out_lines", "out_comp", "rts", "rt_comp", "max_vertices", "max_primitives", "payload_words", "vertex_lines", "textures",
            "tex_used", "samples", "samples_branch", "samples_loop", "samples_unknown_loop", "samples_weighted", "tex_kind", "tex_dim",
            "cbuf", "cbuf_c1", "cbuf_driver", "cbuf_game", "cbuf_indexed", "gload", "gstore", "atomics", "sload", "sstore", "local_ops",
            "branches", "branch_cls", "discards", "loops", "subgroup_ops", "cc_ops", "select_u1", "isbe_ops", "fp_ops", "transcendental",
            "aco_instructions", "code_size", "vgprs", "sgprs", "lds", "scratch", "spilled_vgprs", "spilled_sgprs", "waves_per_simd",
            "latency", "inv_throughput", "valu", "salu", "vmem", "smem", "subgroup", "cost_unit_cycles", "cost_unit2_cycles"]
    out = []
    for r in rows:
        if "nvn_stage" not in r:
            r["nvn_stage"] = -1
        e = exe_of(r)
        d = {c: r.get(c, "") for c in cols}
        d["kind"] = KIND.get(r["nvn_stage"], "?")
        for c in ("tex_kind", "tex_dim", "loops", "branch_cls", "workgroup"):
            d[c] = json.dumps(r.get(c, ""))
        if e:
            d.update(aco_instructions=e.get("Instructions"), code_size=e.get("Code size"), vgprs=e.get("VGPRs"), sgprs=e.get("SGPRs"),
                     lds=e.get("LDS size"), scratch=e.get("Scratch size"), spilled_vgprs=e.get("Spilled VGPRs"),
                     spilled_sgprs=e.get("Spilled SGPRs"), waves_per_simd=e.get("Subgroups per SIMD"), latency=e.get("Latency"),
                     inv_throughput=e.get("Inverse Throughput"), valu=e.get("VALU"), salu=e.get("SALU"), vmem=e.get("VMEM"),
                     smem=e.get("SMEM"), subgroup=e.get("subgroup"))
            c1, c2 = unit_cost(r, e)
            d["cost_unit_cycles"], d["cost_unit2_cycles"] = round(c1, 3), "" if c2 is None else round(c2, 3)
        out.append(d)
    with open(os.path.join(a.out, "stages.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, cols)
        w.writeheader()
        w.writerows(out)

    P = print
    by = collections.defaultdict(list)
    for r, d in zip(rows, out):
        by[d["kind"]].append((r, d))
    P("## Census aggregates\n")
    P("| Kind | Stages | Translated | Compiled | Errors | Header only |")
    P("|---|---|---|---|---|---|")
    for k in ("vertex", "fragment", "mesh", "task", "compute"):
        v = by[k]
        P("| %s | %d | %d | %d | %d | %d |" % (k, len(v), sum(1 for r, d in v if "ir" in r), sum(1 for r, d in v if r.get("compiled") == 0),
                                              sum(1 for r, d in v if "error" in r), sum(1 for r, d in v if r.get("header_only"))))
    errs = collections.Counter(r["error"].split(":")[0][:60] for r in rows if "error" in r)
    if errs:
        P("\nErrors by kind:", dict(errs.most_common(8)))

    P("\n### Size and cost (median / p90 / max)\n")
    P("| Kind | IR instructions | SPIR-V instructions | ACO instructions | VGPRs | LDS bytes | Inverse throughput (cycles a wave) | Cost unit (SIMD cycles) | Scratch > 0 | Spills > 0 |")
    P("|---|---|---|---|---|---|---|---|---|---|")
    unit_name = {"vertex": "per vertex", "fragment": "per pixel", "mesh": "per workgroup", "task": "per workgroup", "compute": "per invocation"}
    for k in ("vertex", "fragment", "mesh", "task", "compute"):
        v = [(r, d) for r, d in by[k] if d.get("inv_throughput") not in ("", None)]
        if not v:
            continue

        def tri(name):
            x = [float(d[name]) for r, d in v if d.get(name) not in ("", None)]
            return "%s / %s / %s" % tuple(("%.0f" % q(x, p)) if q(x, p) >= 10 else ("%.2f" % q(x, p)) for p in (0.5, 0.9, 1.0))
        P("| %s | %s | %s | %s | %s | %s | %s | %s %s | %d | %d |" % (k, tri("ir"), tri("spirv"), tri("aco_instructions"), tri("vgprs"), tri("lds"),
                                                                tri("inv_throughput"), tri("cost_unit_cycles"), unit_name[k],
                                                                sum(1 for r, d in v if (d["scratch"] or 0) > 0),
                                                                sum(1 for r, d in v if (d["spilled_vgprs"] or 0) > 0)))

    P("\n### Inputs and outputs (median / p90 / max)\n")

    def dist(v, name):
        x = [float(r.get(name, 0) or 0) for r, d in v if "ir" in r]
        return "%g / %g / %g" % (q(x, .5), q(x, .9), q(x, 1))
    P("| Kind | In lines | In components | Out lines | Out components | Targets | Target components |")
    P("|---|---|---|---|---|---|---|")
    for k in ("vertex", "fragment"):
        v = by[k]
        P("| %s | %s | %s | %s | %s | %s | %s |" % (k, dist(v, "in_lines"), dist(v, "in_comp"), dist(v, "out_lines"), dist(v, "out_comp"), dist(v, "rts"), dist(v, "rt_comp")))
    v = by["mesh"]
    P("\nMesh: max vertices %s, max primitives %s, vertex output lines %s, payload words %s, threads %s." % (
        dist(v, "max_vertices"), dist(v, "max_primitives"), dist(v, "vertex_lines"), dist(v, "payload_words"),
        "/".join(str(x) for x in sorted(collections.Counter((r.get("workgroup") or [0])[0] for r, d in v if "ir" in r)))))
    v = by["task"]
    P("Task: payload words %s, threads %s." % (dist(v, "payload_words"),
                                                dict(collections.Counter((r.get("workgroup") or [0])[0] for r, d in v if "ir" in r))))
    v = by["compute"]
    P("Compute: workgroup sizes (most common): %s; shared memory bytes %s." % (
        collections.Counter("x".join(str(x) for x in (r.get("workgroup") or [])) for r, d in v if "ir" in r).most_common(8), dist(v, "shared")))

    P("\n### Texture samples\n")
    P("| Kind | Stages with samples | Samples median / p90 / max | Total samples | In a branch | In a loop | In a loop of unknown bound | Distinct textures median / max |")
    P("|---|---|---|---|---|---|---|---|")
    for k in ("vertex", "fragment", "mesh", "task", "compute"):
        v = [(r, d) for r, d in by[k] if "ir" in r]
        s = [r.get("samples", 0) for r, d in v]
        tot = sum(s)
        P("| %s | %d of %d | %s | %d | %d | %d | %d | %g / %g |" % (k, sum(1 for x in s if x), len(v), "%g / %g / %g" % (q(s, .5), q(s, .9), q(s, 1)), tot,
                                                                sum(r.get("samples_branch", 0) for r, d in v), sum(r.get("samples_loop", 0) for r, d in v),
                                                                sum(r.get("samples_unknown_loop", 0) for r, d in v),
                                                                q([r.get("tex_used", 0) for r, d in v], .5), q([r.get("tex_used", 0) for r, d in v], 1)))
    kinds, dims = collections.Counter(), collections.Counter()
    for r in rows:
        if r.get("nvn_stage") == 1:
            kinds.update(r.get("tex_kind", {}))
            dims.update(r.get("tex_dim", {}))
    P("\nFragment samples by instruction: %s" % ", ".join("%s %d" % kv for kv in kinds.most_common()))
    P("Fragment samples by dimension: %s" % ", ".join("%s %d" % kv for kv in dims.most_common()))

    P("\n### Memory and control flow (sums over the stages of a kind; stages with any)\n")
    P("| Kind | Uniform loads (c1 / driver / game; indexed) | By-address loads | Stores | Atomics | Shared loads / stores | Branches (const / uniform / data / input) | Loops (const / uniform / data / input; trips known) | Discards |")
    P("|---|---|---|---|---|---|---|---|---|")
    for k in ("vertex", "fragment", "mesh", "task", "compute"):
        v = [r for r, d in by[k] if "ir" in r]
        S = lambda n: sum(r.get(n, 0) or 0 for r in v)
        H = lambda n: sum(1 for r in v if r.get(n, 0))
        bc = [sum(r.get("branch_cls", [0, 0, 0, 0])[i] for r in v) for i in range(4)]
        lc = collections.Counter(l["cls"] for r in v for l in r.get("loops", []))
        known = sum(1 for r in v for l in r.get("loops", []) if l["trips"] >= 0)
        P("| %s | %d / %d / %d; %d | %d (%d stages) | %d (%d) | %d (%d) | %d / %d | %d / %d / %d / %d | %d / %d / %d / %d; %d | %d (%d) |" % (
            k, S("cbuf_c1"), S("cbuf_driver"), S("cbuf_game"), S("cbuf_indexed"), S("gload"), H("gload"), S("gstore"), H("gstore"), S("atomics"),
            H("atomics"), S("sload"), S("sstore"), bc[0], bc[1], bc[2], bc[3], lc["const"], lc["uniform"], lc["data"], lc["input"], known,
            S("discards"), H("discards")))

    P("\n### Translation size against the game's own code (median / p90)\n")
    P("| Kind | Native instructions (the game's) | IR / native | SPIR-V / native | ACO / native |")
    P("|---|---|---|---|---|")
    for k in ("vertex", "fragment", "mesh", "task", "compute"):
        v = [(r, d) for r, d in by[k] if r.get("native") and d.get("aco_instructions") not in ("", None)]
        if not v:
            continue
        nat = [r["native"] for r, d in v]
        rat = lambda f: [f(r, d) / r["native"] for r, d in v]
        ir, sp, ac = rat(lambda r, d: r["ir"]), rat(lambda r, d: r["spirv"]), rat(lambda r, d: float(d["aco_instructions"]))
        P("| %s | %g / %g | %.2f / %.2f | %.2f / %.2f | %.2f / %.2f |" % (k, q(nat, .5), q(nat, .9), q(ir, .5), q(ir, .9), q(sp, .5), q(sp, .9), q(ac, .5), q(ac, .9)))

    P("\n### Translation markers (sums; share of IR instructions)\n")
    P("| Kind | IR instructions | Subgroup ops (uniform-register emulation) | Condition-code ops | SelectU1 | Stage buffer (ISBE) ops | Shared memory ops | SPIR-V / IR |")
    P("|---|---|---|---|---|---|---|---|")
    for k in ("vertex", "fragment", "mesh", "task", "compute"):
        v = [r for r, d in by[k] if "ir" in r]
        ir = sum(r["ir"] for r in v) or 1
        S = lambda n: sum(r.get(n, 0) or 0 for r in v)
        P("| %s | %d | %d (%.1f%%) | %d (%.1f%%) | %d (%.1f%%) | %d | %d | %.2f |" % (k, ir, S("subgroup_ops"), 100 * S("subgroup_ops") / ir, S("cc_ops"),
                                                                      100 * S("cc_ops") / ir, S("select_u1"), 100 * S("select_u1") / ir, S("isbe_ops"),
                                                                      S("sload") + S("sstore"), S("spirv") / ir))
    ops = collections.Counter()
    for r in rows:
        if r.get("nvn_stage") == 1:
            ops.update(r.get("ops", {}))
    P("\nFragment stages' 25 most frequent IR ops: %s" % ", ".join("%s %d" % kv for kv in ops.most_common(25)))

    P("\n### Top 20 by cost unit (SIMD cycles), per kind\n")
    for k in ("fragment", "vertex", "mesh", "task", "compute"):
        v = sorted([(r, d) for r, d in by[k] if d.get("cost_unit_cycles") not in ("", None)], key=lambda x: -float(x[1]["cost_unit_cycles"]))
        P("\n**%s** (%s):\n" % (k, unit_name[k]))
        P("| Code key | Programs | Cost unit | IT | ACO instructions | VGPRs | LDS | Scratch | Samples (loop / unknown) | Loops | Branches |")
        P("|---|---|---|---|---|---|---|---|---|---|---|")
        for r, d in v[:20]:
            P("| `%s` | %s | %s | %s | %s | %s | %s | %s | %d (%d / %d) | %d | %d |" % (r["key"], r.get("programs"), d["cost_unit_cycles"], d["inv_throughput"],
                                                                           d["aco_instructions"], d["vgprs"], d["lds"], d["scratch"], r.get("samples", 0),
                                                                           r.get("samples_loop", 0), r.get("samples_unknown_loop", 0),
                                                                           len(r.get("loops", [])), r.get("branches", 0)))


if __name__ == "__main__":
    sys.exit(main())
