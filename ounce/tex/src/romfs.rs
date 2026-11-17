//! Romfs files as the converter opens them: zstd (with the game's dictionaries), SARC archives, BNTX inside either.

use ounce_dump::zstd;
use std::collections::HashMap;

use crate::bntx::{u16_at, u32_at};

/// The game's zstd dictionaries, keyed by their id.
#[derive(Default)]
pub struct Dicts(HashMap<u32, (zstd::Dict, Vec<u8>)>);

impl Dicts {
    /// The dictionaries in a zstd SARC of raw dictionaries (a romfs file the game description names).
    pub fn load(path: &std::path::Path) -> Result<Dicts, String> {
        let mut d = Dicts::default();
        if !path.exists() {
            return Ok(d);
        }
        let data = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let sarc = d.decompress(&data).map_err(|e| format!("{}: {e}", path.display()))?;
        for (_k, v) in sarc_unpack(&sarc).map_err(|e| format!("{}: {e}", path.display()))? {
            if v.starts_with(&[0x37, 0xa4, 0x30, 0xec]) && v.len() >= 8 {
                d.0.insert(u32_at(v, 4)?, (zstd::Dict::new(v)?, v.to_vec()));
            }
        }
        Ok(d)
    }

    /// The raw bytes of the dictionary a frame names (None: the frame names none).
    pub fn of_frame(&self, data: &[u8]) -> Result<Option<&[u8]>, String> {
        match zstd::dict_id_of_frame(data) {
            0 => Ok(None),
            id => self.0.get(&id).map(|d| Some(&d.1[..])).ok_or_else(|| format!("zstd dictionary {id:#x} missing")),
        }
    }

    /// The content of one zstd frame, or the bytes as they are when they are not zstd.
    pub fn decompress<'a>(&self, data: &'a [u8]) -> Result<std::borrow::Cow<'a, [u8]>, String> {
        if !data.starts_with(&zstd::MAGIC) {
            return Ok(std::borrow::Cow::Borrowed(data));
        }
        let size = zstd::frame_content_size(&data[..data.len().min(18)]).ok_or("zstd frame without its content size")?;
        let id = zstd::dict_id_of_frame(data);
        let out = if id != 0 {
            let dict = self.0.get(&id).ok_or_else(|| format!("zstd dictionary {id:#x} missing"))?;
            zstd::decompress_with(data, size as usize, &dict.0)?
        } else {
            zstd::decompress(zstd::Format::Std, data, size as usize)?
        };
        Ok(std::borrow::Cow::Owned(out))
    }
}

/// A SARC's members in its node order (a name met again keeps its first place and takes the later data); a member
/// without a name is named by its hash (%08x).
pub fn sarc_unpack(data: &[u8]) -> Result<Vec<(String, &[u8])>, String> {
    if !data.starts_with(b"SARC") {
        return Err("not a SARC".into());
    }
    let doff = u32_at(data, 0xc)? as usize;
    let n = u16_at(data, 0x14 + 6)? as usize;
    let sfnt = 0x14 + 12 + 16 * n;
    let mut out: Vec<(String, &[u8])> = Vec::with_capacity(n);
    let mut at: HashMap<String, usize> = HashMap::new();
    for i in 0..n {
        let o = 0x14 + 12 + 16 * i;
        let (h, attr, s, e) = (u32_at(data, o)?, u32_at(data, o + 4)?, u32_at(data, o + 8)? as usize, u32_at(data, o + 12)? as usize);
        let name = if attr >> 24 != 0 {
            let p = (sfnt + 8 + (attr & 0xFFFF) as usize * 4).min(data.len());
            let raw = &data[p..];
            let raw = &raw[..memchr::memchr(0, raw).unwrap_or(raw.len())];
            String::from_utf8(raw.to_vec()).map_err(|_| "a member name that is not UTF-8")?
        } else {
            format!("{h:08x}")
        };
        let (a, b) = ((doff + s).min(data.len()), (doff + e).min(data.len()));
        let v = &data[a..b.max(a)];
        match at.get(&name) {
            Some(&k) => out[k].1 = v,
            None => {
                at.insert(name.clone(), out.len());
                out.push((name, v));
            }
        }
    }
    Ok(out)
}

/// The BNTX files in a romfs file: itself, inside zstd, inside SARCs (to 4 levels), or at an 8-aligned offset
/// (`path@0xOFF`). Err only where the scan reads past the end of a candidate's header.
pub fn find_bntx(dicts: &Dicts, data: &[u8], wher: &str, depth: u32) -> Result<Vec<(String, Vec<u8>)>, String> {
    let Ok(b) = dicts.decompress(data) else { return Ok(Vec::new()) };
    if b.starts_with(b"SARC") && depth < 4 {
        if let Ok(members) = sarc_unpack(&b) {
            let mut out = Vec::new();
            let all = members.iter().try_for_each(|(k, v)| {
                out.extend(find_bntx(dicts, v, &format!("{wher}::{k}"), depth + 1)?);
                Ok::<_, String>(())
            });
            if all.is_ok() {
                return Ok(out);
            }
        }
    }
    let mut out = Vec::new();
    let finder = memchr::memmem::Finder::new(b"BNTX");
    let mut o = finder.find(&b);
    while let Some(p) = o {
        if p % 8 == 0 && b.get(p + 8..p + 12) == Some(&[1, 1, 4, 0]) {
            let size = u32_at(&b, p + 0x1c)? as usize;
            let path = if p == 0 { wher.to_string() } else { format!("{wher}@{p:#x}") };
            out.push((path, b[p..(p + size).min(b.len())].to_vec()));
            o = finder.find(&b[(p + size.max(4)).min(b.len())..]).map(|q| q + (p + size.max(4)).min(b.len()));
        } else {
            o = finder.find(&b[p + 4..]).map(|q| q + p + 4);
        }
    }
    Ok(out)
}
