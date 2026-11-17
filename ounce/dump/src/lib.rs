//! A player's dump read into the folder the lift and the game read: `<out>/exefs` (plain NSOs) and `<out>/romfs`. The
//! dump is a .nup package or a folder holding `exefs/` and `romfs/`. The exefs comes first, so the lift can start
//! while the romfs is still being written.

pub mod extract;
pub mod nso;
pub mod nup;
pub mod standin;
pub mod zstd;

#[cfg(test)]
mod tests;

use extract::{Output, Progress, Written};
use nup::{Nup, Part, Title};
use std::collections::BTreeMap;
use std::io;
use std::path::{Path, PathBuf};

pub enum Source {
    Nup { nup: Nup, title: Title },
    Folder(PathBuf),
}

pub struct Dump {
    pub source: Source,
    pub out: PathBuf,
}

/// What writing one part did.
#[derive(Debug, Default)]
pub struct Done {
    pub written: Written,
    /// the dump's folder is the output: nothing copied
    pub in_place: bool,
    /// a line for each module taken from another title, unpacked, or file given a stand-in
    pub notes: Vec<String>,
}

impl Dump {
    /// A folder (with `exefs/` and `romfs/`) or a .nup (`title`: its index, version or id; default the newest), to be
    /// written under `out`.
    pub fn open(path: &Path, title: Option<&str>, out: &Path) -> Result<Dump, String> {
        let source = if path.is_dir() {
            for p in ["exefs", "romfs"] {
                if !path.join(p).is_dir() {
                    return Err(format!("{}: a dump folder holds exefs/ and romfs/; {p}/ is missing", path.display()));
                }
            }
            Source::Folder(path.to_path_buf())
        } else {
            let nup = Nup::open(path, false)?;
            let title = nup.pick_title(title)?.clone();
            Source::Nup { nup, title }
        };
        Ok(Dump { source, out: out.to_path_buf() })
    }

    pub fn describe(&self) -> String {
        match &self.source {
            Source::Nup { nup, title } => format!(
                "{}: {:.2} GB, {} titles; title {} ({:016x} v{})",
                nup.path.display(),
                nup.file_size as f64 / 1e9,
                nup.titles.len(),
                title.index,
                title.id,
                title.version
            ),
            Source::Folder(p) => format!("{} (a folder)", p.display()),
        }
    }

    /// `<out>/exefs`, its modules made whole and plain.
    pub fn exefs(&self, progress: &mut Progress) -> Result<Done, String> {
        let mut d = self.part(Part::Exefs, progress)?;
        if let Source::Nup { nup, title } = &self.source {
            d.notes.extend(nso::heal(nup, title, &self.out)?);
        }
        d.notes.extend(nso::unpack_exefs(&self.out)?);
        Ok(d)
    }

    /// The exefs in memory, as `exefs` would write it: the modules (made whole and plain) and main.npdm, by name.
    pub fn exefs_files(&self) -> Result<(BTreeMap<String, Vec<u8>>, Vec<String>), String> {
        let wanted = |n: &str| n == "main.npdm" || nso::NAMES.contains(&n);
        let mut files = BTreeMap::new();
        let mut notes = Vec::new();
        match &self.source {
            Source::Nup { nup, title } => {
                if title.exefs == nup::NONE {
                    return Err(format!("title {} has no exefs", title.index));
                }
                for (rel, i) in nup.walk(title.exefs)? {
                    if wanted(&rel) {
                        let data = nup.read(&nup.files[i], &rel).map_err(|e| format!("exefs/{rel}: {e}"))?;
                        files.insert(rel, data);
                    }
                }
                notes.extend(nso::heal_files(nup, title, &mut files)?);
            }
            Source::Folder(src) => {
                for e in std::fs::read_dir(src.join("exefs")).map_err(|e| format!("{}: {e}", src.display()))?.flatten() {
                    let n = e.file_name().to_string_lossy().into_owned();
                    if wanted(&n) {
                        files.insert(n, std::fs::read(e.path()).map_err(|err| format!("{}: {err}", e.path().display()))?);
                    }
                }
            }
        }
        for (n, b) in files.iter_mut() {
            if let (_, Some(plain)) = nso::plain_nso(n, b)? {
                *b = plain;
                notes.push(format!("{n}: ZBIC -> plain NSO"));
            }
        }
        Ok((files, notes))
    }

    /// `<out>/romfs`, its damaged .zs files given stand-ins.
    pub fn romfs(&self, progress: &mut Progress) -> Result<Done, String> {
        let mut d = self.part(Part::Romfs, progress)?;
        if !d.in_place {
            for (p, other) in standin::stand_in_damaged(&self.out.join("romfs"), &mut |_, _| {})? {
                d.notes.push(match other {
                    Some(o) => format!("{} is damaged: replaced by a copy of {}", p.display(), o.display()),
                    None => format!("{} is damaged and has no stand-in", p.display()),
                });
            }
        }
        Ok(d)
    }

    fn part(&self, part: Part, progress: &mut Progress) -> Result<Done, String> {
        match &self.source {
            Source::Nup { nup, title } => {
                let dir = part.dir(title);
                if dir == nup::NONE {
                    return Err(format!("title {} has no {}", title.index, part.name()));
                }
                let members = nup.walk(dir)?;
                let files: Vec<(String, u64)> = members.iter().map(|(p, i)| (p.clone(), nup.files[*i].size)).collect();
                let mut copy = |i: usize, o: &mut Output| -> io::Result<()> {
                    let (rel, k) = &members[i];
                    nup.copy_to(&nup.files[*k], o, rel, None)
                };
                Ok(Done { written: extract::extract(&self.out, part, &files, &mut copy, progress)?, ..Done::default() })
            }
            Source::Folder(src) => {
                let from = src.join(part.name());
                let to = self.out.join(part.name());
                if to.canonicalize().is_ok_and(|t| from.canonicalize().is_ok_and(|f| f == t)) {
                    return Ok(Done { in_place: true, ..Done::default() });
                }
                let files = extract::list_folder(&from)?;
                let mut copy = |i: usize, o: &mut Output| -> io::Result<()> {
                    // (File to File: copy_file_range, which shares the data on a file system that can)
                    let n = io::copy(&mut std::fs::File::open(from.join(&files[i].0))?, &mut o.file)?;
                    o.wrote(n);
                    Ok(())
                };
                Ok(Done { written: extract::extract(&self.out, part, &files, &mut copy, progress)?, ..Done::default() })
            }
        }
    }
}
