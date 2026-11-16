"""The lift's input as plain data, for the Rust lifter (ounce/lift): the image and the facts of it the image files do not
hold, the patches' declarations and rows, and Python's plan, summaries and records as the diagnostic's reference. The format is
ounce/lift/EXPORT.md. Made from the game: written under work/, never committed. (It is outside nx2/lift2: the build
keys its objects by every source there.)

    work/venv/bin/python -I nx2/lift_export.py --game games/oot work/ounce/export
"""
import json
import os
import subprocess
import sys
import time

NX2 = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(NX2)
FORMAT = 4


def commit():
    """git HEAD of this tree, "+dirty" when the lifter, the host or the game description differ from it"""
    head = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--", "nx2", "host", "games"],
                           capture_output=True, text=True).stdout.strip()
    return head + ("+dirty" if dirty else "")


def setup(game_path, work=None):
    """(plan path, plan, gen, PATCH_FUNCs, partitions) as prepare() has them; stops if the plan's summaries are not those
    of the patches as declared now (prepare would compute them again: run a build first)"""
    if NX2 not in sys.path:
        sys.path.insert(0, NX2)
    import gamedesc
    gamedesc.use(game_path)
    from lift2 import game, patch_scan, probes
    work = work or os.environ.get("OOT_WORK", os.path.join(ROOT, "work"))
    sa = os.path.join(work, "sa")
    gen, plan_path = os.path.join(sa, "gen"), os.path.join(sa, "plan.pickle")
    if not os.path.exists(plan_path):
        raise SystemExit("export: no plan %s: lift the game first (scripts/relift.sh)" % plan_path)
    plan = game._plan(plan_path)
    cache = game.summary_cache(plan_path, plan["summary_base"], game.summary_effects(plan["hle"], game.replaced(gen)))
    if plan.get("summary_cache") != os.path.basename(cache):
        raise SystemExit("export: the plan's summaries (%s) are not those of the patches now (%s): build once first"
                         % (plan.get("summary_cache"), os.path.basename(cache)))
    funcs = patch_scan.check_funcs(plan, gen)
    probes.resolve(plan, plan_path)
    return plan_path, plan, gen, funcs, game.partitions(plan)


def input_key(plan, gen, funcs):
    """what the IR is made of, as one key: the lifter's code, the plan's facts and summaries, the patches' rows and
    PATCH_FUNCs, the HLE'd functions and their declarations (the export and the golden carry it: one made from other
    inputs is stale)"""
    import hashlib
    from lift2 import game, host_effects
    hle = host_effects.hle_names(gen)
    decl = {n: (host_effects.clobbers(n), host_effects.hle_reads(n), host_effects.answer(n), host_effects.stops(n))
            for n in sorted(set(hle.values()))}
    return hashlib.sha1(repr((game.code_key(), game.facts_key(plan), plan.get("summary_cache"),
                              sorted(game.patch_rows().items()), sorted((a, f.name, f.orig_name) for a, f in funcs.items()),
                              sorted(hle.items()), sorted(decl.items()))).encode(), usedforsecurity=False).hexdigest()


def answers(hle):
    """[[address, symbol, value]] of the answered functions (PATCH_ANSWER), by address: game.answers_ir's module"""
    from lift2 import host_effects
    out = []
    for a, n in sorted(hle.items()):
        v = host_effects.answer(n)
        if v is not None:
            out.append([a, n, v & 0xFFFFFFFFFFFFFFFF])
    return out


def part_name(tag, k):
    return "lift_%s%03d" % (tag, k)


def mask(m):
    return "%x" % m


def bound_json(addr, b):
    return {"addr": addr, "kind": b.kind, "reads": mask(b.reads), "clobbers": mask(b.clobbers), "keeps": mask(b.keeps),
            "must": mask(b.must), "results": mask(b.results), "noreturn": bool(b.noreturn), "symbol": b.symbol,
            "tail": b.tail, "answer": None if b.answer is None else b.answer & 0xFFFFFFFFFFFFFFFF}


def runs(pcs):
    out = []
    for pc in pcs:
        if out and out[-1][0] + 4 * out[-1][1] == pc:
            out[-1][1] += 1
        else:
            out.append([pc, 1])
    return out


def row_json(p):
    d = {"pc": p.pc, "word": p.word, "fn": p.fn, "args": list(p.args), "result": p.result}
    if type(p).__name__ == "At":
        d["mode"] = "instead" if p.replace else "before" if p.before else "after"
        d["io"] = list(p.io)
        d["guest"] = list(p.guest) if isinstance(p.guest, tuple) else p.guest
    return d


def summary_inputs(plan, hle, funcs):
    """what the summaries and the call-boundary records read beyond the partitions' functions (EXPORT.md, format 3):
    the program's entries and PLT stubs, the symbol names the noreturn seeds match, the HLE'd functions' declared
    effects, the rows' declared effects by pc, the PATCH_FUNCs' declarations, and the bodies the summaries see where
    they differ from the lifted ones"""
    import pickle
    from horizon import NORETURN_NAMES, NORETURN_PARTS
    from lift2 import game, host_effects, summary
    with open(plan["summary_input"], "rb") as f:
        d = pickle.load(f)
    if d["hle"] != hle or set(plan["hooks"]) != set(hle):
        raise SystemExit("export: the summaries' HLE'd functions are not the image's hooks")
    over = []
    for e, (pcs, words) in sorted(d["funcs"].items()):
        b = plan["bodies"][e]
        if list(b[0]) != list(pcs) or list(b[1]) != list(words):
            over.append({"entry": e, "pcs": runs(list(pcs)), "words": "".join("%08x" % x for x in words)})
    if set(d["funcs"]) != set(plan["bodies"]):
        raise SystemExit("export: the summaries' functions are not the plan's")
    facts = game.replaced(os.path.join(os.path.dirname(plan["summary_input"]), "gen"))
    if set(facts) != set(funcs):
        raise SystemExit("export: the summaries' PATCH_FUNCs are not the build's")
    hle_rows = []
    for a, n in sorted(hle.items()):
        v = host_effects.answer(n)
        hle_rows.append([a, n, bool(host_effects.stops(n)), None if v is None else v & 0xFFFFFFFFFFFFFFFF,
                         host_effects.clobbers(n) is not None, mask(sum(summary.BIT[r] for r in host_effects.hle_reads(n)))])
    rows = []
    for pc, (bu, au, bd, ad, calls, repl) in sorted(summary.rows().items()):
        rows.append([pc, mask(bu), mask(au), mask(bd), mask(ad), calls if calls == "computed" else list(calls), repl])
    return {
        "entries": sorted(plan["entries"]),
        "plt": [[s, t, slot] for s, (t, slot) in sorted(plan["plt"].items())],
        "names": [[e, n] for e, n in sorted(d["names"].items())],
        "noreturn_names": list(NORETURN_NAMES), "noreturn_parts": list(NORETURN_PARTS),
        "hle": hle_rows,
        "row_effects": rows,
        "patch_funcs": [[e, mask(r), mask(res), bool(o), list(g)] for e, (r, res, o, g) in sorted(facts.items())],
        "summary_bodies": over,
    }


def pack_json(e, s):
    """a packed summary (summary.pack) as check/summaries.json has it"""
    (inputs, clobbers, mustdef, preserved, results, returns, demote, unknown, frame, pairs, boundary, fp, fpcr) = s
    return [e, mask(inputs), mask(clobbers), mask(mustdef), mask(preserved), mask(results), bool(returns),
            list(demote), bool(unknown), frame, pairs, bool(boundary), bool(fp), bool(fpcr)]


def image_facts(plan_path):
    """(the image as the lift built it, its facts the image files do not hold): EdenImage at ASLR 0 with the standalone
    HLE set, as nx2/lift.py --standalone builds it; checked against the lift's image.bin"""
    import hashlib
    import kit
    import nso
    import standalone
    import image as eden_image
    exefs = kit.need_exefs()
    hle, stubbed = standalone.hle_set(nso.load_exefs(exefs))
    img = eden_image.EdenImage(exefs, 0, hle=hle | stubbed)
    path = os.path.join(os.path.dirname(plan_path), "image", "image.bin")
    with open(path, "rb") as f:
        disk = f.read()
    if hashlib.sha256(bytes(img.mem)).hexdigest() != hashlib.sha256(disk).hexdigest():
        raise SystemExit("export: the image built now is not the lift's %s: lift again" % path)
    return img, hashlib.sha256(disk).hexdigest()


def words_key(words):
    """a body's words as check/plan.json keys them: sha256 of their little-endian bytes, 16 hex digits"""
    import hashlib
    import struct
    return hashlib.sha256(struct.pack("<%dI" % len(words), *words)).hexdigest()[:16]


_CENSUS = {}


def _census(names):
    from lift2 import code_ptrs
    a, p, kinds, odd = code_ptrs.count(_CENSUS["plan"], names, _CENSUS["texts"])
    return [a + p, kinds, [[pc, why] for pc, why in odd]]


def check_plan(plan, parts, texts, jobs=8):
    """the diagnostic's reference of the discovery and the plan (check/plan.json): Python's plan and summary input"""
    import multiprocessing as mp
    import pickle
    with open(plan["summary_input"], "rb") as f:
        raw = pickle.load(f)["funcs"]
    bodies, raw_diff = [], []
    for e, (pcs, words) in sorted(plan["bodies"].items()):
        bodies.append([e, runs(list(pcs)), words_key(list(words))])
        rp, rw = raw[e]
        if list(rp) != list(pcs) or list(rw) != list(words):
            raw_diff.append([e, runs(list(rp)), words_key(list(rw))])
    _CENSUS.update(plan=plan, texts=texts)
    with mp.get_context("fork").Pool(jobs) as pool:
        census = pool.map(_census, [names for _, names in parts])
    return {
        "entries": sorted(plan["entries"]), "bodies": bodies, "raw_bodies": raw_diff,
        "insns": [[e, n] for e, n in sorted(plan["insns"].items())],
        "jt": [[br, list(t)] for br, t in sorted(plan["jt"].items())],
        "jt_index": [[br, v[0], v[1], v[2], list(v[3])] for br, v in sorted(plan["jt_index"].items())],
        "plt": [[s, t, slot] for s, (t, slot) in sorted(plan["plt"].items())],
        "no_call": sorted(plan["no_call"]), "sj_rets": [[e] + list(r) for e, r in sorted(plan["sj_rets"].items())],
        "hooks": sorted(plan["hooks"]), "why": sorted(plan["why"].items()), "hot": sorted(plan["hot"]),
        "code_excluded": sorted(plan["code_excluded"]),
        "partitions": [[tag, names] for tag, names in parts],
        "census": census,
    }


def reach_of(plan, names, callee, entries, rows_checked):
    """the callees whose records the functions' lift reads (EXPORT.md, "callee(t) and bound(c)"), after the checks
    lift_part's probes.find / probes.at make"""
    from lift2 import code_ptrs, probes
    jt, jt_index, plt = plan["jt"], plan.get("jt_index", {}), plan["plt"]
    reach = set()
    for e in names:
        pcs, words = plan["bodies"][e]
        pcs, words = list(pcs), list(words)
        body = set(pcs)
        if e in rows_checked:
            w = dict(zip(pcs, words))
            probes.find(e, pcs, w.__getitem__)
            for kw in ({"before": True, "jt": jt, "entries": entries}, {}, {"replace": True, "jt": jt}):
                probes.at(e, pcs, w.__getitem__, **kw)
        body_entries = body & entries
        targets = set()
        for pc, word in zip(pcs, words):
            t = code_ptrs._branch_target(pc, word)
            if t is not None:
                t &= 0xFFFFFFFFFFFFFFFF
                if (word & 0xFC000000) == 0x94000000 or t not in body or (t != e and t in body_entries):
                    targets.add(t)
            n = pc + 4
            if n not in body or (n != e and n in body_entries):
                targets.add(n)
        for br in body & (set(jt) | set(jt_index)):
            for t in list(jt.get(br, ())) + list(jt_index[br][3] if br in jt_index else ()):
                if t not in body or (t != e and t in body_entries):
                    targets.add(t)
        if e in plt:
            targets.add(plt[e][0])
        reach |= {callee(t) for t in targets}
        reach.add(e)
    return reach


def main():
    import argparse
    import shutil
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--game", required=True, help="the game description, games/<game>")
    ap.add_argument("out", help="the export's folder (under work/)")
    a = ap.parse_args()
    t0 = time.time()
    plan_path, plan, gen, funcs, parts = setup(a.game)
    from horizon import SETJMP_NAMES
    from lift2 import bounds, game, host_effects, probes
    hle = host_effects.hle_names(gen)
    host_effects.check(hle)
    img, sha = image_facts(plan_path)
    bound = bounds.call_bounds(plan, hle, False, funcs)
    callee = game.callee_fn(plan["plt"])
    entries = set(plan["entries"])
    values, ats = probes.tables()
    os.makedirs(os.path.join(a.out, "check"), exist_ok=True)
    os.makedirs(os.path.join(a.out, "image"), exist_ok=True)
    src = os.path.join(os.path.dirname(plan_path), "image")
    for f in ("image.bin", "image.json"):
        shutil.copyfile(os.path.join(src, f), os.path.join(a.out, "image", f))
    t1 = time.time()
    rows_checked = {p.func for p in values} | {p.func for p in ats}
    reach = set()
    for tag, names in parts:
        reach |= reach_of(plan, names, callee, entries, rows_checked)
    meta = {"format": FORMAT, "commit": commit(), "input_key": input_key(plan, gen, funcs), "code_key": game.code_key(),
            "facts_key": game.facts_key(plan),
            "summary_cache": plan.get("summary_cache"), "verify": False, "omit": True,
            "image_sha256": sha,
            "image_entries": sorted(img.entries),
            "eh_frame_hdr": [[m.name, m.base + m.eh_frame_hdr[0], m.base + m.eh_frame_hdr[1]] for m in img.modules],
            "setjmp_names": list(SETJMP_NAMES),
            "part_insns": game.PART_INSNS,
            "answers": answers(hle),
            "values": [dict(row_json(p), func=p.func) for p in values],
            "rows": [dict(row_json(p), func=p.func) for p in ats],
            "func_names": [[e, f.name, f.orig_name] for e, f in sorted(funcs.items())]}
    si = summary_inputs(plan, hle, funcs)
    for k in ("entries", "plt", "summary_bodies"):   # (computed by the Rust side now: the discovery)
        del si[k]
    if si["names"] != [[e, n] for e, n in sorted(img.names.items())]:
        raise SystemExit("export: the summaries' names are not the image's")
    meta.update(si)
    texts = [(seg["addr"], seg["addr"] + seg["size"]) for mod in json.load(open(os.path.join(src, "image.json")))["modules"]
             for seg in mod["segments"] if seg["name"] == ".text"]
    # (the diagnostic's reference, read by ounce-lift --check only: Python's plan, summaries and records)
    with open(os.path.join(a.out, "check", "plan.json"), "w") as f:
        json.dump(check_plan(plan, parts, texts), f, separators=(",", ":"))
    with open(os.path.join(a.out, "check", "summaries.json"), "w") as f:
        json.dump([pack_json(e, s) for e, s in sorted(plan["summaries"].items())], f, separators=(",", ":"))
    with open(os.path.join(a.out, "check", "bounds.json"), "w") as f:
        json.dump({"bounds": [bound_json(c, bound(c)) for c in sorted(reach)],
                   "own": [bound_json(e, bound.own(e)) for e in sorted(funcs)]}, f, separators=(",", ":"))
    with open(os.path.join(a.out, "meta.json"), "w") as f:
        json.dump(meta, f, separators=(",", ":"))
    size = sum(os.path.getsize(os.path.join(d, x)) for d, _, xs in os.walk(a.out) for x in xs)
    print("export: %d partitions, %d functions, %.1f MB in %s (setup %.0fs, export %.0fs)" % (
        len(parts), sum(len(n) for _, n in parts), size / 1e6, a.out, t1 - t0, time.time() - t1), flush=True)


if __name__ == "__main__":
    main()
