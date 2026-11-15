"""The lift's input as plain data, for the Rust lifter (ounce/lift): every partition prepare() makes, each function's
instructions and every fact lift_part reads of other functions, the patches and the summaries, resolved. The format is
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
FORMAT = 2


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


def export_part(plan, names, funcs, bound, callee, rows_by_func, entries, code_targets):
    """(the partition's functions, its records, its redirects) as EXPORT.md describes them"""
    from lift2 import code_ptrs, probes
    jt, jt_index, plt = plan["jt"], plan.get("jt_index", {}), plan["plt"]
    fns, reach = [], set()
    redirect = {}
    for e in names:
        pcs, words = plan["bodies"][e]
        pcs, words = list(pcs), list(words)
        w = dict(zip(pcs, words))
        body = set(pcs)
        # (the checks lift_part's probes.find / probes.at make: an export exists only if they pass)
        probes.find(e, pcs, w.__getitem__)
        for kw in ({"before": True, "jt": jt, "entries": entries}, {}, {"replace": True, "jt": jt}):
            probes.at(e, pcs, w.__getitem__, **kw)
        # (the targets build_ir reads a record of: a BL's, and a branch or fall-through leaving the function: not
        # inside(), a goto of its body)
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
        # (a BR's jump-table cases leaving the function are tail calls too: build_ir.build's goto of each case, from
        # the plan's targets or the index table's)
        for br in body & (set(jt) | set(jt_index)):
            for t in list(jt.get(br, ())) + list(jt_index[br][3] if br in jt_index else ()):
                if t not in body or (t != e and t in body_entries):
                    targets.add(t)
        p = None
        if e in plt:
            t, slot = plt[e]
            targets.add(t)
            p = {"target": t, "slot": slot, "target_is_code": t in code_targets}
        for t in targets:
            c = callee(t)
            reach.add(c)
            if c != t:
                redirect[t] = c
        rows = rows_by_func.get(e, ([], []))
        f = funcs.get(e)
        fns.append({
            "entry": e, "pcs": runs(pcs), "words": "".join("%08x" % x for x in words), "plt": p,
            "body_entries": sorted(body_entries),
            "jt": [[br, sorted(set(jt[br]))] for br in sorted(body & set(jt))],
            "jt_index": [[br, v[0], v[1], v[2], list(v[3])] for br, v in sorted((br, jt_index[br])
                                                                                for br in body & set(jt_index))],
            "code_sites": [[pc, s[0], s[1]] for pc, s in sorted(code_ptrs.sites(pcs, words, code_targets, jt).items())],
            "values": [row_json(r) for r in rows[0]],
            "rows": [row_json(r) for r in rows[1]],
            "func": {"name": f.name, "orig_name": f.orig_name} if f is not None else None,
            "own": bound_json(e, bound.own(e)),
        })
        reach.add(e)
    return fns, [bound_json(c, bound(c)) for c in sorted(reach)], [[t, c] for t, c in sorted(redirect.items())]


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--game", required=True, help="the game description, games/<game>")
    ap.add_argument("out", help="the export's folder (under work/)")
    a = ap.parse_args()
    t0 = time.time()
    plan_path, plan, gen, funcs, parts = setup(a.game)
    from lift2 import bounds, code_ptrs, game, host_effects, probes
    hle = host_effects.hle_names(gen)
    host_effects.check(hle)
    bound = bounds.call_bounds(plan, hle, False, funcs)
    callee = game.callee_fn(plan["plt"])
    code_targets = code_ptrs.targets(plan)
    entries = set(plan["entries"])
    rows_by_func = {}
    values, ats = probes.tables()
    for p in values:
        rows_by_func.setdefault(p.func, ([], []))[0].append(p)
    for p in ats:
        rows_by_func.setdefault(p.func, ([], []))[1].append(p)
    os.makedirs(os.path.join(a.out, "parts"), exist_ok=True)
    meta_parts = []
    t1 = time.time()
    for k, (tag, names) in enumerate(parts):
        name = part_name(tag, k)
        fns, bds, red = export_part(plan, names, funcs, bound, callee, rows_by_func, entries, code_targets)
        path = os.path.join(a.out, "parts", name + ".json")
        with open(path + ".tmp", "w") as f:
            json.dump({"name": name, "tag": tag, "index": k, "functions": fns, "bounds": bds, "redirect": red}, f,
                      separators=(",", ":"))
        os.replace(path + ".tmp", path)
        meta_parts.append({"name": name, "tag": tag, "index": k, "functions": len(names),
                           "insns": sum(len(plan["bodies"][e][0]) for e in names)})
    meta = {"format": FORMAT, "commit": commit(), "input_key": input_key(plan, gen, funcs), "code_key": game.code_key(),
            "facts_key": game.facts_key(plan),
            "summary_cache": plan.get("summary_cache"), "verify": False, "omit": True,
            "code_targets": sorted(code_targets),
            "sj_rets": [[e] + list(r) for e, r in sorted(plan.get("sj_rets", {}).items())],
            "answers": answers(hle),
            "partitions": meta_parts}
    with open(os.path.join(a.out, "meta.json"), "w") as f:
        json.dump(meta, f, separators=(",", ":"))
    size = sum(os.path.getsize(os.path.join(d, x)) for d, _, xs in os.walk(a.out) for x in xs)
    print("export: %d partitions, %d functions, %.1f MB in %s (setup %.0fs, export %.0fs)" % (
        len(parts), sum(len(n) for _, n in parts), size / 1e6, a.out, t1 - t0, time.time() - t1), flush=True)


if __name__ == "__main__":
    main()
