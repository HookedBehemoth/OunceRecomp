//! Conversions of one game's own files into the overlay the host serves over the romfs (`prepared/terrain`), named by
//! the game description's `overlay_steps`:
//! - `tarb_terrain`: every TARB terrain tile under `TerrainArc/` with its GPU texel blob (the record's last part, B)
//!   deswizzled to packed linear rows, layer after layer; the tile is 260 x 260 texels and B's layout is told by its
//!   layer count and size. The game's resource size table is written beside them with each grown file's entry raised,
//!   since the loader drops a file larger than its entry.
//! - `skybin`: the sky LUT members (`*.skybin`) of the environment archive, three RGBA16F surfaces (2D, 3D, 2D)
//!   deswizzled in place; the archive keeps its size and is compressed again with its dictionary.
//! Files these steps do not recognize are left out, and the host serves the game's own.

use crate::bntx::{to_linear, u16_at, u32_at};
use crate::romfs::Dicts;
use ounce_dump::zstd;
use rayon::prelude::*;
use std::path::Path;

pub const TERRAIN_DIR: &str = "TerrainArc";
const TILE: u32 = 260;
const HEADER: usize = 0x14;
const ZSTD_LEVEL: i32 = 19;

pub const SKY_FILES: [&str; 1] = ["Env/GameScene.Nin_Ounce_NVN2.genvb.zs"];
const SKY_SUFFIX: &str = ".skybin";
/// (width, height, depth, bytes per texel) of the three surfaces, in the member's order
const SKY_SURFACES: [(u32, u32, u32, u32); 3] = [(256, 64, 1, 8), (256, 32, 16, 8), (64, 64, 1, 8)];

/// NVN's automatic block height for a surface `g` GOBs high
fn pick_tile(g: u32) -> u32 {
    match g {
        16.. => 4,
        8.. => 3,
        4.. => 2,
        2.. => 1,
        _ => 0,
    }
}

fn adj_tile(mut shift: u32, unit: u32, dim: u32) -> u32 {
    if shift == 0 {
        return 0;
    }
    let mut x = unit << (shift - 1);
    if x >= dim {
        loop {
            shift -= 1;
            if shift == 0 {
                break;
            }
            x >>= 1;
            if x < dim {
                break;
            }
        }
    }
    shift
}

/// One level-0 tile's layout: (block-linear bytes, layer stride, block height log2, row bytes, rows, linear bytes).
fn tile_layout(bpb: u32, bw: u32, layers: u32) -> (u64, u64, u32, u32, u32, u64) {
    let lw = TILE.div_ceil(bw);
    let lh = lw;
    let tile_h = pick_tile((lh + lh / 2) / 8);
    let th = adj_tile(tile_h, 8, lh);
    let (wg, hg) = ((lw * bpb).div_ceil(64) as u64, lh.div_ceil(8) as u64);
    let mut ls = (wg * ((hg + (1 << th) - 1) >> th)) << (9 + th);
    if layers > 1 {
        let mut t2 = tile_h;
        while t2 > 0 && lh <= 8 << (t2 - 1) {
            t2 -= 1;
        }
        let a = 1u64 << (t2 + 9);
        ls = (ls + a - 1) & !(a - 1);
    }
    (ls * layers as u64, ls, th, lw * bpb, lh, (lw * bpb * lh * layers) as u64)
}

/// (bytes per block, block edge) of the formats a tile's B may hold
const TILE_FORMATS: [(u32, u32); 7] = [(1, 1), (2, 1), (4, 1), (8, 1), (16, 1), (8, 4), (16, 4)];

/// A TARB file's bytes with B linear, or why it is left out.
pub fn convert_tarb(data: &[u8]) -> Result<Vec<u8>, String> {
    if !data.starts_with(b"TARB") {
        return Err("not a TARB".into());
    }
    let (ver, cnt, hs, usz, csz) = (u32_at(data, 4)?, u16_at(data, 8)?, u16_at(data, 10)? as usize, u32_at(data, 12)? as usize, u32_at(data, 16)? as usize);
    if ver != 1 || cnt != 1 || hs != HEADER || HEADER + csz != data.len() {
        return Err(format!("header: version {ver} records {cnt} header {hs:#x} sizes {usz}+{csz} vs file {}", data.len()));
    }
    let rec = zstd::decompress(zstd::Format::Std, &data[hs..], usz).map_err(|e| format!("zstd: {e}"))?;
    if rec.len() < 0x34 {
        return Err(format!("record size {}", rec.len()));
    }
    let (boff, bsz) = (u32_at(&rec, 0x2c)? as i32, u32_at(&rec, 0x30)? as usize);
    let b0 = 0x2c + boff as isize;
    if boff < 0 || b0 as usize + bsz != rec.len() {
        return Err(format!("B at {b0} + {bsz} is not the record's end {}", rec.len()));
    }
    let b0 = b0 as usize;
    let layers = rec[3] as u32;
    let found: Vec<_> = TILE_FORMATS.iter().map(|&(bpb, bw)| tile_layout(bpb, bw, layers)).filter(|l| l.0 == bsz as u64).collect();
    let [(_, ls, th, wbytes, rows, lin_total)] = found[..] else {
        return Err(format!("B: {layers} layers, {bsz} bytes: no known layout"));
    };
    let blob = &rec[b0..];
    let one = crate::bntx::block_linear_size(wbytes, rows, th) as usize;
    let mut new = rec[..b0].to_vec();
    new.reserve(lin_total as usize);
    for l in 0..layers as usize {
        let s = (l * ls as usize).min(blob.len());
        new.extend_from_slice(&to_linear(&blob[s..(s + one).min(blob.len())], wbytes, rows, th));
    }
    new[0x30..0x34].copy_from_slice(&(lin_total as u32).to_le_bytes());
    let mut c = zstd::Compressor::new();
    let frame = c.compress(&new, ZSTD_LEVEL, false);
    let mut out = data[..8].to_vec();
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&(HEADER as u16).to_le_bytes());
    out.extend_from_slice(&(new.len() as u32).to_le_bytes());
    out.extend_from_slice(&(frame.len() as u32).to_le_bytes());
    out.extend_from_slice(&frame);
    Ok(out)
}

fn crc32(b: &[u8]) -> u32 {
    let mut c = !0u32;
    for &x in b {
        c ^= x as u32;
        for _ in 0..8 {
            c = (c >> 1) ^ (0xEDB8_8320 & (c & 1).wrapping_neg());
        }
    }
    !c
}

fn write_atomic(path: &Path, data: &[u8]) -> Result<(), String> {
    std::fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
    let tmp = path.with_extension("tmp");
    std::fs::write(&tmp, data).map_err(|e| format!("{}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| e.to_string())
}

#[derive(Debug, Default)]
pub struct TerrainStats {
    pub converted: usize,
    pub left_out: Vec<(String, String)>,
    pub in_bytes: u64,
    pub out_bytes: u64,
    pub raised: usize,
}

/// Every TARB tile of `romfs/TerrainArc` converted into `out` (same relative paths), and the size table.
pub fn terrain(romfs: &Path, out: &Path, dicts: &Dicts) -> Result<TerrainStats, String> {
    let files = crate::run::walk(romfs, &[], &[format!("{TERRAIN_DIR}/")])?;
    let done: Vec<(String, u64, Result<u64, String>)> = files
        .par_iter()
        .map(|(rel, size, _)| {
            let r = std::fs::read(romfs.join(rel)).map_err(|e| e.to_string()).and_then(|d| convert_tarb(&d)).and_then(|new| {
                write_atomic(&out.join(rel), &new)?;
                Ok(new.len() as u64)
            });
            (rel.clone(), *size, r)
        })
        .collect();
    let mut st = TerrainStats::default();
    let mut grew = std::collections::HashMap::new();
    for (rel, size, r) in done {
        match r {
            Ok(n) => {
                st.converted += 1;
                st.in_bytes += size;
                st.out_bytes += n;
                if n > size {
                    grew.insert(rel, (n - size) as u32);
                }
            }
            Err(why) => st.left_out.push((rel, why)),
        }
    }
    st.raised = size_table(romfs, out, dicts, &grew)?;
    Ok(st)
}

/// The game's resource size table (System/Resource/ResourceSizeTable.*.rsizetable.zs: "RESTBL", u32 version, u32 name
/// length, u32 hashed and u32 named entries; a hashed entry is the CRC-32 of the path and the size) with each grown
/// file's entry raised by what it grew, into `out`; the entries raised.
fn size_table(romfs: &Path, out: &Path, dicts: &Dicts, grew: &std::collections::HashMap<String, u32>) -> Result<usize, String> {
    let folder = romfs.join("System/Resource");
    let mut names: Vec<String> = std::fs::read_dir(&folder)
        .map_err(|e| format!("{}: {e}", folder.display()))?
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.starts_with("ResourceSizeTable.") && n.ends_with(".rsizetable.zs"))
        .collect();
    names.sort();
    let Some(name) = names.first() else { return Ok(0) };
    let data = std::fs::read(folder.join(name)).map_err(|e| e.to_string())?;
    let mut b = dicts.decompress(&data)?.into_owned();
    if !b.starts_with(b"RESTBL") {
        return Err(format!("{name} is not a RESTBL table"));
    }
    let (name_len, hashed, named) = (u32_at(&b, 10)? as usize, u32_at(&b, 14)? as usize, u32_at(&b, 18)? as usize);
    let mut by_crc: std::collections::HashMap<u32, u32> = grew.iter().map(|(r, d)| (crc32(r.as_bytes()), *d)).collect();
    let mut raised = 0;
    for i in 0..hashed {
        let at = 22 + 8 * i;
        if let Some(d) = by_crc.remove(&u32_at(&b, at)?) {
            let size = u32_at(&b, at + 4)? + d;
            b[at + 4..at + 8].copy_from_slice(&size.to_le_bytes());
            raised += 1;
        }
    }
    let base = 22 + 8 * hashed;
    for i in 0..named {
        let at = base + (name_len + 4) * i;
        let raw = &b[at..at + name_len];
        let rel = String::from_utf8_lossy(&raw[..memchr::memchr(0, raw).unwrap_or(name_len)]).into_owned();
        if let Some(d) = grew.get(&rel) {
            let size = u32_at(&b, at + name_len)? + d;
            b[at + name_len..at + name_len + 4].copy_from_slice(&size.to_le_bytes());
            raised += 1;
        }
    }
    write_atomic(&out.join("System/Resource").join(name), &zstd::Compressor::new().compress(&b, 3, false))?;
    Ok(raised)
}

// ---- the sky LUT ----
/// (block height, block depth) log2 in GOBs of a level-0 surface of h rows and d slices (NVN's automatic ones)
fn sky_tiles(h: u32, d: u32) -> (u32, u32) {
    if d > 1 {
        (0, adj_tile(pick_tile(d + d / 2), 1, d))
    } else {
        (adj_tile(pick_tile((h + h / 2) / 8), 8, h), 0)
    }
}

/// A skybin member's bytes with each surface deswizzled (the same size), or why not.
pub fn skybin_linear(member: &[u8]) -> Result<Vec<u8>, String> {
    let mut out = Vec::with_capacity(member.len());
    let mut off = 0usize;
    let mut lay = Vec::new();
    for &(w, h, d, bpp) in &SKY_SURFACES {
        let wb = w * bpp;
        let (th, td) = sky_tiles(h, d);
        let (nbx, nby, nbz) = (wb.div_ceil(64) as usize, (h.div_ceil(8) as usize).div_ceil(1 << th), (d as usize).div_ceil(1 << td));
        let size = (nbx * nby * nbz) << (9 + th + td);
        lay.push((off, wb, h, d, th, td, nbx, nby));
        off += size;
    }
    let lin_total: usize = SKY_SURFACES.iter().map(|&(w, h, d, bpp)| (w * bpp * h * d) as usize).sum();
    if member.len() != off || lin_total != off {
        return Err(format!("member of {} bytes, the layout says {off} block-linear / {lin_total} linear", member.len()));
    }
    for (off, wb, h, d, th, td, nbx, nby) in lay {
        for z in 0..d as usize {
            let (bs, part) = (512usize << (th + td), 512usize << th);
            let (bz, zz) = (z >> td, z & ((1 << td) - 1));
            let mut sl = Vec::with_capacity(nbx * nby * part);
            for by in 0..nby {
                for bx in 0..nbx {
                    let s = off + ((bz * nby + by) * nbx + bx) * bs + zz * part;
                    sl.extend_from_slice(&member[s..s + part]);
                }
            }
            out.extend_from_slice(&to_linear(&sl, wb, h, th));
        }
    }
    Ok(out)
}

/// The environment archive with its skybin members linear, compressed again with its dictionary (no checksum).
pub fn convert_skybin_archive(data: &[u8], dicts: &Dicts) -> Result<(Vec<u8>, Vec<String>), String> {
    if !data.starts_with(&zstd::MAGIC) {
        return Err("not a zstd frame".into());
    }
    let dict = dicts.of_frame(data)?;
    let mut raw = dicts.decompress(data)?.into_owned();
    if !raw.starts_with(b"SARC") || u16_at(&raw, 6)? != 0xFEFF {
        return Err("not a little-endian SARC".into());
    }
    let doff = u32_at(&raw, 0xc)? as usize;
    let n = u16_at(&raw, 0x14 + 6)? as usize;
    let sfnt = 0x14 + 12 + 16 * n;
    let mut members = Vec::new();
    for i in 0..n {
        let o = 0x14 + 12 + 16 * i;
        let (attr, s, e) = (u32_at(&raw, o + 4)?, u32_at(&raw, o + 8)? as usize, u32_at(&raw, o + 12)? as usize);
        if attr >> 24 == 0 {
            continue;
        }
        let p = sfnt + 8 + (attr & 0xFFFF) as usize * 4;
        let nm = &raw[p..];
        let name = String::from_utf8_lossy(&nm[..memchr::memchr(0, nm).unwrap_or(nm.len())]).into_owned();
        members.push((name, doff + s, doff + e));
    }
    members.sort();
    let mut names = Vec::new();
    for (name, s, e) in members {
        if name.to_lowercase().ends_with(SKY_SUFFIX) {
            let lin = skybin_linear(&raw[s..e])?;
            raw[s..e].copy_from_slice(&lin);
            names.push(name);
        }
    }
    if names.is_empty() {
        return Err(format!("no {SKY_SUFFIX} member"));
    }
    let frame = match dict {
        Some(d) => zstd::compress_with(&raw, d, ZSTD_LEVEL, false)?,
        None => zstd::Compressor::new().compress(&raw, ZSTD_LEVEL, false),
    };
    Ok((frame, names))
}

/// The sky archives of `romfs` converted into `out`; a line per file.
pub fn skybin(romfs: &Path, out: &Path, dicts: &Dicts) -> Result<Vec<String>, String> {
    let mut lines = Vec::new();
    for rel in SKY_FILES {
        let src = romfs.join(rel);
        let Ok(data) = std::fs::read(&src) else {
            lines.push(format!("{rel}: not in this romfs"));
            continue;
        };
        let (new, names) = convert_skybin_archive(&data, dicts).map_err(|e| format!("{rel}: {e}"))?;
        write_atomic(&out.join(rel), &new)?;
        lines.push(format!("{rel}: {} deswizzled", names.join(", ")));
    }
    Ok(lines)
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Rng(u64);
    impl Rng {
        fn bytes(&mut self, n: usize) -> Vec<u8> {
            (0..n)
                .map(|_| {
                    self.0 ^= self.0 << 13;
                    self.0 ^= self.0 >> 7;
                    self.0 ^= self.0 << 17;
                    self.0 as u8
                })
                .collect()
        }
    }

    fn in_gob(x: usize, y: usize) -> usize {
        ((x % 64) / 32) * 256 + ((y % 8) / 4) * 128 + ((x % 32) / 16) * 64 + ((y % 4) / 2) * 32 + (y % 2) * 16 + x % 16
    }

    /// byte (x, y, z) of a block-linear surface: blocks of 1 GOB x 2^th GOBs x 2^td slices, x first, then y, then z;
    /// inside a block the GOBs of a slice go down, then the slices
    fn ref_offset(x: usize, y: usize, z: usize, wb: usize, h: usize, th: u32, td: u32) -> usize {
        let (nbx, nby) = (wb.div_ceil(64), h.div_ceil(8).div_ceil(1 << th));
        let (bh, bd) = (1usize << th, 1usize << td);
        let gy = y / 8;
        let block = ((z / bd) * nby + gy / bh) * nbx + x / 64;
        block * 512 * bh * bd + ((z % bd) * bh + gy % bh) * 512 + in_gob(x, y)
    }

    /// what the game's tiles hold: (layers, block-linear size of B) -> linear size
    #[test]
    fn tile_layouts() {
        for (layers, bl, lin) in [(1, 139264, 67600), (6, 835584, 405600), (1, 417792, 270400), (2, 835584, 540800)] {
            let found: Vec<_> = TILE_FORMATS.iter().map(|&(b, w)| tile_layout(b, w, layers)).filter(|l| l.0 == bl).collect();
            assert_eq!(found.len(), 1, "{layers} layers, {bl} bytes");
            assert_eq!(found[0].5, lin);
        }
        assert_eq!(tile_layout(4, 1, 1).2, 4);
        assert_eq!(tile_layout(16, 4, 1).2, 3);
    }

    #[test]
    fn tarb_made_linear() {
        let mut rng = Rng(3);
        for (bpb, bw, layers) in [(16u32, 4u32, 1u32), (16, 4, 6), (4, 1, 1), (4, 1, 2)] {
            let (total, ls, th, wbytes, rows, lin_total) = tile_layout(bpb, bw, layers);
            let lin = rng.bytes(lin_total as usize);
            let mut blob = rng.bytes(total as usize); // (the padding is garbage, as in the game's files)
            let (wb, h) = (wbytes as usize, rows as usize);
            for l in 0..layers as usize {
                for y in 0..h {
                    for x in 0..wb {
                        blob[l * ls as usize + ref_offset(x, y, 0, wb, h, th, 0)] = lin[l * wb * h + y * wb + x];
                    }
                }
            }
            let a = rng.bytes(1000);
            let mut rec = vec![0u8; 0x34];
            rec[..4].copy_from_slice(&[1, 4, 2, layers as u8]);
            rec[0x1a..0x1c].copy_from_slice(&[0x4f, 0x7a]);
            rec[0x1c..0x20].copy_from_slice(&((0x34 - 0x1c) as i32).to_le_bytes());
            rec[0x20..0x24].copy_from_slice(&(a.len() as u32).to_le_bytes());
            rec[0x2c..0x30].copy_from_slice(&((0x34 + a.len() - 0x2c) as i32).to_le_bytes());
            rec[0x30..0x34].copy_from_slice(&(total as u32).to_le_bytes());
            rec.extend_from_slice(&a);
            rec.extend_from_slice(&blob);
            let frame = zstd::compress(zstd::Format::Std, &rec, 3, false);
            let mut f = b"TARB".to_vec();
            f.extend_from_slice(&1u32.to_le_bytes());
            f.extend_from_slice(&1u16.to_le_bytes());
            f.extend_from_slice(&0x14u16.to_le_bytes());
            f.extend_from_slice(&(rec.len() as u32).to_le_bytes());
            f.extend_from_slice(&(frame.len() as u32).to_le_bytes());
            f.extend_from_slice(&frame);
            let new = convert_tarb(&f).unwrap();
            let (usz, csz) = (u32_at(&new, 12).unwrap() as usize, u32_at(&new, 16).unwrap() as usize);
            assert_eq!((&new[..4], u16_at(&new, 10).unwrap(), 0x14 + csz), (&b"TARB"[..], 0x14, new.len()));
            let out = zstd::decompress(zstd::Format::Std, &new[0x14..], usz).unwrap();
            let b0 = 0x34 + a.len();
            assert!(out[b0..] == lin[..], "{bpb} {bw} {layers}: the texels, layer after layer");
            assert_eq!(out[..0x30], rec[..0x30]);
            assert_eq!(out[0x34..b0], rec[0x34..b0]);
            assert_eq!(u32_at(&out, 0x30).unwrap() as u64, lin_total);
        }
        assert!(convert_tarb(b"TARX").unwrap_err().contains("not a TARB"));
    }

    #[test]
    fn sky_surfaces_made_linear() {
        assert_eq!([sky_tiles(64, 1), sky_tiles(32, 16), sky_tiles(32, 1), sky_tiles(8, 4)], [(3, 0), (0, 4), (2, 0), (0, 2)]);
        let mut rng = Rng(5);
        let lin_total: usize = SKY_SURFACES.iter().map(|&(w, h, d, b)| (w * h * d * b) as usize).sum();
        assert_eq!(lin_total, 0x128000);
        let lin = rng.bytes(lin_total);
        let mut bl = vec![0u8; lin_total];
        let (mut off, mut p) = (0usize, 0usize);
        for &(w, h, d, bpp) in &SKY_SURFACES {
            let (wb, (th, td)) = ((w * bpp) as usize, sky_tiles(h, d));
            for z in 0..d as usize {
                for y in 0..h as usize {
                    for x in 0..wb {
                        bl[off + ref_offset(x, y, z, wb, h as usize, th, td)] = lin[p];
                        p += 1;
                    }
                }
            }
            off += wb * (h * d) as usize;
        }
        assert!(skybin_linear(&bl).unwrap() == lin);
        assert!(skybin_linear(&bl[1..]).is_err());
    }
}
