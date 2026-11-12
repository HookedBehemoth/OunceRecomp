#!/usr/bin/env python3
"""(M6b) Per-pass picture checks that can fail (docs/direct/m6b-report.md, section 3).

A frame dump of either back end at full resolution (frozen: RX_VK_DUMP=<present> RX_VK_DUMP_W=4096 RX_VK_DUMP_RAW=1; direct:
HOS_DBG=dump:<present>,dumpw:4096,dumpraw) is packed into a compact lossless store, passes of two runs are lined up by a stable
identity, and each aligned pair gets a verdict from per-tile errors against the frozen back end's own run-to-run noise.

    direct_picture_check.py pack <run>/log.txt <dump dir> <out dir>        a run's dump packed (pass targets only)
    direct_picture_check.py noise <out noise.json> <packed>...             the run-to-run noise of >= 3 frozen runs, per pass
    direct_picture_check.py compare <golden packed> <packed> [--noise N]   per pass verdicts; exit 1 on any failing pass
    direct_picture_check.py selftest                                       the metric on synthetic pictures (tests/ runs it)

The store. index.json lists the items in dump order with their log fields; each picture is <seq>.zst: the zstd-compressed
array, in a form that loses nothing of what the target holds: 8-bit UNORM/sRGB targets as uint8 (the dump's float times 255,
exact), 16-bit float, B10G11R11F and smaller floats as float16 (exact: their mantissas fit), D16 as uint16, D32F, R32F and
RGBA32F as float32, integer targets as the dump's PNG (8 bits, each channel stretched over its range by the dump itself).

The pass identity. A target is (slot c<i> or depth, GPU address, Vulkan format, width, height): both back ends place the
game's targets at the same guest addresses (host/rt_slots.c gives transient targets stable slots). A target's states are its
dumps in order; each is named by the program of its rendering's last draw (both back ends log it; 0: no draw, a cleared
state). The states of one target in two runs are lined up by that name, in order (difflib), so a cleared state is never
compared with a drawn one and a back end that splits a rendering in two does not shift the rest (m4c's "aligned against another
state"). A state lined up with none is reported (missing / extra), never silently passed.

The metric. Values are normalised: UNORM as they are, depth as it is, HDR floats through x / (1 + x) per channel, integer
targets as the dump's stretched PNG; NaN and Inf give the largest error. The picture is cut into 16 x 16 tiles; a tile's error
is the mean absolute difference of each channel over its texels, and the pass's error is, per channel, its worst tile and the
share of tiles above the threshold; texels that are NaN or Inf on one side only are counted apart and fail the pass when there
are more than the noise had. A mean over the whole picture is printed but decides nothing: one wrong tile, or one
wrong channel, fails the pass however the rest cancels.

The threshold. From the noise table: per pass identity, per channel, the worst tile error between any two frozen runs; the
threshold is 2 x that, at least 2/255 (two 8-bit steps). A pass whose noise is above 0.05 in some channel (13 8-bit steps in
a tile's mean) is "not comparable at one frame" (animated, temporally accumulated, amortised cube faces): listed as such, not
passed. A pass the noise table does not have (no three frozen runs lined it up) is "no noise measured": listed, not passed.
"""
import argparse
import difflib
import json
import os
import re
import struct
import sys

LINE = re.compile(r"vk: dump (\d+) (pass (c\d+|depth) prog ([0-9a-f]{16})[^:]*): tex (0x[0-9a-f]+) format (\d+) (\d+)x(\d+):(.*)")
TILE = 16
FLOOR = 2.0 / 255.0
UNSTABLE = 0.05
NAMES = {37: "RGBA8", 43: "RGBA8_SRGB", 44: "BGRA8", 50: "BGRA8_SRGB", 64: "A2B10G10R10", 76: "R16F", 83: "RG16F",
         97: "RGBA16F", 98: "R32UI", 100: "R32F", 103: "RG32F", 109: "RGBA32F", 122: "B10G11R11F", 124: "D16", 126: "D32F",
         129: "S8", 130: "D32F_S8", 9: "R8", 16: "RG8", 70: "R16"}
UNORM8 = {9, 10, 16, 17, 23, 29, 37, 38, 43, 44, 50, 51, 57, 58}
UNORM16 = {70, 77, 84, 91}   # (R16, RG16, ..., RGBA16 UNORM)
HALF = {76, 83, 90, 97, 122, 123}
FLOAT32 = {100, 103, 106, 109, 126, 130}
CHANNELS = {9: 1, 70: 1, 76: 1, 98: 1, 100: 1, 124: 1, 126: 1, 130: 1, 129: 1, 16: 2, 77: 2, 83: 2, 103: 2, 122: 3}
HDR = {76, 83, 90, 97, 122, 123, 100, 103, 106, 109}


def np():
    import numpy   # (work/venv)
    return numpy


def fmt_name(f):
    return NAMES.get(f, str(f))


# ---- packing ----
def read_log(path):
    items, frames = [], None
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if not m:
                d = re.search(r"\(direct: (\d+) frames", line)
                if d:
                    frames = int(d.group(1))
                continue
            seq, what, slot, prog, gpu, fmt, w, h, rest = m.groups()
            items.append({"seq": int(seq), "what": what.strip(), "slot": slot, "prog": prog, "gpu": gpu, "format": int(fmt),
                          "w": int(w), "h": int(h), "rest": rest.strip()})
    return items


def item_files(dump_dir):
    """seq -> (png, f32) paths of the dump folder"""
    out = {}
    for f in os.listdir(dump_dir):
        m = re.match(r"(\d+)_pass_.*\.(png|f32)$", f)
        if m:
            out.setdefault(int(m.group(1)), {})[m.group(2)] = os.path.join(dump_dir, f)
    return out


def read_f32(path):
    N = np()
    with open(path, "rb") as f:
        w, h = struct.unpack("<II", f.read(8))
        a = N.frombuffer(f.read(), N.float32)
    return a[: w * h * 4].reshape(h, w, 4)


def read_png(path):
    from PIL import Image   # (work/venv)
    N = np()
    return N.asarray(Image.open(path).convert("RGB"))


def encode(it, f32, png):
    """The array to store for one item and its kind ('u8', 'u16', 'f16', 'f32', 'png')"""
    N = np()
    fmt, ch = it["format"], CHANNELS.get(it["format"], 4)
    if f32 is None:
        return png, "png"
    a = f32[:, :, :ch]
    if it["slot"] == "depth":
        a = f32[:, :, :1]
        if fmt == 124:
            return N.round(a * 65535.0).astype(N.uint16), "u16"
        return a.astype(N.float32), "f32"
    if fmt in UNORM8:
        return N.round(N.clip(a, 0, 1) * 255.0).astype(N.uint8), "u8"
    if fmt in UNORM16:
        return N.round(N.clip(a, 0, 1) * 65535.0).astype(N.uint16), "u16"
    if fmt in HALF:
        return a.astype(N.float16), "f16"
    return a.astype(N.float32), "f32"


def pack(log, dump_dir, out):
    import zstandard
    N = np()
    os.makedirs(out, exist_ok=True)
    items, files = read_log(log), item_files(dump_dir)
    cctx = zstandard.ZstdCompressor(level=9, threads=-1)
    kept = []
    for it in items:
        fl = files.get(it["seq"], {})
        if "f32" not in fl and "png" not in fl:
            it["missing"] = True
            kept.append(it)
            continue
        f32 = read_f32(fl["f32"]) if "f32" in fl else None
        png = read_png(fl["png"]) if "png" in fl and f32 is None else None
        arr, kind = encode(it, f32, png)
        if arr.shape[0] != it["h"] or arr.shape[1] != it["w"]:
            it["scaled"] = "%dx%d" % (arr.shape[1], arr.shape[0])   # (not full resolution: a thumbnail dump)
        it.update({"kind": kind, "dtype": str(arr.dtype), "shape": list(arr.shape)})
        with open(os.path.join(out, "%04d.zst" % it["seq"]), "wb") as f:
            f.write(cctx.compress(N.ascontiguousarray(arr).tobytes()))
        kept.append(it)
    with open(os.path.join(out, "index.json"), "w") as f:
        json.dump({"log": os.path.abspath(log), "items": kept}, f, indent=0)
    return kept


def load(packed, it):
    import zstandard
    N = np()
    with open(os.path.join(packed, "%04d.zst" % it["seq"]), "rb") as f:
        raw = zstandard.ZstdDecompressor().decompress(f.read())
    return N.frombuffer(raw, N.dtype(it["dtype"])).reshape(it["shape"])


def normalised(it, a):
    """float32 values in about [0, 1] for the metric; NaN/Inf kept (they count as the largest error)"""
    N = np()
    k = it["kind"]
    if k == "u8" or k == "png":
        return a.astype(N.float32) / 255.0
    if k == "u16":
        return a.astype(N.float32) / 65535.0
    x = a.astype(N.float32)
    if it["slot"] != "depth" and it["format"] in HDR:
        with N.errstate(invalid="ignore", over="ignore"):
            x = N.where(N.isfinite(x), N.maximum(x, 0) / (1.0 + N.maximum(x, 0)), x)
    return x


# ---- the metric ----
def tile_errors(a, b):
    """a, b: float32 HxWxC in [0, 1]. Per channel: the tiles' mean |a - b| (an array tiles_y x tiles_x x C); a texel where
    exactly one side is NaN/Inf, or they differ in it, counts 1"""
    N = np()
    h, w = a.shape[:2]
    c = a.shape[2] if a.ndim == 3 else 1
    a, b = a.reshape(h, w, c), b.reshape(h, w, c)
    fa, fb = N.isfinite(a), N.isfinite(b)
    with N.errstate(invalid="ignore"):
        d = N.abs(a - b)
    d = N.where(fa & fb, d, N.where(fa == fb, N.where(a == b, 0.0, 1.0), 1.0)).astype(N.float32)
    d = N.minimum(d, 1.0)
    ty, tx = (h + TILE - 1) // TILE, (w + TILE - 1) // TILE
    pad = N.zeros((ty * TILE, tx * TILE, c), N.float32)
    cnt = N.zeros((ty * TILE, tx * TILE, 1), N.float32)
    pad[:h, :w] = d
    cnt[:h, :w] = 1
    s = pad.reshape(ty, TILE, tx, TILE, c).sum(axis=(1, 3))
    n = cnt.reshape(ty, TILE, tx, TILE, 1).sum(axis=(1, 3))
    return s / N.maximum(n, 1)


def pair_metric(ia, a, ib, b):
    """{max: per channel worst tile, mean: per channel picture mean, tiles: the tile error array} for two items' arrays"""
    N = np()
    if a.shape != b.shape:
        return None
    na, nb = normalised(ia, a), normalised(ib, b)
    t = tile_errors(na, nb)
    with N.errstate(invalid="ignore"):
        mean = [float(N.nanmean(na[..., c]) - N.nanmean(nb[..., c])) for c in range(t.shape[2])]
    odd = int((N.isfinite(na) != N.isfinite(nb)).sum())   # (texels NaN or Inf on one side only)
    return {"max": [float(t[..., c].max()) for c in range(t.shape[2])], "mean_diff": mean, "tiles": t, "nonfinite": odd}


# ---- identity and alignment ----
def target_key(it):
    return "%s %s %s %dx%d" % (it["slot"], it["gpu"], fmt_name(it["format"]), it["w"], it["h"])


def by_target(items):
    out = {}
    for it in items:
        if it.get("missing"):
            continue
        out.setdefault(target_key(it), []).append(it)
    return out


def align(ref_items, run_items):
    """[(ref item or None, run item or None, identity)]: per target, its states lined up by their last program, in order"""
    rt, ut = by_target(ref_items), by_target(run_items)
    rows = []
    for key in sorted(set(rt) | set(ut), key=lambda k: min(i["seq"] for i in rt.get(k, []) + ut.get(k, []))):
        ra, ua = rt.get(key, []), ut.get(key, [])
        sm = difflib.SequenceMatcher(None, [i["prog"] for i in ra], [i["prog"] for i in ua], autojunk=False)
        for op, i0, i1, j0, j1 in sm.get_opcodes():
            if op == "equal":
                for i, j in zip(range(i0, i1), range(j0, j1)):
                    rows.append((ra[i], ua[j], "%s #%d %s" % (key, i, ra[i]["prog"])))
            else:
                for i in range(i0, i1):
                    rows.append((ra[i], None, "%s #%d %s" % (key, i, ra[i]["prog"])))
                for j in range(j0, j1):
                    rows.append((None, ua[j], "%s +%d %s" % (key, j, ua[j]["prog"])))
    return rows


def read_index(packed):
    with open(os.path.join(packed, "index.json")) as f:
        return json.load(f)["items"]


# ---- noise ----
def noise(out, packs):
    if len(packs) < 3:
        print("noise: at least 3 frozen runs (got %d)" % len(packs))
        return 2
    idx = [read_index(p) for p in packs]
    table = {}   # identity (in run 0's terms) -> {"max": per channel, "pairs": n, "present": runs}
    present = {}
    for i in range(len(packs)):
        for j in range(i + 1, len(packs)):
            # (identities named by the first run of the pair: line every pair up against run 0's names through run 0 itself)
            rows = align(idx[0], idx[i]) if i else None
            names = {}
            if rows:
                for r, u, ident in rows:
                    if r and u:
                        names[u["seq"]] = ident
            for r, u, ident in align(idx[i], idx[j]):
                name = ident if i == 0 else names.get(r["seq"]) if r else None
                if r is None or u is None or name is None:
                    continue
                present.setdefault(name, set()).update({i, j})
                m = pair_metric(r, load(packs[i], r), u, load(packs[j], u))
                e = table.setdefault(name, {"max": None, "pairs": 0})
                e["pairs"] += 1
                if m is None:
                    e["shape_mismatch"] = True
                    continue
                e["max"] = m["max"] if e["max"] is None else [max(x, y) for x, y in zip(e["max"], m["max"])]
                e["nonfinite"] = max(e.get("nonfinite", 0), m["nonfinite"])
    for name, e in table.items():
        e["runs"] = len(present.get(name, ()))
        e["unstable"] = bool(e["max"] and max(e["max"]) > UNSTABLE)
    with open(out, "w") as f:
        json.dump({"runs": [os.path.abspath(p) for p in packs], "tile": TILE, "unstable_above": UNSTABLE, "floor": FLOOR,
                   "passes": table}, f, indent=0)
    n_un = sum(1 for e in table.values() if e["unstable"])
    print("noise: %d pass identities from %d runs (%d pairs); %d not comparable at one frame (noise above %.2f)" %
          (len(table), len(packs), len(packs) * (len(packs) - 1) // 2, n_un, UNSTABLE))
    return 0


# ---- compare ----
def verdict_row(r, u, ident, golden, run, noise_table):
    """(status, detail): status PASS, FAIL, UNSTABLE, NO-NOISE, MISSING, EXTRA"""
    if r is None:
        return "EXTRA", "only in the checked run"
    if u is None:
        return "MISSING", "not in the checked run"
    m = pair_metric(r, load(golden, r), u, load(run, u))
    if m is None:
        return "FAIL", "sizes differ"
    e = noise_table.get(ident) if noise_table is not None else None
    if noise_table is not None and (e is None or e.get("max") is None):
        thr = None
    elif noise_table is None:
        thr = [FLOOR] * len(m["max"])
    else:
        thr = [max(2 * x, FLOOR) for x in e["max"]]
    share = [float((m["tiles"][..., c] > (thr[c] if thr else FLOOR)).mean()) for c in range(len(m["max"]))]
    detail = "worst tile %s; tiles over %s; picture mean diff %s" % (
        " ".join("%.4f" % x for x in m["max"]), " ".join("%.1f%%" % (100 * x) for x in share),
        " ".join("%+.4f" % x for x in m["mean_diff"]))
    if thr is None:
        return "NO-NOISE", detail
    detail += "; threshold %s" % " ".join("%.4f" % x for x in thr)
    if e is not None and e.get("unstable"):
        return "UNSTABLE", detail + "; noise %s" % " ".join("%.4f" % x for x in e["max"])
    allowed = e.get("nonfinite", 0) if e is not None else 0
    if m["nonfinite"] > allowed:
        return "FAIL", detail + "; %d texels NaN or Inf on one side only (noise %d)" % (m["nonfinite"], allowed)
    if any(x > t for x, t in zip(m["max"], thr)):
        return "FAIL", detail
    return "PASS", detail


def compare(golden, run, noise_path, show_all=False, quiet=False):
    gi, ri = read_index(golden), read_index(run)
    table = None
    if noise_path:
        with open(noise_path) as f:
            table = json.load(f)["passes"]
    rows = align(gi, ri)
    counts, lines = {}, []
    for r, u, ident in rows:
        st, detail = verdict_row(r, u, ident, golden, run, table)
        counts[st] = counts.get(st, 0) + 1
        it = r or u
        if st != "PASS" or show_all:
            lines.append((st, "%-8s %4s|%-4s %s: %s" % (st, "%03d" % r["seq"] if r else "-", "%03d" % u["seq"] if u else "-",
                                                      ident, detail)))
    order = {"FAIL": 0, "MISSING": 1, "EXTRA": 2, "NO-NOISE": 3, "UNSTABLE": 4, "PASS": 5}
    if not quiet:
        for st, l in sorted(lines, key=lambda x: order[x[0]]):
            print(l)
    total = sum(counts.values())
    bad = counts.get("FAIL", 0) + counts.get("MISSING", 0)
    print("verdict: %s: %d pass states lined up or not; %s" % ("FAIL" if bad else "PASS", total,
          ", ".join("%s %d" % (k, counts[k]) for k in sorted(counts, key=lambda k: order[k]))))
    if counts.get("UNSTABLE") or counts.get("NO-NOISE") or counts.get("EXTRA"):
        print("  (not passed and not failed: UNSTABLE = not comparable at one frame, NO-NOISE = no noise measured, EXTRA = only in "
              "the checked run; listed above)")
    return 1 if bad else 0


# ---- the self-test (synthetic pictures; tests/test_direct_picture_check.py runs it) ----
def selftest():
    import tempfile
    N = np()
    rng = N.random.default_rng(3)
    base = rng.random((64, 96, 4), dtype=N.float32) * 0.5 + 0.25

    def item(seq, prog, fmt=97, slot="c0", gpu="0x1000"):
        return {"seq": seq, "what": "pass %s prog %s" % (slot, prog), "slot": slot, "prog": prog, "gpu": gpu, "format": fmt,
                "w": 96, "h": 64, "rest": ""}

    def write(d, items_arrays):
        import zstandard
        os.makedirs(d, exist_ok=True)
        items = []
        for it, a in items_arrays:
            arr, kind = encode(it, a, None)
            it = dict(it, kind=kind, dtype=str(arr.dtype), shape=list(arr.shape))
            with open(os.path.join(d, "%04d.zst" % it["seq"]), "wb") as f:
                f.write(zstandard.ZstdCompressor().compress(N.ascontiguousarray(arr).tobytes()))
            items.append(it)
        with open(os.path.join(d, "index.json"), "w") as f:
            json.dump({"items": items}, f)

    ok = True
    with tempfile.TemporaryDirectory(prefix="picture-check-") as t:
        P = "0123456789abcdef"
        noise_runs = []
        for k in range(3):   # (three "frozen" runs: tiny noise)
            d = os.path.join(t, "frozen%d" % k)
            write(d, [(item(0, "0" * 16), base * 0), (item(1, P), base + rng.normal(0, 1e-4, base.shape).astype(N.float32))])
            noise_runs.append(d)
        nt = os.path.join(t, "noise.json")
        noise(nt, noise_runs)
        # (1) the same picture: passes
        same = os.path.join(t, "same")
        write(same, [(item(0, "0" * 16), base * 0), (item(1, P), base.copy())])
        r1 = compare(noise_runs[0], same, nt, quiet=True)
        # (2) a cancelling difference: +d on one half, -d on the other; the picture's mean is unchanged: must fail
        canc = base.copy()
        canc[:, :48, :3] += 0.1
        canc[:, 48:, :3] -= 0.1
        cd = os.path.join(t, "cancel")
        write(cd, [(item(0, "0" * 16), base * 0), (item(1, P), canc)])
        r2 = compare(noise_runs[0], cd, nt, quiet=True)
        mean_shift = abs(float(canc[..., :3].mean() - base[..., :3].mean()))
        # (3) one wrong channel (B flat), others right: must fail
        chan = base.copy()
        chan[..., 2] = 0.72
        chd = os.path.join(t, "channel")
        write(chd, [(item(0, "0" * 16), base * 0), (item(1, P), chan)])
        r3 = compare(noise_runs[0], chd, nt, quiet=True)
        # (4) one 16x16 tile wrong in a 96x64 picture: must fail
        one = base.copy()
        one[16:32, 32:48, 0] += 0.2
        od = os.path.join(t, "onetile")
        write(od, [(item(0, "0" * 16), base * 0), (item(1, P), one)])
        r4 = compare(noise_runs[0], od, nt, quiet=True)
        # (5) a state missing (the drawn one): must fail; (6) a cleared state is never compared with a drawn one
        md = os.path.join(t, "missing")
        write(md, [(item(0, "0" * 16), base * 0)])
        r5 = compare(noise_runs[0], md, nt, quiet=True)
        rows = align(read_index(noise_runs[0]), [dict(item(5, P)), dict(item(6, "0" * 16))])
        never_crossed = all(not (r and u and r["prog"] != u["prog"]) for r, u, _ in rows)
        # (7) NaN in the checked run where the reference is finite: must fail
        nan = base.copy()
        nan[3, 3, 1] = float("nan")
        nd = os.path.join(t, "nan")
        write(nd, [(item(0, "0" * 16), base * 0), (item(1, P), nan)])
        r7 = compare(noise_runs[0], nd, nt, quiet=True)
    checks = [("same picture passes", r1 == 0), ("cancelling difference (mean shift %.6f) fails" % mean_shift, r2 == 1),
              ("one flat channel fails", r3 == 1), ("one wrong tile fails", r4 == 1), ("a missing state fails", r5 == 1),
              ("a cleared state is not lined up with a drawn one", never_crossed), ("a NaN fails", r7 == 1)]
    for name, good in checks:
        print("selftest: %s: %s" % (name, "ok" if good else "WRONG"))
        ok &= good
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("log")
    p.add_argument("dump_dir")
    p.add_argument("out")
    p = sub.add_parser("noise")
    p.add_argument("out")
    p.add_argument("packs", nargs="+")
    p = sub.add_parser("compare")
    p.add_argument("golden")
    p.add_argument("run")
    p.add_argument("--noise")
    p.add_argument("--all", action="store_true")
    sub.add_parser("selftest")
    a = ap.parse_args()
    if a.cmd == "pack":
        items = pack(a.log, a.dump_dir, a.out)
        print("packed %d pass targets (%d missing pictures) into %s" % (len(items), sum(1 for i in items if i.get("missing")), a.out))
        return 0
    if a.cmd == "noise":
        return noise(a.out, a.packs)
    if a.cmd == "compare":
        return compare(a.golden, a.run, a.noise, a.all)
    return selftest()


if __name__ == "__main__":
    sys.exit(main())
