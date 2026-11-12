"""The build-time terrain-tile converter (docs/direct/a-converters.md): every TARB file under romfs/TerrainArc/ with its
GPU texel blob (record field B) deswizzled from Tegra block-linear to packed linear rows, in the layout of
vkCmdCopyBufferToImage with bufferRowLength 0 (layer after layer, block rows for BC; the format is unchanged), written
as a TARB file of the same name and structure under --out, for the host's HOS_FS_OVERLAY to serve in place of the game's.

    python -I nx2/prepare_terrain.py [--romfs DIR] [--out DIR] [--jobs 8] [--only PREFIX ...] [--map NAME ...]
    python -I nx2/prepare_terrain.py --verify N [--seed S]     re-swizzle N random outputs, compare with the source, byte-exact
    python -I nx2/prepare_terrain.py --report                  the summary of the output as it is

Game data: it reads the romfs in place (default: work/dump.txt's path + /romfs) and writes only under --out (default
work/prepared/terrain). Nothing it writes may leave the PC (CONTRIBUTING.md). Needs numpy and zstandard; the native
deswizzle is nx2/texconv.cpp (built without the encoders: no third-party source). Niced (10), 8 workers.

The file (docs/direct/m0-terrain-texels.md): "TARB", u32 version 1, u16 records 1, u16 header size 0x14, u32 decompressed
size, u32 zstd size, one zstd frame. The record: +3 layers, +0x1a/+0x1b layout bytes (4f 7a), three (s32 self-relative
offset, u32 size) pairs at +0x1c (A, the CPU copy), +0x24 (C), +0x2c (B, the GPU texels, always last). The tile is
260 x 260 texels; what B holds is told by (layers, size of B): BC5 65x65 blocks, BC6H 6 layers, RGBA8 / R32F, RGBA8 x 2.
What changes in a file: B's bytes (block-linear -> linear, smaller: no GOB padding), the u32 at record +0x30 (B's size),
the header's decompressed and zstd sizes, the zstd frame (level --zstd-level, content size in the frame, no checksum, as
the game's). B's offset field, A, C, the layout bytes and every other byte are as they were. A tile that is not a TARB
(the 3 damaged dumps, the .tscb index files) or whose B does not match one of the known layouts is not written (the host
then serves the game's own file; the report lists them): this tool never guesses.
The game patch at f_80c17040+0x544 (the memcpy of B) is told converted from unconverted by its size argument alone: the
linear size of a layout is never its block-linear size (SIZES below).

--out also holds index.json: per romfs file its size, mtime (ns), BLAKE2b-64 hash, and what was made of it. Incremental: a
file with the size and mtime of its entry is kept; another mtime: the hash decides; another config (CONV_VERSION, zstd
level) redoes everything. Output files are written to a temporary name and renamed.

The resource size table (A-rt, docs/direct/a-rt-report.md): the game's loader drops a file larger than its entry in
System/Resource/ResourceSizeTable.*.rsizetable.zs (an entry is the file's size and an allowance), and a converted file can
be larger than the game's (linear rows compress worse: 288 of Kokiri's 1,521); the game then opens it again forever. So
every run (and --size-table alone) writes that table into --out with each converted file's entry raised by what its file
grew, keeping the allowance: the overlay serves it in place of the game's.

    python -I nx2/prepare_terrain.py --size-table              the table from index.json, nothing converted
"""
import argparse
import hashlib
import importlib.util
import json
import multiprocessing as mp
import os
import random
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
CONV_VERSION = 1
INDEX_VERSION = 1
TILE = 260   # texels (the tile with its border)
HEADER = 0x14
SUBDIR = "TerrainArc"


def _load_textures():
    spec = importlib.util.spec_from_file_location("prepare_textures", os.path.join(HERE, "prepare_textures.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


PT = _load_textures()   # (the native loader, hash64 and the romfs default: one copy of each)


# ---- layouts (host/nvn_layout.c, as work/proto/direct-m0/bl_size.py) -------------------------------------------------
def pick_tile(g):
    return 4 if g >= 16 else 3 if g >= 8 else 2 if g >= 4 else 1 if g >= 2 else 0


def adj_tile(shift, unit, dim):
    if not shift:
        return 0
    x = unit << (shift - 1)
    if x >= dim:
        while True:
            shift -= 1
            if not shift:
                break
            x >>= 1
            if x < dim:
                break
    return shift


def tile_layout(bpb, bw, layers):
    """(block-linear bytes of the surface, layer stride, block height log2, width bytes, rows, linear bytes) of one
    260 x 260 level-0 tile of blocks bw x bw and bpb bytes, `layers` layers (NVN's automatic block height)"""
    lw = lh = -(-TILE // bw)
    tile_h = pick_tile((lh + lh // 2) // 8)
    th = adj_tile(tile_h, 8, lh)
    wg, hg = (lw * bpb + 63) // 64, (lh + 7) // 8
    ls = wg * ((hg + (1 << th) - 1) >> th) << (9 + th)
    if layers > 1:
        t2 = tile_h
        while t2 and lh <= (8 << (t2 - 1)):
            t2 -= 1
        a = 1 << (t2 + 9)
        ls = (ls + a - 1) & ~(a - 1)
    return ls * layers, ls, th, lw * bpb, lh, lw * bpb * lh * layers


# (bytes per block, block edge, name)
FORMATS = [(1, 1, "R8"), (2, 1, "RG8/R16"), (4, 1, "RGBA8/R32F"), (8, 1, "RGBA16F"), (16, 1, "RGBA32F"), (8, 4, "BC1/BC4"),
           (16, 4, "BC2/3/5/6H/7")]


def classify(layers, b_size):
    """(layout, name) of a record by its layer count and B's size, or None. SIZES lists what the game's files hold."""
    found = []
    for bpb, bw, name in FORMATS:
        lay = tile_layout(bpb, bw, layers)
        if lay[0] == b_size:
            found.append((lay, "%s x%d" % (name, layers)))
    return found[0] if len(found) == 1 else None


# what the real files hold (layers, block-linear B size) -> linear size: the patch's test of "converted"
SIZES = {(1, 139264): 67600, (6, 835584): 405600, (1, 417792): 270400, (2, 835584): 540800}


# ---- one file ---------------------------------------------------------------------------------------------------------
class Skip(Exception):
    pass


def parse(data):
    """(header fields, record bytes, B's offset in the record, B's size, layers) of a TARB file; Skip when it is not one
    of the files this tool knows"""
    import zstandard
    if data[:4] != b"TARB":
        raise Skip("not a TARB")
    ver, cnt, hs, usz, csz = struct.unpack_from("<IHHII", data, 4)
    if ver != 1 or cnt != 1 or hs != HEADER or HEADER + csz != len(data):
        raise Skip("header: version %d records %d header %#x sizes %d+%d vs file %d" % (ver, cnt, hs, usz, csz, len(data)))
    try:
        rec = zstandard.ZstdDecompressor().decompress(data[hs:hs + csz], max_output_size=usz)
    except zstandard.ZstdError as ex:
        raise Skip("zstd: %s" % ex)
    if len(rec) != usz or len(rec) < 0x34:
        raise Skip("record size %d, header says %d" % (len(rec), usz))
    _ao, _asz, _co, _csz, boff, bsz = struct.unpack_from("<iIiIiI", rec, 0x1c)
    b0 = 0x2c + boff
    if boff < 0 or b0 + bsz != len(rec):
        raise Skip("B at %d + %d is not the record's end %d" % (b0, bsz, len(rec)))
    return rec, b0, bsz, rec[3]


def deswizzle(nat, b_bytes, layers, lay):
    """The linear texels of every layer of the block-linear blob (numpy uint8)"""
    import numpy as np
    arr = np.frombuffer(b_bytes, np.uint8)
    total, ls, th, wbytes, rows, lin_total = lay
    out = np.empty(lin_total, np.uint8)
    per = wbytes * rows
    one = nat.L.hk_block_linear_size(wbytes, rows, th)
    for l in range(layers):
        out[l * per:(l + 1) * per] = nat.to_linear(arr, l * ls, one, wbytes, rows, th)
    return out


def convert_bytes(nat, data, level, cctx=None):
    """(new file bytes, name, linear size) for a TARB file's bytes; Skip when it is not convertible"""
    import zstandard
    rec, b0, bsz, layers = parse(data)
    got = classify(layers, bsz)
    if got is None:
        raise Skip("B: %d layers, %d bytes: no known layout" % (layers, bsz))
    lay, name = got
    lin = deswizzle(nat, rec[b0:b0 + bsz], layers, lay)
    new = bytearray(rec[:b0])
    new += lin.tobytes()
    struct.pack_into("<I", new, 0x30, len(lin))
    cctx = cctx or zstandard.ZstdCompressor(level=level, write_checksum=False, write_content_size=True)
    frame = cctx.compress(bytes(new))
    return data[:8] + struct.pack("<HHII", 1, HEADER, len(new), len(frame)) + frame, name, len(lin)


def hash64(b):
    return PT.hash64(b)


_W = {}


def worker_init(romfs, out, level, nice):
    import zstandard
    if nice and hasattr(os, "nice"):
        try:
            os.nice(nice)
        except OSError:
            pass
    _W["nat"] = PT.Native(PT.build_native(os.path.join(out, "tool"), encoders=False, quiet=True))
    _W.update(romfs=romfs, out=out, level=level,
              cctx=zstandard.ZstdCompressor(level=level, write_checksum=False, write_content_size=True))


def work_file(job):
    """(rel, size, mtime, old hash) -> an index entry: [size, mtime, hash, out size, name, status, in raw, out raw]"""
    rel, size, mtime, old_hash = job
    t0 = time.perf_counter()
    try:
        data = open(os.path.join(_W["romfs"], rel), "rb").read()
        h = hash64(data)
        if old_hash is not None and h == old_hash:
            return rel, dict(unchanged=True, size=size, mtime=mtime)
        try:
            new, name, lin = convert_bytes(_W["nat"], data, _W["level"], _W["cctx"])
        except Skip as ex:
            return rel, dict(size=size, mtime=mtime, hash=h, status="skip", why=str(ex)[:200])
        dst = os.path.join(_W["out"], rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        tmp = dst + ".tmp"
        with open(tmp, "wb") as f:
            f.write(new)
        os.replace(tmp, dst)
        return rel, dict(size=size, mtime=mtime, hash=h, status="ok", name=name, out_size=len(new), lin=lin,
                         secs=time.perf_counter() - t0)
    except Exception as ex:
        return rel, dict(size=size, mtime=mtime, hash=None, status="error", why=repr(ex)[:300])


# ---- the run ----------------------------------------------------------------------------------------------------------
def config(level):
    return hash64(json.dumps(dict(v=CONV_VERSION, z=level, sizes=sorted(SIZES.items())), sort_keys=True).encode())


def walk(romfs, only):
    out = []
    for dp, dn, fn in os.walk(os.path.join(romfs, SUBDIR)):
        dn.sort()
        for f in sorted(fn):
            full = os.path.join(dp, f)
            rel = os.path.relpath(full, romfs).replace(os.sep, "/")
            if only and not any(rel.startswith(o) for o in only):
                continue
            st = os.stat(full)
            out.append((rel, st.st_size, st.st_mtime_ns))
    return out


def load_index(path, cfg):
    try:
        j = json.load(open(path))
        if j.get("version") == INDEX_VERSION and j.get("config") == cfg:
            return j["files"]
        print("prepare_terrain: the options or the converter changed: every file is made again", flush=True)
    except FileNotFoundError:
        pass
    except Exception as ex:
        print("prepare_terrain: index unreadable (%s): starting over" % ex, flush=True)
    return {}


def save_index(path, cfg, files):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(dict(version=INDEX_VERSION, config=cfg, files=files), f, separators=(",", ":"))
    os.replace(tmp, path)


def run(opts):
    t_start = time.time()
    romfs = opts.romfs or PT.default_romfs()
    if not romfs or not os.path.isdir(os.path.join(romfs, SUBDIR)):
        sys.exit("romfs/TerrainArc not found (--romfs, or work/dump.txt)")
    out = opts.out
    os.makedirs(out, exist_ok=True)
    PT.build_native(os.path.join(out, "tool"), encoders=False, quiet=True)   # (once, before the workers look for it)
    only = list(opts.only or []) + [SUBDIR + "/" + m.strip("/") + "/" for m in (opts.map or [])]
    cfg = config(opts.zstd_level)
    idx_path = os.path.join(out, "index.json")
    old = load_index(idx_path, cfg)
    listing = walk(romfs, only)
    walked = {r for r, _s, _m in listing}
    files, jobs = {}, []
    for rel, size, mtime in listing:
        o = old.get(rel)
        if o and o["size"] == size and o["mtime"] == mtime and o["status"] in ("ok", "skip") and (
                o["status"] == "skip" or os.path.exists(os.path.join(out, rel))):
            files[rel] = o
        else:
            jobs.append((rel, size, mtime, o["hash"] if o and o.get("hash") is not None and o["size"] == size else None))
    for rel, o in old.items():   # (files this run did not look at stay as they are)
        if rel not in walked:
            files[rel] = o
    jobs.sort(key=lambda j: -j[1])
    print("prepare_terrain: %d files looked at, %d unchanged, %d to do (%.0f MB), %d workers, romfs %s -> %s" % (
        len(listing), len(files) - sum(1 for r in files if r not in walked), len(jobs), sum(j[1] for j in jobs) / 1e6,
        opts.jobs, romfs, out), flush=True)
    stats = dict(done=0, in_b=0, out_b=0, lin=0, errors=[], unchanged=0, skipped=[])
    last_ck = last_pr = time.time()

    def results():
        if opts.jobs <= 1:
            worker_init(romfs, out, opts.zstd_level, 0)
            yield from map(work_file, jobs)
            return
        with mp.get_context().Pool(opts.jobs, initializer=worker_init, initargs=(romfs, out, opts.zstd_level, opts.nice),
                                   maxtasksperchild=2000) as pool:
            yield from pool.imap_unordered(work_file, jobs, chunksize=4)
    for rel, r in results():
        stats["done"] += 1
        if r.get("unchanged"):
            stats["unchanged"] += 1
            e = dict(old[rel])
            e["mtime"] = r["mtime"]
            files[rel] = e
            continue
        files[rel] = r
        if r["status"] == "ok":
            stats["in_b"] += r["size"]
            stats["out_b"] += r["out_size"]
            stats["lin"] += r["lin"]
        elif r["status"] == "error":
            stats["errors"].append("%s: %s" % (rel, r["why"]))
            print("prepare_terrain: %s: %s" % (rel, r["why"]), flush=True)
        else:
            stats["skipped"].append((rel, r["why"]))
        now = time.time()
        if now - last_pr > 30:
            last_pr = now
            print("prepare_terrain: %d/%d files, %.0f MB in -> %.0f MB out, %.0f s" % (
                stats["done"], len(jobs), stats["in_b"] / 1e6, stats["out_b"] / 1e6, now - t_start), flush=True)
        if now - last_ck > opts.checkpoint:
            last_ck = now
            save_index(idx_path, cfg, files)
    save_index(idx_path, cfg, files)
    size_table(romfs, out, files)
    wall = time.time() - t_start
    text = "run: %.0f s wall, %d files converted (%.0f MB in), %d unchanged by hash, %.0f MB out (%.0f MB linear texels), %d skipped, %d errors\n" % (
        wall, stats["done"] - stats["unchanged"], stats["in_b"] / 1e6, stats["unchanged"], stats["out_b"] / 1e6, stats["lin"] / 1e6,
        len(stats["skipped"]), len(stats["errors"]))
    text += "\n".join(report(files, out)) + "\n"
    open(os.path.join(out, "report.txt"), "w").write(text)
    print(text, end="", flush=True)
    stats["code"] = 1 if stats["errors"] else 0
    return stats


def size_table(romfs, out, files):
    """the game's resource size table with each converted file's entry raised by the bytes its file grew, written to
    <out>/System/Resource/<its name> (zstd, no dictionary); the entries raised. A romfs with no table: none written"""
    import zlib
    import zstandard
    spec = importlib.util.spec_from_file_location("prompt_assets", os.path.join(HERE, "prompt_assets.py"))
    pa = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pa)
    folder = os.path.join(romfs, "System", "Resource")
    name = next((n for n in sorted(os.listdir(folder)) if n.startswith("ResourceSizeTable.") and n.endswith(".rsizetable.zs")),
                None) if os.path.isdir(folder) else None
    if not name or not os.path.exists(os.path.join(romfs, "Pack", "ZsDic.pack.zs")):
        print("prepare_terrain: no resource size table in the romfs: none written", flush=True)
        return 0
    b = bytearray(pa.read_zs(romfs, "System/Resource/" + name, pa.zs_dictionary(romfs)))
    if b[:6] != b"RESTBL":
        sys.exit("prepare_terrain: %s is not a RESTBL table" % name)
    _ver, name_len, hashed, named = struct.unpack_from("<IIII", b, 6)
    grew = {}
    for rel, e in files.items():
        if e.get("status") == "ok" and e.get("out_size", 0) > e.get("size", 0):
            grew[rel] = e["out_size"] - e["size"]
    by_crc = {zlib.crc32(rel.encode()): d for rel, d in grew.items()}
    raised = 0
    for i in range(hashed):
        at = 22 + 8 * i
        h, size = struct.unpack_from("<II", b, at)
        if h in by_crc:
            struct.pack_into("<I", b, at + 4, size + by_crc.pop(h))
            raised += 1
    base = 22 + 8 * hashed
    for i in range(named):   # (paths whose CRCs collide: by name)
        at = base + (name_len + 4) * i
        rel = bytes(b[at:at + name_len]).split(b"\0")[0].decode("utf-8", "replace")
        if rel in grew:
            size, = struct.unpack_from("<I", b, at + name_len)
            struct.pack_into("<I", b, at + name_len, size + grew[rel])
            raised += 1
    dst = os.path.join(out, "System", "Resource")
    os.makedirs(dst, exist_ok=True)
    tmp = os.path.join(dst, name + ".tmp")
    with open(tmp, "wb") as f:
        f.write(zstandard.ZstdCompressor(level=3).compress(bytes(b)))
    os.replace(tmp, os.path.join(dst, name))
    print("prepare_terrain: %s: %d entries raised (%d converted files grew)" % (name, raised, len(grew)), flush=True)
    return raised


def report(files, out):
    import collections
    kinds = collections.defaultdict(lambda: [0, 0, 0, 0])
    status = collections.Counter(r["status"] for r in files.values())
    L = ["index: %d files (%s)" % (len(files), ", ".join("%s %d" % kv for kv in sorted(status.items())))]
    for r in files.values():
        if r["status"] == "ok":
            k = kinds[r["name"]]
            k[0] += 1
            k[1] += r["size"]
            k[2] += r["out_size"]
            k[3] += r["lin"]
    L.append("%-22s %7s %10s %10s %12s" % ("layout", "files", "in MB", "out MB", "linear MB"))
    tot = [0, 0, 0, 0]
    for name, k in sorted(kinds.items()):
        L.append("%-22s %7d %10.1f %10.1f %12.1f" % (name, k[0], k[1] / 1e6, k[2] / 1e6, k[3] / 1e6))
        tot = [a + b for a, b in zip(tot, k)]
    L.append("%-22s %7d %10.1f %10.1f %12.1f" % ("total", tot[0], tot[1] / 1e6, tot[2] / 1e6, tot[3] / 1e6))
    for rel, r in sorted(files.items()):
        if r["status"] != "ok":
            L.append("  %s %s: %s" % (r["status"], rel, r.get("why", "")))
    return L


# ---- verify -----------------------------------------------------------------------------------------------------------
def verify(opts):
    """N random converted files: the output's frame and header checked, every byte of the record but B equal to the
    source's, B re-swizzled from the output (into two buffers of different filler, so the padding is told from texels)
    and every texel byte equal to the source's, byte for byte"""
    import numpy as np
    romfs = opts.romfs or PT.default_romfs()
    out = opts.out
    nat = PT.Native(PT.build_native(os.path.join(out, "tool"), encoders=False, quiet=True))
    files = json.load(open(os.path.join(out, "index.json")))["files"]
    pool = sorted(r for r, e in files.items() if e["status"] == "ok")
    pick = random.Random(opts.seed).sample(pool, min(opts.verify, len(pool)))
    bad = 0
    pad_nonzero = 0
    t0 = time.time()
    for rel in pick:
        try:
            src = open(os.path.join(romfs, rel), "rb").read()
            dst = open(os.path.join(out, rel), "rb").read()
            if hash64(src) != files[rel]["hash"]:
                raise ValueError("source changed since it was converted")
            srec, sb0, sbsz, layers = parse(src)
            drec, db0, dbsz, dlayers = parse(dst)
            lay, name = classify(layers, sbsz)
            total, ls, th, wbytes, rows, lin_total = lay
            if dlayers != layers or dbsz != lin_total or db0 != sb0 or SIZES.get((layers, sbsz)) != lin_total:
                raise ValueError("layers %d/%d, B %d (want %d), B at %d/%d" % (layers, dlayers, dbsz, lin_total, db0, sb0))
            if srec[:0x30] != drec[:0x30] or srec[0x34:sb0] != drec[0x34:db0]:
                raise ValueError("a byte outside B (and B's size) differs")
            if struct.unpack_from("<I", drec, 0x30)[0] != lin_total:
                raise ValueError("B's size field")
            lin = np.frombuffer(drec, np.uint8, lin_total, db0)
            per = wbytes * rows
            sblob = srec[sb0:sb0 + sbsz]
            one = nat.L.hk_block_linear_size(wbytes, rows, th)
            for l in range(layers):
                layer_lin = np.ascontiguousarray(lin[l * per:(l + 1) * per])
                both = []
                for fill in (0, 0xFF):
                    bl = np.full(one, fill, np.uint8)
                    nat.L.hk_block_linear(bl.ctypes.data, one, layer_lin.ctypes.data, wbytes, rows, th, 0)
                    both.append(bl)
                texel = both[0] == both[1]   # (where both fills agree: a texel; elsewhere the padding)
                s = np.frombuffer(sblob, np.uint8, one, l * ls)
                if not (s[texel] == both[0][texel]).all():
                    raise ValueError("layer %d: a texel differs from the source" % l)
                if int(texel.sum()) != per:
                    raise ValueError("layer %d: %d texel bytes written, want %d" % (l, int(texel.sum()), per))
                pad_nonzero += int((s[~texel] != 0).sum())
            print("verify: %s [%s]: ok" % (rel, name), flush=True)
        except Exception as ex:
            bad += 1
            print("verify: FAIL %s: %s" % (rel, ex), flush=True)
    print("verify: %d of %d files match byte for byte (%.0f s); non-zero padding bytes in the sources: %d" % (
        len(pick) - bad, len(pick), time.time() - t0, pad_nonzero), flush=True)
    return 1 if bad else 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--romfs", help="the romfs folder (default: work/dump.txt's path + /romfs)")
    p.add_argument("--out", default=os.path.join(ROOT, "work", "prepared", "terrain"))
    p.add_argument("--jobs", type=int, default=8)
    p.add_argument("--nice", type=int, default=10)
    p.add_argument("--only", action="append", help="only romfs paths starting with this (repeatable)")
    p.add_argument("--map", action="append", help="only TerrainArc/<NAME>/ (repeatable), e.g. KokiriForest_01")
    p.add_argument("--zstd-level", type=int, default=19)
    p.add_argument("--checkpoint", type=float, default=120)
    p.add_argument("--verify", type=int, metavar="N")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--report", action="store_true")
    p.add_argument("--size-table", action="store_true")
    opts = p.parse_args(argv)
    if opts.size_table:
        size_table(opts.romfs or PT.default_romfs(), opts.out, json.load(open(os.path.join(opts.out, "index.json")))["files"])
        return 0
    if opts.verify:
        return verify(opts)
    if opts.report:
        files = json.load(open(os.path.join(opts.out, "index.json")))["files"]
        print("\n".join(report(files, opts.out)))
        return 0
    if opts.nice and hasattr(os, "nice"):
        try:
            os.nice(opts.nice)
        except OSError:
            pass
        opts.nice = 0
    return run(opts)["code"]


if __name__ == "__main__":
    sys.exit(main())
