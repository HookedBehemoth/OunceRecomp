//! The BC encoders and decoders (src/encoders.cpp) and the ASTC decoder, split into bands of block rows that run in
//! parallel.

use crate::plan::Enc;
use rayon::prelude::*;

extern "C" {
    fn hk_encode(kind: i32, w: u32, h: u32, rgba: *const u8, out: *mut u8, c0: i32, c1: i32, level: i32, radius: i32) -> i32;
    fn hk_decode(kind: i32, w: u32, h: u32, blocks: *const u8, rgba: *mut u8) -> i32;
}

/// blocks a band holds at most (a quarter of a second of BC7 at the slow level)
const BAND_BLOCKS: u32 = 16384;

fn code(e: Enc) -> (i32, usize) {
    match e {
        Enc::Bc1 => (1, 8),
        Enc::Bc4 => (4, 8),
        Enc::Bc5 => (5, 16),
        Enc::Bc7 => (7, 16),
        Enc::Raw => unreachable!("raw has no encoder"),
    }
}

fn band_rows(bw: u32) -> u32 {
    (BAND_BLOCKS / bw.max(1)).max(1)
}

/// RGBA8 rows (w x h) -> BC blocks, row-major. `level` is bc7e's (0 ultrafast .. 6 slowest), `radius` the BC4/BC5
/// endpoint search's.
pub fn encode(e: Enc, w: u32, h: u32, rgba: &[u8], c0: usize, c1: usize, level: i32, radius: i32) -> Vec<u8> {
    let (k, bpb) = code(e);
    let (bw, bh) = (w.div_ceil(4), h.div_ceil(4));
    let mut out = vec![0u8; bw as usize * bh as usize * bpb];
    let rows = band_rows(bw);
    out.par_chunks_mut(rows as usize * bw as usize * bpb).enumerate().for_each(|(i, o)| {
        let y0 = i as u32 * rows * 4;
        let hh = (h - y0).min(rows * 4);
        let src = &rgba[y0 as usize * w as usize * 4..];
        let lvl = if e == Enc::Bc7 { level } else { -1 };
        // SAFETY: src holds hh rows of w RGBA texels; o holds their blocks
        let r = unsafe { hk_encode(k, w, hh, src.as_ptr(), o.as_mut_ptr(), c0 as i32, c1 as i32, lvl, radius) };
        assert_eq!(r, 0, "hk_encode kind {k}");
    });
    out
}

/// One call over the whole image (the bands' reference).
#[cfg(test)]
pub fn encode_whole(e: Enc, w: u32, h: u32, rgba: &[u8], c0: usize, c1: usize, level: i32, radius: i32) -> Vec<u8> {
    let (k, bpb) = code(e);
    let mut out = vec![0u8; (w.div_ceil(4) * h.div_ceil(4)) as usize * bpb];
    let lvl = if e == Enc::Bc7 { level } else { -1 };
    // SAFETY: rgba holds the image, out its blocks
    assert_eq!(unsafe { hk_encode(k, w, h, rgba.as_ptr(), out.as_mut_ptr(), c0 as i32, c1 as i32, lvl, radius) }, 0);
    out
}

/// BC blocks -> RGBA8 rows (w x h; BC4: R, BC5: R G, the rest 0, alpha 255).
pub fn decode(e: Enc, w: u32, h: u32, blocks: &[u8]) -> Vec<u8> {
    let (k, bpb) = code(e);
    let bw = w.div_ceil(4);
    let mut out = vec![0u8; w as usize * h as usize * 4];
    let rows = band_rows(bw);
    out.par_chunks_mut(rows as usize * 4 * w as usize * 4).enumerate().for_each(|(i, o)| {
        let y0 = i as u32 * rows * 4;
        let hh = (h - y0).min(rows * 4);
        let src = &blocks[(i as u32 * rows * bw) as usize * bpb..];
        // SAFETY: src holds the band's blocks, o its rows
        let r = unsafe { hk_decode(k, w, hh, src.as_ptr(), o.as_mut_ptr()) };
        assert_eq!(r, 0, "hk_decode kind {k}");
    });
    out
}

/// ASTC blocks (row-major) -> RGBA8 rows of w x h.
pub fn astc_decode(blocks: &[u8], w: u32, h: u32, bw: u32, bh: u32) -> Result<Vec<u8>, String> {
    let nbx = w.div_ceil(bw);
    let rows = (BAND_BLOCKS / nbx.max(1)).max(1);
    let mut px = vec![0u32; w as usize * h as usize];
    px.par_chunks_mut((rows * bh) as usize * w as usize)
        .enumerate()
        .try_for_each(|(i, o)| {
            let y0 = i as u32 * rows * bh;
            let hh = (h - y0).min(rows * bh);
            let src = &blocks[(i as u32 * rows * nbx) as usize * 16..];
            texture2ddecoder::decode_astc(src, w as usize, hh as usize, bw as usize, bh as usize, o)
        })
        .map_err(|e| format!("ASTC: {e}"))?;
    // (the decoder's texels are B, G, R, A in memory)
    let mut out = Vec::with_capacity(px.len() * 4);
    for p in px {
        let [b, g, r, a] = p.to_le_bytes();
        out.extend_from_slice(&[r, g, b, a]);
    }
    Ok(out)
}
