"""The terrain-tile converter (nx2/prepare_terrain.py) on synthetic TARB files, no game data: the tile layouts of every
format the game's files hold (RGBA8, R32F, BC5, BC6H x6, RGBA8 x2) against the independent GOB reading of the texture
test and against the BNTX tool's layout, a whole file written by this test (block-linear blob with garbage padding, the
CPU copy A, header fields) converted and read back (linear texels equal, every other byte equal, sizes patched), the
files that are not converted (not a TARB, an unknown layout, a damaged frame), and a run over a made-up romfs with the
overlay tree, the incremental skip, --map and the verifier."""
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
spec = importlib.util.spec_from_file_location("prepare_terrain", ROOT / "nx2" / "prepare_terrain.py")
T = importlib.util.module_from_spec(spec)
spec.loader.exec_module(T)
PT = T.PT
import numpy as np   # noqa: E402
import zstandard     # noqa: E402


def ref_offset(x, y, width_bytes, bh):
    """Byte (x, y) of a block-linear surface, written out from the GOB rule (formats.md)"""
    gobs_x = (width_bytes + 63) // 64
    block_rows = 8 * (1 << bh)
    in_gob = ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + x % 16
    return ((y // block_rows) * gobs_x + x // 64) * 512 * (1 << bh) + ((y % block_rows) // 8) * 512 + in_gob


# (name, bytes per block, block edge, layers, texels as the game's files hold them)
KINDS = [("bake_ao BC5", 16, 4, 1, 139264, 67600), ("bake_gi BC6H x6", 16, 4, 6, 835584, 405600),
         ("hght R32F / custom RGBA8", 4, 1, 1, 417792, 270400), ("mate RGBA8 x2", 4, 1, 2, 835584, 540800)]


def make_tarb(bpb, bw, layers, rng, a_copy=True, version=1):
    """(file bytes, the linear texels, A, the record) of a TARB as the game's: a record of 0x34 header bytes, A, then B"""
    total, ls, th, wbytes, rows, lin_total = T.tile_layout(bpb, bw, layers)
    lin = rng.randbytes(lin_total)
    blob = bytearray(rng.randbytes(total))   # (the padding is garbage, as in the game's files)
    for l in range(layers):
        for y in range(rows):
            for x in range(wbytes):
                blob[l * ls + ref_offset(x, y, wbytes, th)] = lin[l * wbytes * rows + y * wbytes + x]
    a = rng.randbytes(270400) if a_copy else b""
    rec = bytearray(0x34)
    rec[0:4] = bytes([1, 4, 2, layers])
    rec[8] = 0x81 if a_copy else 0x01
    rec[0x1a:0x1c] = b"\x4f\x7a"
    if a_copy:
        struct.pack_into("<iI", rec, 0x1c, 0x34 - 0x1c, len(a))
    struct.pack_into("<iI", rec, 0x2c, 0x34 + len(a) - 0x2c, total)
    rec += a + blob
    frame = zstandard.ZstdCompressor(level=3, write_checksum=False).compress(bytes(rec))
    f = b"TARB" + struct.pack("<IHHII", version, 1, 0x14, len(rec), len(frame)) + frame
    return f, lin, a, bytes(rec)


def check_layouts():
    # the four layouts the game's files hold (with their sizes in m0-terrain-texels.md) and nothing else matches them
    for name, bpb, bw, layers, bl, lin in KINDS:
        lay = T.tile_layout(bpb, bw, layers)
        assert lay[0] == bl and lay[5] == lin, (name, lay)
        assert T.SIZES[(layers, bl)] == lin and bl != lin
        got = T.classify(layers, bl)
        assert got is not None and got[0] == lay, (name, got)
    assert T.classify(1, 12345) is None and T.classify(3, 139264) is None
    # (the same size as another layout, told apart by the layer count: 6 x BC6H and 2 x RGBA8 are both 835,584)
    assert T.tile_layout(16, 4, 6)[0] == T.tile_layout(4, 1, 2)[0] == 835584
    assert T.classify(6, 835584)[1] != T.classify(2, 835584)[1]
    # the layout agrees with the BNTX tool's (an independent port of the same rule): level 0 of a 260 x 260 texture
    for fmt, bpb, bw, layers in [((0x1e << 8) | 1, 16, 4, 1), ((0x1f << 8) | 0xa, 16, 4, 6), ((0x0b << 8) | 1, 4, 1, 1),
                                 ((0x0b << 8) | 1, 4, 1, 2), ((0x09 << 8) | 1, 2, 1, 1), ((0x14 << 8) | 5, 4, 1, 1)]:
        total, ls, th, wbytes, rows, lin = T.tile_layout(bpb, bw, layers)
        t = PT.Tex()
        t.format, t.width, t.height, t.mips, t.layers = fmt, 260, 260, 1, layers
        t.bh_log2 = th
        lay, stride = PT.surface_layout(t)
        assert stride == ls and stride * layers == total, (hex(fmt), stride, ls)
        assert lay[0][2] * bpb == wbytes and lay[0][3] == rows and lay[0][4] == th
        assert lay[0][1] == ls or layers > 1
    # RG8 with a full chain (the tile normal map is 9 levels): level 0's block height and size as the tile layout's
    t = PT.Tex()
    t.format, t.width, t.height, t.mips, t.layers, t.bh_log2 = (0x09 << 8) | 1, 260, 260, 9, 1, 4
    lay, _stride = PT.surface_layout(t)
    assert len(lay) == 9 and lay[0][4] == T.tile_layout(2, 1, 1)[2] and lay[0][1] == T.tile_layout(2, 1, 1)[0]
    # the block-linear sizes of the 260-row tiles: 16 GOBs high for 260 rows, 8 for 65 block rows
    assert T.tile_layout(4, 1, 1)[2] == 4 and T.tile_layout(16, 4, 1)[2] == 3


def check_convert(nat):
    rng = random.Random(11)
    for name, bpb, bw, layers, bl, lin_size in KINDS:
        for a_copy in (False, True) if bpb == 4 and layers == 1 else (False,):
            f, lin, a, rec = make_tarb(bpb, bw, layers, rng, a_copy)
            new, kind, n = T.convert_bytes(nat, f, 5)
            assert n == lin_size == len(lin), (name, n)
            ver, cnt, hs, usz, csz = struct.unpack_from("<IHHII", new, 4)
            assert (new[:4], ver, cnt, hs) == (b"TARB", 1, 1, 0x14) and 0x14 + csz == len(new), name
            out_rec = zstandard.ZstdDecompressor().decompress(new[hs:], max_output_size=usz)
            assert len(out_rec) == usz == len(rec) - bl + lin_size, (name, usz)
            b0 = 0x34 + len(a)
            assert out_rec[b0:] == lin, name                                     # (the linear texels, layer after layer)
            assert out_rec[:0x30] == rec[:0x30] and out_rec[0x34:b0] == rec[0x34:b0]   # (everything else as it was, A too)
            assert struct.unpack_from("<I", out_rec, 0x30)[0] == lin_size
            assert struct.unpack_from("<iI", out_rec, 0x2c) == struct.unpack_from("<iI", rec, 0x2c)[:1] + (lin_size,)
            assert out_rec[0x1a:0x1c] == b"\x4f\x7a" and out_rec[3] == layers   # (the layout bytes: the memcpy branch stays)
            assert out_rec[8] == rec[8]
            fr = zstandard.get_frame_parameters(new[hs:])
            assert fr.content_size == usz and not fr.has_checksum
    # (what is not converted says why, and nothing is guessed)
    f, _l, _a, _r = make_tarb(4, 1, 1, rng)
    for bad, why in [(b"NOPE" + f[4:], "not a TARB"), (f[:4] + struct.pack("<I", 2) + f[8:], "header"), (f[:-5], "header"),
                     (f[:0x14] + bytes(len(f) - 0x14), "zstd")]:
        try:
            T.convert_bytes(nat, bad, 3)
        except T.Skip as ex:
            assert why in str(ex), (why, ex)
        else:
            raise AssertionError("converted " + why)
    rec = bytearray(zstandard.ZstdDecompressor().decompress(f[0x14:], max_output_size=1 << 24))
    for why in ("no known layout", "end"):
        r = bytearray(rec)
        if why == "end":
            r.extend(b"x")   # (B is not the last thing in the record)
        else:
            r[3] = 5
        fr = zstandard.ZstdCompressor(level=3).compress(bytes(r))
        try:
            T.convert_bytes(nat, f[:8] + struct.pack("<HHII", 1, 0x14, len(r), len(fr)) + fr, 3)
        except T.Skip as ex:
            assert why in str(ex), (why, ex)
        else:
            raise AssertionError("converted " + why)


def opts_for(romfs, out, **kw):
    o = argparse.Namespace(romfs=str(romfs), out=str(out), jobs=1, nice=0, only=None, map=None, zstd_level=3, checkpoint=1e9,
                           verify=None, seed=1, report=False)
    for k, v in kw.items():
        setattr(o, k, v)
    return o


def check_run(nat, tmp):
    rng = random.Random(3)
    romfs = Path(tmp) / "romfs"
    names = {"Kokiri/0100000000.adult.bake_ao.extm": (16, 4, 1), "Kokiri/0100000000.adult.bake_gi.extm": (16, 4, 6),
             "Kokiri/0100000000.hght": (4, 1, 1), "Kokiri/0100000000.custom.extm": (4, 1, 1), "Kokiri/0100000000.mate": (4, 1, 2),
             "Kokiri/0100000001.mate": (4, 1, 2), "Field/0200000000.hght": (4, 1, 1), "Field/0200000000.mate": (4, 1, 2)}
    src, lins = {}, {}
    (romfs / "TerrainArc" / "Kokiri").mkdir(parents=True)
    (romfs / "TerrainArc" / "Field").mkdir()
    for n, (bpb, bw, layers) in names.items():
        f, lin, _a, _r = make_tarb(bpb, bw, layers, rng, a_copy=n.endswith(("hght", "custom.extm")))
        (romfs / "TerrainArc" / n).write_bytes(f)
        src[n], lins[n] = f, lin
    (romfs / "TerrainArc" / "Kokiri.tscb").write_bytes(b"TSCB" + bytes(100))            # (the index files are not TARB)
    (romfs / "TerrainArc" / "Field" / "damaged.mate").write_bytes(b"\0" * 64)              # (a damaged dump)
    odd = bytearray(zstandard.ZstdDecompressor().decompress(src["Kokiri/0100000000.mate"][0x14:], max_output_size=1 << 24))
    odd[3] = 3   # (a layer count no layout of this size has)
    fr = zstandard.ZstdCompressor(level=3).compress(bytes(odd))
    (romfs / "TerrainArc" / "Field" / "0200000009.hght").write_bytes(b"TARB" + struct.pack("<IHHII", 1, 1, 0x14, len(odd), len(fr)) + fr)
    (romfs / "Other").mkdir()
    (romfs / "Other" / "x.bin").write_bytes(b"TARB")                                       # (outside TerrainArc: not looked at)
    out = Path(tmp) / "terrain"
    st = P_run(romfs, out)
    assert st["code"] == 0 and st["done"] == len(names) + 3, st
    assert sorted(r for r, _w in st["skipped"]) == ["TerrainArc/Field/0200000009.hght", "TerrainArc/Field/damaged.mate", "TerrainArc/Kokiri.tscb"]
    # the overlay tree: the same romfs-relative paths, converted files only
    assert sorted(str(p.relative_to(out)).replace(os.sep, "/") for p in out.rglob("*") if p.is_file() and p.suffix != ".so" and p.name not in ("index.json", "report.txt")
                  and "tool" not in p.parts) == sorted("TerrainArc/" + n for n in names)
    for n in names:
        new = (out / "TerrainArc" / n).read_bytes()
        rec = zstandard.ZstdDecompressor().decompress(new[0x14:], max_output_size=1 << 24)
        assert rec.endswith(lins[n]), n
    assert T.verify(opts_for(romfs, out, verify=50)) == 0
    # incremental: nothing to do; a touched file is kept by its hash; a changed one is made again; another level redoes all
    assert P_run(romfs, out)["done"] == 0
    p = romfs / "TerrainArc" / "Kokiri" / "0100000000.hght"
    os.utime(p, ns=(time.time_ns(), time.time_ns() + 10**9))
    st = P_run(romfs, out)
    assert st["done"] == 1 and st["unchanged"] == 1, st
    f, lin, _a, _r = make_tarb(4, 1, 1, rng)
    p.write_bytes(f)
    st = P_run(romfs, out)
    assert st["done"] == 1 and st["unchanged"] == 0 and st["out_b"] > 0, st
    assert zstandard.ZstdDecompressor().decompress((out / "TerrainArc" / "Kokiri" / "0100000000.hght").read_bytes()[0x14:], max_output_size=1 << 24).endswith(lin)
    os.remove(out / "TerrainArc" / "Field" / "0200000000.hght")   # (an output that went missing is made again)
    assert P_run(romfs, out)["done"] == 1
    assert P_run(romfs, out, zstd_level=5)["done"] == len(names) + 3
    # --map and --only look at one folder
    out2 = Path(tmp) / "terrain2"
    st = P_run(romfs, out2, map=["Kokiri"])
    assert st["done"] == 6 and not (out2 / "TerrainArc" / "Field").exists(), st
    st = P_run(romfs, out2, only=["TerrainArc/Field/0200000000"])
    assert st["done"] == 2, st
    # a corrupted output is caught by the verifier
    q = out / "TerrainArc" / "Kokiri" / "0100000001.mate"
    good = q.read_bytes()
    rec = bytearray(zstandard.ZstdDecompressor().decompress(good[0x14:], max_output_size=1 << 24))
    rec[-7] ^= 0x55
    fr = zstandard.ZstdCompressor(level=3).compress(bytes(rec))
    q.write_bytes(good[:0x0c] + struct.pack("<II", len(rec), len(fr)) + fr)
    assert T.verify(opts_for(romfs, out, verify=50)) == 1


def P_run(romfs, out, **kw):
    return T.run(opts_for(romfs, out, **kw))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cxx")
    a = ap.parse_args()
    if a.cxx:
        os.environ["CXX"] = a.cxx
    check_layouts()
    with tempfile.TemporaryDirectory(prefix="prepare-terrain-") as tmp:
        nat = PT.Native(PT.build_native(os.path.join(tmp, "tool"), False, quiet=True))
        check_convert(nat)
        check_run(nat, tmp)
    print("prepare_terrain: ok")


if __name__ == "__main__":
    main()
