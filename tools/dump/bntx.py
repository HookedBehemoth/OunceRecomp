"""BNTX (NintendoWare texture container, version 4.1.1, platform 'Ounc' = Switch 2) reader + Tegra block-linear deswizzle.

    python tools/dump/bntx.py <file.bntx[.zs]> [--check]     summary of every texture (--check: deswizzle mip0 and sanity-check blocks)

Layout (offsets verified against the romfs files, see work/assets/formats.md):
  file header   +0x00 'BNTX' +0x08 u32 version 0x00040101  +0x0C BOM FEFF  +0x0E u8 align log2  +0x0F u8 addr bits (0x40)
                +0x10 u32 file-name offset  +0x14 u16 flags  +0x16 u16 first block (_STR) offset  +0x18 u32 _RLT offset
                +0x1C u32 file size
  'Ounc' block  +0x20 char[4] platform  +0x24 u32 texture count  +0x28 u64 -> u64[count] BRTI offsets
                +0x30 u64 -> BRTD  +0x38 u64 -> _DIC  +0x40 u64 memory pool offset (0x58)  +0x48 u64 current pool  +0x50 u32 base pool off
  BRTI          +0x00 'BRTI' +0x04 u32 next +0x08 u32 size
                +0x10 u8 flags  +0x11 u8 storage dim  +0x12 u16 tile mode  +0x14 u16 swizzle  +0x16 u16 mip count
                +0x18 u32 sample count  +0x1C u32 format  +0x20 u32 gpu access flags
                +0x24 u32 width  +0x28 height  +0x2C depth  +0x30 array layers  +0x34 u32 block-height log2
                +0x38 u32 layout2  (+0x3C..+0x50 reserved)  +0x50 u32 image size (all mips, all layers)  +0x54 u32 alignment
                +0x58 u8[4] channel sources  +0x5C u8 texture target
                +0x60 u64 name  +0x68 u64 parent  +0x70 u64 -> u64[mips] mip offsets (absolute, layer 0)
                +0x78 u64 user data  +0x80 u64 nvn texture (0x100 B, zero in file)  +0x88 u64 texture view (0x100 B)
                +0x90 u64 descriptor slot  +0x98 u64 user-data dict
  BRTD          +0x00 'BRTD' +0x08 u64 block size (incl. 0x10 header); image data follows.
"""
import struct, sys, os
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:] = [p for p in sys.path if os.path.abspath(p or '.') != _HERE] + [_HERE]   # (tools/ has a dis.py that shadows the stdlib)

# format high byte -> (name, block w, block h, bytes per block). Low byte: 1 unorm 2 snorm 3 uint 4 sint 5 float 6 srgb 7 depth 8 ufloat? 0xa ufloat(BC6)
FMT = {
    0x01: ('R4G4', 1, 1, 1), 0x02: ('R8', 1, 1, 1), 0x03: ('R4G4B4A4', 1, 1, 2), 0x04: ('A4B4G4R4', 1, 1, 2),
    0x05: ('R5G5B5A1', 1, 1, 2), 0x06: ('A1B5G5R5', 1, 1, 2), 0x07: ('R5G6B5', 1, 1, 2), 0x08: ('B5G6R5', 1, 1, 2),
    0x09: ('R8G8', 1, 1, 2), 0x0a: ('R16', 1, 1, 2), 0x0b: ('R8G8B8A8', 1, 1, 4), 0x0c: ('B8G8R8A8', 1, 1, 4),
    0x0d: ('R9G9B9E5', 1, 1, 4), 0x0e: ('R10G10B10A2', 1, 1, 4), 0x0f: ('R11G11B10', 1, 1, 4),
    0x10: ('B10G11R11', 1, 1, 4), 0x12: ('R16G16', 1, 1, 4), 0x13: ('D24S8', 1, 1, 4), 0x14: ('R32', 1, 1, 4),
    0x15: ('R16G16B16A16', 1, 1, 8), 0x16: ('D32S8', 1, 1, 8), 0x17: ('R32G32', 1, 1, 8),
    0x18: ('R32G32B32', 1, 1, 12), 0x19: ('R32G32B32A32', 1, 1, 16),
    0x1a: ('BC1', 4, 4, 8), 0x1b: ('BC2', 4, 4, 16), 0x1c: ('BC3', 4, 4, 16), 0x1d: ('BC4', 4, 4, 8),
    0x1e: ('BC5', 4, 4, 16), 0x1f: ('BC6H', 4, 4, 16), 0x20: ('BC7', 4, 4, 16),
    0x2d: ('ASTC4x4', 4, 4, 16), 0x2e: ('ASTC5x4', 5, 4, 16), 0x2f: ('ASTC5x5', 5, 5, 16), 0x30: ('ASTC6x5', 6, 5, 16),
    0x31: ('ASTC6x6', 6, 6, 16), 0x32: ('ASTC8x5', 8, 5, 16), 0x33: ('ASTC8x6', 8, 6, 16), 0x34: ('ASTC8x8', 8, 8, 16),
    0x35: ('ASTC10x5', 10, 5, 16), 0x36: ('ASTC10x6', 10, 6, 16), 0x37: ('ASTC10x8', 10, 8, 16),
    0x38: ('ASTC10x10', 10, 10, 16), 0x39: ('ASTC12x10', 12, 10, 16), 0x3a: ('ASTC12x12', 12, 12, 16),
    0x3b: ('B5G5R5A1', 1, 1, 2),
}
VARIANT = {1: 'UNORM', 2: 'SNORM', 3: 'UINT', 4: 'SINT', 5: 'FLOAT', 6: 'SRGB', 7: 'DEPTH', 8: 'UINT_TO_FLOAT', 9: 'SINT_TO_FLOAT', 0xa: 'UFLOAT'}
# BNTX texture 'dim' at BRTI+0x5C (not NVN's TextureTarget enum). 1/3/5 verified (5 = all multi-layer textures, 3 = a
# 6-layer texture); the rest from Switch-Toolbox.
TARGET = {0: '1D', 1: '2D', 2: '3D', 3: 'Cube', 4: '1DArray', 5: '2DArray', 6: '2DMS', 7: '2DMSArray', 8: 'CubeArray'}


def fmt_name(f):
    t = FMT.get(f >> 8)
    return '%s_%s' % (t[0] if t else 'fmt%02x' % (f >> 8), VARIANT.get(f & 0xff, 'v%02x' % (f & 0xff)))


def _s(b, o):
    """BNTX string: u16 length, then chars."""
    n = struct.unpack_from('<H', b, o)[0]
    return b[o + 2:o + 2 + n].decode('utf-8', 'replace')


class Texture:
    pass


def parse_header(b):
    """Parse everything up to the image data; b may be just a prefix covering the header section."""
    assert b[:4] == b'BNTX', b[:4]
    h = {}
    h['version'], = struct.unpack_from('<I', b, 8)
    h['align_log2'], h['addr_bits'] = b[0xE], b[0xF]
    h['name_off'], h['flags'], h['first_block'], h['rlt_off'], h['file_size'] = struct.unpack_from('<IHHII', b, 0x10)
    h['platform'] = b[0x20:0x24].decode('latin1')
    cnt, = struct.unpack_from('<I', b, 0x24)
    tbl, brtd, dic, pool = struct.unpack_from('<QQQQ', b, 0x28)
    h['brtd'], h['dic'] = brtd, dic
    texs = []
    for i in range(cnt):
        o, = struct.unpack_from('<Q', b, tbl + 8 * i)
        assert b[o:o + 4] == b'BRTI', hex(o)
        t = Texture()
        t.off = o
        t.flags, t.dim, t.tile_mode, t.swizzle, t.mips, t.samples, t.format, t.access = struct.unpack_from('<BBHHHIII', b, o + 0x10)
        t.width, t.height, t.depth, t.layers, t.bh_log2, t.layout2 = struct.unpack_from('<6I', b, o + 0x24)
        t.image_size, t.alignment = struct.unpack_from('<II', b, o + 0x50)
        t.comp_sel = tuple(b[o + 0x58:o + 0x5c])
        t.target = b[o + 0x5c]
        name, parent, mipp, ud, tp, vp, ds, udd = struct.unpack_from('<8Q', b, o + 0x60)
        t.name = _s(b, name)
        t.mip_offsets = list(struct.unpack_from('<%dQ' % t.mips, b, mipp))
        t.user_data, t.user_dict = ud, udd
        texs.append(t)
    h['textures'] = texs
    return h


def header_prefix_len(b):
    """Bytes needed for parse_header: up to the BRTD data block (where the header section ends)."""
    return struct.unpack_from('<Q', b, 0x30)[0] + 0x10


# ---- Tegra (Maxwell/Ampere) block-linear layout -------------------------------------------------------------------
GOB_W, GOB_H = 64, 8  # bytes x rows; 512 bytes per GOB


def div_up(a, b):
    return (a + b - 1) // b


def mip_block_dims(rows, depth, bh, bd=1):
    """Block height (in GOBs) shrinks for small mips like the hardware/tegra_swizzle does."""
    while bh > 1 and rows <= (bh * GOB_H) // 2:
        bh //= 2
    while bd > 1 and depth <= bd // 2:
        bd //= 2
    return bh, bd


def mip_size(width_bytes, rows, depth, bh, bd=1):
    """Size of one block-linear surface (one mip of one layer)."""
    bh, bd = mip_block_dims(rows, depth, bh, bd)
    gw = div_up(width_bytes, GOB_W)
    gh = div_up(rows, GOB_H * bh)
    gd = div_up(depth, bd)
    return gw * gh * gd * 512 * bh * bd


def surface_layout(t):
    """[(offset within layer, size, w_blocks, h_blocks, depth, bh)] per mip, plus the layer stride."""
    name, bw, bhpx, bpb = FMT[t.format >> 8]
    bh = 1 << t.bh_log2
    out, off = [], 0
    for m in range(t.mips):
        w, h, d = max(1, t.width >> m), max(1, t.height >> m), max(1, t.depth >> m)
        wb, hb = div_up(w, bw), div_up(h, bhpx)
        mbh, _ = mip_block_dims(hb, d, bh)
        s = mip_size(wb * bpb, hb, d, bh)
        out.append((off, s, wb, hb, d, mbh))
        off += s
    # layers are aligned to mip0's block (GOB * block height)
    if t.layers > 1:
        a = 512 * (1 << t.bh_log2) if t.mips >= 1 else 512
        a = 512 * out[0][5]
        off = div_up(off, a) * a
    return out, off


def gob_offset(x, y):
    """Byte offset of (x bytes, y rows) within a 64x8 GOB."""
    return ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16)


def deswizzle(data, width_bytes, rows, bh, bpb=16, depth=1):
    """Block-linear -> linear (rows of width_bytes). numpy-vectorised over 16-byte units (bpb must divide 16 or be 16-aligned)."""
    import numpy as np
    gw = div_up(width_bytes, GOB_W)
    # per 16-byte unit coordinates
    xs = np.arange(0, width_bytes, 16)
    ys = np.arange(rows)
    X, Y = np.meshgrid(xs, ys)
    blk = (Y // (GOB_H * bh)) * gw + X // GOB_W
    off = blk * (512 * bh) + ((Y % (GOB_H * bh)) // GOB_H) * 512 + gob_offset(X, Y)
    src = np.frombuffer(data, np.uint8)
    need = int(off.max()) + 16
    if len(src) < need:   # short input (e.g. trying a block height the surface was not laid out with): zero-pad
        src = np.concatenate([src, np.zeros(need - len(src), np.uint8)])
    idx = off[..., None] + np.arange(16)
    lin = src[idx.ravel()].reshape(rows, -1)[:, :width_bytes]
    if depth != 1:
        raise NotImplementedError('3D block-linear (block depth) not implemented')
    return lin.tobytes()


def texture_data(b, t, layer=0, mip=0):
    """Swizzled bytes of one mip of one layer (b = whole file)."""
    lay, stride = surface_layout(t)
    off, size = lay[mip][0], lay[mip][1]
    base = t.mip_offsets[0] + layer * stride
    return b[base + off:base + off + size]


def linear_mip(b, t, layer=0, mip=0):
    lay, _ = surface_layout(t)
    _, size, wb, hb, d, mbh = lay[mip]
    bpb = FMT[t.format >> 8][3]
    return deswizzle(texture_data(b, t, layer, mip), wb * bpb, hb, mbh, bpb)


# ---- block sanity checks -------------------------------------------------------------------------------------------
# Compressed blocks of neighbouring pixels have similar headers/endpoints. "Roughness" = mean bit difference of the first
# few bytes of each block to its right and lower neighbour. The correct layout minimises it; verify() compares the
# header's block height against every other block height and against a pitch-linear reading.
def block_features(lin, wb, hb, fmt):
    """Per-block numeric features that vary smoothly over an image: endpoint colours for BC1-5, the mode/header bits
    (compared for equality) for BC6H/BC7/ASTC, the texel values for uncompressed formats. Returns (array, exact)."""
    import numpy as np
    bpb = FMT[fmt >> 8][3]
    a = np.frombuffer(lin, np.uint8)[:wb * hb * bpb].reshape(hb, wb, bpb)
    t = fmt >> 8
    def c565(w):
        w = w.astype(np.int32)
        return np.stack([(w >> 11) & 31, (w >> 5) & 63, w & 31], -1).astype(np.float32)
    if t == 0x1a or t in (0x1b, 0x1c):
        o = 0 if t == 0x1a else 8
        w0 = a[..., o] | (a[..., o + 1].astype(np.uint16) << 8); w1 = a[..., o + 2] | (a[..., o + 3].astype(np.uint16) << 8)
        return (c565(w0) + c565(w1)), False
    if t == 0x1d:
        return a[..., 0:2].astype(np.float32).sum(-1, keepdims=True), False
    if t == 0x1e:
        return np.stack([a[..., 0].astype(np.float32) + a[..., 1], a[..., 8].astype(np.float32) + a[..., 9]], -1), False
    if t == 0x15:
        return np.frombuffer(a.tobytes(), np.float16).reshape(hb, wb, 4).astype(np.float32), False
    if bpb <= 4 and FMT[t][1] == 1:
        return a.astype(np.float32), False
    # BC6H / BC7 / ASTC: header bits; ASTC block mode is the low 11 bits, BC7 mode the lowest set bit of byte 0
    if 0x2d <= t <= 0x3a:
        return (a[..., 0] | (a[..., 1].astype(np.uint16) << 8)).astype(np.int32)[..., None] & 0x1fff, True
    return a[..., 0:1].astype(np.int32), True


def roughness(lin, wb, hb, fmt):
    """Mean neighbour difference over the mean difference of random block pairs (1 = no spatial structure)."""
    import numpy as np
    f, exact = block_features(lin, wb, hb, fmt)
    d = (lambda x, y: (x != y).any(-1).astype(np.float32)) if exact else (lambda x, y: np.abs(x - y).sum(-1))
    num = []
    if wb > 1: num.append(d(f[:, 1:], f[:, :-1]).ravel())
    if hb > 1: num.append(d(f[1:], f[:-1]).ravel())
    if not num: return 1.0
    num = np.concatenate(num)
    flat = f.reshape(-1, f.shape[-1])
    rng = np.random.default_rng(0)
    den = d(flat[rng.integers(0, len(flat), 20000)], flat[rng.integers(0, len(flat), 20000)]).mean()
    return float(num.mean() / den) if den > 0 else 1.0


def decode_linear(lin, t, w, h):
    """Decode a deswizzled mip to an (h, w, C) float array: texture2ddecoder (in the venv) for BCn/ASTC, numpy views
    for the plain formats. None for formats it cannot read."""
    import numpy as np
    k, bw, bhp, bpb = FMT[t.format >> 8]
    W, H = div_up(w, bw) * bw, div_up(h, bhp) * bhp
    if bw > 1:
        import texture2ddecoder as td
        f = t.format >> 8
        if 0x2d <= f <= 0x3a:
            out = td.decode_astc(lin, W, H, bw, bhp)
        else:
            fn = {0x1a: td.decode_bc1, 0x1c: td.decode_bc3, 0x1d: td.decode_bc4, 0x1e: td.decode_bc5,
                  0x1f: td.decode_bc6, 0x20: td.decode_bc7}.get(f)
            if fn is None: return None
            out = fn(lin, W, H)
        return np.frombuffer(out, np.uint8).reshape(H, W, 4)[:h, :w].astype(np.float32)
    dt = {0x15: (np.float16, 4), 0x0a: (np.uint16, 1), 0x09: (np.uint8, 2), 0x02: (np.uint8, 1), 0x0b: (np.uint8, 4),
          0x12: (np.uint16, 2), 0x14: (np.float32, 1)}.get(t.format >> 8)
    if dt is None: return None
    return np.frombuffer(lin, dt[0])[:w * h * dt[1]].reshape(h, w, dt[1]).astype(np.float32)


def decode_mip(b, t, layer=0, mip=0, bh=None, pitch=False):
    """Deswizzle (header layout, or a forced block height / pitch-linear reading for comparison) and decode."""
    import numpy as np
    lay, _ = surface_layout(t)
    _, size, wb, hb, d, mbh = lay[mip]
    bpb = FMT[t.format >> 8][3]
    raw = texture_data(b, t, layer, mip)
    if pitch:
        p = div_up(wb * bpb, 64) * 64
        raw = raw + bytes(max(0, p * hb - len(raw)))
        lin = np.frombuffer(raw, np.uint8)[:p * hb].reshape(hb, p)[:, :wb * bpb].tobytes()
    else:
        lin = deswizzle(raw, wb * bpb, hb, bh or mbh, bpb)
    return decode_linear(lin, t, max(1, t.width >> mip), max(1, t.height >> mip))


def verify(b, t, layer=0):
    """Layout check without looking at images: decode mip0 under every candidate layout (each block height, and
    pitch-linear) and correlate its 2x2 box-filtered version with mip1 decoded with the header layout. The correct
    layout correlates strongly (the mip chain is a downsample of mip0); wrong ones near zero.
    Returns {'bh=N'|'pitch': r, 'header': 'bh=N'} or None (one mip / undecodable format)."""
    import numpy as np, warnings
    if t.mips < 2: return None
    m1 = decode_mip(b, t, layer, 1)
    if m1 is None: return None
    lay, _ = surface_layout(t)
    hb = lay[0][3]
    out = {}
    cands = [('bh=%d' % c, dict(bh=c)) for c in sorted({mip_block_dims(hb, 1, c)[0] for c in (1, 2, 4, 8, 16, 32)})]
    for k, kw in cands + [('pitch', dict(pitch=True))]:
        m0 = decode_mip(b, t, layer, 0, **kw)
        h1, w1 = min(m1.shape[0], m0.shape[0] // 2), min(m1.shape[1], m0.shape[1] // 2)
        if h1 < 2 or w1 < 2: return None
        ds = m0[:2 * h1, :2 * w1].reshape(h1, 2, w1, 2, -1).mean((1, 3))
        x, y = ds.ravel(), m1[:h1, :w1].ravel()
        with warnings.catch_warnings():
            warnings.simplefilter('ignore')
            r = np.corrcoef(x, y)[0, 1]
        out[k] = float(0 if np.isnan(r) else r)
    out['header'] = 'bh=%d' % lay[0][5]
    return out


def summary(path, check=False):
    import zs
    b = zs.decompress(open(path, 'rb').read())
    h = parse_header(b)
    print('%s: BNTX v%d.%d.%d platform %r, %d texture(s), file %d bytes, align 2^%d' % (
        os.path.basename(path), h['version'] >> 16, (h['version'] >> 8) & 0xff, h['version'] & 0xff,
        h['platform'], len(h['textures']), h['file_size'], h['align_log2']))
    for t in h['textures']:
        lay, stride = surface_layout(t)
        print('  %-40s %-18s %s %dx%dx%d layers=%d mips=%d bh=%d tile=%d dim=%d flags=%#x access=%#x layout2=%#x'
              % (t.name, fmt_name(t.format), TARGET.get(t.target, t.target), t.width, t.height, t.depth, t.layers,
                 t.mips, 1 << t.bh_log2, t.tile_mode, t.dim, t.flags, t.access, t.layout2))
        mipsz = [t.mip_offsets[i + 1] - t.mip_offsets[i] for i in range(t.mips - 1)]
        print('    image_size=%#x computed=%#x  mip sizes file=%s computed=%s' % (
            t.image_size, stride * t.layers, [hex(x) for x in mipsz[:4]], [hex(x[1]) for x in lay[:4]]))
        if check and t.depth == 1:
            v = verify(b, t)
            if v is None:
                print('    layout check: n/a (single mip or format not decodable)'); continue
            hdr = v.pop('header')
            best = max(v, key=v.get)
            print('    layout check: header %s, best %s (%s)' % (hdr, best, ' '.join('%s:%.3f' % kv for kv in v.items())))


if __name__ == '__main__':
        summary(sys.argv[1], '--check' in sys.argv)
