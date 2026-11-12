"""meshlab inventory: every mesh stage a captured frame draws, through meshtool, grouped into families by code.

    work/venv/bin/python -I tools/meshlab/inventory.py <frame.bin>... --stages <dir of seed stage .bin> --out <dir>

For each distinct mesh code key in the frames (meshlab list), meshtool translates the stage (with its task stage when the
program has one) and summarises it; the families are the stages whose IR is the same once every literal number is masked
(the constants the game's compiler baked in: offsets into the uniform buffers, attribute counts, scales). Writes
<out>/inventory.txt (a line a stage), <out>/families.txt, and each stage's IR, print and SPIR-V. Local output: it is made
from the game's code and stays under work/.
"""
import argparse
import collections
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def frame_draws(meshlab, frame):
    out = subprocess.run([meshlab, frame, "list"], capture_output=True, text=True).stdout
    draws = []
    for line in out.splitlines():
        m = re.match(r"\s+#(\d+)\s+(task\+mesh|mesh indirect|mesh)\s+([0-9a-f]{16}) mesh ([0-9a-f]{16}) task ([0-9a-f]{16}) groups (\d+)", line)
        if m:
            draws.append({"seq": int(m[1]), "kind": m[2], "key": m[3], "mesh": m[4], "task": m[5], "groups": int(m[6])})
    return draws


def summary(text):
    d = {}
    for line in text.splitlines():
        k, _, v = line.partition(" ")
        d[k] = v
    return d


def normalised(ir_c):
    s = re.sub(r"0x[0-9a-f]+", "#", ir_c)
    s = re.sub(r"(?<![A-Za-z_])\d+(\.\d+)?(e[-+]?\d+)?f?", "#", s)
    s = re.sub(r"v#", "v", s)
    return hashlib.sha1(s.encode()).hexdigest()[:12]


def shape(ir_c):
    """The stage's control shape: its if / loop / return nesting, without the expressions"""
    out = []
    for line in ir_c.splitlines():
        t = line.strip()
        if t.startswith("if (") or t.startswith("loop") or t.startswith("}") or t == "return;" or t.startswith("} while"):
            out.append(t[:2] if t.startswith("if") else t[:5])
    return hashlib.sha1("".join(out).encode()).hexdigest()[:12], sum(1 for x in out if x.startswith("loop"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames", nargs="+")
    ap.add_argument("--stages", default=os.path.join(ROOT, "work", "proto", "direct-m0", "tm", "stages_all"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--meshlab", default=os.path.join(ROOT, "work", "meshlab", "meshlab"))
    ap.add_argument("--meshtool", default=os.path.join(ROOT, "work", "meshlab", "meshtool"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    stages = {}
    for frame in a.frames:
        scene = os.path.basename(os.path.dirname(os.path.dirname(os.path.abspath(frame))))
        for d in frame_draws(a.meshlab, frame):
            s = stages.setdefault(d["mesh"], {"task": d["task"], "programs": set(), "draws": collections.Counter(), "kinds": set(), "groups": 0})
            s["programs"].add(d["key"])
            s["draws"][scene] += 1
            s["kinds"].add(d["kind"])
            s["groups"] += d["groups"]
    rows = []
    for code, s in sorted(stages.items()):
        prefix = os.path.join(a.out, code)
        cmd = [a.meshtool, os.path.join(a.stages, "7_%s.bin" % code), prefix]
        if s["task"] != "0" * 16:
            cmd.append(os.path.join(a.stages, "6_%s.bin" % s["task"]))
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print("meshtool failed on %s: %s" % (code, r.stderr.strip()), file=sys.stderr)
            continue
        info = summary(r.stdout)
        ir_c = open(prefix + ".ir.c").read()
        info["norm"] = normalised(ir_c)
        info["shape"], info["loops"] = shape(ir_c)
        info.update(code=code, task=s["task"], programs=len(s["programs"]), draws=dict(s["draws"]), kinds=sorted(s["kinds"]), groups=s["groups"])
        ops = dict(x.split(":") for x in info.get("ops", "").split() if ":" in x)
        info["atomics"] = sum(int(v) for k, v in ops.items() if "Atomic" in k)
        info["stores"] = sum(int(v) for k, v in ops.items() if k.startswith("WriteGlobal") or k.startswith("WriteStorage"))
        info["global_loads"] = sum(int(v) for k, v in ops.items() if k.startswith("LoadGlobal"))
        info["storage_loads"] = sum(int(v) for k, v in ops.items() if k.startswith("LoadStorage"))
        info["isbe_writes"] = sum(int(v) for k, v in ops.items() if k.startswith("IsbeWrite"))
        info["textures"] = info.get("textures", "0")
        rows.append(info)
    with open(os.path.join(a.out, "inventory.txt"), "w") as f:
        for r in rows:
            f.write("%s task %s programs %d draws %s kinds %s groups %d native %s local %s v/p %s/%s payload %s cbufs [%s] ssbos [%s] tex %s "
                    "loops %s atomics %d stores %d storage_loads %d global_loads %d norm %s shape %s vlines %d\n" % (
                        r["code"], r["task"], r["programs"], r["draws"], "/".join(r["kinds"]), r["groups"], r["native_instructions"], r["local_memory"],
                        r["max_vertices"], r["max_primitives"], r["payload_words"], r["cbufs"].strip(), r["ssbos"].strip(), r["textures"],
                        r["loops"], r["atomics"], r["stores"], r["storage_loads"], r["global_loads"], r["norm"], r["shape"],
                        len(r["vertex_lines"].split())))
    fams = collections.defaultdict(list)
    for r in rows:
        fams[(r["task"], r["norm"])].append(r["code"])
    shapes = collections.defaultdict(list)
    for r in rows:
        shapes[(r["task"], r["shape"])].append(r["code"])
    with open(os.path.join(a.out, "families.txt"), "w") as f:
        f.write("%d mesh stages; %d distinct after masking literals; %d distinct control shapes\n" % (len(rows), len(fams), len(shapes)))
        for (task, sh), codes in sorted(shapes.items(), key=lambda x: -len(x[1])):
            f.write("shape %s task %s: %d stages: %s\n" % (sh, task, len(codes), " ".join(codes)))
        for (task, n), codes in sorted(fams.items(), key=lambda x: -len(x[1])):
            f.write("norm %s task %s: %d stages: %s\n" % (n, task, len(codes), " ".join(codes)))
    print(open(os.path.join(a.out, "families.txt")).read())


if __name__ == "__main__":
    main()
