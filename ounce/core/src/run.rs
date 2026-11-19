//! A game run from the checkout's build (`ounce run`, `ounce test`). The game reads its romfs from the config's
//! dump and writes its log into `work/<game>/runs/<name>/`; settings (renderer/api/settings.h) are passed as `--set
//! key=value`. A window run keeps the user's own saves (the data folder) and options file (the game folder's); a headless
//! run is muted, takes screenshots and keeps its saves and options file in the run folder, so it never changes the user's. While the game runs, `timeline.txt` gets one line a second (seconds, presents, RSS in MB) and `mem.txt`
//! the free and shared memory (MB).

use crate::config::Config;
use crate::games::Game;
use crate::layout::Layout;
use std::io::{Read, Seek, Write};
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Default)]
pub struct Options {
    pub headless: bool,
    /// stop the game after this long
    pub seconds: Option<u64>,
    /// record the input into the data folder's rec/<name>
    pub record: Option<String>,
    /// replay rec/<name> (from its copy of the saves, with the switches its header names)
    pub replay: Option<String>,
    /// the bench setting: measure this many frames once the scene is ready
    pub bench: Option<u32>,
    /// settings: key=value, passed as --set
    pub set: Vec<(String, String)>,
    /// the run folder's name (default: play, or headless)
    pub name: Option<String>,
    /// a screenshot every this many presents (headless; default 300)
    pub shot_every: Option<u32>,
    /// another game folder than the checkout's build (a player's folder from `ounce build --out`): its own romfs/
    /// when it has one
    pub build: Option<PathBuf>,
}

pub struct Outcome {
    pub dir: PathBuf,
    /// one line: how long, how it ended, the peak RSS
    pub summary: String,
}

/// A warning line when the game folder was built from another commit than the checkout's.
pub fn stale_warning(layout: &Layout, stamp: &Path) -> Option<String> {
    let built = std::fs::read_to_string(stamp).ok()?.lines().next()?.trim().to_string();
    let head = Command::new("git").arg("-C").arg(&layout.root).args(["rev-parse", "HEAD"]).output().ok()?;
    let head = String::from_utf8_lossy(&head.stdout).trim().to_string();
    (built != head).then(|| {
        format!("warning: this build is of {}, the checkout is at {}: run `ounce build {}`", short(&built), short(&head), layout.game)
    })
}

fn short(commit: &str) -> &str {
    &commit[..commit.len().min(10)]
}

/// (MemAvailable, Shmem) in MB
fn meminfo() -> (u64, u64) {
    let text = std::fs::read_to_string("/proc/meminfo").unwrap_or_default();
    let field = |k: &str| text.lines().find_map(|l| l.strip_prefix(k)).and_then(|r| r.split_whitespace().next()?.parse::<u64>().ok()).unwrap_or(0) >> 10;
    (field("MemAvailable:"), field("Shmem:"))
}

fn rss_mb(pid: u32) -> u64 {
    let text = std::fs::read_to_string(format!("/proc/{pid}/status")).unwrap_or_default();
    text.lines().find_map(|l| l.strip_prefix("VmRSS:")).and_then(|r| r.split_whitespace().next()?.parse::<u64>().ok()).unwrap_or(0) >> 10
}

/// The last "] vk: <n> presents" in the log's text since `pos`.
fn presents_since(log: &Path, pos: &mut u64, last: &mut u64) {
    let Ok(mut f) = std::fs::File::open(log) else { return };
    if f.seek(std::io::SeekFrom::Start(*pos)).is_err() {
        return;
    }
    let mut buf = Vec::new();
    let _ = f.read_to_end(&mut buf);
    // (only whole lines: the rest is read again next time)
    let end = buf.iter().rposition(|&b| b == b'\n').map_or(0, |i| i + 1);
    *pos += end as u64;
    for line in String::from_utf8_lossy(&buf[..end]).lines() {
        if let Some(Ok(n)) = line.split("] vk: ").nth(1).and_then(|r| r.split_once(" presents")).map(|(n, _)| n.parse()) {
            *last = n;
        }
    }
}

fn copy_tree(from: &Path, to: &Path) -> Result<(), String> {
    std::fs::create_dir_all(to).map_err(|e| format!("{}: {e}", to.display()))?;
    for e in std::fs::read_dir(from).map_err(|e| format!("{}: {e}", from.display()))?.flatten() {
        let (src, dst) = (e.path(), to.join(e.file_name()));
        if e.file_type().map_err(|e| e.to_string())?.is_dir() {
            copy_tree(&src, &dst)?;
        } else {
            std::fs::copy(&src, &dst).map_err(|e| format!("{}: {e}", src.display()))?;
        }
    }
    Ok(())
}

/// Kernel lines of a GPU reset or a ring timeout since `since` (seconds since the epoch).
fn gpu_resets(since: u64) -> Vec<String> {
    let Ok(o) = Command::new("journalctl").args(["-k", "-q", "--no-pager", "--since"]).arg(format!("@{since}")).output() else {
        return Vec::new();
    };
    String::from_utf8_lossy(&o.stdout).lines().filter(|l| l.contains("GPU reset(") || (l.contains("ring ") && l.contains("timeout"))).map(String::from).collect()
}

/// Runs the game until it ends, or until `seconds`.
pub fn run(layout: &Layout, config: &Config, game: &Game, o: &Options) -> Result<Outcome, String> {
    let folder = o.build.clone().unwrap_or_else(|| layout.game_dir());
    let exe = folder.join(&game.exe);
    if !exe.is_file() {
        return Err(format!("no build of {} ({} is missing): run `ounce build {}`", game.name, exe.display(), game.name));
    }
    if let Some(w) = stale_warning(layout, &folder.join("build.txt")) {
        eprintln!("{w}");
    }
    let romfs = if folder.join("romfs").is_dir() { folder.join("romfs") } else { config.dump(&game.name)?.join("romfs") };
    let name = o.name.clone().unwrap_or_else(|| if o.headless { "headless".into() } else { "play".into() });
    let dir = layout.run_dir(&name);
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;

    let mut set: Vec<(String, String)> = Vec::new();
    let s = |p: &Path| p.to_string_lossy().into_owned();
    // (the saves: a window run plays with the user's own, any other run with a copy of its own in the run folder)
    let user = if o.headless || o.replay.is_some() { dir.join("user") } else { config.data_dir() };
    if let Some(r) = &o.record {
        let rec = layout.recordings(config).join(r);
        if rec.exists() {
            return Err(format!("{} exists: record under another name", rec.display()));
        }
        std::fs::create_dir_all(rec.parent().unwrap()).map_err(|e| e.to_string())?;
        set.push(("input.record".into(), s(&rec)));
    }
    if let Some(r) = &o.replay {
        let rec = layout.recordings(config).join(r);
        let input = rec.join("input.rec");
        let data = std::fs::read(&input).map_err(|e| format!("{}: {e}", input.display()))?;
        let head = String::from_utf8_lossy(data.split(|&b| b == b'\n').next().unwrap_or(&[]));
        // (the settings the recording was made with, from its first line)
        set.extend(record_settings(&head));
        set.push(("input.replay".into(), s(&input)));
        if rec.join("user").is_dir() {
            copy_tree(&rec.join("user"), &user)?;
        }
    }
    if o.headless {
        let shots = dir.join("shots");
        std::fs::create_dir_all(&shots).map_err(|e| e.to_string())?;
        set.push(("headless".into(), "1".into()));
        set.push(("audio".into(), "0".into()));
        set.push(("shots".into(), s(&shots)));
        let every = o.shot_every.unwrap_or(if o.bench.is_some() { 1_000_000 } else { 300 });
        set.push(("shot_every".into(), every.to_string()));
    }
    if let Some(n) = o.bench {
        set.push(("bench".into(), format!("{n},60")));
    }
    set.extend(o.set.iter().cloned());
    std::fs::create_dir_all(&user).map_err(|e| format!("{}: {e}", user.display()))?;

    let need = config.run_free_mem_gb() << 10;
    if meminfo().0 < need {
        eprintln!("waiting for {} GB of free memory", need >> 10);
        while meminfo().0 < need {
            std::thread::sleep(Duration::from_secs(2));
        }
    }

    let file = |n: &str| std::fs::File::create(dir.join(n)).map_err(|e| format!("{}: {e}", dir.join(n).display()));
    let mut cmd = Command::new(&exe);
    cmd.arg(folder.join("image.bin"))
        .arg("--romfs")
        .arg(&romfs)
        .arg("--user")
        .arg(&user)
        .current_dir(&dir)
        .stdin(Stdio::null())
        .stdout(file("stdout.txt")?)
        .stderr(file("log.txt")?);
    // (the game dies with ounce: killed or interrupted, it would otherwise go on holding the GPU)
    unsafe {
        cmd.pre_exec(|| {
            libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGKILL);
            Ok(())
        });
    }
    let start_epoch = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs());
    let t0 = Instant::now();
    if o.headless || o.replay.is_some() {
        // (a run of the tests' own: its options file in the run folder, empty at the start)
        cmd.arg("--options").arg(dir.join("ounce_options.ini"));
    }
    for (k, v) in &set {
        cmd.arg("--set").arg(format!("{k}={v}"));
    }
    let mut child = cmd.spawn().map_err(|e| format!("{}: {e}", exe.display()))?;
    let pid = child.id();
    let (mut timeline, mut mem) = (file("timeline.txt")?, file("mem.txt")?);
    let (mut pos, mut presents, mut peak) = (0u64, 0u64, 0u64);
    let mut how = None;
    for t in 0.. {
        if let Some(st) = child.try_wait().map_err(|e| e.to_string())? {
            how = Some(match st.code() {
                Some(c) => format!("exited with {c}"),
                None => format!("ended by {st}"),
            });
            break;
        }
        if o.seconds.is_some_and(|n| t >= n) {
            break;
        }
        let rss = rss_mb(pid);
        peak = peak.max(rss);
        presents_since(&dir.join("log.txt"), &mut pos, &mut presents);
        let (avail, shm) = meminfo();
        let _ = writeln!(timeline, "{t} {presents} {rss}");
        let _ = writeln!(mem, "{avail} {shm}");
        // (one sample a second, counted from the start)
        let next = Duration::from_secs(t + 1);
        std::thread::sleep(next.saturating_sub(t0.elapsed()));
    }
    let how = match how {
        Some(h) => h,
        None => {
            unsafe { libc::kill(pid as i32, libc::SIGTERM) };
            let t = Instant::now();
            while child.try_wait().map_err(|e| e.to_string())?.is_none() {
                if t.elapsed() > Duration::from_secs(2) {
                    let _ = child.kill();
                    let _ = child.wait();
                    break;
                }
                std::thread::sleep(Duration::from_millis(100));
            }
            format!("stopped after {} s", o.seconds.unwrap_or(0))
        }
    };
    for l in gpu_resets(start_epoch) {
        eprintln!("GPU: {l}");
    }
    let summary = format!("run '{name}': {} s, {how}, peak RSS {peak} MB", t0.elapsed().as_secs());
    Ok(Outcome { dir, summary })
}

/// The settings an input recording's first line names: `key=value` words (version 2), or the HOS_ names of version 1
fn record_settings(head: &str) -> Vec<(String, String)> {
    let mut out = Vec::new();
    for w in head.split_whitespace() {
        let Some((k, v)) = w.split_once('=') else { continue };
        let key = match k {
            "build" => continue,
            "HOS_OUTPUT_RES" => "output_res",
            "HOS_LANGUAGE" => "language",
            "HOS_FPS_MAX" => match v {
                "120" => {
                    out.push(("frame_rate".into(), "120".into()));
                    continue;
                }
                _ => continue,
            },
            k if k.starts_with("HOS_") => continue,
            k => k,
        };
        out.push((key.to_string(), v.to_string()));
    }
    out
}
