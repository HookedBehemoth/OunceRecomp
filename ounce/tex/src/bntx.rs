//! BNTX files (the texture container of the romfs) and the Tegra block-linear layout of their surfaces.

/// (name, block width, block height, bytes per block) of a BNTX format's high byte; the low byte is the variant.
pub fn fmt(hi: u32) -> Option<(&'static str, u32, u32, u32)> {
    Some(match hi {
        0x02 => ("R8", 1, 1, 1),
        0x07 => ("R5G6B5", 1, 1, 2),
        0x09 => ("R8G8", 1, 1, 2),
        0x0a => ("R16", 1, 1, 2),
        0x0b => ("R8G8B8A8", 1, 1, 4),
        0x0c => ("B8G8R8A8", 1, 1, 4),
        0x0e => ("R10G10B10A2", 1, 1, 4),
        0x0f => ("R11G11B10", 1, 1, 4),
        0x12 => ("R16G16", 1, 1, 4),
        0x14 => ("R32", 1, 1, 4),
        0x15 => ("R16G16B16A16", 1, 1, 8),
        0x17 => ("R32G32", 1, 1, 8),
        0x19 => ("R32G32B32A32", 1, 1, 16),
        0x1a => ("BC1", 4, 4, 8),
        0x1b => ("BC2", 4, 4, 16),
        0x1c => ("BC3", 4, 4, 16),
        0x1d => ("BC4", 4, 4, 8),
        0x1e => ("BC5", 4, 4, 16),
        0x1f => ("BC6H", 4, 4, 16),
        0x20 => ("BC7", 4, 4, 16),
        0x2d => ("ASTC4x4", 4, 4, 16),
        0x2e => ("ASTC5x4", 5, 4, 16),
        0x2f => ("ASTC5x5", 5, 5, 16),
        0x30 => ("ASTC6x5", 6, 5, 16),
        0x31 => ("ASTC6x6", 6, 6, 16),
        0x32 => ("ASTC8x5", 8, 5, 16),
        0x33 => ("ASTC8x6", 8, 6, 16),
        0x34 => ("ASTC8x8", 8, 8, 16),
        0x35 => ("ASTC10x5", 10, 5, 16),
        0x36 => ("ASTC10x6", 10, 6, 16),
        0x37 => ("ASTC10x8", 10, 8, 16),
        0x38 => ("ASTC10x10", 10, 10, 16),
        0x39 => ("ASTC12x10", 12, 10, 16),
        0x3a => ("ASTC12x12", 12, 12, 16),
        _ => return None,
    })
}

pub fn fmt_name(f: u32) -> String {
    let t = fmt(f >> 8).map_or(format!("fmt{:02x}", f >> 8), |t| t.0.to_string());
    let v = match f & 0xff {
        1 => "UNORM".to_string(),
        2 => "SNORM".into(),
        3 => "UINT".into(),
        4 => "SINT".into(),
        5 => "FLOAT".into(),
        6 => "SRGB".into(),
        0xa => "UFLOAT".into(),
        x => format!("v{x:02x}"),
    };
    format!("{t}_{v}")
}

pub fn is_astc(f: u32) -> bool {
    (0x2d..=0x3a).contains(&(f >> 8))
}

pub fn is_bc(f: u32) -> bool {
    (0x1a..=0x20).contains(&(f >> 8))
}

/// One BRTI (texture) of a BNTX; offsets are relative to the BNTX's start.
#[derive(Debug, Clone)]
pub struct Tex {
    pub off: usize,
    pub mips: u32,
    pub samples: u32,
    pub format: u32,
    pub width: u32,
    pub height: u32,
    pub depth: u32,
    pub layers: u32,
    pub bh_log2: u32,
    pub image_size: u32,
    pub alignment: u32,
    pub comp_sel: [u8; 4],
    pub target: u8,
    pub name: String,
    pub mip_offsets: Vec<u64>,
}

pub struct Bntx {
    pub rlt_off: u32,
    pub brtd: u64,
    pub textures: Vec<Tex>,
}

fn rd<const N: usize>(b: &[u8], o: usize) -> Result<[u8; N], String> {
    o.checked_add(N)
        .and_then(|e| b.get(o..e))
        .map(|s| s.try_into().unwrap())
        .ok_or_else(|| format!("read of {N} bytes at {o:#x} past the end ({:#x})", b.len()))
}
pub fn u16_at(b: &[u8], o: usize) -> Result<u16, String> {
    rd::<2>(b, o).map(u16::from_le_bytes)
}
pub fn u32_at(b: &[u8], o: usize) -> Result<u32, String> {
    rd::<4>(b, o).map(u32::from_le_bytes)
}
pub fn u64_at(b: &[u8], o: usize) -> Result<u64, String> {
    rd::<8>(b, o).map(u64::from_le_bytes)
}

/// The file header and every BRTI.
pub fn parse(b: &[u8]) -> Result<Bntx, String> {
    if !b.starts_with(b"BNTX") {
        return Err("not a BNTX".into());
    }
    let rlt_off = u32_at(b, 0x18)?;
    let cnt = u32_at(b, 0x24)?;
    let tbl = u64_at(b, 0x28)? as usize;
    let brtd = u64_at(b, 0x30)?;
    let mut textures = Vec::with_capacity(cnt as usize);
    for i in 0..cnt as usize {
        let o = u64_at(b, tbl + 8 * i)? as usize;
        if b.get(o..o + 4) != Some(b"BRTI") {
            return Err(format!("BRTI {i} at {o:#x}"));
        }
        let mips = u16_at(b, o + 0x16)? as u32;
        let name_at = u64_at(b, o + 0x60)? as usize;
        let mipp = u64_at(b, o + 0x70)? as usize;
        let n = u16_at(b, name_at)? as usize;
        let name = String::from_utf8_lossy(b.get(name_at + 2..(name_at + 2 + n).min(b.len())).unwrap_or(&[])).into_owned();
        let mip_offsets = (0..mips as usize).map(|m| u64_at(b, mipp + 8 * m)).collect::<Result<_, _>>()?;
        textures.push(Tex {
            off: o,
            mips,
            samples: u32_at(b, o + 0x18)?,
            format: u32_at(b, o + 0x1c)?,
            width: u32_at(b, o + 0x24)?,
            height: u32_at(b, o + 0x28)?,
            depth: u32_at(b, o + 0x2c)?,
            layers: u32_at(b, o + 0x30)?,
            // (the texture layout: its low nibble the block height, its high nibble a 3D texture's block depth)
            bh_log2: u32_at(b, o + 0x34)? & 0xF,
            image_size: u32_at(b, o + 0x50)?,
            alignment: u32_at(b, o + 0x54)?,
            comp_sel: rd::<4>(b, o + 0x58)?,
            target: rd::<1>(b, o + 0x5c)?[0],
            name,
            mip_offsets,
        });
    }
    Ok(Bntx { rlt_off, brtd, textures })
}

/// A mip's block height (log2 GOBs): halved while the mip's rows fit in half a block (runtime/nvn_layout.c adj_tile).
pub fn mip_block_height(rows: u32, mut bh_log2: u32) -> u32 {
    while bh_log2 > 0 && rows <= (8 << bh_log2) / 2 {
        bh_log2 -= 1;
    }
    bh_log2
}

/// One mip of a layer: its offset in the layer, its block-linear size, its width and height in blocks, its block height.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Mip {
    pub off: u64,
    pub size: u64,
    pub wb: u32,
    pub hb: u32,
    pub bh: u32,
}

/// The mips of one layer and the layer stride.
pub fn surface_layout(t: &Tex) -> (Vec<Mip>, u64) {
    let (_, bw, bhp, bpb) = fmt(t.format >> 8).expect("a known format");
    let mut out = Vec::with_capacity(t.mips as usize);
    let mut off = 0u64;
    for m in 0..t.mips {
        let w = (t.width >> m).max(1);
        let h = (t.height >> m).max(1);
        let (wb, hb) = (w.div_ceil(bw), h.div_ceil(bhp));
        let mbh = mip_block_height(hb, t.bh_log2);
        let size = (wb as u64 * bpb as u64).div_ceil(64) * (hb as u64).div_ceil(8 << mbh) * (512u64 << mbh);
        out.push(Mip { off, size, wb, hb, bh: mbh });
        off += size;
    }
    if t.layers > 1 && !out.is_empty() {
        let a = 512u64 << out[0].bh;
        off = off.div_ceil(a) * a;
    }
    (out, off)
}

/// Byte (x, y) of a block-linear surface: GOBs of 64 bytes x 8 rows, blocks 1 GOB wide and 1 << bh_log2 GOBs tall,
/// row-major; inside a GOB the Switch 2's order, 16 bytes of a row contiguous.
fn bl_offset(x: u32, y: u32, gobs_x: u32, bh_log2: u32) -> u64 {
    let rows_per_block = 8u32 << bh_log2;
    let block = (y / rows_per_block) as u64 * gobs_x as u64 + (x / 64) as u64;
    block * (512u64 << bh_log2)
        + ((y % rows_per_block) / 8) as u64 * 512
        + (((x % 64) / 32) * 256 + ((y % 8) / 4) * 128 + ((x % 32) / 16) * 64 + ((y % 4) / 2) * 32 + (y % 2) * 16 + x % 16) as u64
}

pub fn block_linear_size(width_bytes: u32, rows: u32, bh_log2: u32) -> u64 {
    (width_bytes as u64).div_ceil(64) * (rows as u64).div_ceil(8 << bh_log2) * (512u64 << bh_log2)
}

/// A block-linear surface of width_bytes x rows -> packed linear rows; bytes past `bl`'s end read as zeros.
pub fn to_linear(bl: &[u8], width_bytes: u32, rows: u32, bh_log2: u32) -> Vec<u8> {
    let gobs_x = width_bytes.div_ceil(64);
    let mut lin = vec![0u8; width_bytes as usize * rows as usize];
    for y in 0..rows {
        let row = &mut lin[y as usize * width_bytes as usize..][..width_bytes as usize];
        let mut x = 0;
        while x < width_bytes {
            let n = 16.min(width_bytes - x) as usize;
            let o = bl_offset(x, y, gobs_x, bh_log2) as usize;
            if o < bl.len() {
                let k = n.min(bl.len() - o);
                row[x as usize..x as usize + k].copy_from_slice(&bl[o..o + k]);
            }
            x += 16;
        }
    }
    lin
}

/// Packed linear rows -> a block-linear surface of block_linear_size bytes (the padding zero).
pub fn to_block_linear(lin: &[u8], width_bytes: u32, rows: u32, bh_log2: u32) -> Vec<u8> {
    let gobs_x = width_bytes.div_ceil(64);
    let mut bl = vec![0u8; block_linear_size(width_bytes, rows, bh_log2) as usize];
    for y in 0..rows {
        let mut x = 0;
        while x < width_bytes {
            let n = 16.min(width_bytes - x) as usize;
            let o = bl_offset(x, y, gobs_x, bh_log2) as usize;
            let s = y as usize * width_bytes as usize + x as usize;
            bl[o..o + n].copy_from_slice(&lin[s..s + n]);
            x += 16;
        }
    }
    bl
}

// ---- ASTC block scan ----
fn bits(b: &[u8], pos: u32, n: u32) -> u32 {
    let mut v = 0;
    for i in 0..n {
        v |= (((b[((pos + i) >> 3) as usize] >> ((pos + i) & 7)) & 1) as u32) << i;
    }
    v
}

fn ise_bits(count: u32, q: u32) -> u32 {
    const KIND: [u8; 12] = [0, 1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 0];
    const NB: [u32; 12] = [1, 0, 2, 0, 1, 3, 1, 2, 4, 2, 3, 5];
    match KIND[q as usize] {
        1 => (8 * count + 4) / 5 + count * NB[q as usize],
        2 => (7 * count + 2) / 3 + count * NB[q as usize],
        _ => count * NB[q as usize],
    }
}

/// The 2D block mode (ASTC spec C.2.10): (weight bits, dual plane), None for a reserved mode.
fn block_mode_2d(bm: u32) -> Option<(u32, bool)> {
    let (mut base_q, mut h, mut d, a) = ((bm >> 4) & 1, (bm >> 9) & 1, (bm >> 10) & 1, (bm >> 5) & 3);
    let (x, y);
    if bm & 3 != 0 {
        base_q |= (bm & 3) << 1;
        let mut b = (bm >> 7) & 3;
        match (bm >> 2) & 3 {
            0 => (x, y) = (b + 4, a + 2),
            1 => (x, y) = (b + 8, a + 2),
            2 => (x, y) = (a + 2, b + 8),
            _ => {
                b &= 1;
                if bm & 0x100 != 0 {
                    (x, y) = (b + 2, a + 2)
                } else {
                    (x, y) = (a + 2, b + 6)
                }
            }
        }
    } else {
        base_q |= ((bm >> 2) & 3) << 1;
        if (bm >> 2) & 3 == 0 {
            return None;
        }
        let b = (bm >> 9) & 3;
        match (bm >> 7) & 3 {
            0 => (x, y) = (12, a + 2),
            1 => (x, y) = (a + 2, 12),
            2 => {
                (x, y) = (a + 6, b + 6);
                d = 0;
                h = 0;
            }
            _ => match (bm >> 5) & 3 {
                0 => (x, y) = (6, 10),
                1 => (x, y) = (10, 6),
                _ => return None,
            },
        }
    }
    let count = x * y * (d + 1);
    if base_q < 2 || base_q - 2 + 6 * h > 11 || count > 64 {
        return None;
    }
    let wb = ise_bits(count, base_q - 2 + 6 * h);
    (24..=96).contains(&wb).then_some((wb, d != 0))
}

/// [blocks with an HDR endpoint mode, HDR void-extent blocks, reserved or invalid blocks, LDR void-extent blocks]
pub fn astc_scan(blocks: &[u8], counts: &mut [u64; 4]) {
    for b in blocks.chunks_exact(16) {
        let bm = bits(b, 0, 11);
        if bm & 0x1FF == 0x1FC {
            counts[if (bm >> 9) & 1 != 0 { 1 } else { 3 }] += 1;
            continue;
        }
        let Some((wb, dual)) = block_mode_2d(bm) else {
            counts[2] += 1;
            continue;
        };
        let parts = bits(b, 11, 2) + 1;
        if parts == 4 && dual {
            counts[2] += 1;
            continue;
        }
        let mut cem = [0u32; 4];
        if parts == 1 {
            cem[0] = bits(b, 13, 4);
        } else {
            let low = bits(b, 23, 6);
            if low & 3 == 0 {
                for c in cem.iter_mut().take(parts as usize) {
                    *c = (low >> 2) & 0xF;
                }
            } else {
                let hs = 3 * parts - 4;
                let enc = low | (bits(b, 128 - wb - hs, hs) << 6);
                let base = (enc & 3).wrapping_sub(1);
                let mut bp = 2;
                for c in cem.iter_mut().take(parts as usize) {
                    *c = base.wrapping_add((enc >> bp) & 1) << 2;
                    bp += 1;
                }
                for c in cem.iter_mut().take(parts as usize) {
                    *c |= (enc >> bp) & 3;
                    bp += 2;
                }
            }
        }
        if cem[..parts as usize].iter().any(|c| matches!(c, 2 | 3 | 7 | 11 | 14 | 15)) {
            counts[0] += 1;
        }
    }
}
