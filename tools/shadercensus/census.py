"""The shader census: every stage of the game's shader seed through censustool, in shards, resumable.

    work/venv/bin/python -I tools/shadercensus/census.py <seed> --out <dir> [--jobs N] [--shard 250] [--compile]

<seed> is nx2/shader_census.py's output (hookshot_shaders.bin, made from the player's romfs). Each shard of stages runs
censustool once (a stage that crashes it is recorded as an error and the rest of the shard is run again without it) and
writes <out>/shards/<first>.jsonl; a shard already complete is not run again, so the census can be stopped and resumed. At
the end every shard is joined into <out>/stages.jsonl. All output is made from the game's code and stays under work/.
Compiling (--compile) makes pipelines on the GPU but draws nothing: it needs no GPU lock.
"""
import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def run_shard(tool, seed, first, count, out, compile_):
    path = os.path.join(out, "shards", "%06d.jsonl" % first)
    if os.path.exists(path):
        with open(path) as f:
            if sum(1 for _ in f) == count:
                return first, count, 0
    todo = list(range(first, first + count))
    lines = {}
    crashes = 0
    env = dict(os.environ, MESA_SHADER_CACHE_DISABLE="1")
    while todo:
        cmd = [tool, seed, "0", "0", "--only"] + [str(i) for i in todo]
        if compile_:
            cmd.append("--compile")
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        for line in r.stdout.splitlines():
            try:
                d = json.loads(line)
            except ValueError:
                continue
            lines[d["index"]] = line
        if r.returncode == 0 and all(i in lines for i in todo):
            break
        # (a crash: the stage it began last is the one; recorded as an error, the rest run again)
        begun = [int(x.split()[1]) for x in r.stderr.splitlines() if x.startswith("@@stage ")]
        bad = begun[-1] if begun else todo[0]
        if bad not in lines:
            lines[bad] = json.dumps({"index": bad, "error": "censustool exited %d on this stage" % r.returncode})
        crashes += 1
        todo = [i for i in todo if i not in lines]
    with open(path + ".tmp", "w") as f:
        for i in range(first, first + count):
            f.write(lines.get(i, json.dumps({"index": i, "error": "no result"})) + "\n")
    os.replace(path + ".tmp", path)
    return first, count, crashes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("seed")
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--shard", type=int, default=250)
    ap.add_argument("--compile", action="store_true")
    ap.add_argument("--tool", default=os.path.join(ROOT, "work", "shadercensus", "censustool"))
    a = ap.parse_args()
    os.makedirs(os.path.join(a.out, "shards"), exist_ok=True)
    shape = json.loads(subprocess.run([a.tool, a.seed, "-1", "-1"], capture_output=True, text=True, check=True).stdout)
    n = shape["stages"]
    started = time.time()
    done = 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        futs = [ex.submit(run_shard, a.tool, a.seed, f, min(a.shard, n - f), a.out, a.compile) for f in range(0, n, a.shard)]
        for fu in concurrent.futures.as_completed(futs):
            first, count, crashes = fu.result()
            done += count
            print("%6d / %d stages (%.0f s)%s" % (done, n, time.time() - started, ", %d crashed" % crashes if crashes else ""), flush=True)
    with open(os.path.join(a.out, "stages.jsonl"), "w") as f:
        for first in range(0, n, a.shard):
            with open(os.path.join(a.out, "shards", "%06d.jsonl" % first)) as g:
                f.write(g.read())
    print("wrote", os.path.join(a.out, "stages.jsonl"))


if __name__ == "__main__":
    sys.exit(main())
