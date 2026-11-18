//! One BNTX made into the store's records and compressed frames.

use crate::bntx::{self, fmt, surface_layout, Tex};
use crate::native;
use crate::plan::{choose_target, compared_channels, frames_of, prepare_input, psnr_of, Kind, Plan, SW_A, SW_B, SW_G, SW_R};
use rayon::prelude::*;

pub const FLAG_BH1: u16 = 1;
pub const FLAG_ALPHA_FORCED: u16 = 2;
pub const FLAG_ASTC_HDR: u16 = 4;
pub const FLAG_DEDUP: u16 = 8;
/// every mip's data starts at a multiple of this in the texture's raw data (vkCmdCopyBufferToImage on a transfer queue)
pub const MIP_ALIGN: u64 = 4;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
    Ok = 0,
    Refused = 1,
    Unsupported = 2,
    Error = 3,
}

/// The encoder settings; they decide the prepared bytes.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Settings {
    /// bc7e level, 0 ultrafast .. 6 slowest
    pub bc7_level: i32,
    pub zstd_level: i32,
    pub frame_kb: u64,
}

impl Settings {
    /// The store's quality.
    pub const QUALITY: Settings = Settings { bc7_level: 4, zstd_level: 19, frame_kb: 1024 };
    /// A quick build: bc7e ultrafast (about 4.5 minutes for OoT on 16 threads).
    pub const FAST: Settings = Settings { bc7_level: 0, zstd_level: 3, frame_kb: 1024 };
}

/// A texture record of the index, less where its mips and frames are.
#[derive(Debug, Clone, PartialEq)]
pub struct Rec {
    pub index: u32,
    pub name: String,
    pub nvn_format: u32,
    pub vk_format: u32,
    pub kind: Kind,
    pub status: Status,
    pub view: u8,
    pub target: u8,
    pub map: [u8; 4],
    pub comp_sel: [u8; 4],
    pub dropped: u8,
    pub bh_log2: u8,
    pub flags: u16,
    pub width: u32,
    pub height: u32,
    pub depth: u32,
    pub layers: u32,
    pub mips: u32,
    pub raw_size: u64,
    pub tex_hash: u64,
    pub brti_off: u32,
    pub image_size: u32,
    pub data_off: u32,
    pub psnr: f32,
    pub psnr_min: f32,
    pub alignment: u32,
    pub hdr_blocks: u32,
    pub rule: u8,
    pub psnr_first: f32,
}

/// One compressed frame of a texture's raw data (or of a BNTX's metadata).
#[derive(Debug, Clone)]
pub struct Frame {
    pub raw_off: u64,
    pub raw_size: u32,
    pub data: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct TexOut {
    pub rec: Rec,
    /// (offset in the raw data, bytes of one layer) per mip
    pub mips: Vec<(u64, u32)>,
    pub frames: Vec<Frame>,
}

/// One BNTX: its file record's fields and its textures.
#[derive(Debug, Clone)]
pub struct BntxOut {
    pub path: String,
    pub bntx_size: u64,
    pub head_len: u32,
    pub rlt_off: u32,
    pub meta: Frame,
    pub flags: u32,
    pub textures: Vec<TexOut>,
}

/// BLAKE2b with an 8-byte digest, as u64 (the store's hashes)
pub fn hash64(parts: &[&[u8]]) -> u64 {
    let mut s = blake2b_simd::Params::new().hash_length(8).to_state();
    for p in parts {
        s.update(p);
    }
    u64::from_le_bytes(s.finalize().as_bytes().try_into().unwrap())
}

fn view_of(target: u8) -> Option<u8> {
    Some(match target {
        0..=5 => target,
        8 => 6,
        _ => return None,
    })
}

thread_local! {
    static CCTX: std::cell::RefCell<ounce_dump::zstd::Compressor> = std::cell::RefCell::new(ounce_dump::zstd::Compressor::new());
}

/// a standard zstd frame with its content size and checksum
pub fn compress(data: &[u8], level: i32) -> Vec<u8> {
    CCTX.with(|c| c.borrow_mut().compress(data, level, true))
}

fn base_rec(b: &[u8], t: &Tex, index: u32) -> Rec {
    let data_off = t.mip_offsets.first().copied().unwrap_or(0) as u32;
    let end = (data_off as usize + t.image_size as usize).min(b.len());
    let brti = &b[(t.off + 0x10).min(b.len())..(t.off + 0x60).min(b.len())];
    Rec {
        index,
        name: t.name.clone(),
        nvn_format: t.format,
        vk_format: 0,
        kind: Kind::None,
        status: Status::Unsupported,
        view: view_of(t.target).unwrap_or(1),
        target: t.target,
        map: [SW_R, SW_G, SW_B, SW_A],
        comp_sel: t.comp_sel,
        dropped: 0,
        bh_log2: t.bh_log2 as u8,
        flags: if t.bh_log2 == 1 { FLAG_BH1 } else { 0 },
        width: t.width,
        height: t.height,
        depth: t.depth,
        layers: t.layers,
        mips: t.mips,
        raw_size: 0,
        tex_hash: hash64(&[brti, &b[(data_off as usize).min(end)..end]]),
        brti_off: t.off as u32,
        image_size: t.image_size,
        data_off,
        psnr: 0.0,
        psnr_min: 0.0,
        alignment: t.alignment,
        hdr_blocks: 0,
        rule: 0,
        psnr_first: 0.0,
    }
}

/// A texture that failed: its name and format, the rest zero.
fn error_rec(t: &Tex, index: u32) -> Rec {
    Rec {
        index,
        name: t.name.clone(),
        nvn_format: t.format,
        vk_format: 0,
        kind: Kind::None,
        status: Status::Error,
        view: 1,
        target: 0,
        map: [SW_R, SW_G, SW_B, SW_A],
        comp_sel: [2, 3, 4, 5],
        dropped: 0,
        bh_log2: 0,
        flags: 0,
        width: 0,
        height: 0,
        depth: 0,
        layers: 0,
        mips: 0,
        raw_size: 0,
        tex_hash: 0,
        brti_off: 0,
        image_size: 0,
        data_off: 0,
        psnr: 0.0,
        psnr_min: 0.0,
        alignment: 0,
        hdr_blocks: 0,
        rule: 0,
        psnr_first: 0.0,
    }
}

/// One image (a mip of a layer) as the plan makes it.
struct Image {
    data: Vec<u8>,
    se: f64,
    vals: u64,
    worst: Option<f64>,
    hdr: [u64; 4],
}

struct Encoded {
    raw: Vec<u8>,
    mips: Vec<(u64, u32)>,
    psnr: f64,
    psnr_min: f64,
    hdr: [u64; 4],
    flags: u16,
}

fn encode_plan(b: &[u8], t: &Tex, plan: &Plan, s: &Settings, rec_flags: u16) -> Result<Encoded, String> {
    let (_, bw, bhp, bpb) = fmt(t.format >> 8).unwrap();
    let (lay, stride) = surface_layout(t);
    let base = t.mip_offsets[0];
    let mut flags = rec_flags;
    if plan.force_alpha {
        flags |= FLAG_ALPHA_FORCED;
    }
    let pairs = compared_channels(plan);
    let jobs: Vec<(usize, u32)> = (0..lay.len()).flat_map(|m| (0..t.layers).map(move |l| (m, l))).collect();
    let images: Vec<Image> = jobs
        .par_iter()
        .map(|&(m, layer)| -> Result<Image, String> {
            let mip = lay[m];
            let (w, h) = ((t.width >> m).max(1), (t.height >> m).max(1));
            let off = (base + layer as u64 * stride + mip.off) as usize;
            let size = (mip.size as usize).min(b.len().saturating_sub(off));
            let src = if off < b.len() { &b[off..off + size] } else { &[][..] };
            let lin = bntx::to_linear(src, mip.wb * bpb, mip.hb, mip.bh);
            let Some(enc) = plan.enc else {
                return Ok(Image { data: lin, se: 0.0, vals: 0, worst: None, hdr: [0; 4] });
            };
            let mut hdr = [0u64; 4];
            bntx::astc_scan(&lin, &mut hdr);
            let mut rgba = native::astc_decode(&lin, w, h, bw, bhp)?;
            let vals = w as u64 * h as u64 * pairs.len() as u64;
            let src_rgba = rgba.clone();
            prepare_input(&mut rgba, plan);
            let blk = native::encode(enc, w, h, &rgba, 0, 1, s.bc7_level, 0);
            let dec = native::decode(enc, w, h, &blk);
            let mut se = 0.0;
            for &(sc, dc) in &pairs {
                let sum: i64 = dec.chunks_exact(4).zip(src_rgba.chunks_exact(4)).map(|(d, x)| (d[dc] as i64 - x[sc] as i64).pow(2)).sum();
                se += sum as f64;
            }
            let worst = (w as u64 * h as u64 >= 4096 && !pairs.is_empty()).then(|| psnr_of(se, vals));
            Ok(Image { data: blk, se, vals, worst, hdr })
        })
        .collect::<Result<_, _>>()?;
    let mut raw = Vec::with_capacity(images.iter().map(|i| i.data.len() + 4).sum());
    let mut mips = Vec::with_capacity(lay.len());
    let (mut se, mut vals, mut worst, mut hdr) = (0.0f64, 0u64, 99.0f64, [0u64; 4]);
    for (&(m, _), img) in jobs.iter().zip(images) {
        if mips.len() == m {
            let pad = (MIP_ALIGN - raw.len() as u64 % MIP_ALIGN) % MIP_ALIGN;
            raw.resize(raw.len() + pad as usize, 0);
            mips.push((raw.len() as u64, img.data.len() as u32));
        }
        raw.extend_from_slice(&img.data);
        se += img.se;
        vals += img.vals;
        if let Some(p) = img.worst {
            worst = worst.min(p);
        }
        for i in 0..4 {
            hdr[i] += img.hdr[i];
        }
    }
    let (psnr, psnr_min) = if plan.enc.is_some() {
        if hdr[0] + hdr[1] != 0 {
            flags |= FLAG_ASTC_HDR;
        }
        (if vals != 0 { psnr_of(se, vals) } else { 99.0 }, worst)
    } else {
        (99.0, 0.0)
    };
    Ok(Encoded { raw, mips, psnr, psnr_min, hdr, flags })
}

fn convert_texture(b: &[u8], t: &Tex, index: u32, s: &Settings) -> Result<TexOut, String> {
    let mut rec = base_rec(b, t, index);
    let mut out = TexOut { rec: rec.clone(), mips: Vec::new(), frames: Vec::new() };
    if fmt(t.format >> 8).is_none() || t.mips == 0 {
        return Ok(out);
    }
    if t.depth != 1 || view_of(t.target).is_none() || t.samples > 1 {
        return Ok(out);
    }
    let (_, stride) = surface_layout(t);
    let end = rec.data_off as u64 + t.image_size as u64;
    if stride * t.layers as u64 != t.image_size as u64 || end > b.len() as u64 {
        return Ok(out);
    }
    let Ok(plan) = choose_target(t.format, t.comp_sel) else { return Ok(out) };
    let e = encode_plan(b, t, &plan, s, rec.flags)?;
    rec.kind = plan.kind;
    rec.vk_format = plan.vk;
    rec.map = plan.map;
    rec.dropped = plan.dropped;
    rec.flags = e.flags;
    rec.raw_size = e.raw.len() as u64;
    rec.psnr = e.psnr as f32;
    rec.psnr_min = e.psnr_min as f32;
    rec.hdr_blocks = (e.hdr[0] + e.hdr[1]).min(u32::MAX as u64) as u32;
    rec.status = Status::Ok;
    let mut ranges = Vec::with_capacity(e.mips.len());
    for (i, &(o, _)) in e.mips.iter().enumerate() {
        let next = e.mips.get(i + 1).map_or(e.raw.len() as u64, |m| m.0);
        ranges.push((o, next - o));
    }
    let parts = frames_of(e.raw.len() as u64, &ranges, s.frame_kb << 10, 64 << 10);
    out.frames = parts
        .par_iter()
        .map(|&(a, n)| Frame { raw_off: a, raw_size: n as u32, data: compress(&e.raw[a as usize..(a + n) as usize], s.zstd_level) })
        .collect();
    out.mips = e.mips;
    out.rec = rec;
    Ok(out)
}

/// Every texture of one BNTX (`path`: romfs-relative, `outer::inner` inside a container, `@0xOFF` embedded).
pub fn convert_bntx(b: &[u8], path: &str, s: &Settings) -> Result<BntxOut, String> {
    let h = bntx::parse(b)?;
    let head_len = h.brtd + 0x10;
    let rlt = if h.rlt_off > 0 && h.rlt_off as usize <= b.len() { h.rlt_off as usize } else { b.len() };
    let mut meta = b[..(head_len as usize).min(b.len())].to_vec();
    meta.extend_from_slice(&b[rlt..]);
    let textures = h
        .textures
        .par_iter()
        .enumerate()
        .map(|(i, t)| {
            convert_texture(b, t, i as u32, s)
                .unwrap_or_else(|_| TexOut { rec: error_rec(t, i as u32), mips: Vec::new(), frames: Vec::new() })
        })
        .collect();
    Ok(BntxOut {
        path: path.to_string(),
        bntx_size: b.len() as u64,
        head_len: head_len as u32,
        rlt_off: rlt as u32,
        meta: Frame { raw_off: 0, raw_size: meta.len() as u32, data: compress(&meta, s.zstd_level) },
        flags: (path.contains("::") || path.contains('@')) as u32,
        textures,
    })
}
