//! `--exclusive` against a lock of the test's own (XDG_RUNTIME_DIR in a temporary folder): a second ounce waits for
//! the holder, a child ounce of the holder goes on, and an ounce under an outer flock of the lock stops at once.
//! Each ounce runs `run <no such game> --exclusive`, which fails naming the game right after it has the lock.
#![cfg(target_os = "linux")]

use std::fs::File;
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use std::time::{Duration, Instant};

fn runtime_dir(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("ounce-excl-{name}-{}", std::process::id()));
    std::fs::create_dir_all(d.join("ounce")).unwrap();
    d
}

fn ounce(rt: &Path) -> Command {
    let mut c = Command::new(env!("CARGO_BIN_EXE_ounce"));
    c.args(["run", "no-such-game", "--exclusive"]).env("XDG_RUNTIME_DIR", rt).env_remove("OUNCE_EXCLUSIVE");
    c.stdout(Stdio::piped()).stderr(Stdio::piped());
    c
}

fn stderr(o: &Output) -> String {
    String::from_utf8_lossy(&o.stderr).into_owned()
}

/// the lock held by this test process (an ancestor of the ounce it starts)
fn hold(rt: &Path) -> File {
    let f = File::create(rt.join("ounce/exclusive.lock")).unwrap();
    f.lock().unwrap();
    f
}

/// the lock held by another process (not an ancestor): `flock <lock> sleep 30`, killed to let it go
fn hold_elsewhere(rt: &Path) -> std::process::Child {
    let lock = rt.join("ounce/exclusive.lock");
    let c = Command::new("flock").arg(&lock).args(["sleep", "30"]).spawn().expect("flock(1)");
    // (until it has the lock)
    let f = File::create(&lock).unwrap();
    while f.try_lock().is_ok() {
        f.unlock().unwrap();
        std::thread::sleep(Duration::from_millis(20));
    }
    c
}

fn release(mut c: std::process::Child) {
    // (flock's child, sleep, would keep the lock: the whole group goes)
    let _ = Command::new("pkill").arg("-P").arg(c.id().to_string()).status();
    let _ = c.kill();
    let _ = c.wait();
}

#[test]
fn a_second_ounce_waits_for_the_holder() {
    let rt = runtime_dir("wait");
    let held = hold_elsewhere(&rt);
    let child = ounce(&rt).spawn().unwrap();
    std::thread::sleep(Duration::from_millis(1500));
    let t = Instant::now();
    release(held);
    let o = child.wait_with_output().unwrap();
    let e = stderr(&o);
    assert!(e.contains("waiting for"), "{e}");
    assert!(e.contains("no game"), "it went on once the lock was free: {e}");
    assert!(t.elapsed() < Duration::from_secs(5));
    std::fs::remove_dir_all(&rt).unwrap();
}

#[test]
fn a_child_of_the_holder_goes_on() {
    let rt = runtime_dir("child");
    let held = hold(&rt);
    let o = ounce(&rt).env("OUNCE_EXCLUSIVE", std::process::id().to_string()).output().unwrap();
    let e = stderr(&o);
    assert!(!e.contains("waiting") && e.contains("no game"), "{e}");
    drop(held);
    // (a marker naming a process that does not hold the lock is not trusted: pid 1 is everyone's ancestor)
    let held = hold_elsewhere(&rt);
    let child = ounce(&rt).env("OUNCE_EXCLUSIVE", "1").spawn().unwrap();
    std::thread::sleep(Duration::from_millis(800));
    release(held);
    let e = stderr(&child.wait_with_output().unwrap());
    assert!(e.contains("waiting for") && e.contains("no game"), "{e}");
    std::fs::remove_dir_all(&rt).unwrap();
}

#[test]
fn an_outer_flock_is_an_error_at_once() {
    let rt = runtime_dir("flock");
    let lock = rt.join("ounce/exclusive.lock");
    File::create(&lock).unwrap();
    let t = Instant::now();
    let mut c = Command::new("flock");
    c.arg(&lock).arg(env!("CARGO_BIN_EXE_ounce")).args(["run", "no-such-game", "--exclusive"]).env("XDG_RUNTIME_DIR", &rt);
    let o = c.output().expect("flock(1)");
    let e = stderr(&o);
    assert!(!o.status.success() && e.contains("run ounce without flock"), "{e}");
    assert!(t.elapsed() < Duration::from_secs(3));
    std::fs::remove_dir_all(&rt).unwrap();
}
