//! Writing a dump's files under an output folder: names checked, each file written to a temporary and renamed, links
//! refused, an interrupted run resumed, a full disk reported with the file and the room left.

use crate::nup::Part;
use std::fs::File;
use std::io::{self, Write};
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

const WINDOWS_DEVICES: [&str; 6] = ["CON", "CONIN$", "CONOUT$", "PRN", "AUX", "NUL"];

fn is_device(stem: &str) -> bool {
    let s = stem.trim_end_matches(' ').to_uppercase();
    if WINDOWS_DEVICES.contains(&s.as_str()) {
        return true;
    }
    let mut c = s.chars();
    let p: String = c.by_ref().take(3).collect();
    let rest: Vec<char> = c.collect();
    (p == "COM" || p == "LPT") && rest.len() == 1 && "123456789¹²³".contains(rest[0])
}

/// The path `rel` (a package's name, '/'-separated) takes under `out/part`. Refused: names that could leave the
/// folder or that Windows cannot hold, and any link on the way, at the file or at its temporary (`rel` +
/// `temporary`), and a temporary with other hard links.
pub fn extraction_path(out: &Path, part: &str, rel: &str, temporary: &str) -> Result<PathBuf, String> {
    let parts: Vec<&str> = rel.split('/').collect();
    let unsafe_name = rel.is_empty()
        || rel.contains('\\')
        || parts.iter().any(|p| {
            p.is_empty()
                || *p == "."
                || *p == ".."
                || p.ends_with(' ')
                || p.ends_with('.')
                || p.chars().any(|c| "<>:\"|?*".contains(c) || (c as u32) < 32)
                || is_device(p.split('.').next().unwrap_or(""))
        });
    if unsafe_name {
        return Err(format!("unsafe path in the package: {rel:?}"));
    }
    let root = real_path(out)?;
    let mut at = root.clone();
    let mut paths = vec![at.clone()];
    for p in std::iter::once(part).chain(parts.iter().copied()) {
        at.push(p);
        paths.push(at.clone());
    }
    let dst = at;
    let mut tmp = dst.clone().into_os_string();
    tmp.push(temporary);
    let tmp = PathBuf::from(tmp);
    paths.push(tmp.clone());
    for p in &paths {
        let Ok(m) = std::fs::symlink_metadata(p) else { continue };
        if m.file_type().is_symlink() {
            return Err(format!("extraction path is a link or reparse point: {}", p.display()));
        }
        if *p == tmp && m.nlink() > 1 {
            return Err(format!("extraction temporary file has other hard links: {}", p.display()));
        }
    }
    Ok(dst)
}

/// `p` absolute with its existing part's links resolved, as Python's os.path.realpath does.
fn real_path(p: &Path) -> Result<PathBuf, String> {
    let abs = std::path::absolute(p).map_err(|e| format!("{}: {e}", p.display()))?;
    let mut base = abs.as_path();
    let mut rest = Vec::new();
    loop {
        if let Ok(c) = base.canonicalize() {
            let mut r = c;
            r.extend(rest.iter().rev());
            return Ok(r);
        }
        match (base.parent(), base.file_name()) {
            (Some(parent), Some(name)) => {
                rest.push(name.to_os_string());
                base = parent;
            }
            _ => return Ok(abs),
        }
    }
}

/// Where a part's files go as they are written, and the progress lines.
pub struct Progress {
    every: Option<Duration>,
    label: String,
    next: Option<Instant>,
    pub written: u64,
    pub files: usize,
}

impl Progress {
    /// A line every `every` (None: quiet).
    pub fn new(every: Option<Duration>, label: &str) -> Progress {
        Progress { every, label: label.into(), next: every.map(|e| Instant::now() + e), written: 0, files: 0 }
    }

    fn add(&mut self, n: u64) {
        self.written += n;
        if let (Some(every), Some(next)) = (self.every, self.next) {
            let now = Instant::now();
            if now >= next {
                println!("  {}{:.1} MiB written ({} files completed)", self.label, self.written as f64 / (1u64 << 20) as f64, self.files);
                self.next = Some(now + every);
            }
        }
    }
}

/// The open temporary a file is written to; counts what is written for the progress lines.
pub struct Output<'a> {
    pub file: File,
    progress: &'a mut Progress,
}

impl Output<'_> {
    /// Counts bytes written to `file` directly.
    pub fn wrote(&mut self, n: u64) {
        self.progress.add(n);
    }
}

impl Write for Output<'_> {
    fn write(&mut self, b: &[u8]) -> io::Result<usize> {
        let n = self.file.write(b)?;
        self.progress.add(n as u64);
        Ok(n)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.file.flush()
    }
}

/// What a part's extraction did.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct Written {
    pub files: usize,
    pub bytes: u64,
}

/// Each of `files` ((path under the part, size)) written under `out/<part>` by `copy` (its index, the open
/// temporary), unless a file of that size is already there.
pub fn extract(
    out: &Path,
    part: Part,
    files: &[(String, u64)],
    copy: &mut dyn FnMut(usize, &mut Output) -> io::Result<()>,
    progress: &mut Progress,
) -> Result<Written, String> {
    let mut w = Written::default();
    for (i, (rel, size)) in files.iter().enumerate() {
        let dst = extraction_path(out, part.name(), rel, ".part")?;
        if std::fs::metadata(&dst).is_ok_and(|m| m.len() == *size) {
            continue; // (a second run picks up where the first stopped)
        }
        let parent = dst.parent().unwrap();
        std::fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
        let mut tmp = dst.clone().into_os_string();
        tmp.push(".part");
        let tmp = PathBuf::from(tmp);
        let result = File::create(&tmp).and_then(|file| {
            let mut o = Output { file, progress };
            copy(i, &mut o)?;
            o.file.flush()?;
            drop(o);
            std::fs::rename(&tmp, &dst)
        });
        if let Err(e) = result {
            // (the room is measured before the failed file's space is freed)
            let msg = match e.raw_os_error() {
                Some(libc::ENOSPC) => format!(
                    "No space left on device writing {}/{rel} ({size} bytes): {}",
                    part.name(),
                    free_gb(parent).map_or("free space unknown".into(), |g| format!("{g:.1} GB free there"))
                ),
                _ => format!("{}/{rel}: {e}", part.name()),
            };
            let _ = std::fs::remove_file(&tmp);
            return Err(msg);
        }
        w.files += 1;
        w.bytes += size;
        progress.files += 1;
    }
    Ok(w)
}

/// The room on the disk of `dir`, in GB.
pub fn free_gb(dir: &Path) -> Option<f64> {
    use std::os::unix::ffi::OsStrExt;
    let c = std::ffi::CString::new(dir.as_os_str().as_bytes()).ok()?;
    // SAFETY: a valid C string and an out struct
    let mut s: libc::statvfs = unsafe { std::mem::zeroed() };
    (unsafe { libc::statvfs(c.as_ptr(), &mut s) } == 0).then(|| s.f_bavail as f64 * s.f_frsize as f64 / 1e9)
}

/// Every file below `dir` ((path under it, size)), sorted. A link or a name that is not UTF-8 is refused.
pub fn list_folder(dir: &Path) -> Result<Vec<(String, u64)>, String> {
    let mut out = Vec::new();
    let mut pending = vec![(dir.to_path_buf(), String::new())];
    while let Some((d, prefix)) = pending.pop() {
        let mut entries: Vec<_> = std::fs::read_dir(&d).map_err(|e| format!("{}: {e}", d.display()))?.collect::<Result<_, _>>().map_err(|e| format!("{}: {e}", d.display()))?;
        entries.sort_by_key(|e| e.file_name());
        for e in entries {
            let name = e.file_name().into_string().map_err(|n| format!("{}: a name that is not UTF-8: {n:?}", d.display()))?;
            let t = e.file_type().map_err(|e| e.to_string())?;
            if t.is_symlink() {
                return Err(format!("the dump folder holds a link: {}", e.path().display()));
            }
            if t.is_dir() {
                pending.push((e.path(), format!("{prefix}{name}/")));
            } else if t.is_file() {
                out.push((format!("{prefix}{name}"), e.metadata().map_err(|e| e.to_string())?.len()));
            }
        }
    }
    out.sort();
    Ok(out)
}
