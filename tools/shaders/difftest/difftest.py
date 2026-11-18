"""The differential's driver: difftest (tools/shaders/difftest) over a seed in shards, resumable, then summed up.

    work/venv/bin/python -I tools/shaders/difftest/difftest.py d1|d2|d3 <seed> --out <dir> [--jobs N] [--shard S]
        [--tool work/difftest/difftest] [--names sm70_encode.rs]... -- [difftest options: --a old --b new --inputs K ...]

Each shard runs difftest once and becomes <out>/shards/<first>.jsonl; a complete shard is not run again, and a stage that
crashes difftest gets an error line while the rest of its shard runs again. The joined lines go to <out>/stages.jsonl and
the summary to <out>/summary.txt (and stdout). d1's seed is a micro seed (difftest micro), its listing <seed>.tsv beside
it. Everything written is made from the game's code and stays under work/.
"""
import argparse
import collections
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools", "shadercensus"))
import opcodes  # noqa: E402

NAK = "/usr/src/debug/mesa/mesa-26.2.7/src/nouveau/compiler/nak/sm70_encode.rs"


def stage_count(seed):
    with open(seed, "rb") as f:
        head = f.read(16)
    return int.from_bytes(head[12:16], "little")


def run_shard(tool, mode, seed, first, count, out, extra):
    path = os.path.join(out, "shards", "%06d.jsonl" % first)
    if os.path.exists(path):
        with open(path) as f:
            if sum(1 for _ in f) == count:
                return first, count, 0
    lines, crashes, at = {}, 0, first
    while at < first + count:
        r = subprocess.run([tool, mode, seed, "--first", str(at), "--count", str(first + count - at)] + extra, capture_output=True, text=True)
        for line in r.stdout.splitlines():
            if line.startswith("{"):
                lines[json.loads(line)["index"]] = line
        if r.returncode == 0 and all(i in lines for i in range(at, first + count)):
            break
        begun = [int(x.split()[1]) for x in r.stderr.splitlines() if x.startswith("@@stage ")]
        bad = begun[-1] if begun else at
        lines.setdefault(bad, json.dumps({"index": bad, "a": "difftest exited %d on this stage: %s" % (r.returncode, r.stderr.strip()[-300:]), "b": ""}))
        crashes += 1
        at = bad + 1
    with open(path + ".tmp", "w") as f:
        for i in range(first, first + count):
            f.write(lines.get(i, json.dumps({"index": i, "a": "no result", "b": ""})) + "\n")
    os.replace(path + ".tmp", path)
    return first, count, crashes


def side(d, s):
    return d.get(s) == "ok"


def short(reason):
    return re.sub(r"0x[0-9a-f]+|\b\d+\b", "#", reason)[:90]


def summary_common(rows, w):
    both = [d for d in rows if side(d, "a") and side(d, "b")]
    w("stages %d: both fronts translate %d, A only %d, B only %d, neither %d" % (
        len(rows), len(both), sum(side(d, "a") and not side(d, "b") for d in rows), sum(side(d, "b") and not side(d, "a") for d in rows),
        sum(not side(d, "a") and not side(d, "b") for d in rows)))
    for s in "ab":
        reasons = collections.Counter(short(d[s]) for d in rows if not side(d, s))
        if reasons:
            w("%s's errors:" % s.upper())
            for r, n in reasons.most_common(12):
                w("  %6d %s" % (n, r))
    return both


def summary_runs(rows, w, names, micro, seed):
    both = summary_common(rows, w)
    errors = [d for d in both if "run_error" in d]
    ran = [d for d in both if "run_error" not in d]
    differ = [d for d in ran if not d.get("equal", True)]
    w("ran %d (run errors %d): equal %d, differ %d; not comparable (every input capped) %d; with capped inputs %d; with undefined reads %d" % (
        len(ran), len(errors), sum(d.get("equal", False) and d["compared"] > 0 for d in ran), len(differ), sum(d["compared"] == 0 for d in ran),
        sum(d["capped"] > 0 for d in ran), sum(d["undefined"] > 0 for d in ran)))
    for r, n in collections.Counter(short(d["run_error"]) for d in errors).most_common(12):
        w("  run error %6d %s" % (n, r))
    if micro:
        listing = {}
        if os.path.exists(seed + ".tsv"):
            for line in open(seed + ".tsv"):
                f = line.rstrip("\n").split("\t")
                listing[int(f[0])] = (int(f[4]), int(f[5]))
        per = collections.defaultdict(lambda: [0, 0, 0, 0, 0])   # (encodings, instructions, rejected by A, differ, instructions differing)
        for d in rows:
            op = int(d.get("op", "fff"), 16)
            n = listing.get(d["index"], (0, 0))[0]
            p = per[op]
            p[0] += 1
            p[1] += n
            p[2] += not side(d, "a")
            bad = d.get("equal") is False
            p[3] += bad
            p[4] += n if bad else 0
        total = sum(p[1] for p in per.values())
        tested = sum(listing.get(d["index"], (0, 0))[0] for d in ran)
        if total:
            w("instructions in the seeds: %d; their encodings ran by both fronts: %.1f%%" % (total, 100.0 * tested / total))
        w("| opcode | name | encodings | instructions | A rejects | differ | instructions differing |\n|---|---|---|---|---|---|---|")
        for op, p in sorted(per.items(), key=lambda t: (-t[1][3], -t[1][1])):
            w("| %03x | %s | %d | %d | %d | %d | %d |" % (op, opcodes.name_of(op, names), p[0], p[1], p[2], p[3], p[4]))
    if not differ:
        return
    keys = collections.Counter(re.sub(r"^mem\.[0-9a-f]{16}$", "mem.storage", d["diff"]["key"]) for d in differ)
    w("first differing key:")
    for k, n in keys.most_common():
        w("  %6d %s" % (n, k))
    w("first differences (up to 20):")
    for d in differ[:20]:
        f = d["diff"]
        w("  %d %s%s input %d %s %s: %s vs %s (%d items)" % (d["index"], d["key"], " " + d.get("inst", ""), f["input"], f["key"], f["item"], f["a"][:40],
                                                             f["b"][:40], f["items"]))
    if micro:
        return
    # (suspects: the native opcodes of the differing stages, by the share of their stages that differ)
    bad = {d["index"] for d in differ}
    having, having_bad = collections.Counter(), collections.Counter()
    for i, (stage, control, code) in enumerate(opcodes.read_seed(seed)):
        ops = {x & 0xfff for x in opcodes.instructions(stage, control, code)}
        having.update(ops)
        if i in bad:
            having_bad.update(ops)
    w("| opcode | name | differing stages | stages | share |\n|---|---|---|---|---|")
    for op, n in sorted(having_bad.items(), key=lambda t: (-t[1] / having[t[0]], -t[1]))[:40]:
        w("| %03x | %s | %d | %d | %.2f |" % (op, opcodes.name_of(op, names), n, having[op], n / having[op]))


def summary_info(rows, w):
    both = summary_common(rows, w)
    differ = [d for d in both if not d["equal"]]
    w("Info compared %d: equal %d, differ %d" % (len(both), len(both) - len(differ), len(differ)))
    fields = collections.Counter()
    example = {}
    for d in differ:
        for k, v in d["diffs"].items():
            fam = re.sub(r"\[\d+\]", "[]", k)
            fields[fam] += 1
            example.setdefault(fam, (d["index"], k, v))
    if fields:
        w("| field | stages | example |\n|---|---|---|")
        for k, n in fields.most_common():
            i, name, v = example[k]
            w("| %s | %d | stage %d %s: %s vs %s |" % (k, n, i, name, v[0][:40], v[1][:40]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("mode", choices=["d1", "d2", "d3"])
    ap.add_argument("seed")
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--shard", type=int, default=500)
    ap.add_argument("--tool", default=os.path.join(ROOT, "work", "difftest", "difftest"))
    ap.add_argument("--names", action="append", default=[], help="opcode names (opcodes.py --names), NAK's encoder by default")
    ap.add_argument("extra", nargs="*", help="difftest options, after --")
    a = ap.parse_args()
    os.makedirs(os.path.join(a.out, "shards"), exist_ok=True)
    n = stage_count(a.seed)
    started = time.time()
    done = 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        futs = [ex.submit(run_shard, a.tool, a.mode, a.seed, f, min(a.shard, n - f), a.out, a.extra) for f in range(0, n, a.shard)]
        for fu in concurrent.futures.as_completed(futs):
            first, count, crashes = fu.result()
            done += count
            print("%6d / %d stages (%.0f s)%s" % (done, n, time.time() - started, ", %d crashed" % crashes if crashes else ""), flush=True)
    rows = []
    with open(os.path.join(a.out, "stages.jsonl"), "w") as f:
        for first in range(0, n, a.shard):
            with open(os.path.join(a.out, "shards", "%06d.jsonl" % first)) as g:
                text = g.read()
            f.write(text)
            rows += [json.loads(line) for line in text.splitlines()]
    names = {}
    for path in a.names or [NAK]:
        if os.path.exists(path):
            for k, v in (opcodes.nak_names(path) if path.endswith(".rs") else opcodes.text_names(path)).items():
                names.setdefault(k, v)
    out = []
    w = out.append
    w("# difftest %s %s %s (%.0f s wall)" % (a.mode, a.seed, " ".join(a.extra), time.time() - started))
    if a.mode == "d2":
        summary_info(rows, w)
    else:
        w("CPU: %.0f s in difftest's stage lines" % (sum(d.get("ms", 0) for d in rows) / 1000))
        summary_runs(rows, w, names, a.mode == "d1", a.seed)
    text = "\n".join(out) + "\n"
    with open(os.path.join(a.out, "summary.txt"), "w") as f:
        f.write(text)
    print(text, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
