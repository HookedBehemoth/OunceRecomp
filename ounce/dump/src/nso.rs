//! The exefs's modules after extraction: one an update does not hold whole taken from another title, and ZBIC
//! (NSO flags bit 7, firmware 22+) segments rewritten as a plain NSO, which the lift reads.

use crate::extract::extraction_path;
use crate::nup::{Nup, Title, NONE};
use crate::zstd::{self, Format};
use sha2::{Digest, Sha256};
use std::path::Path;

pub const NAMES: [&str; 13] = ["rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk"];
const ZBIC_FLAG: u32 = 1 << 7;

/// Each module of `t`'s exefs that is not an NSO taken from another title of the package; what it did, a line each.
pub fn heal(nup: &Nup, t: &Title, out: &Path) -> Result<Vec<String>, String> {
    let mut notes = Vec::new();
    for m in NAMES {
        let p = extraction_path(out, "exefs", m, ".part")?;
        let have: Vec<u8> = std::fs::read(&p).map(|b| b[..b.len().min(16)].to_vec()).unwrap_or_default();
        if have.starts_with(b"NSO0") {
            continue;
        }
        let mut found = false;
        'titles: for other in nup.titles.iter().rev() {
            if other.index == t.index || other.exefs == NONE {
                continue;
            }
            for (rel, i) in nup.walk(other.exefs)? {
                let r = &nup.files[i];
                if rel != m {
                    continue;
                }
                let data = nup.read(r, &rel).map_err(|e| format!("exefs/{m}: {e}"))?;
                if data.starts_with(b"NSO0") {
                    let tmp = p.with_file_name(format!("{m}.part"));
                    std::fs::write(&tmp, &data).and_then(|_| std::fs::rename(&tmp, &p)).map_err(|e| format!("{}: {e}", p.display()))?;
                    notes.push(format!("{m}: taken from title {} (v{})", other.index, other.version));
                    found = true;
                    break 'titles;
                }
            }
        }
        if !found && !have.is_empty() {
            notes.push(format!("{m} is not a game module in any title: it starts {}", have.iter().map(|b| format!("{b:02x}")).collect::<String>()));
        }
    }
    Ok(notes)
}

#[derive(Debug, PartialEq, Eq)]
pub enum Unpacked {
    NotNso,
    /// LZ4 or uncompressed: kept
    Kept,
    /// ZBIC: rewritten plain
    Plain,
}

/// Rewrites a ZBIC NSO as a plain one in place, through `path` + ".plain". The segment hashes are of the
/// uncompressed data, so they stay valid; they are checked.
pub fn unpack_nso(path: &Path, tmp: &Path) -> Result<Unpacked, String> {
    let raw = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    if !raw.starts_with(b"NSO0") || raw.len() < 0x100 {
        return Ok(Unpacked::NotNso);
    }
    let u32_at = |o: usize| u32::from_le_bytes(raw[o..o + 4].try_into().unwrap());
    let flags = u32_at(0xc);
    if flags & ZBIC_FLAG == 0 {
        return Ok(Unpacked::Kept);
    }
    let name = path.display();
    let mut segs = Vec::new();
    for i in 0..3 {
        let (foff, size, csize) = (u32_at(0x10 + 0x10 * i) as usize, u32_at(0x10 + 0x10 * i + 8) as usize, u32_at(0x60 + 4 * i) as usize);
        let d = raw.get(foff..foff + csize).ok_or_else(|| format!("{name} segment {i}: outside the file"))?;
        let d = if flags & (1 << i) != 0 {
            zstd::decompress(Format::Zbic, d, size).map_err(|e| format!("{name} segment {i}: ZBIC: {e}"))?
        } else {
            d.to_vec()
        };
        if d.len() != size {
            return Err(format!("{name} segment {i}: {} bytes, {size} expected", d.len()));
        }
        if flags & (8 << i) != 0 && Sha256::digest(&d)[..] != raw[0xa0 + 0x20 * i..0xc0 + 0x20 * i] {
            return Err(format!("{name} segment {i}: hash mismatch after ZBIC"));
        }
        segs.push((foff, d));
    }
    // (the header, then what lies between it and the first segment: the module's name, which the header points at)
    let first = segs.iter().map(|s| s.0).min().unwrap().max(0x100);
    let mut hdr = raw[..0x100].to_vec();
    let mut body = raw.get(0x100..first).ok_or_else(|| format!("{name}: a segment inside the header"))?.to_vec();
    for (i, (_, d)) in segs.iter().enumerate() {
        hdr[0x10 + 0x10 * i..0x14 + 0x10 * i].copy_from_slice(&(0x100 + body.len() as u32).to_le_bytes());
        hdr[0x60 + 4 * i..0x64 + 4 * i].copy_from_slice(&(d.len() as u32).to_le_bytes());
        body.extend_from_slice(d);
    }
    // (uncompressed; the hash bits stay)
    hdr[0xc..0x10].copy_from_slice(&(flags & !(ZBIC_FLAG | 7)).to_le_bytes());
    hdr.extend_from_slice(&body);
    std::fs::write(tmp, &hdr).and_then(|_| std::fs::rename(tmp, path)).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(Unpacked::Plain)
}

/// `unpack_nso` for each module in `out/exefs`; a line for each rewritten.
pub fn unpack_exefs(out: &Path) -> Result<Vec<String>, String> {
    let mut notes = Vec::new();
    for m in NAMES {
        let p = extraction_path(out, "exefs", m, ".plain")?;
        if p.exists() && unpack_nso(&p, &p.with_file_name(format!("{m}.plain")))? == Unpacked::Plain {
            notes.push(format!("{m}: ZBIC -> plain NSO"));
        }
    }
    Ok(notes)
}
