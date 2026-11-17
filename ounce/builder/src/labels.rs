//! The game's labels in the executable's symbol table (nx2/label_syms.py): f_<address> becomes <label>.f_<address>.
//! Only .symtab's names change: the new names are added to .strtab, which lld writes last before the section
//! headers, so the loaded bytes, the map and every other section stay where they are.

use std::collections::HashMap;
use std::path::Path;

/// {address: label} of a label file: `<8 hex digits> <label> [# note]` lines, '#' comments
pub fn read(path: &Path) -> Result<HashMap<u64, String>, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut labels = HashMap::new();
    let mut seen = HashMap::new();
    for (n, line) in text.lines().enumerate() {
        let body = line.split('#').next().unwrap().trim();
        if body.is_empty() {
            continue;
        }
        let bad = || format!("{}:{}: not '<8 hex digits> <label> [# note]': {line:?}", path.display(), n + 1);
        let mut w = body.split_whitespace();
        let (Some(a), Some(name), None) = (w.next(), w.next(), w.next()) else { return Err(bad()) };
        let ok_name = name.starts_with(|c: char| c.is_ascii_lowercase())
            && name.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_');
        if a.len() != 8 || !ok_name {
            return Err(bad());
        }
        let a = u64::from_str_radix(a, 16).map_err(|_| bad())?;
        if labels.insert(a, name.to_string()).is_some() || seen.insert(name.to_string(), a).is_some() {
            return Err(format!("{}:{}: an address or a label listed twice: {line:?}", path.display(), n + 1));
        }
    }
    Ok(labels)
}

fn u16_at(b: &[u8], o: usize) -> usize {
    u16::from_le_bytes(b[o..o + 2].try_into().unwrap()) as usize
}
fn u32_at(b: &[u8], o: usize) -> usize {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap()) as usize
}
fn u64_at(b: &[u8], o: usize) -> usize {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap()) as usize
}

/// Renames in place: (functions named, the labels' addresses with no f_<address> symbol)
pub fn apply(exe: &Path, labels: &HashMap<u64, String>) -> Result<(usize, Vec<u64>), String> {
    let mut b = std::fs::read(exe).map_err(|e| format!("{}: {e}", exe.display()))?;
    if b.len() < 64 || &b[..4] != b"\x7fELF" || b[4] != 2 || b[5] != 1 {
        return Err(format!("{}: not a little-endian ELF64 file", exe.display()));
    }
    let (shoff, shentsize, shnum) = (u64_at(&b, 0x28), u16_at(&b, 0x3a), u16_at(&b, 0x3c));
    let sh = |k: usize| shoff + k * shentsize;
    let symtab = (0..shnum).find(|&k| u32_at(&b, sh(k) + 4) == 2).ok_or("no .symtab")?;
    let strtab = u32_at(&b, sh(symtab) + 0x28);
    let (str_off, str_size) = (u64_at(&b, sh(strtab) + 0x18), u64_at(&b, sh(strtab) + 0x20));
    if str_off + str_size > shoff || b[str_off + str_size..shoff].iter().any(|&x| x != 0) {
        return Err("the symbol names are not the last section before the section headers".into());
    }
    let (sym_off, sym_size) = (u64_at(&b, sh(symtab) + 0x18), u64_at(&b, sh(symtab) + 0x20));
    let name_at = |b: &[u8], o: usize| -> String {
        let s = &b[str_off + o..str_off + str_size];
        String::from_utf8_lossy(&s[..s.iter().position(|&c| c == 0).unwrap_or(s.len())]).into_owned()
    };
    let mut names = b[str_off..str_off + str_size].to_vec();
    let (mut named, mut found) = (0usize, std::collections::HashSet::new());
    for s in (sym_off..sym_off + sym_size).step_by(24) {
        let name = name_at(&b, u32_at(&b, s));
        let Some(hex) = name.strip_prefix("f_").or_else(|| name.split_once(".f_").map(|x| x.1)) else { continue };
        let Some(a) = u64::from_str_radix(hex, 16).ok().filter(|_| hex.len() == 8) else { continue };
        let Some(label) = labels.get(&a) else { continue };
        found.insert(a);
        if name.contains(".f_") {
            named += 1;   // (labelled already: a second run changes nothing)
            continue;
        }
        let at = names.len() as u32;
        names.extend_from_slice(format!("{label}.f_{a:08x}").as_bytes());
        names.push(0);
        b[s..s + 4].copy_from_slice(&at.to_le_bytes());
        named += 1;
    }
    let mut missing: Vec<u64> = labels.keys().copied().filter(|a| !found.contains(a)).collect();
    missing.sort();
    if names.len() != str_size {
        let headers = b[shoff..shoff + shnum * shentsize].to_vec();
        b.truncate(str_off);
        b.extend_from_slice(&names);
        while b.len() % 8 != 0 {
            b.push(0);
        }
        let new_shoff = b.len();
        b.extend_from_slice(&headers);
        b[0x28..0x30].copy_from_slice(&(new_shoff as u64).to_le_bytes());
        let st = new_shoff + strtab * shentsize;
        b[st + 0x20..st + 0x28].copy_from_slice(&(names.len() as u64).to_le_bytes());
        let tmp = exe.with_extension("labels.tmp");
        std::fs::write(&tmp, &b).map_err(|e| format!("{}: {e}", tmp.display()))?;
        let perm = std::fs::metadata(exe).map_err(|e| e.to_string())?.permissions();
        std::fs::set_permissions(&tmp, perm).map_err(|e| e.to_string())?;
        std::fs::rename(&tmp, exe).map_err(|e| format!("{}: {e}", exe.display()))?;
    }
    Ok((named, missing))
}
