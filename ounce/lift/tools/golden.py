"""The golden of the Rust lifter's phase 1: the Python lifter's IR text of every partition (game.lift_part, verify off),
zstd-compressed, with each function's byte range and sha1 (ounce/lift/EXPORT.md, "Golden"). Made from the game: it
stays under work/.

    work/venv/bin/python -I ounce/lift/tools/golden.py --game games/oot [--jobs N] [--only lift_c000,...] work/ounce/golden

Beside the IR: every other generated compile input (gen/, rt64/, compile.json), gated by its sha256 in meta.json's
`files`, with image.bin's (the run-time image). reference/ holds intermediate data that is no compile input and that
nothing reads at run time, kept to check the Rust image stage against: reference/image/image.json and functions.json
(meta.json `reference`).

Workers are forked from a process that holds the plan (copy on write: one plan's memory, not one a worker).
"""
import concurrent.futures
import gc
import hashlib
import json
import multiprocessing
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "nx2"))

ANSWERS = "lift_answers"   # (the answers' module: game.answers_ir, prepare's lift_answers.o)
DEFINE = re.compile(rb"^define [^@\n]*@([\w.$]+)\(", re.M)
_STATE = {}


def index(text):
    """{name: [start, end, sha1]} of every function the IR text defines: its define line to its closing "}" line"""
    out = {}
    for m in DEFINE.finditer(text):
        end = text.index(b"\n}\n", m.start()) + 3
        out[m.group(1).decode()] = [m.start(), end, hashlib.sha1(text[m.start():end]).hexdigest()]
    return out


def _lift(k):
    from lift2 import game
    tag, names = _STATE["parts"][k]
    t = time.time()
    text = game.lift_part((_STATE["plan_path"], names, _STATE["funcs"]))
    import resource
    return k, text.encode(), time.time() - t, resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024


# the lift's generated C and lists (nx2/lift.py --standalone --write: tables.write, write_code_slots, standalone.*)
GEN = ("a64_table.c", "a64_hooks.c", "a64_code_slots.c", "a64_stubs.c", "hos_image.c", "nvn2_exports.c", "functions.txt")
# (the image's description and the discovery's list: no compile input, nothing reads them at run time; reference/)
REFERENCE = ("image.json", "functions.json")
FILES_NOTE = ("the compile inputs' sha256 (uncompressed), each file under its path with .zst; image/image.bin, the "
              "run-time image hookshot loads, by its sha256 alone")
REFERENCE_NOTE = ("not compile inputs: intermediate data of the image stage, read by nothing at run time (the Rust "
                  "pipeline keeps it in memory), kept to check that stage against")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def lift_mark(sa):
    p = os.path.join(sa, "LIFT_OK")
    return open(p).read() if os.path.exists(p) else None


def compile_inputs(out, sa, parts, names):
    """everything else the build compiles that the lift and build generate: the lift's C (gen/), the guest model's
    header and runtime (nx2/rtgen.py, rt64/), the image's description, and the compile flags of each partition, the
    answers module and the generated C; and, under reference/, the image's intermediate data, no compile input
    (({path: sha256} of the compile inputs, {path: sha256} of the reference files) for meta.json)"""
    from compression import zstd
    import build
    import rtgen
    from lift2 import game, llvm
    files = {}

    def put(rel, data):
        os.makedirs(os.path.dirname(os.path.join(out, rel)), exist_ok=True)
        with open(os.path.join(out, rel) + ".zst", "wb") as f:
            f.write(zstd.compress(data, level=9))
        files[rel] = hashlib.sha256(data).hexdigest()
    for f in GEN:
        put("gen/" + f, open(os.path.join(sa, "gen", f), "rb").read())
    ref = {}
    for f in REFERENCE:
        put("reference/image/" + f, open(os.path.join(sa, "image", f), "rb").read())
        ref["reference/image/" + f] = files.pop("reference/image/" + f)
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        rtgen.write(d)
        for f in sorted(os.listdir(d)):
            put("rt64/" + f, open(os.path.join(d, f), "rb").read())
    clang = llvm.check("clang")
    march = build.target() + build.CPU
    opt, hot = "O1", os.environ.get("RX_HOT_OPT", "O2")   # (build.py's --opt default and prepare's hot level)
    unit = []
    for k, (tag, _) in enumerate(parts):
        level = hot if tag == "h" else opt
        unit.append({"name": names[k], "tag": tag, "level": level, "clang": game.part_flags(level, march),
                     "opt": game.opt_argv(clang, level, march)})
    unit.append({"name": ANSWERS, "tag": "c", "level": unit[0]["level"], "clang": unit[0]["clang"], "opt": unit[0]["opt"]})
    gen_c = {"sources": ["gen/" + f for f in GEN if f.endswith(".c")] + ["rt64/a64_rt.c"],
             "clang": build.flags("<gen>", "<rt64>", opt)}
    with open(os.path.join(out, "compile.json"), "w") as f:
        json.dump({"partitions": unit, "generated_c": gen_c,
                   "note": "clang flags without the input and output; opt: the `opt` command the IR goes through first "
                           "(then clang with -Xclang -disable-llvm-passes), null if none; <gen>, <rt64>: the folders"},
                  f, indent=1)
    return files, ref


def main():
    import argparse
    from compression import zstd
    import lift_export
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--game", required=True)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--only", default="", help="partition names, comma-separated (default all)")
    ap.add_argument("--force", action="store_true", help="made again even if its stamp matches the inputs")
    ap.add_argument("--no-index", action="store_true", help="the IR only (a determinism run)")
    ap.add_argument("out")
    a = ap.parse_args()
    if os.environ.get("HOOKSHOT_LIFT_VERIFY"):
        raise SystemExit("golden: the golden is of the build without HOOKSHOT_LIFT_VERIFY")
    t0 = time.time()
    plan_path, plan, gen, funcs, parts = lift_export.setup(a.game)
    key = lift_export.input_key(plan, gen, funcs)
    sa = os.path.dirname(os.path.realpath(plan_path))   # (the lift's folder: the plan, its gen/ and image/)
    mark = lift_mark(sa)
    if mark is None:
        raise SystemExit("golden: %s has no LIFT_OK: a lift in progress or stopped" % sa)
    for f in ("a64_hooks.c",):   # (the export read the HLE'd functions from this checkout's gen/: the lift's)
        if sha256(os.path.join(gen, f)) != sha256(os.path.join(sa, "gen", f)):
            raise SystemExit("golden: %s/%s is not the lift's (%s): copy the lift's gen/" % (gen, f, sa))
    old = os.path.join(a.out, "meta.json")
    if not a.only and not a.force and os.path.exists(old) and json.load(open(old)).get("input_key") == key and \
            json.load(open(old)).get("lift") == mark:
        print("golden: %s is of these inputs (input_key %s): kept (--force to make it again)" % (a.out, key[:12]))
        return
    _STATE.update(plan_path=plan_path, funcs=funcs, parts=parts)
    names = [lift_export.part_name(tag, k) for k, (tag, _) in enumerate(parts)]
    want = [k for k, n in enumerate(names) if not a.only or n in a.only.split(",")]
    os.makedirs(a.out, exist_ok=True)
    t1 = time.time()
    secs = {}
    # (forked workers share the plan; frozen, the collector never touches its objects, so their pages stay shared. A
    # worker killed (out of memory) breaks the pool: the run stops instead of waiting)
    gc.freeze()
    rss = {}
    with concurrent.futures.ProcessPoolExecutor(a.jobs, mp_context=multiprocessing.get_context("fork")) as ex:
        for k, text, s, r in (f.result() for f in concurrent.futures.as_completed([ex.submit(_lift, k) for k in want])):
            rss[names[k]] = r
            secs[names[k]] = round(s, 1)
            base = os.path.join(a.out, names[k])
            with open(base + ".ll.zst.tmp", "wb") as f:
                f.write(zstd.compress(text, level=9))
            os.replace(base + ".ll.zst.tmp", base + ".ll.zst")
            if not a.no_index:
                with open(base + ".idx.json", "w") as f:
                    json.dump(index(text), f, separators=(",", ":"))
            print("golden: %s %.1f MB in %.0fs (worker peak RSS %d MB)" % (names[k], len(text) / 1e6, s, r), flush=True)
    from lift2 import game, host_effects
    if not a.only:   # (the answered functions' generic entries: game.answers_ir, its own module)
        text = game.answers_ir(host_effects.hle_names(gen)).encode()
        base = os.path.join(a.out, ANSWERS)
        with open(base + ".ll.zst", "wb") as f:
            f.write(zstd.compress(text, level=9))
        if not a.no_index:
            with open(base + ".idx.json", "w") as f:
                json.dump(index(text), f, separators=(",", ":"))
        want_names = [names[k] for k in want] + [ANSWERS]
        files, ref = compile_inputs(a.out, sa, parts, names)
        files["image/image.bin"] = sha256(os.path.join(sa, "image", "image.bin"))
    else:
        files, ref = {}, {}
        want_names = [names[k] for k in want]
    if lift_mark(sa) != mark:
        raise SystemExit("golden: the lift in %s changed during the run: made again" % sa)
    meta = {"commit": lift_export.commit(), "input_key": key, "lift": mark,
            "dump": json.load(open(os.path.join(sa, "game.json"))), "files": files, "files note": FILES_NOTE,
            "reference": ref, "reference note": REFERENCE_NOTE, "code_key": game.code_key(), "facts_key": game.facts_key(plan),
            "partitions": want_names, "seconds": secs, "jobs": a.jobs, "worker peak RSS MB": max(rss.values(), default=0),
            "wall seconds": round(time.time() - t1)}
    with open(os.path.join(a.out, "meta.json"), "w") as f:
        json.dump(meta, f, indent=1)
    print("golden: %d partitions in %s (setup %.0fs, lift %.0fs with %d jobs)" % (
        len(want), a.out, t1 - t0, time.time() - t1, a.jobs), flush=True)


if __name__ == "__main__":
    main()
