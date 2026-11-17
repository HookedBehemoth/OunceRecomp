//! Equality of two ELF64 relocatable objects as linker input: their allocated sections (name, type, flags,
//! alignment, bytes), their symbols other than FILE and SECTION ones (name, type, binding, visibility, section,
//! value, size) and their relocations (section, offset, type, target, addend). The file symbol and `.comment` are
//! left out, so an object compiled from IR can be compared with clang's of the C it replaces.

use std::collections::BTreeSet;

#[derive(Default)]
struct Parts {
    sections: Vec<String>,
    symbols: Vec<String>,
    relocs: Vec<String>,
}

fn u16_(d: &[u8], o: usize) -> usize {
    u16::from_le_bytes(d[o..o + 2].try_into().unwrap()) as usize
}
fn u32_(d: &[u8], o: usize) -> u64 {
    u32::from_le_bytes(d[o..o + 4].try_into().unwrap()) as u64
}
fn u64_(d: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(d[o..o + 8].try_into().unwrap())
}

fn parts(d: &[u8]) -> Result<Parts, String> {
    if d.len() < 64 || &d[..4] != b"\x7fELF" || d[4] != 2 || d[5] != 1 || u16_(d, 16) != 1 {
        return Err("not a little-endian ELF64 relocatable object".into());
    }
    let (shoff, shn, shstr) = (u64_(d, 0x28) as usize, u16_(d, 0x3c), u16_(d, 0x3e));
    // (name, type, flags, offset, size, link, info, align, entsize)
    let sh: Vec<(u64, u64, u64, usize, usize, usize, u64, u64)> = (0..shn)
        .map(|i| {
            let o = shoff + 64 * i;
            (u32_(d, o), u32_(d, o + 4), u64_(d, o + 8), u64_(d, o + 24) as usize, u64_(d, o + 32) as usize, u32_(d, o + 40) as usize,
             u32_(d, o + 44), u64_(d, o + 48))
        })
        .collect();
    let cstr = |at: usize| -> String {
        let end = d[at..].iter().position(|&b| b == 0).map_or(d.len(), |n| at + n);
        String::from_utf8_lossy(&d[at..end]).into_owned()
    };
    let names: Vec<String> = sh.iter().map(|s| cstr(sh[shstr].3 + s.0 as usize)).collect();
    let mut p = Parts::default();
    let symtab = sh.iter().position(|s| s.1 == 2).ok_or("no symbol table")?;
    let (so, sn, strtab) = (sh[symtab].3, sh[symtab].4 / 24, sh[symtab].5);
    let mut targets = Vec::with_capacity(sn);
    for k in 0..sn {
        let o = so + 24 * k;
        let (name, info, other, shndx) = (u32_(d, o) as usize, d[o + 4], d[o + 5], u16_(d, o + 6));
        let n = if name != 0 { cstr(sh[strtab].3 + name) } else { String::new() };
        let sec = if shndx > 0 && shndx < 0xff00 { names[shndx].clone() } else { format!("#{shndx:x}") };
        let t = info & 15;
        targets.push(if t == 3 { format!("section {sec}") } else { n.clone() });
        if t != 3 && t != 4 {
            p.symbols.push(format!("{n} type {t} bind {} vis {} in {sec} at {:#x} size {}", info >> 4, other & 3, u64_(d, o + 8), u64_(d, o + 16)));
        }
    }
    for (i, s) in sh.iter().enumerate() {
        if s.2 & 2 != 0 {
            let bytes = if s.1 == 8 {
                format!("{} zero bytes", s.4)
            } else {
                use sha1::Digest;
                format!("{} bytes sha1 {:x}", s.4, sha1::Sha1::digest(&d[s.3..s.3 + s.4]))
            };
            p.sections.push(format!("{} type {} flags {:#x} align {} {bytes}", names[i], s.1, s.2, s.7));
        }
        if s.1 == 4 {
            for k in 0..s.4 / 24 {
                let o = s.3 + 24 * k;
                let info = u64_(d, o + 8);
                let target = targets.get((info >> 32) as usize).cloned().unwrap_or_default();
                p.relocs.push(format!("{} +{:#x} type {} -> {target} {:+}", names[s.6 as usize], u64_(d, o), info & 0xffff_ffff, u64_(d, o + 16) as i64));
            }
        }
    }
    for v in [&mut p.sections, &mut p.symbols, &mut p.relocs] {
        v.sort_unstable();
    }
    Ok(p)
}

/// One line per part (sections, symbols, relocations): the counts and equal, or the first entry of each side that
/// the other lacks. Ok(true) when all three are equal.
pub fn compare(a: &[u8], b: &[u8]) -> Result<(bool, Vec<String>), String> {
    let (pa, pb) = (parts(a)?, parts(b)?);
    let mut lines = Vec::new();
    let mut same = true;
    for (what, x, y) in [("sections", &pa.sections, &pb.sections), ("symbols", &pa.symbols, &pb.symbols), ("relocations", &pa.relocs, &pb.relocs)] {
        if x == y {
            lines.push(format!("{what}: {} equal", x.len()));
            continue;
        }
        same = false;
        let (sx, sy): (BTreeSet<&String>, BTreeSet<&String>) = (x.iter().collect(), y.iter().collect());
        let cut = |s: &str| s.chars().take(160).collect::<String>();
        let only_a = x.iter().find(|e| !sy.contains(e)).map_or("-".into(), |e| cut(e));
        let only_b = y.iter().find(|e| !sx.contains(e)).map_or("-".into(), |e| cut(e));
        lines.push(format!("{what}: {} vs {}, DIFFER\n  only in the first: {only_a}\n  only in the second: {only_b}", x.len(), y.len()));
    }
    Ok((same, lines))
}
