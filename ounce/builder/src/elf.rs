//! What an ELF file needs of the system it runs on: its DT_NEEDED libraries and the symbol versions it asks of them
//! (`.gnu.version_r`). `ounce tool portable <file>...` checks a release's programs against the sysroot's glibc.

use std::collections::BTreeSet;
use std::path::Path;

/// The libraries a release's programs may name: glibc's and libgcc's, which every Linux has. The rest (Vulkan,
/// SDL, X11) is loaded at run time.
pub const SYSTEM: &[&str] = &["libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1", "libgcc_s.so.1", "ld-linux-x86-64.so.2"];

pub struct Needs {
    pub libs: Vec<String>,
    /// (library, version name), such as ("libc.so.6", "GLIBC_2.34")
    pub versions: BTreeSet<(String, String)>,
}

fn u16_at(d: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes(d.get(at..at + 2)?.try_into().ok()?))
}
fn u32_at(d: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(d.get(at..at + 4)?.try_into().ok()?))
}
fn u64_at(d: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_le_bytes(d.get(at..at + 8)?.try_into().ok()?))
}
fn cstr(d: &[u8], at: usize) -> String {
    let s = d.get(at..).unwrap_or(&[]);
    String::from_utf8_lossy(&s[..s.iter().position(|&b| b == 0).unwrap_or(s.len())]).into_owned()
}

/// A 64-bit little-endian ELF file's needs; None for anything else (a static file has none)
pub fn needs(d: &[u8]) -> Option<Needs> {
    if d.get(..6)? != b"\x7fELF\x02\x01" {
        return None;
    }
    let (shoff, shentsize, shnum) = (u64_at(d, 0x28)? as usize, u16_at(d, 0x3a)? as usize, u16_at(d, 0x3c)? as usize);
    // (sh_type, sh_offset, sh_size, sh_link) of every section
    let secs: Vec<(u32, usize, usize, usize)> = (0..shnum)
        .filter_map(|k| {
            let h = shoff + k * shentsize;
            Some((u32_at(d, h + 4)?, u64_at(d, h + 0x18)? as usize, u64_at(d, h + 0x20)? as usize, u32_at(d, h + 0x28)? as usize))
        })
        .collect();
    let mut n = Needs { libs: Vec::new(), versions: BTreeSet::new() };
    for &(ty, off, size, link) in &secs {
        let strtab = secs.get(link).map_or(0, |s| s.1);
        match ty {
            // SHT_DYNAMIC: DT_NEEDED entries
            6 => {
                for e in (off..off + size).step_by(16) {
                    match u64_at(d, e)? {
                        0 => break,
                        1 => n.libs.push(cstr(d, strtab + u64_at(d, e + 8)? as usize)),
                        _ => {}
                    }
                }
            }
            // SHT_GNU_verneed: per library (vn_cnt, vn_file, vn_aux, vn_next), its versions (vna_name, vna_next)
            0x6fff_fffe => {
                let mut vn = off;
                loop {
                    let (cnt, file, aux, next) = (u16_at(d, vn + 2)?, u32_at(d, vn + 4)?, u32_at(d, vn + 8)?, u32_at(d, vn + 12)?);
                    let lib = cstr(d, strtab + file as usize);
                    let mut a = vn + aux as usize;
                    for _ in 0..cnt {
                        n.versions.insert((lib.clone(), cstr(d, strtab + u32_at(d, a + 8)? as usize)));
                        a += u32_at(d, a + 12)? as usize;
                    }
                    if next == 0 {
                        break;
                    }
                    vn += next as usize;
                }
            }
            _ => {}
        }
    }
    Some(n)
}

/// "GLIBC_2.34" -> (2, 34)
fn glibc_version(v: &str) -> Option<(u32, u32)> {
    let mut p = v.strip_prefix("GLIBC_")?.split('.').map(|x| x.parse::<u32>().ok());
    Some((p.next()??, p.next().flatten().unwrap_or(0)))
}

/// One line about `path`'s needs; Err when it names a library outside SYSTEM or a glibc newer than the sysroot's
pub fn check(path: &Path) -> Result<String, String> {
    let d = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let Some(n) = needs(&d) else { return Ok(format!("{}: not a dynamic ELF file", path.display())) };
    let mut bad: Vec<String> = n.libs.iter().filter(|l| !SYSTEM.contains(&l.as_str())).cloned().collect();
    let newest = n.versions.iter().filter_map(|(_, v)| glibc_version(v)).max().unwrap_or((0, 0));
    if newest > crate::sysroot::GLIBC {
        let over: Vec<&str> = n.versions.iter().filter(|(_, v)| glibc_version(v).is_some_and(|g| g > crate::sysroot::GLIBC)).map(|(_, v)| v.as_str()).collect();
        bad.push(over.join(" "));
    }
    // (a C++ library linked dynamically: the player's may be older)
    bad.extend(n.versions.iter().filter(|(_, v)| v.starts_with("GLIBCXX_") || v.starts_with("CXXABI_")).map(|(l, v)| format!("{l} {v}")));
    let line = format!("{}: needs {}; glibc {}.{} at the newest", path.display(), n.libs.join(" "), newest.0, newest.1);
    if bad.is_empty() { Ok(line) } else { Err(format!("{line}; not portable: {}", bad.join(", "))) }
}

/// `ounce tool portable <file>...`
pub fn command(args: &[String]) {
    if args.is_empty() {
        crate::fail("tool portable <file>...");
    }
    let mut ok = true;
    for a in args {
        match check(Path::new(a)) {
            Ok(l) => println!("{l}"),
            Err(l) => {
                println!("{l}");
                ok = false;
            }
        }
    }
    if !ok {
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn reads_its_own_needs() {
        let d = std::fs::read(std::env::current_exe().unwrap()).unwrap();
        let n = super::needs(&d).unwrap();
        assert!(n.libs.iter().any(|l| l == "libc.so.6"));
        assert!(n.versions.iter().any(|(l, v)| l == "libc.so.6" && v.starts_with("GLIBC_2.")));
    }
}
