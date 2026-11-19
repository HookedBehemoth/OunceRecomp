//! `--exclusive`: one build or game run on the machine at a time, through one lock file per user
//! (`$XDG_RUNTIME_DIR/ounce/exclusive.lock` on Linux, `%LOCALAPPDATA%\ounce\exclusive.lock` on Windows). `main`
//! takes it once, before the command runs, and holds it until ounce exits; nothing else takes it.
//!
//! It cannot deadlock on itself:
//! - a child ounce inherits `OUNCE_EXCLUSIVE=<holder pid>` and goes on without waiting while that holder is its
//!   ancestor and holds the lock;
//! - a holder that is an ancestor but not ounce (an outer `flock` on the lock file, seen in /proc/locks) is an error;
//! - a waiter prints the holder (its pid and command, written beside the lock) every 30 s.
//!
//! `--mem <size>` runs ounce again, once, in a systemd user scope with that memory cap and no swap (Linux); where no
//! scope can be made it warns and goes on without a cap. On Windows ounce puts itself in a job object whose processes
//! together may commit at most that much.

use crate::fail;
use std::fs::File;
use std::io::Write;
use std::path::PathBuf;
use std::time::{Duration, Instant};

/// the marker a child inherits: the pid of the ounce holding the lock
const HOLDER_ENV: &str = "OUNCE_EXCLUSIVE";
/// set in the process systemd-run starts, so that it does not make another scope
const SCOPE_ENV: &str = "OUNCE_MEM_SCOPE";

fn dir() -> PathBuf {
    #[cfg(windows)]
    let base = std::env::var_os("LOCALAPPDATA").map(PathBuf::from).unwrap_or_else(std::env::temp_dir);
    #[cfg(not(windows))]
    let base = std::env::var_os("XDG_RUNTIME_DIR")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| std::env::temp_dir().join(format!("ounce-{}", unsafe { libc::getuid() })));
    base.join("ounce")
}

pub fn lock_path() -> PathBuf {
    dir().join("exclusive.lock")
}

fn holder_path() -> PathBuf {
    dir().join("exclusive.holder")
}

/// `pid`'s parent (Linux); None elsewhere or when it is gone
fn parent(pid: u32) -> Option<u32> {
    let stat = std::fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
    // (the command in the second field may hold spaces and parentheses: the fields after the last ')')
    stat.rsplit_once(')')?.1.split_whitespace().nth(1)?.parse().ok()
}

/// whether `pid` is this process or one of its ancestors
fn is_ancestor(pid: u32) -> bool {
    let mut p = std::process::id();
    for _ in 0..256 {
        if p == pid {
            return true;
        }
        match parent(p) {
            Some(q) if q > 0 && q != p => p = q,
            _ => return false,
        }
    }
    false
}

fn command_of(pid: u32) -> String {
    let raw = std::fs::read(format!("/proc/{pid}/cmdline")).unwrap_or_default();
    let s: Vec<String> = raw.split(|&b| b == 0).filter(|a| !a.is_empty()).map(|a| String::from_utf8_lossy(a).into_owned()).collect();
    if s.is_empty() { "?".into() } else { s.join(" ") }
}

/// The pids holding a lock on `file` (not the ones waiting), from /proc/locks; empty where there is none.
#[cfg(target_os = "linux")]
fn lock_holders(file: &File) -> Vec<u32> {
    use std::os::unix::fs::MetadataExt;
    let Ok(m) = file.metadata() else { return Vec::new() };
    // (by inode alone: on btrfs stat's device is the subvolume's, not the one /proc/locks shows)
    let ino = m.ino().to_string();
    let text = std::fs::read_to_string("/proc/locks").unwrap_or_default();
    // ("1: FLOCK  ADVISORY  WRITE 1234 00:1a:5678 0 EOF"; a waiter's line has "->" after the number)
    text.lines()
        .filter(|l| !l.contains("->"))
        .filter_map(|l| {
            let f: Vec<&str> = l.split_whitespace().collect();
            (f.len() >= 6 && f[5].rsplit(':').next() == Some(ino.as_str())).then(|| f[4].parse().ok()).flatten()
        })
        .collect()
}

#[cfg(not(target_os = "linux"))]
fn lock_holders(_: &File) -> Vec<u32> {
    Vec::new()
}

/// Who holds the lock, as one line: the holder file when its pid still holds it, else the pids /proc/locks names.
fn describe_holder(file: &File) -> String {
    let pids = lock_holders(file);
    let text = std::fs::read_to_string(holder_path()).unwrap_or_default();
    let mut lines = text.lines();
    let named: Option<u32> = lines.next().and_then(|p| p.trim().parse().ok());
    match named {
        Some(p) if pids.is_empty() || pids.contains(&p) => format!("pid {p}: {}", lines.collect::<Vec<_>>().join(" ")),
        _ if !pids.is_empty() => pids.iter().map(|&p| format!("pid {p}: {}", command_of(p))).collect::<Vec<_>>().join("; "),
        _ => "unknown".into(),
    }
}

/// Takes the lock, waiting for it, unless an ancestor ounce holds it already. Held until the process exits.
pub fn acquire() {
    let path = lock_path();
    std::fs::create_dir_all(dir()).unwrap_or_else(|e| fail(format!("{}: {e}", dir().display())));
    let file = File::options().create(true).truncate(false).write(true).open(&path).unwrap_or_else(|e| fail(format!("{}: {e}", path.display())));
    // (the marker counts while the ounce it names is an ancestor and still holds the lock)
    if let Some(pid) = std::env::var(HOLDER_ENV).ok().and_then(|v| v.parse::<u32>().ok()) {
        if cfg!(not(target_os = "linux")) || (is_ancestor(pid) && lock_holders(&file).contains(&pid)) {
            return;
        }
    }
    let (t0, mut said) = (Instant::now(), None::<Instant>);
    loop {
        match file.try_lock() {
            Ok(()) => break,
            Err(std::fs::TryLockError::WouldBlock) => {}
            Err(std::fs::TryLockError::Error(e)) => fail(format!("{}: {e}", path.display())),
        }
        if let Some(&p) = lock_holders(&file).iter().find(|&&p| is_ancestor(p)) {
            fail(format!(
                "a caller of this ounce (pid {p}: {}) holds {}: run ounce without flock; --exclusive takes the lock itself",
                command_of(p),
                path.display()
            ));
        }
        if said.is_none_or(|t| t.elapsed() >= Duration::from_secs(30)) {
            eprintln!("ounce: waiting for {} ({:.0} s): held by {}", path.display(), t0.elapsed().as_secs_f64(), describe_holder(&file));
            said = Some(Instant::now());
        }
        std::thread::sleep(Duration::from_millis(250));
    }
    if said.is_some() {
        eprintln!("ounce: got {} after {:.0} s", path.display(), t0.elapsed().as_secs_f64());
    }
    let args: Vec<String> = std::env::args().collect();
    let _ = std::fs::File::create(holder_path()).and_then(|mut f| writeln!(f, "{}\n{}", std::process::id(), args.join(" ")));
    // (set before any thread is started)
    std::env::set_var(HOLDER_ENV, std::process::id().to_string());
    std::mem::forget(file);
}

/// Runs this ounce again in a systemd user scope capped at `size` (MemoryMax, no swap). Returns when it is already in
/// one, or when no scope can be made (with a warning).
pub fn in_scope(size: &str) {
    if std::env::var_os(SCOPE_ENV).is_some() {
        return;
    }
    if !size.trim_end_matches(['K', 'M', 'G', 'T']).parse::<u64>().is_ok_and(|n| n > 0) {
        fail(format!("--mem {size}: a size such as 8G"));
    }
    #[cfg(target_os = "linux")]
    {
        use std::os::unix::process::CommandExt;
        let probe = std::process::Command::new("systemd-run")
            .args(["--user", "--scope", "-q", "true"])
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status();
        if probe.is_ok_and(|s| s.success()) {
            let exe = std::env::current_exe().unwrap_or_else(|e| fail(e));
            let err = std::process::Command::new("systemd-run")
                .args(["--user", "--scope", "-q", "-p"])
                .arg(format!("MemoryMax={size}"))
                .args(["-p", "MemorySwapMax=0", "--"])
                .arg(exe)
                .args(std::env::args_os().skip(1))
                .env(SCOPE_ENV, size)
                .exec();
            fail(format!("systemd-run: {err}"));
        }
    }
    #[cfg(windows)]
    {
        // (a job object whose processes together may commit at most `size`: this ounce and every process it starts,
        // which a job's children join by themselves)
        use windows_sys::Win32::System::JobObjects::*;
        use windows_sys::Win32::System::Threading::GetCurrentProcess;
        let (n, unit) = size.split_at(size.trim_end_matches(['K', 'M', 'G', 'T']).len());
        let shift = match unit {
            "K" => 10,
            "M" => 20,
            "G" => 30,
            "T" => 40,
            _ => 0,
        };
        let bytes = n.parse::<u64>().unwrap_or(0) << shift;
        // SAFETY: a job made here and kept for the process's life, its limits struct, this process's pseudo-handle
        let ok = unsafe {
            let job = CreateJobObjectW(std::ptr::null(), std::ptr::null());
            let mut l: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
            l.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_JOB_MEMORY;
            l.JobMemoryLimit = bytes as usize;
            !job.is_null()
                && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &l as *const _ as *const _, std::mem::size_of_val(&l) as u32) != 0
                && AssignProcessToJobObject(job, GetCurrentProcess()) != 0
        };
        if ok {
            std::env::set_var(SCOPE_ENV, size);
            return;
        }
        eprintln!("ounce: warning: no job object could be made: running without the {size} memory cap");
    }
    #[cfg(not(windows))]
    eprintln!("ounce: warning: no systemd user scope here: running without the {size} memory cap");
}
