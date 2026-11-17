//! A romfs file for the game folder's copy: every BNTX's texel data zeroed and the file compressed again. The game
//! reads a texture file's headers; the back end takes the texels from the prepared store and never from the file.

use crate::bntx::{u32_at, u64_at};
use crate::romfs::{sarc_unpack, Dicts};
use ounce_dump::zstd;
use std::ops::Range;

/// What hollowing a file did.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct Hollowed {
    /// texel bytes zeroed (of the decompressed file)
    pub zeroed: u64,
    /// BNTX files left whole: inside a compressed member of an archive
    pub kept: u32,
}

/// The texel ranges of the BNTX files in `b` (`base`: where `b` starts in the file), as `find_bntx` finds them; BNTX
/// files inside a compressed archive member are counted in `kept`.
fn texel_ranges(dicts: &Dicts, b: &[u8], base: usize, depth: u32, out: &mut Vec<Range<usize>>, kept: &mut u32) {
    if b.starts_with(&zstd::MAGIC) {
        if let Ok(inner) = dicts.decompress(b) {
            let mut n = Vec::new();
            texel_ranges(dicts, &inner, 0, depth + 1, &mut n, kept);
            *kept += n.len() as u32;
        }
        return;
    }
    if b.starts_with(b"SARC") && depth < 4 {
        if let Ok(members) = sarc_unpack(b) {
            for (_, v) in members {
                let at = v.as_ptr() as usize - b.as_ptr() as usize;
                texel_ranges(dicts, v, base + at, depth + 1, out, kept);
            }
            return;
        }
    }
    let finder = memchr::memmem::Finder::new(b"BNTX");
    let mut o = finder.find(b);
    while let Some(p) = o {
        if p % 8 == 0 && b.get(p + 8..p + 12) == Some(&[1, 1, 4, 0]) {
            let Ok(size) = u32_at(b, p + 0x1c) else { return };
            let end = (p + size as usize).min(b.len());
            let x = &b[p..end];
            // (the texels: after the BRTD section's header, up to the relocation table that follows them)
            if let (Ok(rlt), Ok(brtd)) = (u32_at(x, 0x18), u64_at(x, 0x30)) {
                let lo = (brtd as usize).saturating_add(0x10);
                let hi = if rlt > 0 && rlt as usize <= x.len() { rlt as usize } else { x.len() };
                if lo < hi && x.get(brtd as usize..brtd as usize + 4) == Some(b"BRTD") {
                    out.push(base + p + lo..base + p + hi);
                }
            }
            let next = (p + (size as usize).max(4)).min(b.len());
            o = finder.find(&b[next..]).map(|q| q + next);
        } else {
            o = finder.find(&b[p + 4..]).map(|q| q + p + 4);
        }
    }
}

/// `data` (a romfs file) with its BNTX texels zeroed, compressed again as it was (the same dictionary and checksum
/// flag) at `level`; None when it holds no texels to zero.
pub fn hollow(dicts: &Dicts, data: &[u8], level: i32) -> Result<Option<(Vec<u8>, Hollowed)>, String> {
    let framed = data.starts_with(&zstd::MAGIC);
    let mut b = dicts.decompress(data)?.into_owned();
    let (mut ranges, mut h) = (Vec::new(), Hollowed::default());
    texel_ranges(dicts, &b, 0, 0, &mut ranges, &mut h.kept);
    if ranges.is_empty() {
        return Ok(None);
    }
    for r in ranges {
        h.zeroed += r.len() as u64;
        b[r].fill(0);
    }
    if !framed {
        return Ok(Some((b, h)));
    }
    // (the frame header's descriptor: bit 2, a content checksum follows the frame)
    let checksum = data.get(4).is_some_and(|d| d & 4 != 0);
    let out = match dicts.of_frame(data)? {
        Some(dict) => zstd::compress_with(&b, dict, level, checksum)?,
        None => zstd::compress(zstd::Format::Std, &b, level, checksum),
    };
    Ok(Some((out, h)))
}
