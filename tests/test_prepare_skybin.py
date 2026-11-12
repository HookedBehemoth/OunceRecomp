"""The sky LUT converter (nx2/prepare_skybin.py) on synthetic data, no game data: the three surfaces' layout (sizes and
NVN's automatic block height and depth), a member made block-linear by an independent reading of the GOB rule (2D and
3D) converted to linear rows and back, a whole zstd SARC with a trained dictionary converted (the skybin members linear,
every other byte equal, the archive's size and the dictionary id kept), the verifier, and the files it refuses."""
import argparse
import importlib.util
import os
import random
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prepare_skybin", ROOT / "nx2" / "prepare_skybin.py")
S = importlib.util.module_from_spec(spec)
spec.loader.exec_module(S)
import numpy as np   # noqa: E402
import zstandard     # noqa: E402


def in_gob(x, y):
    """(the Switch 2's GOB, as host/nvn_layout.c and the frozen renderer use it)"""
    return ((x % 64) // 32) * 256 + ((y % 8) // 4) * 128 + ((x % 32) // 16) * 64 + ((y % 4) // 2) * 32 + (y % 2) * 16 + x % 16


def ref_offset(x, y, z, wb, h, th, td):
    """Byte (x, y, z) of a block-linear surface: blocks of 1 GOB x 2^th GOBs x 2^td slices, x first, then y, then z;
    inside a block the GOBs of a slice go down, then the slices"""
    nbx, nby = (wb + 63) // 64, -(-((h + 7) // 8) // (1 << th))
    bh, bd = 1 << th, 1 << td
    gy = y // 8
    block = ((z // bd) * nby + gy // bh) * nbx + x // 64
    return block * 512 * bh * bd + ((z % bd) * bh + gy % bh) * 512 + in_gob(x, y)


def ref_swizzle(lin, surfaces):
    lay, total = S.layout(surfaces)
    out = bytearray(total)
    p = 0
    for off, wb, h, d, th, td, _size in lay:
        for z in range(d):
            for y in range(h):
                for x in range(wb):
                    out[off + ref_offset(x, y, z, wb, h, th, td)] = lin[p]
                    p += 1
    return bytes(out)


def check_layout():
    lay, total = S.layout()
    assert total == 0x128000, hex(total)   # (what f_80846070 checks the file against: the three storages summed)
    assert [(th, td, size) for _o, _wb, _h, _d, th, td, size in lay] == [(3, 0, 131072), (0, 4, 1048576), (3, 0, 32768)], lay
    assert S.tiles(32, 16) == (0, 4) and S.tiles(64, 1) == (3, 0) and S.tiles(32, 1) == (2, 0) and S.tiles(8, 4) == (0, 2)


SMALL = [(16, 16, 1, 8), (16, 8, 4, 8), (8, 32, 1, 8)]   # (a 2D, a 3D of 4 slices, a 2D taller than wide: 64-byte rows)


def check_roundtrip(nat):
    rng = random.Random(7)
    for surfaces in (SMALL, S.SURFACES):
        _lay, total = S.layout(surfaces)
        lin = rng.randbytes(total)
        bl = ref_swizzle(lin, surfaces)
        assert S.to_linear(nat, bl, surfaces) == lin, surfaces
        assert S.to_block_linear(nat, lin, surfaces) == bl, surfaces
    try:
        S.to_linear(nat, b"\0" * 1000)
    except S.Skip:
        pass
    else:
        raise AssertionError("a member of the wrong size was converted")


def make_sarc(members):
    """A SARC as the game's (little-endian, names in SFNT, data 0x80-aligned)"""
    names = b""
    offs = []
    for name, _data in members:
        offs.append(len(names) // 4)
        n = name.encode() + b"\0"
        names += n + b"\0" * (-len(n) % 4)
    n = len(members)
    hdr_end = 0x14 + 12 + 16 * n + 8 + len(names)
    doff = (hdr_end + 0x7f) & ~0x7f
    data, nodes = b"", b""
    for (name, blob), o in zip(members, offs):
        data += b"\0" * (-len(data) % 0x80)
        nodes += struct.pack("<IIII", hash(name) & 0xffffffff, 0x01000000 | o, len(data), len(data) + len(blob))
        data += blob
    sfat = b"SFAT" + struct.pack("<HHI", 12, n, 0x65) + nodes
    sfnt = b"SFNT" + struct.pack("<HH", 8, 0) + names
    head = b"SARC" + struct.pack("<HHIIHH", 0x14, 0xFEFF, doff + len(data), doff, 0x100, 0)
    b = head + sfat + sfnt
    return b + b"\0" * (doff - len(b)) + data


def check_file(nat):
    rng = random.Random(11)
    _lay, total = S.layout()
    lin = rng.randbytes(total)
    bl = ref_swizzle(lin, S.SURFACES)
    env = rng.randbytes(1000)
    sarc = make_sarc([("gamescene.bgenv", env), ("res/Master_Field.skybin", bl), ("res/master_field.skybin", bl)])
    samples = [rng.randbytes(64) + b"SARC gamescene skybin" * 8 for _ in range(400)]
    dic = zstandard.train_dictionary(4096, samples)
    dicts = {dic.dict_id(): dic}
    src = zstandard.ZstdCompressor(level=3, dict_data=dic).compress(sarc)
    new, names = S.convert_bytes(nat, src, dicts)
    assert names == ["res/Master_Field.skybin", "res/master_field.skybin"], names
    assert zstandard.get_frame_parameters(new).dict_id == dic.dict_id()
    out = S.decompress(new, dicts)
    assert len(out) == len(sarc)
    m = S.sarc_members(out)
    assert m == S.sarc_members(sarc)
    for name, (s, e) in m.items():
        assert out[s:e] == (env if name == "gamescene.bgenv" else lin), name
    mask = np.zeros(len(sarc), bool)
    for name, (s, e) in m.items():
        if name.endswith(".skybin"):
            mask[s:e] = True
    a, b = np.frombuffer(sarc, np.uint8), np.frombuffer(out, np.uint8)
    assert (a[~mask] == b[~mask]).all()
    assert S.verify(nat, src, new, dicts) == ""
    broken = bytearray(out)
    broken[m["res/master_field.skybin"][0] + 5] ^= 0x40
    assert S.verify(nat, src, zstandard.ZstdCompressor(level=3, dict_data=dic).compress(bytes(broken)), dicts) != ""
    # refused: not zstd, no skybin member, a member of another size, a dictionary that is not there
    for bad, why in ((sarc, "not zstd"), (zstandard.ZstdCompressor().compress(make_sarc([("a.bgenv", env)])), "no member"),
                     (zstandard.ZstdCompressor().compress(make_sarc([("x.skybin", bl[:-512])])), "size"),
                     (src, "dictionary")):
        try:
            S.convert_bytes(nat, bad, {} if why == "dictionary" else dicts)
        except S.Skip:
            continue
        raise AssertionError("converted: " + why)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cxx")
    a = ap.parse_args()
    if a.cxx:
        os.environ["CXX"] = a.cxx
    check_layout()
    with tempfile.TemporaryDirectory(prefix="prepare-skybin-") as tmp:
        nat = S.PT.Native(S.PT.build_native(os.path.join(tmp, "tool"), False, quiet=True))
        check_roundtrip(nat)
        check_file(nat)
    print("prepare_skybin: ok")


if __name__ == "__main__":
    sys.exit(main())
