//! What the release tools fetch and run (ffmpeg.rs, sysroot.rs, appdir.rs): a pinned file downloaded once by curl and
//! checked by SHA-256, and a child process whose output goes to a log.

use crate::fail;
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};
use std::process::Command;

/// A file of a pinned release: where it is and its SHA-256
pub struct Pinned {
    pub file: &'static str,
    pub url: &'static str,
    pub sha256: &'static str,
}

pub fn sha256(p: &Path) -> String {
    std::fs::read(p).map(|b| Sha256::digest(&b).iter().map(|x| format!("{x:02x}")).collect::<String>()).unwrap_or_default()
}

/// Runs `c`, its output appended to `log`; fails naming `what` and the log
pub fn run(mut c: Command, what: &str, log: &Path) {
    let prog = c.get_program().to_string_lossy().into_owned();
    let f = std::fs::OpenOptions::new().create(true).append(true).open(log).unwrap_or_else(|e| fail(format!("{}: {e}", log.display())));
    let st = c.stdout(f.try_clone().unwrap_or_else(|e| fail(e))).stderr(f).status().unwrap_or_else(|e| {
        fail(if e.kind() == std::io::ErrorKind::NotFound { format!("{what}: {prog} is not installed") } else { format!("{what}: {prog}: {e}") })
    });
    if !st.success() {
        fail(format!("{what} failed ({st}); the log is {}", log.display()));
    }
}

/// `dl/<file>` under `dir`, fetched unless it is there with its hash
pub fn fetch(dir: &Path, p: &Pinned, log: &Path) -> PathBuf {
    let file = dir.join("dl").join(p.file);
    if sha256(&file) == p.sha256 {
        return file;
    }
    std::fs::create_dir_all(dir.join("dl")).unwrap_or_else(|e| fail(e));
    let part = file.with_extension("part");
    println!("fetching {}", p.url);
    let mut c = Command::new("curl");
    c.args(["-fsSL", "--retry", "3", "-o"]).arg(&part).arg(p.url);
    run(c, &format!("the download of {}", p.file), log);
    let got = sha256(&part);
    if got != p.sha256 {
        let _ = std::fs::remove_file(&part);
        fail(format!("{}: SHA-256 {got}, not {}", p.url, p.sha256));
    }
    std::fs::rename(&part, &file).unwrap_or_else(|e| fail(e));
    file
}
