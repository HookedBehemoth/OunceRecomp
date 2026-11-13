"""Which stages of captured mesh programs RADV compiles with register spills to scratch memory.

    python3 -I tools/meshlab/spill_scan.py --meshlab <meshlab> [--repl <dir> | --late <kinds>] [--fs-real] [--demand]
                                           [--out <dir>] [--jobs N] <frame.bin | roll.bin:present>...

For every mesh program of each capture (one draw of it), `RADV_DEBUG=shaders meshlab <frame> stats <seq...>` makes its shader
objects (nothing is drawn) and RADV prints each stage as ACO compiled it; this counts `scratch_store_dword` and
`scratch_load_dword` (ACO's spills of vector registers: storage:vgpr_spill) in each stage after register allocation, and
reads the stage's size. A mesh stage that spills is the Hyrule Field loss's kind (docs/direct/hf-fix-report.md). Prints one
line a stage code (mesh stages by their code key, task stages as the compute shader the back end makes of them, fragment
stages by program: the harness's constant one unless --fs-real), spilling ones first. --late <kinds>: the stages of these
kinds (mesh, task, fragment) with the back end's register-pressure pass, as the game makes them
(docs/direct/spv-late-report.md; tools/meshlab/late_gate.py runs this before and after). The captures hold game data and
stay under work/; the table holds code keys and counts only.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys


def demand_of(ir):
    """The most vector registers the values of ACO's program hold at once, from its print after register allocation: each
    temporary from its first to its last line (straight-line code is exact; a loop's back edge is not followed)"""
    first, last, size = {}, {}, {}
    for n, line in enumerate(ir.split("\n")):
        for m in re.finditer(r"%(\d+):v\[(\d+)(?:-(\d+))?\]", line):
            i, a, b = int(m.group(1)), int(m.group(2)), int(m.group(3) or m.group(2))
            first.setdefault(i, n)
            last[i], size[i] = n, b - a + 1
    events = {}
    for i in first:
        events[first[i]] = events.get(first[i], 0) + size[i]
        events[last[i] + 1] = events.get(last[i] + 1, 0) - size[i]
    live = peak = 0
    for n in sorted(events):
        live += events[n]
        peak = max(peak, live)
    return peak


def stages_of(text):
    """[(stage name, stores, loads, spill operands, instructions, register demand)] from one RADV_DEBUG=shaders output"""
    out = []
    parts = re.split(r"^shader: MESA_SHADER_(\w+)$", text, flags=re.M)
    for i in range(1, len(parts), 2):
        name, body = parts[i], parts[i + 1]
        ra = body.split("After RA:", 1)
        ir = ra[1].split("disasm:", 1)[0] if len(ra) > 1 else ""
        dis = body.split("disasm:", 1)[1] if "disasm:" in body else ""
        stores = len(re.findall(r"^\s*scratch_store_dword ", ir, flags=re.M))
        loads = len(re.findall(r"= scratch_load_dword ", ir))
        spill = len(re.findall(r"storage:vgpr_spill", ir))
        instructions = len(re.findall(r"^\t\S.*;\s[0-9a-f]{8}", dis, flags=re.M))
        out.append((name, stores, loads, spill, instructions, demand_of(ir)))
    return out


def scan(meshlab, frames, repl=None, late=None, fs_real=False, demand=False, out=None, jobs=4, log=sys.stderr):
    """{(kind, code): [stores, loads, instructions, programs, scenes, register demand]} over the captures' mesh programs:
    one meshlab a capture makes every program's shader objects; its "@@object <kind> <program> <code>" lines say whose
    compile RADV prints next"""
    rows = {}
    work = []
    for spec in frames:
        frame, _, present = spec.partition(":")
        sel = ["--present", present] if present else []
        listing = subprocess.run([meshlab, frame, "list"] + sel, capture_output=True, text=True).stdout
        seen = {}
        for m in re.finditer(r"^\s+#(\d+)\s+(task\+mesh|mesh indirect|mesh)\s+([0-9a-f]{16}) mesh ([0-9a-f]{16}) task ([0-9a-f]{16})", listing, flags=re.M):
            seen.setdefault(m.group(3), m.group(1))
        scene = os.path.basename(os.path.dirname(os.path.dirname(frame))) + (":" + present if present else "")
        print("%s: %d mesh programs" % (scene, len(seen)), file=log)
        work.append((frame, sel, scene, seen))

    def one(item):
        frame, sel, scene, seen = item
        env = dict(os.environ, RADV_DEBUG="shaders", MESA_SHADER_CACHE_DISABLE="1")
        if demand:
            env["ACO_DEBUG"] = "nosched"
        cmd = [meshlab, frame, "stats"] + sorted(seen.values(), key=int) + sel + (["--repl", repl] if repl else []) + (["--late", late] if late else [])
        cmd += ["--fs", "real"] if fs_real else []
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if out:
            os.makedirs(out, exist_ok=True)
            open(os.path.join(out, "%s.txt" % scene.replace(":", "_")), "w").write(r.stderr)
        found = []
        parts = re.split(r"^@@object (\w+) ([0-9a-f]{16}) ([0-9a-f]{16})$", r.stderr, flags=re.M)
        for i in range(1, len(parts), 4):
            kind, prog, code, text = parts[i], parts[i + 1], parts[i + 2], parts[i + 3]
            want = {"mesh": "MESH", "task": "COMPUTE", "fragment": "FRAGMENT"}[kind]
            for name, stores, loads, spill, instructions, dem in stages_of(text):
                if name == want:
                    found.append((kind, prog, code, stores, loads, instructions, dem))
        return item, found, r.returncode

    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for (frame, sel, scene, seen), found, rc in ex.map(one, work):
            made = {prog for kind, prog, *_ in found if kind == "mesh"}
            if rc != 0 or made != set(seen):
                raise SystemExit("%s: meshlab stats failed (%d) or RADV printed %d of %d mesh stages" % (scene, rc, len(made), len(seen)))
            for kind, prog, code, stores, loads, instructions, dem in found:
                row = rows.setdefault((kind, code), [0, 0, 0, set(), set(), 0])
                row[0], row[1], row[2], row[5] = max(row[0], stores), max(row[1], loads), max(row[2], instructions), max(row[5], dem)
                row[3].add(prog), row[4].add(scene)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--meshlab", required=True)
    ap.add_argument("--repl", help="replacement stages (<code>.spv), as meshlab --repl")
    ap.add_argument("--late", help="kinds of stages (mesh,task,fragment) with the back end's register-pressure pass, as meshlab --late")
    ap.add_argument("--fs-real", action="store_true", help="the programs' own fragment stages too (meshlab --fs real)")
    ap.add_argument("--out", help="keep each program's RADV output here")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--demand", action="store_true", help="compile with ACO's scheduler off (ACO_DEBUG=nosched) and print each stage's "
                    "register demand: the scheduler moves loads up until the registers are full, which hides how many the stage needs")
    ap.add_argument("frames", nargs="+")
    a = ap.parse_args()
    rows = scan(a.meshlab, a.frames, a.repl, a.late, a.fs_real, a.demand, a.out, a.jobs)
    order = {"mesh": 0, "task": 1, "fragment": 2}
    print("kind      code              stores loads instructions registers programs scenes")
    spilling = 0
    for (kind, code), (stores, loads, instructions, progs, scenes, demand) in sorted(rows.items(), key=lambda kv: (-(kv[1][0] + kv[1][1] > 0), order.get(kv[0][0], 9), -kv[1][5], kv[0][1])):
        spilling += stores + loads > 0
        print("%-9s %s %6d %5d %12d %9d %s %s" % (kind, code, stores, loads, instructions, demand, ",".join(sorted(progs)) if kind != "fragment" else "-", ",".join(sorted(scenes))))
    print("%d stage codes, %d with scratch spills" % (len(rows), spilling))
    return 0


if __name__ == "__main__":
    sys.exit(main())
