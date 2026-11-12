"""The build-time texture converter (nx2/prepare_textures.py, nx2/texconv.cpp) on synthetic textures, no game data: the
block-linear layout both ways against an independent reading of the GOB rule, the ASTC block scan, the target by channel
swizzle, the frame split, and a whole run over a made-up romfs (BNTX files written here) with the pack and index read back,
the incremental skip and the verifier. The encoders (bc7enc_rdo + ISPC, --bc7enc / work/deps) are used when they are on
this PC; without them the converted textures are left out and the rest still runs."""
import argparse
import importlib.util
import os
import random
import struct
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prepare_textures", ROOT / "nx2" / "prepare_textures.py")
P = importlib.util.module_from_spec(spec)
spec.loader.exec_module(P)
import numpy as np   # noqa: E402
import zstandard     # noqa: E402


def ref_offset(x, y, width_bytes, bh):
    """Byte (x, y) of a block-linear surface, written out from formats.md's description"""
    gobs_x = (width_bytes + 63) // 64
    block_rows = 8 * (1 << bh)
    block = (y // block_rows) * gobs_x + x // 64
    in_gob = ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + x % 16
    return block * 512 * (1 << bh) + ((y % block_rows) // 8) * 512 + in_gob


def check_block_linear(nat):
    rng = random.Random(5)
    for width_bytes, rows, bh in [(16, 4, 0), (64, 8, 0), (8, 3, 0), (200, 37, 2), (4096, 64, 4), (48, 129, 5), (1, 1, 0),
                                  (128, 16, 1), (6, 17, 3)]:
        lin = np.frombuffer(rng.randbytes(width_bytes * rows), np.uint8)
        bl = nat.to_block_linear(lin, width_bytes, rows, bh)
        ref = np.zeros_like(bl)
        for y in range(rows):
            for x in range(width_bytes):
                ref[ref_offset(x, y, width_bytes, bh)] = lin[y * width_bytes + x]
        assert len(bl) == -(-width_bytes // 64) * -(-rows // (8 << bh)) * (512 << bh), (width_bytes, rows, bh)
        assert (bl == ref).all(), ("swizzle differs from the GOB rule", width_bytes, rows, bh)
        back = nat.to_linear(bl, 0, len(bl), width_bytes, rows, bh)
        assert back.tobytes() == lin.tobytes(), ("round trip", width_bytes, rows, bh)
    # (a short source reads as zeros past its end)
    short = nat.to_linear(np.full(100, 7, np.uint8), 0, 100, 64, 8, 0)
    assert all(short[y * 64 + x] == (7 if ref_offset(x, y, 64, 0) < 100 else 0) for y in range(8) for x in range(64))
    # mip block heights: halved while a mip's rows fit in half a block (nvn_layout.c adj_tile)
    assert P.mip_block_height(16, 1) == 1 and P.mip_block_height(8, 1) == 0 and P.mip_block_height(64, 4) == 3
    assert P.mip_block_height(1, 5) == 0 and P.mip_block_height(129, 4) == 4


def astc_block(bm, parts=0, cem=0, low6=None):
    v = bm | (parts << 11)
    if parts == 0:
        v |= cem << 13
    else:
        v |= (low6 if low6 is not None else 0) << 23
    return v.to_bytes(16, "little")


def void_extent(r, g, b, a, hdr=False):
    v = 0x1FC | (int(hdr) << 9) | (3 << 10) | (((1 << 52) - 1) << 12)
    v |= (r * 257) << 64 | (g * 257) << 80 | (b * 257) << 96 | (a * 257) << 112
    return v.to_bytes(16, "little")


def check_astc_scan(nat):
    ok_mode = 0x42   # (4x4 weights of 2 bits: 32 bits, a valid 2D mode)
    blocks = [void_extent(1, 2, 3, 4), void_extent(1, 2, 3, 4, hdr=True), astc_block(ok_mode, 0, 15), astc_block(ok_mode, 0, 8),
              astc_block(0), astc_block(ok_mode, 1, low6=(3 << 2) | 0),   # (2 partitions, shared CEM 3: HDR)
              astc_block(ok_mode, 1, low6=(10 << 2) | 0)]
    counts = nat.astc_scan(np.frombuffer(b"".join(blocks), np.uint8))
    assert counts == [2, 1, 1, 1], counts


def check_targets():
    SR, SG, SB, SA = P.SW_R, P.SW_G, P.SW_B, P.SW_A
    astc55u, astc85s = (0x2f << 8) | 1, (0x32 << 8) | 6
    t = P.choose_target(astc55u, (2, 5, 0, 1))          # normal map: R and A
    assert (t["kind"], t["vk"], t["c0"], t["c1"], t["map"]) == ("bc5", 141, 0, 3, [SR, SR, SR, SG]), t
    assert t["dropped"] == 0b0110
    t = P.choose_target(astc55u, (2, 2, 2, 2))          # one channel
    assert (t["kind"], t["vk"], t["c0"], t["map"]) == ("bc4", 139, 0, [SR] * 4), t
    t = P.choose_target(astc55u, (5, 5, 5, 5))          # one channel, the alpha
    assert (t["kind"], t["c0"]) == ("bc4", 3), t
    t = P.choose_target(astc85s, (2, 3, 4, 5))          # colour + alpha, sRGB
    assert (t["kind"], t["vk"], t["force_alpha"], t["map"]) == ("bc7", 146, False, [SR, SG, SB, SA]), t
    t = P.choose_target(astc55u, (2, 3, 4, 1))          # RAH: alpha unused
    assert (t["kind"], t["vk"], t["force_alpha"], t["dropped"]) == ("bc7", 145, True, 8), t
    t = P.choose_target(astc85s, (2, 2, 2, 5))          # sRGB luminance-alpha: no sRGB BC5, so BC7
    assert (t["kind"], t["vk"], t["fill"]) == ("bc7", 146, 0), t
    assert P.prepare_input(np.array([[[10, 20, 30, 40]]], np.uint8), t).tolist() == [[[10, 10, 10, 40]]]
    t = P.choose_target(astc85s, (2, 3, 4, 1), bc1_opaque=True)
    assert (t["kind"], t["vk"], t["map"][3]) == ("bc1", 132, P.SW_ONE), t
    assert P.choose_target(astc85s, (2, 3, 4, 5), True, alpha_opaque=True)["kind"] == "bc1"
    assert P.choose_target(astc85s, (2, 3, 4, 5), True, alpha_opaque=False)["kind"] == "bc7"
    assert P.choose_target(astc55u, (2, 3, 4, 1), bc1_opaque=True)["kind"] == "bc7"   # (BC1 only for sRGB colour)
    for fmt, kind, vk in [((0x1a << 8) | 6, "copy_bc", 134), ((0x1f << 8) | 0xa, "copy_bc", 143), ((0x1e << 8) | 2, "copy_bc", 142),
                          ((0x0a << 8) | 1, "copy", 70), ((0x15 << 8) | 5, "copy", 97), ((0x0f << 8) | 5, "copy", 122)]:
        t = P.choose_target(fmt, (2, 3, 4, 5))
        assert (t["kind"], t["vk"]) == (kind, vk), (hex(fmt), t)
    assert P.choose_target(0x7701, (2, 3, 4, 5))["kind"] == "none"
    # the fallback rules: BC7 of the channels read (1), then exact uncompressed (2), for ASTC only
    t = P.choose_target(astc55u, (2, 5, 0, 1), rule=1)
    assert (t["kind"], t["vk"], t["map"], t["used"]) == ("bc7", 145, [SR, SG, SB, SA], [0, 3]), t
    for sel, srgb, vk, pack, m in [((2, 5, 0, 1), False, 16, [0, 3], [SR, SR, SR, SG]),          # normal map R, A: RG8
                                   ((2, 2, 2, 2), False, 9, [0], [SR] * 4), ((5, 5, 5, 5), False, 9, [3], [SR] * 4),
                                   ((3, 3, 3, 3), False, 9, [1], [SR] * 4), ((2, 3, 4, 1), False, 37, [0, 1, 2, 3], [SR, SG, SB, SA]),
                                   ((2, 3, 4, 5), False, 37, [0, 1, 2, 3], [SR, SG, SB, SA]),
                                   ((2, 2, 2, 2), True, 15, [0], [SR] * 4), ((5, 5, 5, 5), True, 9, [3], [SR] * 4),
                                   ((2, 3, 4, 5), True, 43, [0, 1, 2, 3], [SR, SG, SB, SA]), ((2, 5, 0, 1), True, 43, [0, 1, 2, 3], [SR, SG, SB, SA])]:
        t = P.choose_target(((0x2f << 8) | (6 if srgb else 1)), sel, rule=2)
        assert (t["kind"], t["vk"], t["pack"], t["map"]) == ("raw", vk, pack, m), (sel, srgb, t)
    assert P.choose_target(astc55u, (2, 5, 0, 1), rule=2)["dropped"] == 0b0110
    assert P.choose_target((0x1a << 8) | 1, (2, 3, 4, 5), rule=2)["kind"] == "copy_bc"   # (rules only touch ASTC)


def check_frames():
    mips = [(0, 3 << 20), (3 << 20, 1 << 20), (4 << 20, 300 << 10), (4 * 2**20 + (300 << 10), 60 << 10), (4 * 2**20 + (360 << 10), 4 << 10)]
    total = mips[-1][0] + mips[-1][1]
    fr = P.frames_of(total, mips)
    pos = 0
    for a, n in fr:
        assert a == pos and 0 < n <= 1 << 20 and a % 16 == 0, fr
        pos += n
    assert pos == total
    assert [a for a, _n in fr][:5] == [0, 1 << 20, 2 << 20, 3 << 20, 4 << 20]   # (mips of 64 KiB or more alone)
    assert len(fr) == 6 and fr[-1] == (4 * 2**20 + (300 << 10), 64 << 10)    # (the tail of small mips together)


# ---- a made-up romfs ---------------------------------------------------------------------------------------------------
def make_bntx(nat, texs):
    """A BNTX as the game's (formats.md): header, 'Ounc' block, BRTIs, names, mip tables, BRTD with the textures'
    block-linear data, _RLT. texs: dicts of name, fmt, w, h, layers, mips, bh, sel, target, lin {(mip, layer): bytes}"""
    n = len(texs)
    tbl = 0x60
    brti0 = tbl + 8 * n
    brti0 = -(-brti0 // 16) * 16
    strs = brti0 + 0xA0 * n
    blob = bytearray(0x1000)
    names, mipt = [], []
    pos = strs
    for t in texs:
        nb = t["name"].encode()
        names.append(pos)
        struct.pack_into("<H", blob, pos, len(nb))
        blob[pos + 2:pos + 2 + len(nb)] = nb
        pos = -(-(pos + 2 + len(nb) + 1) // 8) * 8
    for t in texs:
        mipt.append(pos)
        pos += 8 * t["mips"]
    brtd = max(0x1000, -(-pos // 0x1000) * 0x1000)
    blob = blob + bytes(brtd + 0x10 - len(blob)) if len(blob) < brtd + 0x10 else blob
    data = bytearray()
    starts = []
    for t in texs:
        o = P.Tex()
        o.format, o.width, o.height, o.mips, o.layers, o.bh_log2 = t["fmt"], t["w"], t["h"], t["mips"], t["layers"], t["bh"]
        lay, stride = P.surface_layout(o)
        t["stride"], t["lay"] = stride, lay
        while (brtd + 0x10 + len(data)) % 512:
            data.append(0)
        starts.append(brtd + 0x10 + len(data))
        img = bytearray(stride * t["layers"])
        _nm, _bw, _bh, bpb = P.FMT[t["fmt"] >> 8]
        for m, (off, size, wb, hb, mbh) in enumerate(lay):
            for layer in range(t["layers"]):
                bl = nat.to_block_linear(t["lin"][(m, layer)], wb * bpb, hb, mbh)
                img[layer * stride + off:layer * stride + off + size] = bl.tobytes()
        data += img
    rlt = brtd + 0x10 + len(data)
    rlt = -(-rlt // 8) * 8
    total = rlt + 0x70
    b = bytearray(total)
    b[:len(blob)] = blob
    b[brtd + 0x10:brtd + 0x10 + len(data)] = data
    b[0:4] = b"BNTX"
    b[8:12] = b"\x01\x01\x04\x00"
    struct.pack_into("<HBB", b, 0x0C, 0xFEFF, 12, 0x40)
    struct.pack_into("<II", b, 0x18, rlt, total)
    b[0x20:0x24] = b"Ounc"
    struct.pack_into("<IQQQ", b, 0x24, n, tbl, brtd, 0)
    b[brtd:brtd + 4] = b"BRTD"
    struct.pack_into("<Q", b, brtd + 8, len(data) + 0x10)
    b[rlt:rlt + 4] = b"_RLT"
    for i, t in enumerate(texs):
        o = brti0 + 0xA0 * i
        struct.pack_into("<Q", b, tbl + 8 * i, o)
        b[o:o + 4] = b"BRTI"
        struct.pack_into("<II", b, o + 4, 0, 0xA0)
        struct.pack_into("<BBHHHIII", b, o + 0x10, 1, 2, 0, 0, t["mips"], 1, t["fmt"], 0)
        struct.pack_into("<6I", b, o + 0x24, t["w"], t["h"], 1, t["layers"], t["bh"], 0x10007)
        struct.pack_into("<II", b, o + 0x50, t["stride"] * t["layers"], 512)
        b[o + 0x58:o + 0x5c] = bytes(t["sel"])
        b[o + 0x5c] = t["target"]
        struct.pack_into("<QQQ", b, o + 0x60, names[i], 0, mipt[i])
        for m, (off, *_r) in enumerate(t["lay"]):
            struct.pack_into("<Q", b, mipt[i] + 8 * m, starts[i] + off)
    return bytes(b)


def tex_spec(name, fmt, w, h, layers, mips, bh, sel=(2, 3, 4, 5), target=1, filler=None, seed=0):
    rng = random.Random(seed)
    _nm, bw, bhp, bpb = P.FMT[fmt >> 8]
    lin = {}
    for m in range(mips):
        mw, mh = max(1, w >> m), max(1, h >> m)
        n = -(-mw // bw) * -(-mh // bhp) * bpb
        for layer in range(layers):
            lin[(m, layer)] = filler(m, layer, n) if filler else rng.randbytes(n)
    return dict(name=name, fmt=fmt, w=w, h=h, layers=layers, mips=mips, bh=bh, sel=list(sel), target=target, lin=lin)


def fake_astc_decode(lin, w, h, bw, bh):
    """Void-extent blocks only (the test's ASTC): each block one colour, the top 8 bits of its UNORM16 channels"""
    a = np.frombuffer(lin.tobytes() if hasattr(lin, "tobytes") else lin, np.uint8).reshape(-1, 16)
    col = a[:, 9::2][:, :4]   # (bytes 8.. hold 4 x u16: their high bytes are 9, 11, 13, 15)
    bx, by = -(-w // bw), -(-h // bh)
    img = col.reshape(by, bx, 4).repeat(bh, 0).repeat(bw, 1)
    return np.ascontiguousarray(img[:h, :w])


def astc_filler(colour_of):
    def f(m, layer, n):
        out = bytearray()
        for i in range(n // 16):
            out += void_extent(*colour_of(m, layer, i))
        return bytes(out)
    return f


def opts_for(romfs, out, **kw):
    o = argparse.Namespace(romfs=str(romfs), out=str(out), jobs=1, nice=0, files=None, only=None, prune=False, rebuild=False,
                           bc1_opaque=False, min_psnr=35.0, bc7_level=1, bc45_radius=2, zstd_level=3, frame_kb=64,
                           pack_mb=1, checkpoint=1e9, bc7enc=kw.pop("bc7enc", None), ispc=kw.pop("ispc", None), verify=None,
                           seed=1, report=False, kept_only=kw.pop("kept_only", False))
    for k, v in kw.items():
        setattr(o, k, v)
    return o


def check_run(nat, tmp, encoders, bc7enc, ispc):
    kw = dict(bc7enc=bc7enc, ispc=ispc, kept_only=not encoders)
    romfs = Path(tmp) / "romfs"
    (romfs / "Tex").mkdir(parents=True)
    (romfs / "Other").mkdir()
    out = Path(tmp) / "store"
    cz = zstandard.ZstdCompressor(level=3)
    kept = [tex_spec("Kept_BC1", (0x1a << 8) | 6, 100, 60, 1, 5, 2, seed=1),
            tex_spec("Kept_RGBA8_Array", (0x0b << 8) | 1, 33, 17, 4, 3, 1, target=5, seed=2),
            tex_spec("Kept_BC4_Big", (0x1d << 8) | 1, 2048, 1024, 1, 3, 4, seed=3),
            tex_spec("Kept_R16_Odd", (0x0a << 8) | 1, 5, 3, 3, 3, 0, target=5, seed=4)]
    files = {"Tex/Kept_BC1.bntx.zs": [kept[0]], "Tex/Kept_Two.bntx.zs": [kept[1], kept[2]], "Tex/Kept_R16.bntx.zs": [kept[3]]}
    conv = []
    if encoders:
        colour = lambda m, layer, i: ((i * 7 + m * 40) % 256, (i * 13 + layer * 50) % 256, (i * 3) % 256, (255 - i) % 256)
        conv = [tex_spec("Nrm", (0x2f << 8) | 1, 40, 35, 1, 3, 0, sel=(2, 5, 0, 1), filler=astc_filler(colour)),
                tex_spec("Alb", (0x32 << 8) | 6, 64, 30, 2, 2, 0, target=5, filler=astc_filler(colour)),
                tex_spec("AO", (0x2d << 8) | 1, 16, 16, 1, 1, 0, sel=(2, 2, 2, 2), filler=astc_filler(colour)),
                tex_spec("AO_Copy", (0x2d << 8) | 1, 16, 16, 1, 1, 0, sel=(2, 2, 2, 2), filler=astc_filler(colour))]
        conv[3]["name"] = "AO_Copy"
        files["Tex/Conv.bntx.zs"] = conv[:3]
        files["Tex/AO_Copy.bntx.zs"] = [conv[3]]
    bntx = {}
    for path, texs in files.items():
        bntx[path] = make_bntx(nat, texs)
        (romfs / path).write_bytes(cz.compress(bntx[path]))
    (romfs / "Other" / "not_a_texture.bin").write_bytes(b"nothing here" * 100)
    P.astc_decode = fake_astc_decode
    st = P.run(opts_for(romfs, out, **kw))
    assert st["code"] == 0 and st["files"] == len(files) + 1, st
    cfg, entries, n_packs = P.read_index(str(out / "index.bin"))
    assert n_packs >= 2, n_packs   # (pack_mb 1: rolled over)
    by_path = {e["path"]: e for e in entries}
    assert set(by_path) == set(files), by_path.keys()
    for path, texs in files.items():
        e = by_path[path]
        b = bntx[path]
        meta = P.read_frames(str(out), [e["meta_frame"]], e["meta_frame"][4])
        assert meta == b[:e["head_len"]] + b[e["rlt_off"]:], path
        assert e["bntx_size"] == len(b) and e["src_hash"] == P.hash64((romfs / path).read_bytes())
        for spec_t, t in zip(texs, e["textures"]):
            r = t["rec"]
            assert r["name"] == spec_t["name"] and r["status"] == "ok", r
            raw = P.read_frames(str(out), t["frames"], r["raw_size"])
            for m in range(spec_t["mips"]):
                mo, lb = t["mips"][m]
                assert mo % 4 == 0 and (m == 0 or mo >= t["mips"][m - 1][0] + t["mips"][m - 1][1] * spec_t["layers"]), t["mips"]
                for layer in range(spec_t["layers"]):
                    seg = raw[mo + layer * lb:mo + (layer + 1) * lb]
                    w, h = max(1, spec_t["w"] >> m), max(1, spec_t["h"] >> m)
                    if r["kind"].startswith("copy"):
                        assert seg == spec_t["lin"][(m, layer)], (spec_t["name"], m, layer)
                    else:
                        src = fake_astc_decode(np.frombuffer(spec_t["lin"][(m, layer)], np.uint8), w, h, *P.FMT[spec_t["fmt"] >> 8][1:3])
                        dec = nat.decode(r["kind"], seg, w, h)
                        plan = P.choose_target(spec_t["fmt"], spec_t["sel"])
                        for sc, dc in P.compared_channels(plan):
                            assert np.abs(dec[..., dc].astype(int) - src[..., sc]).max() <= 40, (spec_t["name"], m, layer, sc)
            if r["kind"] != "copy_bc" and r["kind"] != "copy":
                assert r["psnr"] >= 35 and r["vk_format"] in (139, 141, 146), r
            for f in t["frames"]:
                assert f[4] <= 64 << 10
    kinds = {t["rec"]["name"]: t["rec"]["kind"] for e in entries for t in e["textures"]}
    assert kinds["Kept_BC1"] == "copy_bc" and kinds["Kept_RGBA8_Array"] == "copy"
    if encoders:
        assert (kinds["Nrm"], kinds["Alb"], kinds["AO"]) == ("bc5", "bc7", "bc4"), kinds
        a, c = [t for e in entries for t in e["textures"] if t["rec"]["name"] in ("AO", "AO_Copy")]
        assert a["frames"] == c["frames"] and (a["rec"]["flags"] | c["rec"]["flags"]) & P.FLAG_DEDUP   # (same content)
    # (the verifier on the store)
    assert P.verify(opts_for(romfs, out, verify=50, **kw)) == 0
    # incremental: nothing to do; a touched file is unchanged by its hash; a changed one is made again
    st = P.run(opts_for(romfs, out, **kw))
    assert st["files"] == 0 and st["code"] == 0, st
    t1 = romfs / "Tex/Kept_BC1.bntx.zs"
    os.utime(t1, ns=(time.time_ns(), time.time_ns() + 10**9))
    st = P.run(opts_for(romfs, out, **kw))
    assert st["files"] == 1 and st["unchanged"] == 1, st
    kept[0]["lin"][(0, 0)] = bytes(len(kept[0]["lin"][(0, 0)]))
    t1.write_bytes(cz.compress(make_bntx(nat, [kept[0]])))
    st = P.run(opts_for(romfs, out, **kw))
    assert st["files"] == 1 and st["unchanged"] == 0 and st["tex"] == 1, st
    _cfg, entries2, _n = P.read_index(str(out / "index.bin"))
    assert len(entries2) == len(entries)
    e = [x for x in entries2 if x["path"] == "Tex/Kept_BC1.bntx.zs"][0]
    raw = P.read_frames(str(out), e["textures"][0]["frames"], e["textures"][0]["rec"]["raw_size"])
    assert raw[:len(kept[0]["lin"][(0, 0)])] == kept[0]["lin"][(0, 0)]
    # another option: everything made again
    st = P.run(opts_for(romfs, out, **kw, min_psnr=30.0))
    assert st["files"] == len(files), st


def check_fallback(nat, tmp, bc7enc, ispc):
    """Textures that cannot reach the floor as BC5 / BC7 are prepared uncompressed, exactly, and the rule is recorded"""
    romfs = Path(tmp) / "romfs2"
    (romfs / "Tex").mkdir(parents=True)
    out = Path(tmp) / "store2"
    rnd = random.Random(9)
    noise = lambda m, layer, i: tuple(rnd.randrange(256) for _ in range(4))   # (a new colour for every ASTC block)
    specs = [tex_spec("NrmHard", (0x2f << 8) | 1, 40, 35, 1, 3, 0, sel=(2, 5, 0, 1), filler=astc_filler(noise)),
             tex_spec("OneHard", (0x2d << 8) | 1, 32, 32, 1, 2, 0, sel=(2, 2, 2, 2), filler=astc_filler(noise)),
             tex_spec("AlbHard", (0x2d << 8) | 6, 32, 20, 2, 2, 0, target=5, sel=(2, 3, 4, 1), filler=astc_filler(noise))]
    b = make_bntx(nat, specs)
    (romfs / "Tex" / "Hard.bntx.zs").write_bytes(zstandard.ZstdCompressor(level=3).compress(b))
    P.astc_decode = fake_astc_decode
    kw = dict(bc7enc=bc7enc, ispc=ispc, min_psnr=99.5)   # (no lossy target reaches 99.5 dB: every one ends exact)
    assert P.run(opts_for(romfs, out, **kw))["code"] == 0
    _cfg, entries, _n = P.read_index(str(out / "index.bin"))
    assert len(entries) == 1
    want = {"NrmHard": (16, [0, 3]), "OneHard": (9, [0]), "AlbHard": (43, [0, 1, 2, 3])}
    for spec_t, t in zip(specs, entries[0]["textures"]):
        r = t["rec"]
        vk, pack = want[spec_t["name"]]
        assert (r["status"], r["kind"], r["rule"], r["vk_format"]) == ("ok", "raw", P.RULE["raw"], vk), r
        assert r["psnr"] == 99.0 and r["psnr_first"] < 99.5, r
        raw = P.read_frames(str(out), t["frames"], r["raw_size"])
        for m in range(spec_t["mips"]):
            mo, lb = t["mips"][m]
            w, h = max(1, spec_t["w"] >> m), max(1, spec_t["h"] >> m)
            for layer in range(spec_t["layers"]):
                src = fake_astc_decode(np.frombuffer(spec_t["lin"][(m, layer)], np.uint8), w, h, *P.FMT[spec_t["fmt"] >> 8][1:3])
                if spec_t["name"] == "AlbHard":
                    src = src.copy()
                    src[..., 3] = 255   # (the swizzle reads R, G, B: the unused alpha is forced to 255)
                    want_bytes = src.tobytes()
                else:
                    want_bytes = src[..., pack].tobytes()
                assert lb == len(want_bytes) and raw[mo + layer * lb:mo + (layer + 1) * lb] == want_bytes, (spec_t["name"], m, layer)
    assert P.verify(opts_for(romfs, out, verify=5, **kw)) == 0
    # (a record left refused by an older run is made again, though its file is unchanged)
    st = P.run(opts_for(romfs, out, **kw))
    assert st["files"] == 0, st


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cxx")
    ap.add_argument("--bc7enc", default=P.default_bc7enc())
    ap.add_argument("--ispc", default=P.default_ispc())
    a = ap.parse_args()
    if a.cxx:
        os.environ["CXX"] = a.cxx
    check_targets()
    check_frames()
    with tempfile.TemporaryDirectory(prefix="prepare-textures-") as tmp:
        encoders = bool(a.ispc and os.path.exists(a.ispc) and os.path.exists(os.path.join(a.bc7enc, "bc7e.ispc")))
        tool = os.path.join(tmp, "store", "tool")
        nat = P.Native(P.build_native(tool, encoders, a.bc7enc, a.ispc, quiet=True))
        check_block_linear(nat)
        check_astc_scan(nat)
        if not encoders:
            print("(no bc7enc_rdo / ISPC here: the converted textures are not checked)")
        check_run(nat, tmp, encoders, a.bc7enc, a.ispc)
        if encoders:
            check_fallback(nat, tmp, a.bc7enc, a.ispc)
    print("prepare_textures: ok")


if __name__ == "__main__":
    main()
