"""Two ounce-lift outputs compared, every change with its reason: the gate of a lifter change that alters the output
(ounce/lift/GOLDEN-CHANGES.md).

    work/venv/bin/python -I ounce/lift/tools/diffout.py <old> <new> [--show N] [--list FILE]
    work/venv/bin/python -I ounce/lift/tools/diffout.py --store <out> <golden dir>     (keep an output as a golden)

An output is ounce-lift's directory (<part>.ll, gen/, rt64/, facts.tsv, image/image.bin) or one stored by --store
(each file .zst, image.bin by its sha256 in meta.json). Functions are compared across partitions (a function may move
between them). A changed function's reason is a change of its own facts (facts.tsv: its body, summary and call
record) or of the facts of a function its IR names, or a change of a hook its IR names (f_<addr> or fv_<addr>, the
hook's entries in gen/a64_hooks.ll); a function whose IR is the same but for its values' numbers is "renumbered" (the
lift numbers values as it builds, a block it drops included); one that only adds or removes write-through stores to
the context (and their slots' addresses) is "context stores only", counted apart: which stores the lift may omit is
a rule of its own, each change of it argued in GOLDEN-CHANGES.md; a change with none is UNEXPLAINED. Exit status 1 when any change is unexplained.
"""
import argparse
import hashlib
import json
import os
import re
import sys
from concurrent.futures import ProcessPoolExecutor

DEFINE = re.compile(rb"^define [^\n]*?@([A-Za-z0-9_.$]+)\(", re.M)
REF = re.compile(rb"@(f_[0-9a-f]{8})\b")
HOOK_REF = re.compile(rb"@f(?:v)?_([0-9a-f]{8})\b")
HOOK_LINE = re.compile(rb"(?:f|fv)_([0-9a-f]{8})\b")
VALUE = re.compile(rb"%v\d+\b")
CTX_STORE = re.compile(r"^  (?:store (?:i64|<2 x i64>|i8) [^,]+, ptr %cx\.\w+, align \d+|%cx\.\w+ = getelementptr i8, ptr %ctx, i64 \d+)$")


def stores_only(a, b):
    """the two texts differ only by context stores (and their slots' addresses) added or removed"""
    import difflib
    d = [l for l in difflib.ndiff(a.decode().split("\n"), b.decode().split("\n")) if l[:2] in ("- ", "+ ")]
    return bool(d) and all(CTX_STORE.match(l[2:]) for l in d)


def renumbered(text):
    """the function's text with its values numbered by their first appearance"""
    names = {}
    return VALUE.sub(lambda m: b"%%v%d" % names.setdefault(m.group(0), len(names)), text)
FIELDS = ("insns", "body", "inputs", "clobbers", "mustdef", "preserved", "results", "returns", "demote", "unknown",
          "boundary", "record")


def zstd():
    from compression import zstd as z
    return z


def stored(d):
    return os.path.exists(os.path.join(d, "meta.json")) and not os.path.exists(os.path.join(d, "facts.tsv"))


def read(d, rel):
    """a file of an output, or None"""
    p = os.path.join(d, rel)
    if os.path.exists(p):
        return open(p, "rb").read()
    if os.path.exists(p + ".zst"):
        return zstd().decompress(open(p + ".zst", "rb").read())
    return None


def parts(d):
    names = set()
    for f in os.listdir(d):
        for suf in (".ll", ".ll.zst"):
            if f.endswith(suf):
                names.add(f[:-len(suf)])
    return sorted(names)


def split(text):
    """{name: function text}, and the text outside functions"""
    out, rest, at = {}, [], 0
    for m in DEFINE.finditer(text):
        end = text.index(b"\n}\n", m.start()) + 3
        rest.append(text[at:m.start()])
        out[m.group(1).decode()] = text[m.start():end]
        at = end
    rest.append(text[at:])
    return out, b"".join(rest)


def index_part(args):
    d, name = args
    fs, rest = split(read(d, name + ".ll"))
    return name, {n: hashlib.sha1(t).hexdigest() for n, t in fs.items()}, hashlib.sha1(rest).hexdigest()


def index(d, jobs):
    """({function: (partition, sha1)}, {partition: sha1 of the text outside functions})"""
    fn, outside = {}, {}
    with ProcessPoolExecutor(jobs) as ex:
        for name, fs, rest in ex.map(index_part, [(d, p) for p in parts(d)]):
            outside[name] = rest
            for n, h in fs.items():
                fn[n] = (name, h)
    return fn, outside


def facts(d):
    t = read(d, "facts.tsv")
    if t is None:
        return None
    lines = t.decode().splitlines()
    head = lines[0].split("\t")
    return {l.split("\t")[0]: dict(zip(head[1:], l.split("\t")[1:])) for l in lines[1:]}


def texts(d, want):
    """{function: text} of the functions in want ({function: partition})"""
    by = {}
    for f, p in want.items():
        by.setdefault(p, []).append(f)
    out = {}
    for p, fs in by.items():
        got, _ = split(read(d, p + ".ll"))
        for f in fs:
            out[f] = got[f]
    return out


def gen_files(d):
    out = {}
    for sub in ("gen", "rt64"):
        p = os.path.join(d, sub)
        if os.path.isdir(p):
            for f in os.listdir(p):
                out[sub + "/" + (f[:-4] if f.endswith(".zst") else f)] = None
    return sorted(out)


def hook_lines(d):
    """{address: the entries of gen/a64_hooks.ll (a definition or a comment; a64_hooks.c's lines in an older output)
    naming f_<address> or fv_<address>}"""
    out = {}
    ll = read(d, "gen/a64_hooks.ll")
    entries = re.split(rb"\n(?=define |declare |; )", ll) if ll is not None else (read(d, "gen/a64_hooks.c") or b"").splitlines()
    for line in entries:
        for m in HOOK_LINE.finditer(line):
            out.setdefault(m.group(1), set()).add(line)
    return out


def image_sha(d):
    p = os.path.join(d, "image", "image.bin")
    if os.path.exists(p):
        return hashlib.sha256(open(p, "rb").read()).hexdigest()
    return json.load(open(os.path.join(d, "meta.json")))["image_sha256"]


def store(out, g, jobs):
    """the output kept as a golden: every file .zst, image.bin by its sha256"""
    z = zstd()
    os.makedirs(g, exist_ok=True)
    meta = {"image_sha256": image_sha(out), "files": {}}
    for root, _, files in os.walk(out):
        for f in files:
            src = os.path.join(root, f)
            rel = os.path.relpath(src, out)
            if rel == "image/image.bin":
                continue
            data = open(src, "rb").read()
            meta["files"][rel] = hashlib.sha256(data).hexdigest()
            dst = os.path.join(g, rel + ".zst")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, "wb").write(z.compress(data, 3))
    json.dump(meta, open(os.path.join(g, "meta.json"), "w"), indent=1, sort_keys=True)
    print("stored %s -> %s (%d files)" % (out, g, len(meta["files"])))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("--store", action="store_true", help="keep <old> (an output) as the golden <new>")
    ap.add_argument("--show", type=int, default=30, help="changed functions listed")
    ap.add_argument("--list", help="write every changed function with its reasons to this file")
    ap.add_argument("-j", type=int, default=8)
    a = ap.parse_args()
    if a.store:
        store(a.old, a.new, a.j)
        return 0
    fa, fb = facts(a.old), facts(a.new)
    ia, oa = index(a.old, a.j)
    ib, ob = index(a.new, a.j)
    # facts
    fchanged = {}
    if fa is not None and fb is not None:
        for f in sorted(set(fa) | set(fb)):
            x, y = fa.get(f), fb.get(f)
            if x != y:
                fchanged[f] = ["added"] if x is None else ["removed"] if y is None else [k for k in FIELDS if x[k] != y[k]]
    # functions
    changed = sorted(f for f in set(ia) | set(ib) if ia.get(f, (0, 0))[1] != ib.get(f, (0, 0))[1])
    moved = sum(1 for f in set(ia) & set(ib) if ia[f][0] != ib[f][0])
    ta = texts(a.old, {f: ia[f][0] for f in changed if f in ia})
    tb = texts(a.new, {f: ib[f][0] for f in changed if f in ib})
    ha, hb = hook_lines(a.old), hook_lines(a.new)
    hchanged = {h for h in set(ha) | set(hb) if ha.get(h) != hb.get(h)}
    rows, unexplained, nstores = [], 0, 0
    for f in changed:
        why = []
        if f not in ia or f not in ib:
            why.append("added" if f not in ia else "removed")
        if f in fchanged:
            why.append("own " + ",".join(fchanged[f]))
        refs = set(REF.findall(ta.get(f, b""))) | set(REF.findall(tb.get(f, b"")))
        for r in sorted(x.decode() for x in refs):
            if r != f and r in fchanged:
                why.append("callee %s %s" % (r, ",".join(fchanged[r])))
        hooks = set(HOOK_REF.findall(ta.get(f, b""))) | set(HOOK_REF.findall(tb.get(f, b"")))
        for h in sorted(hooks & hchanged):
            why.append("hook %s" % h.decode())
        if not why and f in ta and f in tb and renumbered(ta[f]) == renumbered(tb[f]):
            why.append("renumbered")
        if not why and f in ta and f in tb and stores_only(ta[f], tb[f]):
            why.append("context stores only")
            nstores += 1
        if not why:
            why = ["UNEXPLAINED"]
            unexplained += 1
        rows.append((f, why))
    # report
    print("old %s\nnew %s" % (a.old, a.new))
    print("functions: %d old, %d new, %d changed (%d unexplained, %d context stores only), %d moved partition" % (
        len(ia), len(ib), len(changed), unexplained, nstores, moved))
    by_field = {}
    for f, ks in fchanged.items():
        for k in ks:
            by_field[k] = by_field.get(k, 0) + 1
    print("facts changed: %d functions; by field: %s" % (len(fchanged), json.dumps(by_field, sort_keys=True)))
    po = sorted(p for p in set(oa) | set(ob) if oa.get(p) != ob.get(p))
    print("partitions whose text outside functions differs (declarations, header): %d%s" % (
        len(po), (": " + " ".join(po[:10])) if po else ""))
    files = sorted(set(gen_files(a.old)) | set(gen_files(a.new)) | {"facts.tsv"})
    for g in files:
        x, y = read(a.old, g), read(a.new, g)
        if x != y:
            xl, yl = (x or b"").splitlines(), (y or b"").splitlines()
            sx, sy = set(xl), set(yl)
            print("file %s differs: %d lines only old, %d only new" % (g, len([l for l in xl if l not in sy]),
                                                                       len([l for l in yl if l not in sx])))
    ima, imb = image_sha(a.old), image_sha(a.new)
    print("image.bin %s" % ("equal" if ima == imb else "DIFFERS"))
    for f, why in rows[:a.show]:
        print("  %s: %s" % (f, "; ".join(why)))
    if len(rows) > a.show:
        print("  ... %d more" % (len(rows) - a.show))
    if a.list:
        with open(a.list, "w") as fh:
            for f, why in rows:
                fh.write("%s\t%s\n" % (f, "; ".join(why)))
            for f, ks in sorted(fchanged.items()):
                fh.write("facts\t%s\t%s\n" % (f, ",".join(ks)))
    return 1 if unexplained else 0


if __name__ == "__main__":
    sys.exit(main())
