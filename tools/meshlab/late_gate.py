"""The gate of the back end's register-pressure pass (gpu/direct/direct_spv_late.h; docs/direct/spv-late-report.md).

The pass keeps a block's new order by its own count of the values held; what decides whether a stage spills is ACO's
register allocation. So before the pass is on (and after a change to it, to the translator or to the driver) RADV's
compile of every stage is held against it, and the stages' outputs on captured draws:

    late_gate.py captures --meshlab <meshlab> --out <dir> [--spvlate <spvlate>] [--kinds mesh[,task,fragment]] [--margin 8]
                          [--compare] <frame.bin | roll.bin:present>...
        every mesh program of the captures (HOS_DBG=meshcap frames) compiled as the game's shader objects, as the
        translator emitted its stages and with the pass (meshlab stats --late): scratch spills (the driver's scheduler on,
        as the game compiles) and register demand (ACO_DEBUG=nosched) of every stage, before and after. Nothing is drawn.
        --compare: then every captured mesh draw's geometry with the pass against the translator's, bit for bit
        (meshlab compare --late mesh under the GPU lock, tools/meshlab/gpu_run.sh: one draw each, the rasterizer off).

    late_gate.py seed --censustool <censustool> --seed <hookshot_shaders.bin> --out <dir> [--spvlate <spvlate>]
                      [--kinds mesh] [--margin 8] [--jobs 6] [--budget N] [--sink-peak N]
        every stage of these kinds in the game's shader seed (nx2/shader_census.py: all scenes, visited or not) translated
        offline (tools/shadercensus) and made as the game's shader object, before and after. Compile only.

It FAILS when a stage that did not spill spills with the pass, a stage that did spills more by more than 4 words or an
eighth, a stage needs more registers than before by more than --margin (and is then within 32 of the 256 there are), or
(--spvlate) a stage's SPIR-V does not keep its instructions or stops being valid. Tables go to <dir>; they hold code keys and counts only, the
compiler's prints beside them hold game code and stay under work/.
"""
import argparse
import concurrent.futures
import json
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spill_scan   # noqa: E402

LIMIT = 256   # (vector registers a thread)


def verdict(before, after, margin, out, label):
    """before/after: {(kind, code): (spills, demand)}; writes the table, returns the failures"""
    fails, lines = [], []
    count = {}
    for key in sorted(set(before) | set(after)):
        kind, code = key
        if key not in before or key not in after:
            fails.append("%s %s: compiled only %s" % (kind, code, "before" if key in before else "after"))
            continue
        (s0, d0), (s1, d1) = before[key], after[key]
        c = count.setdefault(kind, {"n": 0, "spill_before": 0, "spill_after": 0, "limit_before": 0, "limit_after": 0, "lower": 0, "higher": 0, "worst_rise": 0})
        c["n"] += 1
        c["spill_before"] += s0 > 0
        c["spill_after"] += s1 > 0
        c["limit_before"] += d0 >= LIMIT - 16
        c["limit_after"] += d1 >= LIMIT - 16
        c["lower"] += d1 < d0
        c["higher"] += d1 > d0
        c["worst_rise"] = max(c["worst_rise"], d1 - d0)
        why = ""
        if s1 > 0 and s0 == 0:
            why = "SPILLS WITH THE PASS"
        elif s1 > s0 + max(4, s0 // 8):
            why = "SPILLS MORE WITH THE PASS"
        elif s1 > 0:
            why = "still spills"
        elif d1 > d0 + margin and d1 >= LIMIT - 32:
            why = "NEEDS MORE REGISTERS NEAR THE LIMIT"
        if why and why != "still spills":
            fails.append("%s %s: %s (spills %d -> %d, registers %d -> %d)" % (kind, code, why, s0, s1, d0, d1))
        lines.append((-(s0 + s1 > 0), kind, -max(d0, d1), "%-9s %s %6d %6d %9d %9d %s" % (kind, code, s0, s1, d0, d1, why)))
    with open(os.path.join(out, "%s_table.txt" % label), "w") as f:
        f.write("kind      code              spills  after registers     after\n")
        for *_, line in sorted(lines):
            f.write(line + "\n")
    summary = []
    for kind, c in sorted(count.items()):
        summary.append("%s: %d stages; with scratch spills %d before, %d after; at %d registers or more %d before, %d after; "
                       "register demand lower in %d, higher in %d (by %d at most)" % (kind, c["n"], c["spill_before"], c["spill_after"], LIMIT - 16,
                                                                                     c["limit_before"], c["limit_after"], c["lower"], c["higher"], c["worst_rise"]))
    return fails, summary


def instructions(path):
    data = open(path, "rb").read()
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    out, i = [], 5
    while i < len(words):
        wc = words[i] >> 16
        if not wc:
            break
        out.append(words[i:i + wc])
        i += wc
    return out


def valid(a, before, label, fails, summary):
    """every stage's SPIR-V of a folder through the pass (spvlate): the same instructions, and spirv-val accepts it"""
    after = before + "_late"
    r = subprocess.run([a.spvlate, "--dir", before, after, "--budget", str(a.budget), "--sink-peak", str(a.sink_peak)], capture_output=True, text=True)
    if r.returncode != 0:
        fails.append("%s: spvlate failed" % label)
        return
    val = shutil.which("spirv-val")
    n = changed = 0
    for name in sorted(os.listdir(after)):
        if not name.endswith(".spv"):
            continue
        n += 1
        x, y = instructions(os.path.join(before, name)), instructions(os.path.join(after, name))
        changed += x != y
        if sorted(x) != sorted(y):
            fails.append("%s: %s: NOT THE SAME INSTRUCTIONS with the pass" % (label, name))
        if val and subprocess.run([val, "--target-env", "vulkan1.3", "--scalar-block-layout", os.path.join(after, name)], capture_output=True).returncode != 0:
            ok = subprocess.run([val, "--target-env", "vulkan1.3", "--scalar-block-layout", os.path.join(before, name)], capture_output=True).returncode == 0
            if ok:
                fails.append("%s: %s: NOT VALID SPIR-V with the pass" % (label, name))
    summary.append("%s: %d stages' SPIR-V through the pass (%d changed): the same instructions%s" % (label, n, changed, ", spirv-val accepts each" if val else "; spirv-val not found"))
    shutil.rmtree(before, ignore_errors=True), shutil.rmtree(after, ignore_errors=True)


def captures(a):
    os.makedirs(a.out, exist_ok=True)
    fs_real = "fragment" in a.kinds.split(",")
    res = {}
    for label, late in (("before", None), ("after", a.kinds)):
        for demand in (False, True):
            name = label + ("_demand" if demand else "")
            print("captures, %s%s" % (label, ", register demand" if demand else ""), file=sys.stderr)
            res[name] = spill_scan.scan(a.meshlab, a.frames, late=late, fs_real=fs_real, demand=demand, jobs=a.jobs,
                                        out=os.path.join(a.out, "radv_" + name) if a.keep else None)
    pair = lambda label: {k: (v[0] + v[1], res[label + "_demand"][k][5]) for k, v in res[label].items() if k in res[label + "_demand"]}
    fails, summary = verdict(pair("before"), pair("after"), a.margin, a.out, "captures")
    if a.spvlate:
        for spec in a.frames:
            frame, _, present = spec.partition(":")
            scene = os.path.basename(os.path.dirname(os.path.dirname(frame))) + ("_" + present if present else "")
            d = os.path.join(a.out, "spv_" + scene)
            os.makedirs(d, exist_ok=True)
            subprocess.run([a.meshlab, frame, "extract", d] + (["--present", present] if present else []), capture_output=True, check=True)
            valid(a, d, scene, fails, summary)
    if a.compare:
        gpu_run = os.path.join(HERE, "gpu_run.sh")
        for spec in a.frames:
            frame, _, present = spec.partition(":")
            scene = os.path.basename(os.path.dirname(os.path.dirname(frame))) + ("_" + present if present else "")
            out = os.path.join(a.out, "compare_%s.txt" % scene)
            cmd = [gpu_run, out, frame, "compare", "all", "--late", "mesh", "--quiet"] + (["--present", present] if present else [])
            r = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, MESHLAB=a.meshlab, MESHLAB_TIMEOUT="900"))
            m = re.search(r"^compare: (\d+) equal, (\d+) different, (\d+) without", r.stdout, flags=re.M)
            if not m:
                fails.append("%s: the compare did not finish (%s)" % (scene, out))
            else:
                summary.append("%s: %s of %d mesh draws equal with the pass" % (scene, m.group(1), int(m.group(1)) + int(m.group(2))))
                if int(m.group(2)) or r.returncode != 0:
                    fails.append("%s: %s draws DIFFERENT with the pass (%s)" % (scene, m.group(2), out))
    return finish(a, "captures", fails, summary)


def seed_scan(a, late, demand, label):
    """{(kind, code): (spills, demand, instructions)}: censustool --objects in shards, RADV's print read from stderr"""
    nvn = {"vertex": "0", "fragment": "1", "compute": "5", "task": "6", "mesh": "7"}
    kinds = {nvn[k] for k in a.kinds.split(",")}
    listing = subprocess.run([a.censustool, a.seed, "0", "0", "--list"], capture_output=True, text=True, check=True).stdout
    todo = [l.split()[0] for l in listing.splitlines() if l.split()[1] in kinds]
    if a.only:
        todo = [i for i in todo if i in set(a.only.split(","))]
    shards = [todo[i:i + a.shard] for i in range(0, len(todo), a.shard)]
    env = dict(os.environ, RADV_DEBUG="shaders", MESA_SHADER_CACHE_DISABLE="1")
    if demand:
        env["ACO_DEBUG"] = "nosched"
    rows, lates = {}, {}

    def one(n):
        cmd = [a.censustool, a.seed, "0", "0", "--objects", "--budget", str(a.budget), "--sink-peak", str(a.sink_peak)] + (["--late", late] if late else []) + ["--only"] + shards[n]
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        found = []
        parts = re.split(r"^@@object (\d+) ([0-9a-f]{16}) (\w+)$", r.stderr, flags=re.M)
        for i in range(1, len(parts), 4):
            index, code, kind, text = parts[i], parts[i + 1], parts[i + 2], parts[i + 3].split("@@end", 1)[0]
            want = {"mesh": "MESH", "task": "COMPUTE", "compute": "COMPUTE", "fragment": "FRAGMENT", "vertex": "VERTEX"}[kind]
            for name, stores, loads, spill, instructions, dem in spill_scan.stages_of(text):
                if name == want:
                    found.append((kind, code, stores + loads, dem, instructions))
        info = {}
        for line in r.stdout.splitlines():
            try:
                d = json.loads(line)
            except ValueError:
                continue
            if "late" in d:
                info[d["key"]] = d["late"]
        return n, found, info, r.returncode

    done = 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for n, found, info, rc in ex.map(one, range(len(shards))):
            done += len(shards[n])
            if rc != 0:
                print("seed, %s: a shard of censustool exited %d" % (label, rc), file=sys.stderr)
            for kind, code, spills, dem, instructions in found:
                old = rows.get((kind, code))
                if old is None or (spills, dem) > old[:2]:
                    rows[(kind, code)] = (spills, dem, instructions)
            lates.update(info)
            print("seed, %s: %d / %d stages" % (label, done, len(todo)), file=sys.stderr)
    return rows, lates, len(todo)


def seed(a):
    os.makedirs(a.out, exist_ok=True)
    res, n_asked = {}, 0
    lates = {}
    for label, late in (("before", None), ("after", a.kinds)):
        for demand in (False, True):
            name = label + ("_demand" if demand else "")
            res[name], info, n_asked = seed_scan(a, late, demand, name)
            if label == "after" and not demand:
                lates = info
    pair = lambda label: {k: (v[0], res[label + "_demand"][k][1]) for k, v in res[label].items() if k in res[label + "_demand"]}
    fails, summary = verdict(pair("before"), pair("after"), a.margin, a.out, "seed")
    if a.spvlate:
        nvn = {"vertex": "0", "fragment": "1", "compute": "5", "task": "6", "mesh": "7"}
        kinds = {nvn[k] for k in a.kinds.split(",")}
        listing = subprocess.run([a.censustool, a.seed, "0", "0", "--list"], capture_output=True, text=True, check=True).stdout
        todo = [l.split()[0] for l in listing.splitlines() if l.split()[1] in kinds]
        d = os.path.join(a.out, "spv_seed")
        os.makedirs(d, exist_ok=True)
        subprocess.run([a.censustool, a.seed, "0", "0", "--spv", d, "--spv-only", "--only"] + todo, capture_output=True)
        valid(a, d, "seed", fails, summary)
    summary.insert(0, "%d stages of the seed asked for, %d distinct codes compiled before, %d after" % (n_asked, len(res["before"]), len(res["after"])))
    if lates:
        ms = sorted(v["ms"] for v in lates.values())
        summary.append("the pass: %d stages changed of %d; its time a stage %.2f ms median, %.2f ms at most, %.0f ms in all" % (
            sum(1 for v in lates.values() if v["moved"]), len(lates), ms[len(ms) // 2], ms[-1], sum(ms)))
    return finish(a, "seed", fails, summary)


def finish(a, label, fails, summary):
    with open(os.path.join(a.out, "%s_verdict.txt" % label), "w") as f:
        for line in summary:
            f.write(line + "\n")
        for line in fails:
            f.write("FAIL: " + line + "\n")
        f.write("%s\n" % ("GATE FAILED: %d" % len(fails) if fails else "GATE PASSED"))
    print(open(os.path.join(a.out, "%s_verdict.txt" % label)).read(), end="")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    c = sub.add_parser("captures")
    c.add_argument("--meshlab", required=True)
    c.add_argument("--kinds", default="mesh")
    c.add_argument("--compare", action="store_true")
    c.add_argument("--keep", action="store_true", help="keep RADV's prints in <out>/radv_*")
    c.add_argument("--budget", type=int, default=160, help="for --spvlate: LateOptions::budget (meshlab uses the pass's defaults)")
    c.add_argument("--sink-peak", type=int, default=0)
    c.add_argument("frames", nargs="+")
    s = sub.add_parser("seed")
    s.add_argument("--censustool", required=True)
    s.add_argument("--seed", required=True)
    s.add_argument("--kinds", default="mesh")
    s.add_argument("--shard", type=int, default=10)
    s.add_argument("--only", help="stage indices of the seed, comma separated")
    s.add_argument("--budget", type=int, default=160, help="LateOptions::budget (gpu/direct/direct_spv_late.h)")
    s.add_argument("--sink-peak", type=int, default=0, help="LateOptions::sink_peak")
    for p in (c, s):
        p.add_argument("--out", required=True)
        p.add_argument("--spvlate", help="tools/meshlab/spvlate: every stage's SPIR-V through the pass must keep its instructions and stay valid (spirv-val)")
        p.add_argument("--margin", type=int, default=8)
        p.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()
    return captures(a) if a.mode == "captures" else seed(a)


if __name__ == "__main__":
    sys.exit(main())
