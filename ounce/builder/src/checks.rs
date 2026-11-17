//! The build's checks of what it made: the back end's symbols against the executable's exports.

use std::collections::HashSet;
use std::path::Path;

/// (name, defined, weak, versioned) of every symbol in an ELF64 file's dynamic symbol table
fn dynsyms(d: &[u8]) -> Result<Vec<(String, bool, bool, bool)>, String> {
    if d.len() < 64 || &d[..4] != b"\x7fELF" || d[4] != 2 {
        return Err("not an ELF64 file".into());
    }
    let u16_ = |o: usize| u16::from_le_bytes(d[o..o + 2].try_into().unwrap()) as usize;
    let u32_ = |o: usize| u32::from_le_bytes(d[o..o + 4].try_into().unwrap()) as usize;
    let u64_ = |o: usize| u64::from_le_bytes(d[o..o + 8].try_into().unwrap()) as usize;
    let (shoff, shn) = (u64_(0x28), u16_(0x3c));
    let sh = |i: usize, f: usize| shoff + 64 * i + f;
    let find = |t: usize| (0..shn).find(|&i| u32_(sh(i, 4)) == t);
    let Some(ds) = find(11) else { return Ok(Vec::new()) };
    let (off, size, link) = (u64_(sh(ds, 24)), u64_(sh(ds, 32)), u32_(sh(ds, 40)));
    let stroff = u64_(sh(link, 24));
    let versym = find(0x6fff_ffff).map(|i| u64_(sh(i, 24)));
    let mut out = Vec::new();
    for k in 1..size / 24 {
        let o = off + 24 * k;
        let (name, info, shndx) = (u32_(o), d[o + 4], u16_(o + 6));
        let at = stroff + name;
        let end = d[at..].iter().position(|&b| b == 0).map_or(d.len(), |n| at + n);
        let n = String::from_utf8_lossy(&d[at..end]).into_owned();
        let ver = versym.map_or(0, |v| u16_(v + 2 * k) & 0x7fff);
        out.push((n, shndx != 0, info >> 4 == 2, ver > 1));
    }
    Ok(out)
}

/// The back end is loaded with RTLD_NOW: a symbol it leaves undefined that is not weak, not a system library's
/// (versioned) and not one the executable exports stops it loading, with only a black window to show for it. The
/// names of such symbols, or Ok.
pub fn backend_symbols(so: &Path, exe: &Path) -> Result<(), String> {
    let read = |p: &Path| std::fs::read(p).map_err(|e| format!("{}: {e}", p.display()));
    let lib = dynsyms(&read(so)?)?;
    let exported: HashSet<String> = dynsyms(&read(exe)?)?.into_iter().filter(|s| s.1).map(|s| s.0).collect();
    let bad: Vec<&str> = lib.iter().filter(|s| !s.1 && !s.2 && !s.3 && !s.0.is_empty() && !exported.contains(&s.0)).map(|s| s.0.as_str()).collect();
    if bad.is_empty() {
        return Ok(());
    }
    Err(format!(
        "{} has {} unresolved symbol(s); it would not load: {}{}\n  a host function wants __attribute__((weak)); one of our own globals wants an extern at namespace scope, not inside an anonymous namespace",
        so.display(),
        bad.len(),
        bad.iter().take(20).copied().collect::<Vec<_>>().join(" "),
        if bad.len() > 20 { " ..." } else { "" }
    ))
}
