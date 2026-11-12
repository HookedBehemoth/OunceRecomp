"""The build-time converter of the sky LUT file (docs/direct/m4e-report.md, task 1): the game's environment archive
Env/GameScene.Nin_Ounce_NVN2.genvb.zs (a zstd SARC, with the game's dictionary) holds res/Master_Field.skybin (and a
second copy named res/master_field.skybin), the precomputed sky / ambient LUTs as the console's texture storage:
three RGBA16F surfaces, Tegra block-linear, back to back, no header:

    256 x 64             131,072 bytes   (2D, block height 8 GOBs)
    256 x 32 x 16      1,048,576 bytes   (3D, NVN target 2: block height 1 GOB, block depth 16)
    64 x 64               32,768 bytes   (2D, block height 8 GOBs)

f_80846070 memcpys the three parts into the storage of the three textures (two sets: the game checks that the part
sizes sum to the file's size). The direct back end has no block-linear code, so this tool writes the same archive
under --out with each skybin member's surfaces deswizzled to packed linear rows (slice after slice), the layout of
vkCmdCopyBufferToImage with bufferRowLength 0. Every surface's linear size equals its block-linear size here (all are
whole blocks), so the member and the archive keep their sizes; every other byte of the archive is as it was, and the
frame is compressed again with the same dictionary (content size in the frame, no checksum). The host's patch
(host/direct_sky_lut.c) uploads the parts when this file was served from the prepared overlay and draws the
placeholder (zeros, counted) when it was not.

    python -I nx2/prepare_skybin.py [--romfs DIR] [--out DIR]       default --out: work/prepared/terrain (the overlay root)
    python -I nx2/prepare_skybin.py --verify                         re-swizzle the output, compare with the source, byte-exact

Game data: it reads the romfs in place and writes only under --out. Nothing it writes may leave the PC
(CONTRIBUTING.md). The deswizzle is nx2/texconv.cpp (built without the encoders); the block heights are NVN's automatic
ones (host/nvn_layout.c: a 2D surface's block height from its height x 1.5; a 3D one is 1 GOB high and its block
depth comes from its depth x 1.5). A 3D surface's slice is gathered into a 2D block-linear surface first (its blocks
are 512 << tileH bytes apart within a block of 512 << (tileH + tileD)), so texconv's 2D deswizzle serves both."""
import argparse
import importlib.util
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
CONV_VERSION = 1
FILES = ["Env/GameScene.Nin_Ounce_NVN2.genvb.zs"]
MEMBER_SUFFIX = ".skybin"
# (width, height, depth, bytes per texel) of the three parts, in the file's order (texwatch, run m4d_tw_k: NVN format
# 0x29 RGBA16F, targets 2D, 3D, 2D; one level each)
SURFACES = [(256, 64, 1, 8), (256, 32, 16, 8), (64, 64, 1, 8)]


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


PT = _load("prepare_textures")   # (the native loader, the dictionaries, the romfs default)
TR = _load("prepare_terrain")    # (NVN's automatic block height)


class Skip(Exception):
    pass


def tiles(h, d):
    """(block height, block depth) log2 in GOBs of a level-0 surface of h rows and d slices (NVN's automatic ones)"""
    if d > 1:
        return 0, TR.adj_tile(TR.pick_tile(d + d // 2), 1, d)
    return TR.adj_tile(TR.pick_tile((h + h // 2) // 8), 8, h), 0


def layout(surfaces=SURFACES):
    """[(offset in the member, row bytes, rows, depth, tileH, tileD, block-linear size)], the member's size"""
    out, off = [], 0
    for w, h, d, bpp in surfaces:
        wb = w * bpp
        th, td = tiles(h, d)
        nbx, nby, nbz = (wb + 63) // 64, -(-((h + 7) // 8) // (1 << th)), -(-d // (1 << td))
        size = nbx * nby * nbz << (9 + th + td)
        out.append((off, wb, h, d, th, td, size))
        off += size
    return out, off


def slice_blocks(off, wb, h, d, th, td, z):
    """The byte ranges (start, end) of slice z's GOB columns, in the order of a 2D block-linear surface of tileH th"""
    nbx, nby = (wb + 63) // 64, -(-((h + 7) // 8) // (1 << th))
    bs, part = 512 << (th + td), 512 << th
    bz, zz = divmod(z, 1 << td)
    return [off + ((bz * nby + by) * nbx + bx) * bs + zz * part for by in range(nby) for bx in range(nbx)], part


def sarc_members(data):
    """{name: (start, end)} of a SARC's members (absolute offsets)"""
    if data[:4] != b"SARC":
        raise Skip("not a SARC")
    _hdr, bom, _size, doff = struct.unpack_from("<HHII", data, 4)
    if bom != 0xFEFF:
        raise Skip("SARC byte order %#x" % bom)
    n = struct.unpack_from("<H", data, 0x14 + 6)[0]
    sfnt = 0x14 + 12 + 16 * n
    out = {}
    for i in range(n):
        h, attr, s, e = struct.unpack_from("<IIII", data, 0x14 + 12 + 16 * i)
        name = data[sfnt + 8 + (attr & 0xFFFF) * 4:].split(b"\0", 1)[0].decode() if attr >> 24 else "%08x" % h
        out[name] = (doff + s, doff + e)
    return out


def to_linear(nat, member, surfaces=SURFACES):
    """The member's bytes with each surface deswizzled (same size), or Skip"""
    import numpy as np
    lay, total = layout(surfaces)
    lin_total = sum(wb * h * d for _o, wb, h, d, _th, _td, _s in lay)
    if len(member) != total or lin_total != total:
        raise Skip("member of %d bytes, the layout says %d block-linear / %d linear" % (len(member), total, lin_total))
    arr = np.frombuffer(member, np.uint8)
    out = np.empty(total, np.uint8)
    p = 0
    for off, wb, h, d, th, td, size in lay:
        for z in range(d):
            if d == 1:
                sl = arr[off:off + size]
            else:
                starts, part = slice_blocks(off, wb, h, d, th, td, z)
                sl = np.concatenate([arr[s:s + part] for s in starts])
            out[p:p + wb * h] = nat.to_linear(sl, 0, len(sl), wb, h, th)
            p += wb * h
    return out.tobytes()


def to_block_linear(nat, lin, surfaces=SURFACES):
    """The inverse (the verifier)"""
    import numpy as np
    lay, total = layout(surfaces)
    lin = np.frombuffer(lin, np.uint8)
    out = np.zeros(total, np.uint8)
    p = 0
    for off, wb, h, d, th, td, size in lay:
        for z in range(d):
            bl = nat.to_block_linear(lin[p:p + wb * h], wb, h, th)
            p += wb * h
            if d == 1:
                out[off:off + len(bl)] = bl
                continue
            starts, part = slice_blocks(off, wb, h, d, th, td, z)
            for i, s in enumerate(starts):
                out[s:s + part] = bl[i * part:(i + 1) * part]
    return out.tobytes()


def convert_bytes(nat, data, dicts=None):
    """(new .zs bytes, member names) of a genvb.zs file's bytes"""
    import zstandard
    if data[:4] != b"\x28\xb5\x2f\xfd":
        raise Skip("not a zstd frame")
    dict_id = zstandard.get_frame_parameters(data).dict_id
    dicts = PT._DICTS if dicts is None else dicts
    if dict_id and dict_id not in dicts:
        raise Skip("zstd dictionary %#x missing" % dict_id)
    d = dicts[dict_id] if dict_id else None
    raw = bytearray((zstandard.ZstdDecompressor(dict_data=d) if d else zstandard.ZstdDecompressor()).decompress(
        data, max_output_size=1 << 31))
    names = []
    for name, (s, e) in sorted(sarc_members(bytes(raw)).items()):
        if name.lower().endswith(MEMBER_SUFFIX):
            raw[s:e] = to_linear(nat, bytes(raw[s:e]))
            names.append(name)
    if not names:
        raise Skip("no %s member" % MEMBER_SUFFIX)
    c = zstandard.ZstdCompressor(level=19, dict_data=d, write_checksum=False, write_content_size=True) if d else \
        zstandard.ZstdCompressor(level=19, write_checksum=False, write_content_size=True)
    return c.compress(bytes(raw)), names


def decompress(data, dicts=None):
    import zstandard
    dicts = PT._DICTS if dicts is None else dicts
    i = zstandard.get_frame_parameters(data).dict_id
    return (zstandard.ZstdDecompressor(dict_data=dicts[i]) if i else zstandard.ZstdDecompressor()).decompress(
        data, max_output_size=1 << 31)


def verify(nat, src, out, dicts=None):
    """'' when every byte of out's archive is the source's but the skybin members, and those re-swizzle to the source"""
    a, b = decompress(src, dicts), decompress(out, dicts)
    if len(a) != len(b):
        return "sizes %d vs %d" % (len(a), len(b))
    ma = sarc_members(a)
    if ma != sarc_members(b):
        return "members differ"
    mask = bytearray(len(a))
    for name, (s, e) in ma.items():
        if name.lower().endswith(MEMBER_SUFFIX):
            if to_block_linear(nat, b[s:e]) != a[s:e]:
                return "%s: re-swizzled bytes differ" % name
            mask[s:e] = b"\1" * (e - s)
    for i in range(len(a)):
        if not mask[i] and a[i] != b[i]:
            return "byte %#x differs outside the members" % i
    return ""


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--romfs")
    ap.add_argument("--out", default=os.path.join(ROOT, "work", "prepared", "terrain"))
    ap.add_argument("--verify", action="store_true")
    o = ap.parse_args(argv)
    romfs = o.romfs or PT.default_romfs()
    if not romfs or not os.path.isdir(romfs):
        sys.exit("romfs not found (--romfs, or work/dump.txt)")
    PT.load_dicts(romfs)
    nat = PT.Native(PT.build_native(os.path.join(o.out, "tool"), encoders=False, quiet=True))
    bad = 0
    for rel in FILES:
        src_p, dst_p = os.path.join(romfs, rel), os.path.join(o.out, rel)
        if not os.path.exists(src_p):
            print("prepare_skybin: %s: not in this romfs" % rel)
            continue
        src = open(src_p, "rb").read()
        if o.verify:
            why = verify(nat, src, open(dst_p, "rb").read()) if os.path.exists(dst_p) else "not converted"
            print("prepare_skybin: %s: %s" % (rel, why or "matches"))
            bad += bool(why)
            continue
        try:
            new, names = convert_bytes(nat, src)
        except Skip as ex:
            print("prepare_skybin: %s: not converted: %s" % (rel, ex))
            bad += 1
            continue
        os.makedirs(os.path.dirname(dst_p), exist_ok=True)
        with open(dst_p + ".tmp", "wb") as f:
            f.write(new)
        os.replace(dst_p + ".tmp", dst_p)
        print("prepare_skybin: %s: %s deswizzled (%d -> %d bytes) -> %s" % (rel, ", ".join(names), len(src), len(new), dst_p))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
