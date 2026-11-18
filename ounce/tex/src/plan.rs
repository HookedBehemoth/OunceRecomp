//! What a texture becomes: the target format by its source format, and what of its content the encoder may change.

use crate::bntx::{fmt, is_astc, is_bc};

pub const SW_ONE: u8 = 2;
pub const SW_R: u8 = 3;
pub const SW_G: u8 = 4;
pub const SW_B: u8 = 5;
pub const SW_A: u8 = 6;
const IDENT: [u8; 4] = [SW_R, SW_G, SW_B, SW_A];

pub const VK_BC7: u32 = 145;
pub const VK_BC7_SRGB: u32 = 146;

/// The kinds of the store's texture records (byte 24).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Kind {
    None = 0,
    CopyBc = 1,
    Copy = 2,
    Bc7 = 3,
    Bc5 = 4,
    Bc4 = 5,
    Bc1 = 6,
    Raw = 7,
}

impl Kind {
    pub fn name(self) -> &'static str {
        ["none", "copy_bc", "copy", "bc7", "bc5", "bc4", "bc1", "raw"][self as usize]
    }
}

/// The encoder a plan runs: None for formats kept as they are.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Enc {
    Bc1,
    Bc4,
    Bc5,
    Bc7,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Plan {
    pub kind: Kind,
    pub vk: u32,
    pub enc: Option<Enc>,
    /// per source channel R..A, the VkComponentSwizzle that reads it from the prepared image
    pub map: [u8; 4],
    pub dropped: u8,
    /// the source channels the swizzle reads
    pub used: Vec<usize>,
    /// the channel unused colour channels are filled with
    pub fill: Option<usize>,
    pub force_alpha: bool,
}

/// VkFormat of a kept (format high byte, variant)
fn vk_kept(hi: u32, var: u32) -> Option<u32> {
    Some(match (hi, var) {
        (0x1a, 1) => 133,
        (0x1a, 6) => 134,
        (0x1b, 1) => 135,
        (0x1b, 6) => 136,
        (0x1c, 1) => 137,
        (0x1c, 6) => 138,
        (0x1d, 1) => 139,
        (0x1d, 2) => 140,
        (0x1e, 1) => 141,
        (0x1e, 2) => 142,
        (0x1f, 0xa) => 143,
        (0x1f, 5) => 144,
        (0x20, 1) => 145,
        (0x20, 6) => 146,
        (0x02, 1) => 9,
        (0x02, 2) => 10,
        (0x02, 3) => 13,
        (0x02, 4) => 14,
        (0x09, 1) => 16,
        (0x09, 2) => 17,
        (0x09, 3) => 20,
        (0x09, 4) => 21,
        (0x0a, 1) => 70,
        (0x0a, 2) => 71,
        (0x0a, 3) => 74,
        (0x0a, 4) => 75,
        (0x0a, 5) => 76,
        (0x0b, 1) => 37,
        (0x0b, 2) => 38,
        (0x0b, 3) => 41,
        (0x0b, 4) => 42,
        (0x0b, 6) => 43,
        (0x0c, 1) => 44,
        (0x0c, 6) => 50,
        (0x0f, 5) => 122,
        (0x12, 1) => 77,
        (0x12, 2) => 78,
        (0x12, 5) => 83,
        (0x14, 3) => 98,
        (0x14, 4) => 99,
        (0x14, 5) => 100,
        (0x15, 1) => 91,
        (0x15, 5) => 97,
        (0x17, 5) => 103,
        (0x19, 5) => 109,
        _ => return None,
    })
}

/// The source channels (0..3 = R..A) a BNTX swizzle reads.
pub fn used_channels(comp_sel: [u8; 4]) -> Vec<usize> {
    let mut v: Vec<usize> = comp_sel.iter().filter(|&&c| (2..=5).contains(&c)).map(|&c| c as usize - 2).collect();
    v.sort();
    v.dedup();
    v
}

/// The target of a texture. ASTC is one format per NVN format whatever the content (BC7, sRGB for sRGB; see
/// `astc_target`): a texture the game makes of an ASTC format and fills by copies from loaded ones is made in that format
/// too. Err: why there is none.
pub fn choose_target(fmt_code: u32, comp_sel: [u8; 4]) -> Result<Plan, String> {
    let (hi, var) = (fmt_code >> 8, fmt_code & 0xff);
    let none = || Err(format!("format {}", crate::bntx::fmt_name(fmt_code)));
    if fmt(hi).is_none() {
        return none();
    }
    let base = Plan { kind: Kind::None, vk: 0, enc: None, map: IDENT, dropped: 0, used: vec![0, 1, 2, 3], fill: None, force_alpha: false };
    if !is_astc(fmt_code) {
        let Some(vk) = vk_kept(hi, var) else { return none() };
        return Ok(Plan { kind: if is_bc(fmt_code) { Kind::CopyBc } else { Kind::Copy }, vk, ..base });
    }
    let used = used_channels(comp_sel);
    let dropped: u8 = (0..4).filter(|c| !used.contains(c)).map(|c| 1u8 << c).sum();
    let fill = used.iter().copied().find(|&c| c < 3);
    let force_alpha = !used.contains(&3);
    Ok(Plan { kind: Kind::Bc7, vk: astc_target(var == 6), enc: Some(Enc::Bc7), dropped, used, fill, force_alpha, ..base })
}

/// The VkFormat of every ASTC texture (renderer/vk_formats.cpp holds the same): BC7, the one BC format with ASTC's
/// four channels at 8 bits.
pub fn astc_target(srgb: bool) -> u32 {
    if srgb {
        VK_BC7_SRGB
    } else {
        VK_BC7
    }
}

/// The encoder's input (RGBA8 rows, changed in place): unused colour channels filled with the first used one (or 0),
/// alpha 255 when it is not read.
pub fn prepare_input(rgba: &mut [u8], p: &Plan) {
    if p.enc.is_none() {
        return;
    }
    let fills: Vec<usize> = (0..3).filter(|c| !p.used.contains(c)).collect();
    if fills.is_empty() && !p.force_alpha {
        return;
    }
    for px in rgba.chunks_exact_mut(4) {
        for &c in &fills {
            px[c] = p.fill.map_or(0, |f| px[f]);
        }
        if p.force_alpha {
            px[3] = 255;
        }
    }
}

/// (source channel, decoded channel) pairs the quality check compares.
pub fn compared_channels(p: &Plan) -> Vec<(usize, usize)> {
    if p.enc.is_none() {
        return Vec::new();
    }
    p.used.iter().map(|&c| (c, c)).collect()
}

/// (raw offset, size) of a texture's frames: each mip of `small` bytes or more in equal parts of at most frame_max
/// (16-byte aligned), then the tail of smaller mips together, in parts of at most frame_max.
pub fn frames_of(raw_len: u64, mip_ranges: &[(u64, u64)], frame_max: u64, small: u64) -> Vec<(u64, u64)> {
    let mut out = Vec::new();
    let mut tail: Option<u64> = None;
    for &(off, size) in mip_ranges {
        if size >= small && tail.is_none() {
            let n = size.div_ceil(frame_max);
            let part = size.div_ceil(n).div_ceil(16) * 16;
            let mut a = off;
            while a < off + size {
                out.push((a, part.min(off + size - a)));
                a += part;
            }
        } else if tail.is_none() {
            tail = Some(off);
        }
    }
    if let Some(t) = tail {
        let mut a = t;
        while a < raw_len {
            out.push((a, frame_max.min(raw_len - a)));
            a += frame_max;
        }
    }
    out
}

pub fn psnr_of(se: f64, n: u64) -> f64 {
    if n == 0 {
        return 99.0;
    }
    let mse = se / n as f64;
    if mse <= 0.0 {
        99.0
    } else {
        99f64.min(10.0 * (255.0 * 255.0 / mse).log10())
    }
}
