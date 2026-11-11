"""The build-time texture converter (asset path step A1, work/assets/native/asset-path.md): every BNTX texture of the
romfs, deswizzled to linear blocks, ASTC re-encoded to BCn, zstd-compressed into packs with an index, for the renderer to
copy straight into Vulkan images (vkCmdCopyBufferToImage) instead of decoding ASTC and unswizzling at run time.

    python -I nx2/prepare_textures.py [--romfs DIR] [--out DIR] [--jobs 8] [--files LIST] [--only PREFIX ...]
    python -I nx2/prepare_textures.py --verify N [--seed S]      decode N random prepared textures back, compare
    python -I nx2/prepare_textures.py --report                   the summary of the store as it is

Game data: it reads the romfs in place (default: the path in work/dump.txt + /romfs) and writes only under --out
(default work/prepared/tex). Nothing it writes may leave the PC (CONTRIBUTING.md). Needs numpy and zstandard, and for
ASTC sources texture2ddecoder (work/venv has all three); the native half (nx2/texconv.cpp) is built on first use with
a C++ compiler, the bc7enc_rdo sources (--bc7enc, default work/deps/bc7enc_rdo: MIT / public domain; bc7e.ispc
Apache-2.0) and ISPC (--ispc). Niced (--nice 10) with 8 workers by default, because a game run may be measuring.

What becomes of a texture (choose_target, by the texture's own channel swizzle at BRTI+0x58: 0 zero, 1 one, 2..5 R..A;
"used" = the source channels the swizzle reads):
    ASTC, UNORM, used within {R}                -> BC4_UNORM  (R)
    ASTC, UNORM, two channels used (normal maps' R,A) -> BC5_UNORM (first used -> R, second -> G)
    other ASTC                                  -> BC7 (sRGB stays sRGB); alpha unused: alpha forced to 255, unused
                                                   colour channels filled with the first used one
    --bc1-opaque: ASTC sRGB colour whose alpha is unused or 255 everywhere -> BC1_RGB_SRGB
    BC1..BC7 (BC6H too: the HDR textures)        -> kept, blocks as they are (only deswizzled)
    R8, RG8, RGBA8, R16, RG16, RGBA16F, R11G11B10F, R32F ... -> kept, deswizzled
    anything else, 3D, or a layout that does not add up -> not prepared (status 'unsupported': the runtime path)
The quality check: every converted image (each mip of each layer) is decoded back and compared with the ASTC decode on
the used channels. A texture whose PSNR over all its images is below --min-psnr (35 dB) is refused: no data, status
'refused', the runtime path keeps it. No RDO.

Store (all little-endian; FORMAT_VERSION 1):
  pack_NNN.bin   64-byte header {char[8] "HKTXPAK1", u32 version, u32 pack id, 48 x 0}, then zstd frames back to back.
                 Every frame is a standard zstd frame (no dictionary, content size and checksum in it), at most 1 MiB of
                 content (--frame-kb). Packs are append-only and roll over at --pack-mb (1024).
  index.bin      header (128 B):
                   0 char[8] "HKTXIDX1"   8 u32 version   12 u32 header size (128)   16 u64 config hash
                  24 u32 files  28 u32 textures  32 u32 mips  36 u32 frames  40 u32 packs  44 u32 string bytes
                  48 u64 offsets of: files, textures, mips, frames, strings (48, 56, 64, 72, 80)
                  88 u16 sizes of a file, texture, mip and frame record (64, 128, 16, 32)   96..127 0
                 file record (64 B), one per BNTX (a romfs file, or one inside a SARC: path "outer::inner"):
                   0 u32 path offset, 4 u32 path length (UTF-8 in the string table; romfs-relative, '/')
                   8 u64 source size (the romfs file)  16 u64 source hash (BLAKE2b-64 of the romfs file's bytes)
                  24 u64 source mtime (ns; only for the incremental skip)  32 u64 BNTX size (decompressed)
                  40 u32 first texture  44 u32 textures  48 u32 metadata frame (BNTX [0, BRTD+0x10) then [_RLT, end))
                  52 u32 header length (BRTD+0x10)  56 u32 _RLT offset  60 u32 flags (1: inside a container)
                 texture record (128 B):
                   0 u32 file  4 u32 index in the BNTX  8 u32 name offset  12 u32 name length
                  16 u32 NVN/BNTX format (BRTI+0x1C)  20 u32 VkFormat of the prepared data (0: none)
                  24 u8 kind (KIND)  25 u8 status (STATUS)  26 u8 VkImageViewType  27 u8 BNTX target (BRTI+0x5C)
                  28 u8[4] channel map: per source channel R,G,B,A, the VkComponentSwizzle that reads it from the
                     prepared image (3..6 = R..A). The renderer composes a view's swizzle with it: final[c] =
                     map[guest[c] - R] for guest R..A, ZERO / ONE as they are
                  32 u8[4] the BNTX swizzle (BRTI+0x58)  36 u8 dropped (bit c: source channel c is not kept)
                  37 u8 block height log2 (BRTI+0x34)  38 u16 flags (FLAG_*)
                  40 u32 width  44 height  48 depth  52 layers  56 mips  60 u32 first mip record
                  64 u32 first frame  68 u32 frames  72 u64 raw size (all mips, all layers, linear)
                  80 u64 content hash (BLAKE2b-64 of BRTI [0x10, 0x60) and the swizzled image bytes)
                  88 u32 BRTI offset in the BNTX  92 u32 image size (BRTI+0x50)  96 u32 data offset (mip 0 in the BNTX)
                 100 f32 PSNR (dB, all images; 0 when not converted)  104 f32 worst image's PSNR (images >= 4096 px)
                 108 u32 alignment (BRTI+0x54)  112 u32 ASTC blocks with HDR endpoint modes  116..127 0
                 mip record (16 B): u64 offset of the mip in the texture's raw data (a multiple of 4: zero padding
                     before it when the mip above ends unaligned), u32 bytes of one layer, u32 0.
                     Layer l of mip m is at offset + l * layer bytes; rows of blocks, packed (bufferRowLength 0). A
                     texture with status ok has `mips` of them from its first; the others have none.
                 frame record (32 B): u16 pack, u16 0, u32 compressed size, u64 offset in the pack, u64 offset in the
                     texture's raw data, u32 raw size, u32 0. A texture's frames cover its raw data in order; a frame
                     holds one mip's bytes (a mip of 64 KiB or more is split in equal parts of at most 1 MiB) or the
                     tail of small mips together. Textures with the same content hash and descriptor share frames.
  scan.json      romfs files that hold no BNTX, with size and mtime (so a re-run does not open them again).
  report.txt     the last run's summary.
How A2 reads it: asset-path.md, "A1 result". Incremental: a romfs file with the size and mtime of its record is kept as it
is; with another mtime its hash decides; another config hash (CONV_VERSION, the options) redoes everything. Old pack bytes of
redone files stay as garbage until --rebuild.
"""
import argparse
import ctypes
import hashlib
import json
import math
import os
import random
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
CONV_VERSION = 1      # bump when the output of a texture changes
FORMAT_VERSION = 1    # bump when the index or pack layout changes
SKIP_DIRS = ("Sound", "Voice", "TerrainArc")   # audio, and TARB terrain tiles (not BNTX: asset-path.md A6)

KIND = {"none": 0, "copy_bc": 1, "copy": 2, "bc7": 3, "bc5": 4, "bc4": 5, "bc1": 6}
KIND_NAME = {v: k for k, v in KIND.items()}
STATUS = {"ok": 0, "refused": 1, "unsupported": 2, "error": 3}
STATUS_NAME = {v: k for k, v in STATUS.items()}
FLAG_BH1 = 1            # bh_log2 == 1: the block height asked about in formats.md (layout as the header says)
FLAG_ALPHA_FORCED = 2   # alpha set to 255 (the swizzle does not read it)
FLAG_ASTC_HDR = 4       # HDR endpoint blocks seen in the ASTC source
FLAG_DEDUP = 8          # frames shared with an earlier texture of the same content
MIP_ALIGN = 4           # every mip's data starts at a multiple of this in the texture's raw data
SW_ZERO, SW_ONE, SW_R, SW_G, SW_B, SW_A = 1, 2, 3, 4, 5, 6   # VkComponentSwizzle

# BNTX format high byte -> (name, block w, block h, bytes per block). Low byte: VARIANT.
FMT = {
    0x02: ("R8", 1, 1, 1), 0x07: ("R5G6B5", 1, 1, 2), 0x09: ("R8G8", 1, 1, 2), 0x0a: ("R16", 1, 1, 2),
    0x0b: ("R8G8B8A8", 1, 1, 4), 0x0c: ("B8G8R8A8", 1, 1, 4), 0x0e: ("R10G10B10A2", 1, 1, 4),
    0x0f: ("R11G11B10", 1, 1, 4), 0x12: ("R16G16", 1, 1, 4), 0x14: ("R32", 1, 1, 4),
    0x15: ("R16G16B16A16", 1, 1, 8), 0x17: ("R32G32", 1, 1, 8), 0x19: ("R32G32B32A32", 1, 1, 16),
    0x1a: ("BC1", 4, 4, 8), 0x1b: ("BC2", 4, 4, 16), 0x1c: ("BC3", 4, 4, 16), 0x1d: ("BC4", 4, 4, 8),
    0x1e: ("BC5", 4, 4, 16), 0x1f: ("BC6H", 4, 4, 16), 0x20: ("BC7", 4, 4, 16),
    0x2d: ("ASTC4x4", 4, 4, 16), 0x2e: ("ASTC5x4", 5, 4, 16), 0x2f: ("ASTC5x5", 5, 5, 16), 0x30: ("ASTC6x5", 6, 5, 16),
    0x31: ("ASTC6x6", 6, 6, 16), 0x32: ("ASTC8x5", 8, 5, 16), 0x33: ("ASTC8x6", 8, 6, 16), 0x34: ("ASTC8x8", 8, 8, 16),
    0x35: ("ASTC10x5", 10, 5, 16), 0x36: ("ASTC10x6", 10, 6, 16), 0x37: ("ASTC10x8", 10, 8, 16),
    0x38: ("ASTC10x10", 10, 10, 16), 0x39: ("ASTC12x10", 12, 10, 16), 0x3a: ("ASTC12x12", 12, 12, 16),
}
VARIANT = {1: "UNORM", 2: "SNORM", 3: "UINT", 4: "SINT", 5: "FLOAT", 6: "SRGB", 0xa: "UFLOAT"}
# (format high byte, variant) -> VkFormat, for the formats kept as they are
VK_KEPT = {
    (0x1a, 1): 133, (0x1a, 6): 134, (0x1b, 1): 135, (0x1b, 6): 136, (0x1c, 1): 137, (0x1c, 6): 138,
    (0x1d, 1): 139, (0x1d, 2): 140, (0x1e, 1): 141, (0x1e, 2): 142, (0x1f, 0xa): 143, (0x1f, 5): 144,
    (0x20, 1): 145, (0x20, 6): 146,
    (0x02, 1): 9, (0x02, 2): 10, (0x02, 3): 13, (0x02, 4): 14,
    (0x09, 1): 16, (0x09, 2): 17, (0x09, 3): 20, (0x09, 4): 21,
    (0x0a, 1): 70, (0x0a, 2): 71, (0x0a, 3): 74, (0x0a, 4): 75, (0x0a, 5): 76,
    (0x0b, 1): 37, (0x0b, 2): 38, (0x0b, 3): 41, (0x0b, 4): 42, (0x0b, 6): 43, (0x0c, 1): 44, (0x0c, 6): 50,
    (0x0f, 5): 122, (0x12, 1): 77, (0x12, 2): 78, (0x12, 5): 83, (0x14, 3): 98, (0x14, 4): 99, (0x14, 5): 100,
    (0x15, 1): 91, (0x15, 5): 97, (0x17, 5): 103, (0x19, 5): 109,
}
VK_BC7, VK_BC7_SRGB, VK_BC5, VK_BC4, VK_BC1_RGB_SRGB, VK_BC1_RGB = 145, 146, 141, 139, 132, 131
# BNTX target (BRTI+0x5C) -> VkImageViewType
VIEW = {0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 8: 6}


def fmt_name(f):
    t = FMT.get(f >> 8)
    return "%s_%s" % (t[0] if t else "fmt%02x" % (f >> 8), VARIANT.get(f & 0xff, "v%02x" % (f & 0xff)))


def is_astc(f):
    return 0x2d <= (f >> 8) <= 0x3a


def is_bc(f):
    return 0x1a <= (f >> 8) <= 0x20


def hash64(*parts):
    h = hashlib.blake2b(digest_size=8)
    for p in parts:
        h.update(p)
    return struct.unpack("<Q", h.digest())[0]


# ---- BNTX (layout: work/assets/formats.md) ----------------------------------------------------------------------------
class Tex:
    pass


def _bstr(b, o):
    n = struct.unpack_from("<H", b, o)[0]
    return b[o + 2:o + 2 + n].decode("utf-8", "replace")


def parse_bntx(b):
    """File header, the 'Ounc' block and every BRTI. Offsets in the file are relative to the BNTX's start."""
    if b[:4] != b"BNTX":
        raise ValueError("not a BNTX")
    h = {"version": struct.unpack_from("<I", b, 8)[0]}
    h["rlt_off"], h["file_size"] = struct.unpack_from("<II", b, 0x18)
    cnt, = struct.unpack_from("<I", b, 0x24)
    tbl, brtd = struct.unpack_from("<QQ", b, 0x28)
    h["brtd"] = brtd
    texs = []
    for i in range(cnt):
        o, = struct.unpack_from("<Q", b, tbl + 8 * i)
        if b[o:o + 4] != b"BRTI":
            raise ValueError("BRTI %d at %#x" % (i, o))
        t = Tex()
        t.off = o
        t.flags, t.dim, t.tile_mode, t.swizzle, t.mips, t.samples, t.format, t.access = struct.unpack_from("<BBHHHIII", b, o + 0x10)
        t.width, t.height, t.depth, t.layers, t.bh_log2, t.layout2 = struct.unpack_from("<6I", b, o + 0x24)
        t.image_size, t.alignment = struct.unpack_from("<II", b, o + 0x50)
        t.comp_sel = tuple(b[o + 0x58:o + 0x5c])
        t.target = b[o + 0x5c]
        name, _parent, mipp = struct.unpack_from("<3Q", b, o + 0x60)
        t.name = _bstr(b, name)
        t.mip_offsets = list(struct.unpack_from("<%dQ" % t.mips, b, mipp)) if t.mips else []
        texs.append(t)
    h["textures"] = texs
    return h


def mip_block_height(rows, bh_log2):
    """A mip's block height (log2 GOBs): halved while the mip's rows fit in half a block (host/nvn_layout.c adj_tile)"""
    while bh_log2 > 0 and rows <= (8 << bh_log2) // 2:
        bh_log2 -= 1
    return bh_log2


def surface_layout(t):
    """([(offset in the layer, swizzled size, width in blocks, height in blocks, block height log2)] per mip, layer stride)"""
    _n, bw, bhp, bpb = FMT[t.format >> 8]
    out, off = [], 0
    for m in range(t.mips):
        w, h = max(1, t.width >> m), max(1, t.height >> m)
        wb, hb = -(-w // bw), -(-h // bhp)
        mbh = mip_block_height(hb, t.bh_log2)
        size = -(-(wb * bpb) // 64) * -(-hb // (8 << mbh)) * (512 << mbh)
        out.append((off, size, wb, hb, mbh))
        off += size
    if t.layers > 1 and out:
        a = 512 << out[0][4]
        off = -(-off // a) * a
    return out, off


# ---- romfs files: zstd with the game's dictionaries, SARC ------------------------------------------------------------
_DICTS = {}


def load_dicts(romfs):
    """The zstd dictionaries of Pack/ZsDic.pack.zs (a zstd SARC of raw dictionaries, keyed by their id)"""
    import zstandard
    _DICTS.clear()
    p = os.path.join(romfs, "Pack", "ZsDic.pack.zs")
    if not os.path.exists(p):
        return
    for _k, v in sarc_unpack(zstd_decompress(open(p, "rb").read())).items():
        if v[:4] == b"\x37\xa4\x30\xec":
            _DICTS[struct.unpack_from("<I", v, 4)[0]] = zstandard.ZstdCompressionDict(v)


def zstd_decompress(data):
    import zstandard
    if data[:4] != b"\x28\xb5\x2f\xfd":
        return data
    i = zstandard.get_frame_parameters(data).dict_id
    if i and i not in _DICTS:
        raise ValueError("zstd dictionary %#x missing" % i)
    d = zstandard.ZstdDecompressor(dict_data=_DICTS[i]) if i else zstandard.ZstdDecompressor()
    return d.decompress(data, max_output_size=1 << 32)


def sarc_unpack(data):
    if data[:4] != b"SARC":
        raise ValueError("not a SARC")
    _hdr, _bom, _size, doff = struct.unpack_from("<HHII", data, 4)
    n = struct.unpack_from("<H", data, 0x14 + 6)[0]
    nodes = [struct.unpack_from("<IIII", data, 0x14 + 12 + 16 * i) for i in range(n)]
    sfnt = 0x14 + 12 + 16 * n
    out = {}
    for h, attr, s, e in nodes:
        name = data[sfnt + 8 + (attr & 0xFFFF) * 4:].split(b"\0", 1)[0].decode() if attr >> 24 else "%08x" % h
        out[name] = data[doff + s:doff + e]
    return out


def find_bntx(data, where, depth=0):
    """[(path, BNTX bytes)] in a romfs file: itself, inside zstd, inside SARCs (to 4 levels), or embedded at an 8-aligned
    offset (path@0xOFF) as the inventory looked for them"""
    try:
        b = zstd_decompress(data)
    except Exception:
        return []
    if b[:4] == b"SARC" and depth < 4:
        out = []
        try:
            for k, v in sarc_unpack(b).items():
                out += find_bntx(v, where + "::" + k, depth + 1)
            return out
        except Exception:
            pass
    out = []
    o = b.find(b"BNTX")
    while o >= 0:
        if o % 8 == 0 and b[o + 8:o + 12] == b"\x01\x01\x04\x00":
            size = struct.unpack_from("<I", b, o + 0x1c)[0]
            out.append((where if o == 0 else "%s@%#x" % (where, o), b[o:o + size] if (o or size != len(b)) else b))
            o = b.find(b"BNTX", o + max(4, size))
        else:
            o = b.find(b"BNTX", o + 4)
    return out


# ---- the native half (nx2/texconv.cpp) ---------------------------------------------------------------------------------
def default_bc7enc():
    return os.environ.get("HOOKSHOT_BC7ENC") or os.path.join(ROOT, "work", "deps", "bc7enc_rdo")


def default_ispc():
    p = os.environ.get("HOOKSHOT_ISPC")
    if p:
        return p
    d = os.path.join(ROOT, "work", "deps", "ispc")
    if os.path.isdir(d):
        for sub in sorted(os.listdir(d), reverse=True):
            c = os.path.join(d, sub, "bin", "ispc")
            if os.path.exists(c):
                return c
    import shutil
    return shutil.which("ispc")


def build_native(tool_dir, encoders=True, bc7enc=None, ispc=None, cxx=None, quiet=False):
    """Build (once per source and options) and return the path of the texconv library. encoders=False: the layout and
    ASTC scan only, which need no third-party source."""
    import shutil
    src = os.path.join(HERE, "texconv.cpp")
    cxx = cxx or os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
    if not cxx:
        raise RuntimeError("no C++ compiler (CXX, clang++, g++)")
    bc7enc = bc7enc or default_bc7enc()
    key = hashlib.sha256(open(src, "rb").read())
    if encoders:
        for f in ("rgbcx.cpp", "bc7decomp.cpp", "bc7e.ispc"):
            if not os.path.exists(os.path.join(bc7enc, f)):
                raise RuntimeError("bc7enc_rdo sources not found in %s (--bc7enc)" % bc7enc)
            key.update(open(os.path.join(bc7enc, f), "rb").read())
    key.update(repr((encoders, cxx)).encode())
    ext = ".dll" if os.name == "nt" else ".so"
    lib = os.path.join(tool_dir, "texconv-%s%s" % (key.hexdigest()[:16], ext))
    if os.path.exists(lib):
        return lib
    os.makedirs(tool_dir, exist_ok=True)
    objs, defs = [], []
    if encoders:
        ispc = ispc or default_ispc()
        if not ispc or not os.path.exists(ispc):
            raise RuntimeError("ISPC not found (--ispc, HOOKSHOT_ISPC)")
        ispc_out = os.path.join(tool_dir, "bc7e.o")
        cmd = [ispc, "-O2", os.path.join(bc7enc, "bc7e.ispc"), "-o", ispc_out, "-h", os.path.join(tool_dir, "bc7e_ispc.h"),
               "--target=sse2,sse4,avx,avx2", "--opt=fast-math", "--opt=disable-assertions", "--pic", "--woff"]
        subprocess.run(cmd, check=True, capture_output=quiet)
        objs = [os.path.join(tool_dir, f) for f in sorted(os.listdir(tool_dir)) if f.startswith("bc7e") and f.endswith(".o")]
        objs += [os.path.join(bc7enc, "rgbcx.cpp"), os.path.join(bc7enc, "bc7decomp.cpp")]
        defs = ["-DHK_BC7ENC", "-I" + tool_dir, "-I" + bc7enc]
    tmp = lib + ".tmp"
    cmd = [cxx, "-O2", "-std=c++17", "-shared", "-fPIC", "-fvisibility=hidden", *defs, src, *objs, "-o", tmp]
    subprocess.run(cmd, check=True, capture_output=quiet)
    os.replace(tmp, lib)
    return lib


class Native:
    def __init__(self, path):
        L = self.L = ctypes.CDLL(path)
        vp, u8p = ctypes.c_void_p, ctypes.c_void_p
        L.hk_version.restype = ctypes.c_int
        L.hk_has_encoders.restype = ctypes.c_int
        L.hk_block_linear.argtypes = [vp, ctypes.c_uint64, vp, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int]
        L.hk_block_linear.restype = None
        L.hk_block_linear_size.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
        L.hk_block_linear_size.restype = ctypes.c_uint64
        L.hk_astc_scan.argtypes = [u8p, ctypes.c_uint64, vp]
        L.hk_astc_scan.restype = None
        L.hk_encode.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_uint32, vp, vp, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.hk_encode.restype = ctypes.c_int
        L.hk_decode.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_uint32, vp, vp]
        L.hk_decode.restype = ctypes.c_int
        self.encoders = bool(L.hk_has_encoders())

    def to_linear(self, src, off, size, width_bytes, rows, bh_log2):
        """One block-linear surface at src[off:off+size] (src: a numpy uint8 array) -> linear bytes"""
        import numpy as np
        out = np.empty(width_bytes * rows, np.uint8)
        size = max(0, min(size, len(src) - off))
        self.L.hk_block_linear(src.ctypes.data + off, size, out.ctypes.data, width_bytes, rows, bh_log2, 1)
        return out

    def to_block_linear(self, lin, width_bytes, rows, bh_log2):
        import numpy as np
        lin = np.ascontiguousarray(np.frombuffer(lin, np.uint8) if isinstance(lin, (bytes, bytearray)) else lin)
        out = np.zeros(self.L.hk_block_linear_size(width_bytes, rows, bh_log2), np.uint8)
        self.L.hk_block_linear(out.ctypes.data, len(out), lin.ctypes.data, width_bytes, rows, bh_log2, 0)
        return out

    def astc_scan(self, lin):
        counts = (ctypes.c_uint64 * 4)()
        self.L.hk_astc_scan(lin.ctypes.data, len(lin) // 16, ctypes.addressof(counts))
        return list(counts)

    def encode(self, kind, rgba, c0=0, c1=1, level=4, radius=5):
        """rgba: (h, w, 4) uint8 -> BC blocks (numpy uint8)"""
        import numpy as np
        h, w = rgba.shape[:2]
        rgba = np.ascontiguousarray(rgba)
        k = {"bc1": 1, "bc4": 4, "bc5": 5, "bc7": 7}[kind]
        out = np.empty(-(-w // 4) * -(-h // 4) * (8 if k in (1, 4) else 16), np.uint8)
        if self.L.hk_encode(k, w, h, rgba.ctypes.data, out.ctypes.data, c0, c1, level if k == 7 else -1, radius):
            raise RuntimeError("texconv built without encoders")
        return out

    def decode(self, kind, blocks, w, h):
        import numpy as np
        k = {"bc1": 1, "bc4": 4, "bc5": 5, "bc7": 7}[kind]
        blocks = np.ascontiguousarray(np.frombuffer(blocks, np.uint8) if isinstance(blocks, (bytes, bytearray)) else blocks)
        out = np.empty((h, w, 4), np.uint8)
        if self.L.hk_decode(k, w, h, blocks.ctypes.data, out.ctypes.data):
            raise RuntimeError("texconv built without decoders")
        return out


# ---- the target of a texture ------------------------------------------------------------------------------------------
def used_channels(comp_sel):
    """The source channels (0..3 = R..A) a BNTX swizzle reads"""
    return sorted({c - 2 for c in comp_sel if 2 <= c <= 5})


def choose_target(fmt, comp_sel, bc1_opaque=False, alpha_opaque=None):
    """What the converter makes of a texture: dict(kind, vk, enc, c0, c1, map, dropped, used, fill, force_alpha), or
    kind 'none' with why. alpha_opaque: whether every texel's alpha is 255 (only asked for --bc1-opaque)."""
    hi, var = fmt >> 8, fmt & 0xff
    ident = [SW_R, SW_G, SW_B, SW_A]
    if hi not in FMT:
        return dict(kind="none", why="format %s" % fmt_name(fmt))
    if not is_astc(fmt):
        vk = VK_KEPT.get((hi, var))
        if vk is None:
            return dict(kind="none", why="format %s" % fmt_name(fmt))
        return dict(kind="copy_bc" if is_bc(fmt) else "copy", vk=vk, enc=None, map=ident, dropped=0, used=[0, 1, 2, 3])
    srgb = var == 6
    used = used_channels(comp_sel)
    dropped = sum(1 << c for c in range(4) if c not in used)
    if not srgb and len(used) <= 1:
        c0 = used[0] if used else 0
        return dict(kind="bc4", vk=VK_BC4, enc="bc4", c0=c0, c1=c0, map=[SW_R] * 4, dropped=sum(1 << c for c in range(4) if c != c0), used=used)
    if not srgb and len(used) == 2:
        c0, c1 = used
        m = [SW_R] * 4
        m[c1] = SW_G
        return dict(kind="bc5", vk=VK_BC5, enc="bc5", c0=c0, c1=c1, map=m, dropped=dropped, used=used)
    colour = [c for c in used if c < 3]
    alpha_unused = 3 not in used
    if bc1_opaque and srgb and len(colour) >= 2 and (alpha_unused or alpha_opaque):
        return dict(kind="bc1", vk=VK_BC1_RGB_SRGB, enc="bc1", map=[SW_R, SW_G, SW_B, SW_ONE], dropped=dropped | 8,
                    used=used, fill=colour[0], force_alpha=True)
    return dict(kind="bc7", vk=VK_BC7_SRGB if srgb else VK_BC7, enc="bc7", map=ident, dropped=dropped, used=used,
                fill=colour[0] if colour else None, force_alpha=alpha_unused)


def prepare_input(rgba, plan):
    """The encoder's input: unused colour channels filled with the first used one, alpha 255 when it is not read"""
    if plan["enc"] not in ("bc7", "bc1"):
        return rgba
    x = rgba
    used = plan["used"]
    for c in range(3):
        if c not in used:
            if x is rgba:
                x = rgba.copy()
            x[..., c] = rgba[..., plan["fill"]] if plan.get("fill") is not None else 0
    if plan.get("force_alpha"):
        if x is rgba:
            x = rgba.copy()
        x[..., 3] = 255
    return x


def compared_channels(plan):
    """[(source channel, decoded channel)] the quality check compares"""
    if plan["enc"] == "bc4":
        return [(plan["c0"], 0)]
    if plan["enc"] == "bc5":
        return [(plan["c0"], 0), (plan["c1"], 1)]
    return [(c, c) for c in plan["used"]]


def frames_of(raw_len, mip_ranges, frame_max=1 << 20, small=64 << 10):
    """[(raw offset, size)] of the frames: each mip of `small` bytes or more in equal parts of at most frame_max (16-byte
    aligned), then the tail of smaller mips together, in parts of at most frame_max"""
    out, tail = [], None
    for off, size in mip_ranges:
        if size >= small and tail is None:
            n = -(-size // frame_max)
            per = -(-size // n)
            part = -(-per // 16) * 16
            for a in range(off, off + size, part):
                out.append((a, min(part, off + size - a)))
        else:
            tail = off if tail is None else tail
    if tail is not None:
        for a in range(tail, raw_len, frame_max):
            out.append((a, min(frame_max, raw_len - a)))
    return out


# ---- one texture ------------------------------------------------------------------------------------------------------
def astc_decode(lin, w, h, bw, bh):
    import numpy as np
    import texture2ddecoder
    W, H = -(-w // bw) * bw, -(-h // bh) * bh
    out = np.frombuffer(texture2ddecoder.decode_astc(lin.tobytes(), W, H, bw, bh), np.uint8).reshape(H, W, 4)
    return out[:h, :w][..., [2, 1, 0, 3]]   # (BGRA)


def images(nat, arr, t):
    """(mip, layer, w, h, linear bytes) of every image, mips outer"""
    lay, stride = surface_layout(t)
    _n, bw, bhp, bpb = FMT[t.format >> 8]
    base = t.mip_offsets[0]
    for m, (off, size, wb, hb, mbh) in enumerate(lay):
        for layer in range(t.layers):
            yield m, layer, max(1, t.width >> m), max(1, t.height >> m), nat.to_linear(arr, base + layer * stride + off, size, wb * bpb, hb, mbh)


def convert_texture(nat, arr, b, t, opts, cctx):
    """Everything the index needs of one texture, and its compressed frames: dict(rec, frames=[(raw_off, raw_size, data)],
    mips=[(raw_off, layer_bytes)])"""
    import numpy as np
    rec = dict(name=t.name, nvn_format=t.format, comp_sel=list(t.comp_sel), target=t.target, width=t.width,
               height=t.height, depth=t.depth, layers=t.layers, mips=t.mips, bh_log2=t.bh_log2, brti_off=t.off,
               image_size=t.image_size, alignment=t.alignment, data_off=t.mip_offsets[0] if t.mips else 0,
               vk_format=0, kind="none", status="unsupported", map=[SW_R, SW_G, SW_B, SW_A], dropped=0, flags=0,
               psnr=0.0, psnr_min=0.0, hdr_blocks=0, raw_size=0, view=VIEW.get(t.target, 1), why="")
    if t.bh_log2 == 1:
        rec["flags"] |= FLAG_BH1
    end = rec["data_off"] + t.image_size
    rec["tex_hash"] = hash64(b[t.off + 0x10:t.off + 0x60], b[rec["data_off"]:end])
    out = dict(rec=rec, frames=[], mips=[])
    if t.format >> 8 not in FMT or not t.mips:
        rec["why"] = "format %s" % fmt_name(t.format)
        return out
    if t.depth != 1 or t.target not in VIEW or t.samples > 1:
        rec["why"] = "shape: depth %d target %d samples %d" % (t.depth, t.target, t.samples)
        return out
    lay, stride = surface_layout(t)
    if stride * t.layers != t.image_size or end > len(b):
        rec["why"] = "layout: %d x %d != %d" % (stride, t.layers, t.image_size)
        return out
    plan = choose_target(t.format, t.comp_sel, opts.bc1_opaque)
    if plan["kind"] == "none":
        rec["why"] = plan["why"]
        return out
    if plan["enc"] and not nat.encoders:
        rec["why"] = "no encoders (--kept-only)"
        return out
    _n, bw, bhp, bpb = FMT[t.format >> 8]
    if plan["enc"] and opts.bc1_opaque and plan["kind"] == "bc7" and t.format & 0xff == 6 and 3 in plan["used"]:
        opaque = all((astc_decode(lin, w, h, bw, bhp)[..., 3] == 255).all() for _m, _l, w, h, lin in images(nat, arr, t))
        plan = choose_target(t.format, t.comp_sel, True, opaque)
    rec.update(kind=plan["kind"], vk_format=plan["vk"], map=plan["map"], dropped=plan["dropped"])
    if plan.get("force_alpha"):
        rec["flags"] |= FLAG_ALPHA_FORCED
    parts, mips = [], []
    raw_off = 0
    se, n_vals, worst, hdr = 0.0, 0, 99.0, [0, 0, 0, 0]
    pairs = compared_channels(plan) if plan["enc"] else []
    cur_mip, mip_start, layer_bytes = -1, 0, 0
    for m, layer, w, h, lin in images(nat, arr, t):
        if plan["enc"]:
            for i, v in enumerate(nat.astc_scan(lin)):
                hdr[i] += v
            src = astc_decode(lin, w, h, bw, bhp)
            blk = nat.encode(plan["enc"], prepare_input(src, plan), plan.get("c0", 0), plan.get("c1", 1), opts.bc7_level, opts.bc45_radius)
            dec = nat.decode(plan["enc"], blk, w, h)
            ise = 0.0
            for sc, dc in pairs:
                d = dec[..., dc].astype(np.int32) - src[..., sc]
                ise += float(np.dot(d.ravel(), d.ravel()))
            se += ise
            n_vals += w * h * len(pairs)
            if w * h >= 4096 and pairs:
                worst = min(worst, psnr_of(ise, w * h * len(pairs)))
            data = blk
        else:
            data = lin
        if m != cur_mip:
            if raw_off % MIP_ALIGN:   # (a mip starts at a multiple of 4: vkCmdCopyBufferToImage on a transfer queue)
                parts.append(bytes(MIP_ALIGN - raw_off % MIP_ALIGN))
                raw_off += MIP_ALIGN - raw_off % MIP_ALIGN
            cur_mip, mip_start, layer_bytes = m, raw_off, len(data)
            mips.append((mip_start, layer_bytes))
        parts.append(data.tobytes())
        raw_off += len(data)
    raw = b"".join(parts)
    del parts
    rec["raw_size"] = len(raw)
    if plan["enc"]:
        rec["psnr"] = psnr_of(se, n_vals) if n_vals else 99.0
        rec["psnr_min"] = worst
        rec["hdr_blocks"] = hdr[0] + hdr[1]
        rec["astc_scan"] = hdr
        if rec["hdr_blocks"]:
            rec["flags"] |= FLAG_ASTC_HDR
        if rec["psnr"] < opts.min_psnr:
            rec.update(status="refused", kind="none", vk_format=0, raw_size=0,
                       why="PSNR %.2f < %.1f (as %s)" % (rec["psnr"], opts.min_psnr, plan["kind"]), refused_kind=plan["kind"])
            return out
    rec["status"] = "ok"
    out["mips"] = mips
    starts = [o for o, _n in mips] + [len(raw)]
    ranges = [(starts[i], starts[i + 1] - starts[i]) for i in range(len(mips))]   # (with a mip's padding)
    for a, n in frames_of(len(raw), ranges, opts.frame_kb << 10):
        out["frames"].append((a, n, cctx.compress(raw[a:a + n])))
    return out


def psnr_of(se, n):
    if n == 0:
        return 99.0
    mse = se / n
    return 99.0 if mse <= 0 else min(99.0, 10 * math.log10(255.0 * 255.0 / mse))


def convert_bntx(nat, b, path, opts, cctx):
    import numpy as np
    h = parse_bntx(b)
    arr = np.frombuffer(b, np.uint8)
    head_len = h["brtd"] + 0x10
    rlt = h["rlt_off"] if 0 < h["rlt_off"] <= len(b) else len(b)
    meta = b[:head_len] + b[rlt:]
    e = dict(path=path, bntx_size=len(b), head_len=head_len, rlt_off=rlt, meta=cctx.compress(meta), meta_size=len(meta),
             flags=1 if ("::" in path or "@" in path) else 0, textures=[])
    for i, t in enumerate(h["textures"]):
        try:
            r = convert_texture(nat, arr, b, t, opts, cctx)
        except Exception as ex:   # (one bad texture keeps the runtime path; the rest of the file is still prepared)
            r = dict(rec=dict(name=t.name, nvn_format=t.format, status="error", kind="none", why=repr(ex)[:200]), frames=[], mips=[])
        r["rec"]["index"] = i
        e["textures"].append(r)
    return e


# ---- workers ----------------------------------------------------------------------------------------------------------
_W = {}


def worker_init(lib, romfs, opts, nice):
    if nice and hasattr(os, "nice"):
        try:
            os.nice(nice)
        except OSError:
            pass
    import zstandard
    _W["nat"] = Native(lib)
    _W["opts"] = opts
    _W["romfs"] = romfs
    _W["cctx"] = zstandard.ZstdCompressor(level=opts.zstd_level, write_checksum=True, write_content_size=True)
    load_dicts(romfs)


def work_file(job):
    """One romfs file: (path, size, mtime, old hash) -> its BNTX entries, or 'unchanged'"""
    path, size, mtime, old_hash = job
    t0 = time.perf_counter()
    c0 = time.process_time()
    try:
        data = open(os.path.join(_W["romfs"], path), "rb").read()
        hsh = hash64(data)
        if old_hash is not None and hsh == old_hash:
            return dict(path=path, size=size, mtime=mtime, hash=hsh, unchanged=True)
        entries = [convert_bntx(_W["nat"], b, sub, _W["opts"], _W["cctx"]) for sub, b in find_bntx(data, path)]
        return dict(path=path, size=size, mtime=mtime, hash=hsh, bntx=entries, secs=time.perf_counter() - t0,
                    cpu=time.process_time() - c0)
    except Exception as ex:
        return dict(path=path, size=size, mtime=mtime, hash=None, error=repr(ex)[:300], bntx=[])


# ---- the store --------------------------------------------------------------------------------------------------------
IDX_MAGIC, PAK_MAGIC = b"HKTXIDX1", b"HKTXPAK1"
FILE_REC, TEX_REC, MIP_REC, FRAME_REC = 64, 128, 16, 32
FILE_FMT = "<IIQQQQIIIIII"
TEX_FMT = "<IIIIIIBBBB4B4BBBHIIIIIIIIQQIIIffII12x"
MIP_FMT = "<QII"
FRAME_FMT = "<HHIQQII"
assert struct.calcsize(FILE_FMT) == FILE_REC and struct.calcsize(TEX_FMT) == TEX_REC
assert struct.calcsize(MIP_FMT) == MIP_REC and struct.calcsize(FRAME_FMT) == FRAME_REC


def pack_name(i):
    return "pack_%03d.bin" % i


class Packs:
    """Append-only packs of zstd frames"""
    def __init__(self, out, limit):
        self.out, self.limit, self.f, self.id = out, limit, None, -1
        ids = [int(n[5:8]) for n in os.listdir(out) if n.startswith("pack_") and n.endswith(".bin")]
        self.count = max(ids) + 1 if ids else 0
        if ids:
            self._open(max(ids))

    def _open(self, i):
        if self.f:
            self.f.close()
        p = os.path.join(self.out, pack_name(i))
        new = not os.path.exists(p)
        self.f = open(p, "ab")
        if new:
            self.f.write(PAK_MAGIC + struct.pack("<II", FORMAT_VERSION, i) + bytes(48))
        self.id = i
        self.count = max(self.count, i + 1)

    def put(self, data):
        if self.f is None or (self.f.tell() + len(data) > self.limit and self.f.tell() > 64):
            self._open(self.count if self.f is not None else 0)
        off = self.f.tell()
        self.f.write(data)
        return self.id, off

    def sync(self):
        if self.f:
            self.f.flush()
            os.fsync(self.f.fileno())

    def close(self):
        if self.f:
            self.sync()
            self.f.close()
            self.f = None


def write_index(path, cfg_hash, entries, n_packs):
    """entries: [file entry] with 'textures': [dict(rec, frames=[(pack, off, csize, raw_off, raw_size)], mips)] and
    'meta_frame': (pack, off, csize, raw_off, raw_size)"""
    strings = bytearray()
    strtab = {}

    def s(x):
        e = x.encode("utf-8")
        if e not in strtab:
            strtab[e] = len(strings)
            strings.extend(e)
        return strtab[e], len(e)
    files, texs, mips, frames = bytearray(), bytearray(), bytearray(), bytearray()
    n_tex = n_mip = n_frame = 0
    for e in entries:
        po, pl = s(e["path"])
        mf = n_frame
        frames += struct.pack(FRAME_FMT, *e["meta_frame"][:1], 0, e["meta_frame"][2], e["meta_frame"][1], e["meta_frame"][3], e["meta_frame"][4], 0)
        n_frame += 1
        files += struct.pack(FILE_FMT, po, pl, e["src_size"], e["src_hash"], e["mtime"], e["bntx_size"], n_tex,
                             len(e["textures"]), mf, e["head_len"], e["rlt_off"], e.get("flags", 0))
        fi = len(files) // FILE_REC - 1
        for t in e["textures"]:
            r = t["rec"]
            no, nl = s(r.get("name", ""))
            mip_first = n_mip
            for mo, lb in t["mips"]:
                mips += struct.pack(MIP_FMT, mo, lb, 0)
                n_mip += 1
            fr_first, fr_n = n_frame, len(t["frames"])   # (shared frames are listed again: the records are small)
            for pk, off, cs, ro, rs in t["frames"]:
                frames += struct.pack(FRAME_FMT, pk, 0, cs, off, ro, rs, 0)
                n_frame += 1
            cs4 = (list(r.get("comp_sel", [2, 3, 4, 5])) + [0] * 4)[:4]
            mp4 = (list(r.get("map", [3, 4, 5, 6])) + [0] * 4)[:4]
            texs += struct.pack(TEX_FMT, fi, r.get("index", 0), no, nl, r.get("nvn_format", 0), r.get("vk_format", 0),
                                KIND[r.get("kind", "none")], STATUS[r.get("status", "error")], r.get("view", 1), r.get("target", 0),
                                *mp4, *cs4, r.get("dropped", 0), r.get("bh_log2", 0), r.get("flags", 0),
                                r.get("width", 0), r.get("height", 0), r.get("depth", 0), r.get("layers", 0), r.get("mips", 0),
                                mip_first, fr_first, fr_n, r.get("raw_size", 0), r.get("tex_hash", 0), r.get("brti_off", 0),
                                r.get("image_size", 0), r.get("data_off", 0), r.get("psnr", 0.0), r.get("psnr_min", 0.0),
                                r.get("alignment", 0), min(r.get("hdr_blocks", 0), 0xFFFFFFFF))
            n_tex += 1
    n_files = len(files) // FILE_REC
    o_files = 128
    o_tex = o_files + len(files)
    o_mips = o_tex + len(texs)
    o_frames = o_mips + len(mips)
    o_str = o_frames + len(frames)
    hdr = IDX_MAGIC + struct.pack("<IIQIIIIIIQQQQQHHHH", FORMAT_VERSION, 128, cfg_hash, n_files, n_tex, n_mip, n_frame,
                                  n_packs, len(strings), o_files, o_tex, o_mips, o_frames, o_str,
                                  FILE_REC, TEX_REC, MIP_REC, FRAME_REC)
    hdr += bytes(128 - len(hdr))
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(hdr)
        f.write(files)
        f.write(texs)
        f.write(mips)
        f.write(frames)
        f.write(strings)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def read_index(path):
    """-> (config hash, [file entry] as write_index takes them, frames as (pack, off, csize, raw_off, raw_size))"""
    b = open(path, "rb").read()
    if b[:8] != IDX_MAGIC:
        raise ValueError("not a texture index")
    (ver, hsize, cfg, n_files, n_tex, n_mip, n_frame, n_packs, n_str, o_files, o_tex, o_mips, o_frames, o_str,
     fr, tr, mr, frr) = struct.unpack_from("<IIQIIIIIIQQQQQHHHH", b, 8)
    if ver != FORMAT_VERSION:
        raise ValueError("index version %d, this tool writes %d" % (ver, FORMAT_VERSION))
    st = b[o_str:o_str + n_str]
    frames = [struct.unpack_from(FRAME_FMT, b, o_frames + i * frr) for i in range(n_frame)]
    frames = [(f[0], f[3], f[2], f[4], f[5]) for f in frames]
    mips = [struct.unpack_from(MIP_FMT, b, o_mips + i * mr)[:2] for i in range(n_mip)]
    texs = []
    for i in range(n_tex):
        v = struct.unpack_from(TEX_FMT, b, o_tex + i * tr)
        (fi, idx, no, nl, nvn, vk, kind, status, view, target, m0, m1, m2, m3, c0, c1, c2, c3, dropped, bh, flags, w, h, d,
         layers, nm, mip_first, fr_first, fr_n, raw, th, brti, isz, doff, psnr, pmin, align, hdr) = v
        rec = dict(index=idx, name=st[no:no + nl].decode("utf-8"), nvn_format=nvn, vk_format=vk, kind=KIND_NAME[kind],
                   status=STATUS_NAME[status], view=view, target=target, map=[m0, m1, m2, m3], comp_sel=[c0, c1, c2, c3],
                   dropped=dropped, bh_log2=bh, flags=flags, width=w, height=h, depth=d, layers=layers, mips=nm,
                   raw_size=raw, tex_hash=th, brti_off=brti, image_size=isz, data_off=doff, psnr=psnr, psnr_min=pmin,
                   alignment=align, hdr_blocks=hdr)
        texs.append((fi, dict(rec=rec, mips=mips[mip_first:mip_first + nm] if status == 0 else [],
                              frames=frames[fr_first:fr_first + fr_n], frame_span=(fr_first, fr_n))))
    entries = []
    for i in range(n_files):
        po, pl, size, hsh, mtime, bsize, tf, tn, mf, hl, rlt, flags = struct.unpack_from(FILE_FMT, b, o_files + i * fr)
        entries.append(dict(path=st[po:po + pl].decode("utf-8"), src_size=size, src_hash=hsh, mtime=mtime, bntx_size=bsize,
                            head_len=hl, rlt_off=rlt, flags=flags, meta_frame=frames[mf],
                            textures=[t for _fi, t in texs[tf:tf + tn]]))
    return cfg, entries, n_packs


def read_frames(out, frames, raw_size):
    """The raw data of a texture from its frames"""
    import zstandard
    d = zstandard.ZstdDecompressor()
    raw = bytearray(raw_size)
    handles = {}
    try:
        for pk, off, cs, ro, rs in frames:
            f = handles.get(pk) or handles.setdefault(pk, open(os.path.join(out, pack_name(pk)), "rb"))
            f.seek(off)
            x = d.decompress(f.read(cs), max_output_size=rs)
            if len(x) != rs:
                raise ValueError("frame at %d:%#x: %d bytes, record says %d" % (pk, off, len(x), rs))
            raw[ro:ro + rs] = x
    finally:
        for f in handles.values():
            f.close()
    return bytes(raw)


# ---- the run ----------------------------------------------------------------------------------------------------------
def default_romfs():
    p = os.path.join(ROOT, "work", "dump.txt")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8"):
            line = line.strip()
            if line and not line.startswith("#"):
                return os.path.join(line, "romfs")
    return None


def config_hash(opts):
    src = open(os.path.join(HERE, "texconv.cpp"), "rb").read()
    cfg = dict(v=CONV_VERSION, bc1=opts.bc1_opaque, floor=opts.min_psnr, bc7=opts.bc7_level, r=opts.bc45_radius,
               z=opts.zstd_level, frame=opts.frame_kb, kept=opts.kept_only, src=hashlib.sha256(src).hexdigest())
    return hash64(json.dumps(cfg, sort_keys=True).encode())


def walk(romfs, only=None, files=None):
    """[(romfs-relative path, size, mtime_ns)] of the files to look at"""
    out = []
    if files:
        for p in files:
            p = p.strip().lstrip("/")
            if not p:
                continue
            try:
                st = os.stat(os.path.join(romfs, p))
                out.append((p, st.st_size, st.st_mtime_ns))
            except OSError:
                pass
        return out
    for d in sorted(os.listdir(romfs)):
        if d in SKIP_DIRS or not os.path.isdir(os.path.join(romfs, d)):
            continue
        for dp, dn, fn in os.walk(os.path.join(romfs, d)):
            dn.sort()
            for f in sorted(fn):
                full = os.path.join(dp, f)
                rel = os.path.relpath(full, romfs).replace(os.sep, "/")
                if only and not any(rel.startswith(o) for o in only):
                    continue
                st = os.stat(full)
                out.append((rel, st.st_size, st.st_mtime_ns))
    return out


def outer_of(path):
    return path.split("::", 1)[0].split("@", 1)[0]


def run(opts):
    import multiprocessing as mp
    t_start = time.time()
    romfs = opts.romfs or default_romfs()
    if not romfs or not os.path.isdir(romfs):
        sys.exit("romfs not found (--romfs, or work/dump.txt)")
    out = opts.out
    os.makedirs(out, exist_ok=True)
    if opts.rebuild:
        for n in os.listdir(out):
            if n.startswith("pack_") or n in ("index.bin", "scan.json"):
                os.remove(os.path.join(out, n))
    lib = build_native(os.path.join(out, "tool"), not opts.kept_only, opts.bc7enc, opts.ispc)
    cfg = config_hash(opts)
    idx_path = os.path.join(out, "index.bin")
    old = {}
    if os.path.exists(idx_path):
        try:
            ocfg, oentries, _n = read_index(idx_path)
            if ocfg == cfg:
                for e in oentries:
                    old.setdefault(outer_of(e["path"]), []).append(e)
            else:
                print("prepare_textures: the options or the converter changed: every texture is made again", flush=True)
        except Exception as ex:
            print("prepare_textures: index unreadable (%s): starting over" % ex, flush=True)
    scan_path = os.path.join(out, "scan.json")
    scan = {}
    if os.path.exists(scan_path):
        try:
            scan = json.load(open(scan_path))
        except Exception:
            scan = {}
    flist = [line for line in open(opts.files, encoding="utf-8")] if opts.files else None
    todo_files = walk(romfs, opts.only, flist)
    walked = {p for p, _s, _m in todo_files}
    keep, jobs = {}, []
    for p, size, mtime in todo_files:
        o = old.get(p)
        if o and o[0]["src_size"] == size and o[0]["mtime"] == mtime and all(records_sound(e) for e in o):
            keep[p] = o
        elif not o and scan.get(p) == [size, mtime]:
            continue
        else:
            jobs.append((p, size, mtime, o[0]["src_hash"] if o and o[0]["src_size"] == size else None))
    for p, o in old.items():   # (files this run did not look at: kept as they are, unless --prune)
        if p not in walked and not opts.prune:
            keep[p] = o
    jobs.sort(key=lambda j: -j[1])
    print("prepare_textures: %d files looked at, %d unchanged, %d to do (%.1f MB), %d workers, romfs %s -> %s" % (
        len(todo_files), len(keep), len(jobs), sum(j[1] for j in jobs) / 1e6, opts.jobs, romfs, out), flush=True)
    packs = Packs(out, opts.pack_mb << 20)
    # (dedup: content key -> the texture dict whose frames are written; reused ones keep their frames)
    seen = {}
    for es in keep.values():
        for e in es:
            for t in e["textures"]:
                if t["frames"]:
                    seen.setdefault(dedup_key(t["rec"]), t)
    done, stats = dict(keep), dict(files=0, tex=0, in_bytes=0, raw=0, comp=0, cpu=0.0, dups=0, errors=[], unchanged=0)
    last_ck = time.time()
    last_pr = time.time()

    def checkpoint():
        packs.sync()
        entries = [e for p in sorted(done) for e in done[p]]
        write_index(idx_path, cfg, entries, packs.count)
        with open(scan_path + ".tmp", "w") as f:
            json.dump(scan, f)
        os.replace(scan_path + ".tmp", scan_path)
    def results():
        if opts.jobs <= 1:   # (in this process: for debugging, and the tests)
            worker_init(lib, romfs, opts, 0)
            yield from map(work_file, jobs)
            return
        with mp.get_context().Pool(opts.jobs, initializer=worker_init, initargs=(lib, romfs, opts, opts.nice),
                                   maxtasksperchild=500) as pool:
            yield from pool.imap_unordered(work_file, jobs)
    for r in results():
        stats["files"] += 1
        p = r["path"]
        if r.get("error"):
            stats["errors"].append("%s: %s" % (p, r["error"]))
            print("prepare_textures: %s: %s" % (p, r["error"]), flush=True)
            continue
        if r.get("unchanged"):
            stats["unchanged"] += 1
            es = old[p]
            for e in es:
                e["mtime"] = r["mtime"]
            done[p] = es
            continue
        stats["in_bytes"] += r["size"]
        stats["cpu"] += r.get("cpu", 0.0)
        if not r["bntx"]:
            scan[p] = [r["size"], r["mtime"]]
            done.pop(p, None)
            continue
        scan.pop(p, None)
        es = []
        for e in r["bntx"]:
            pk, off = packs.put(e["meta"])
            e["meta_frame"] = (pk, off, len(e.pop("meta")), 0, e.pop("meta_size"))
            e.update(src_size=r["size"], src_hash=r["hash"], mtime=r["mtime"])
            for t in e["textures"]:
                stats["tex"] += 1
                rec = t["rec"]
                if rec.get("status") != "ok":
                    continue
                k = dedup_key(rec)
                first = seen.get(k)
                if first is not None and first["frames"]:
                    t["frames"] = first["frames"]
                    rec["flags"] |= FLAG_DEDUP
                    stats["dups"] += 1
                    continue
                fr = []
                for ro, rs, data in t["frames"]:
                    pk, off = packs.put(data)
                    fr.append((pk, off, len(data), ro, rs))
                    stats["comp"] += len(data)
                stats["raw"] += rec["raw_size"]
                t["frames"] = fr
                seen[k] = t
            es.append(e)
        done[p] = es
        now = time.time()
        if now - last_pr > 30:
            last_pr = now
            el = now - t_start
            print("prepare_textures: %d/%d files, %d textures, %.0f MB in -> %.0f MB raw, %.0f MB packed, %.0f s, "
                  "%.0f CPU-s" % (stats["files"], len(jobs), stats["tex"], stats["in_bytes"] / 1e6, stats["raw"] / 1e6,
                                  stats["comp"] / 1e6, el, stats["cpu"]), flush=True)
        if now - last_ck > opts.checkpoint:
            last_ck = now
            checkpoint()
    checkpoint()
    packs.close()
    wall = time.time() - t_start
    entries = [e for p in sorted(done) for e in done[p]]
    rep = report(entries, out)
    rep.insert(0, "run: %.0f s wall, %.0f CPU-s in workers, %d files converted (%.0f MB of romfs), %d unchanged by hash, "
                  "%d textures, %d shared by content, %.0f MB raw written, %.0f MB packed written; %d errors%s" % (
                      wall, stats["cpu"], stats["files"] - stats["unchanged"], stats["in_bytes"] / 1e6, stats["unchanged"],
                      stats["tex"], stats["dups"], stats["raw"] / 1e6, stats["comp"] / 1e6, len(stats["errors"]),
                      "".join("\n  " + x for x in stats["errors"][:20])))
    text = "\n".join(rep) + "\n"
    open(os.path.join(out, "report.txt"), "w").write(text)
    print(text, end="", flush=True)
    stats["code"] = 1 if stats["errors"] else 0
    return stats


def records_sound(e):
    """A kept record still meets the layout rules (else its file is made again): mips start at MIP_ALIGN"""
    return all(mo % MIP_ALIGN == 0 for t in e["textures"] for mo, _lb in t["mips"])


def dedup_key(rec):
    return (rec["tex_hash"], rec["nvn_format"], rec["width"], rec["height"], rec["layers"], rec["mips"], rec["kind"], rec["vk_format"])


def report(entries, out):
    """Counts, sizes and PSNR by target and source format; refusals and the worst textures by target"""
    import collections
    by = collections.defaultdict(lambda: dict(n=0, src=0, raw=0, comp=0, psnr=[]))
    status = collections.Counter()
    why = collections.Counter()
    refused, worst, flags = [], collections.defaultdict(list), collections.Counter()
    hdr = 0
    packed = 0
    for e in entries:
        for t in e["textures"]:
            r = t["rec"]
            status[r["status"]] += 1
            if r["status"] != "ok":
                if r["status"] == "refused":
                    refused.append((r["psnr"], e["path"], r["name"]))
                why[(r["status"], fmt_name(r.get("nvn_format", 0)))] += 1
                continue
            k = (r["kind"], fmt_name(r["nvn_format"]))
            g = by[k]
            g["n"] += 1
            g["src"] += r["image_size"]
            g["raw"] += r["raw_size"]
            if not r["flags"] & FLAG_DEDUP:
                c = sum(f[2] for f in t["frames"])
                g["comp"] += c
            if r["kind"] in ("bc7", "bc5", "bc4", "bc1"):
                g["psnr"].append(r["psnr"])
                worst[r["kind"]].append((r["psnr"], r["psnr_min"], e["path"], r["name"], fmt_name(r["nvn_format"])))
            for bit, nm in ((FLAG_BH1, "bh_log2=1"), (FLAG_ALPHA_FORCED, "alpha forced"), (FLAG_ASTC_HDR, "ASTC HDR blocks"), (FLAG_DEDUP, "shared frames")):
                if r["flags"] & bit:
                    flags[nm] += 1
            hdr += r.get("hdr_blocks", 0)
    for n in os.listdir(out):
        if n.startswith("pack_"):
            packed += os.path.getsize(os.path.join(out, n))
    L = ["store: %d BNTX files, %d textures (%s); packs %.2f GB on disk, index %.1f MB" % (
        len(entries), sum(status.values()), ", ".join("%s %d" % kv for kv in sorted(status.items())), packed / 1e9,
        os.path.getsize(os.path.join(out, "index.bin")) / 1e6 if os.path.exists(os.path.join(out, "index.bin")) else 0)]
    L.append("%-6s %-22s %7s %10s %10s %10s  %s" % ("target", "source", "count", "src MB", "raw MB", "packed MB", "PSNR median / p1 / min"))
    tot = collections.defaultdict(lambda: [0, 0, 0, 0])
    for (kind, src), g in sorted(by.items(), key=lambda kv: (kv[0][0], -kv[1]["src"])):
        p = sorted(g["psnr"])
        ps = "%.1f / %.1f / %.1f" % (p[len(p) // 2], p[len(p) // 100], p[0]) if p else ""
        L.append("%-6s %-22s %7d %10.1f %10.1f %10.1f  %s" % (kind, src, g["n"], g["src"] / 1e6, g["raw"] / 1e6, g["comp"] / 1e6, ps))
        x = tot[kind]
        x[0] += g["n"]; x[1] += g["src"]; x[2] += g["raw"]; x[3] += g["comp"]
    L.append("totals by target:")
    for kind, x in sorted(tot.items()):
        p = sorted(w[0] for w in worst.get(kind, []))
        ps = " PSNR median %.1f p1 %.1f min %.1f" % (p[len(p) // 2], p[len(p) // 100], p[0]) if p else ""
        L.append("  %-8s %7d textures, %9.1f MB source, %9.1f MB raw, %9.1f MB packed%s" % (kind, x[0], x[1] / 1e6, x[2] / 1e6, x[3] / 1e6, ps))
    L.append("flags: " + ", ".join("%s %d" % kv for kv in sorted(flags.items())) + "; ASTC HDR blocks %d" % hdr)
    if why:
        L.append("not prepared: " + ", ".join("%s %s %d" % (s, f, n) for (s, f), n in sorted(why.items())))
    L.append("refused (PSNR below the floor): %d" % len(refused))
    for p, path, name in sorted(refused)[:30]:
        L.append("  %.2f dB %s %s" % (p, path, name))
    for kind, w in sorted(worst.items()):
        L.append("worst %s:" % kind)
        for p, pm, path, name, src in sorted(w)[:8]:
            L.append("  %.2f dB (worst image %.2f) %s %s %s" % (p, pm, src, path, name))
    return L


# ---- verify -----------------------------------------------------------------------------------------------------------
def verify(opts):
    """N random prepared textures: their frames decompressed, sizes checked, the data decoded and compared with the
    source as this tool reads it (kept formats: byte for byte; converted ones: the PSNR recomputed and checked against
    the record)"""
    import numpy as np
    romfs = opts.romfs or default_romfs()
    out = opts.out
    nat = Native(build_native(os.path.join(out, "tool"), not opts.kept_only, opts.bc7enc, opts.ispc))
    load_dicts(romfs)
    _cfg, entries, _n = read_index(os.path.join(out, "index.bin"))
    pool = [(e, t) for e in entries for t in e["textures"] if t["rec"]["status"] == "ok"]
    rng = random.Random(opts.seed)
    pick = rng.sample(pool, min(opts.verify, len(pool)))
    bad = 0
    t0 = time.time()
    for e, t in pick:
        r = t["rec"]
        try:
            raw = read_frames(out, t["frames"], r["raw_size"])
            data = open(os.path.join(romfs, outer_of(e["path"])), "rb").read()
            if hash64(data) != e["src_hash"]:
                raise ValueError("source changed since it was prepared")
            b = dict(find_bntx(data, outer_of(e["path"])))[e["path"]]
            meta = read_frames(out, [e["meta_frame"]], e["meta_frame"][4])
            if meta != b[:e["head_len"]] + b[e["rlt_off"]:]:
                raise ValueError("metadata frame differs")
            h = parse_bntx(b)
            src_t = h["textures"][r["index"]]
            arr = np.frombuffer(b, np.uint8)
            plan = choose_target(src_t.format, src_t.comp_sel, r["kind"] == "bc1", True)
            _n, bw, bhp, _bpb = FMT[src_t.format >> 8]
            se, nv = 0.0, 0
            pos = 0
            for (m, layer, w, h_, lin) in images(nat, arr, src_t):
                mo, lb = t["mips"][m]
                if pos != mo + layer * lb:
                    raise ValueError("mip %d layer %d at %d, record says %d" % (m, layer, pos, mo + layer * lb))
                seg = raw[pos:pos + lb]
                if r["kind"] in ("copy", "copy_bc"):
                    if seg != lin.tobytes():
                        raise ValueError("mip %d layer %d differs from the source" % (m, layer))
                else:
                    src = astc_decode(lin, w, h_, bw, bhp)
                    dec = nat.decode(r["kind"], seg, w, h_)
                    for sc, dc in compared_channels(plan):
                        d = dec[..., dc].astype(np.int32) - src[..., sc]
                        se += float(np.dot(d.ravel(), d.ravel()))
                        nv += w * h_
                pos += lb
                if layer == src_t.layers - 1 and m + 1 < src_t.mips and pos % MIP_ALIGN:
                    pos += MIP_ALIGN - pos % MIP_ALIGN
            if pos != r["raw_size"]:
                raise ValueError("raw size %d, the images add up to %d" % (r["raw_size"], pos))
            msg = "ok"
            if r["kind"] not in ("copy", "copy_bc"):
                p = psnr_of(se, nv) if nv else 99.0
                if abs(p - r["psnr"]) > 0.01:
                    raise ValueError("PSNR %.3f, record says %.3f" % (p, r["psnr"]))
                msg = "ok, %.2f dB" % p
            print("verify: %s %s [%s %s -> %s %dx%dx%d %d mips]: %s" % (e["path"], r["name"], r["kind"], fmt_name(r["nvn_format"]),
                  r["vk_format"], r["width"], r["height"], r["layers"], r["mips"], msg), flush=True)
        except Exception as ex:
            bad += 1
            print("verify: FAIL %s %s: %s" % (e["path"], r["name"], ex), flush=True)
    print("verify: %d of %d textures match (%.0f s)" % (len(pick) - bad, len(pick), time.time() - t0), flush=True)
    return 1 if bad else 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--romfs", help="the romfs folder (default: work/dump.txt's path + /romfs)")
    p.add_argument("--out", default=os.path.join(ROOT, "work", "prepared", "tex"))
    p.add_argument("--jobs", type=int, default=8)
    p.add_argument("--nice", type=int, default=10, help="niceness of the workers (0: not niced)")
    p.add_argument("--files", help="a list of romfs paths, one a line (e.g. a file trace); only these")
    p.add_argument("--only", action="append", help="only romfs paths starting with this (repeatable)")
    p.add_argument("--prune", action="store_true", help="drop records of files this run did not look at")
    p.add_argument("--rebuild", action="store_true", help="delete the store first")
    p.add_argument("--kept-only", action="store_true", help="no encoders (no bc7enc_rdo / ISPC): ASTC stays unprepared")
    p.add_argument("--bc1-opaque", action="store_true", help="BC1 for ASTC sRGB colour with no alpha (off: BC7)")
    p.add_argument("--min-psnr", type=float, default=35.0, help="refuse a conversion below this PSNR (dB)")
    p.add_argument("--bc7-level", type=int, default=4, help="bc7e level, 0 ultrafast .. 6 slowest (4: slow, the -u4 measured)")
    p.add_argument("--bc45-radius", type=int, default=5, help="BC4/BC5 endpoint search radius")
    p.add_argument("--zstd-level", type=int, default=19)
    p.add_argument("--frame-kb", type=int, default=1024)
    p.add_argument("--pack-mb", type=int, default=1024)
    p.add_argument("--checkpoint", type=float, default=120, help="seconds between index checkpoints")
    p.add_argument("--bc7enc", help="bc7enc_rdo sources (default work/deps/bc7enc_rdo, HOOKSHOT_BC7ENC)")
    p.add_argument("--ispc", help="the ISPC compiler (default work/deps/ispc/*/bin/ispc, HOOKSHOT_ISPC)")
    p.add_argument("--verify", type=int, metavar="N", help="check N random prepared textures against the source")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--report", action="store_true", help="print the store's summary")
    opts = p.parse_args(argv)
    if opts.verify:
        return verify(opts)
    if opts.report:
        _c, entries, _n = read_index(os.path.join(opts.out, "index.bin"))
        print("\n".join(report(entries, opts.out)))
        return 0
    if opts.nice and hasattr(os, "nice"):
        try:
            os.nice(opts.nice)
        except OSError:
            pass
        opts.nice = 0   # (the workers inherit it)
    return run(opts)["code"]


if __name__ == "__main__":
    sys.exit(main())
