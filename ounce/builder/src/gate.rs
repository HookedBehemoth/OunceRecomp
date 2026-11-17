//! The memory gate: a compile starts when the memory free, less what the compiles that
//! started a moment ago may still take, keeps its own need and a reserve (HOOKSHOT_MEM_RESERVE_GB, 1.5) free. At least
//! one compile always runs. HOOKSHOT_MEM_GATE=0 turns it off. The free memory is the smaller of MemAvailable and the
//! room under the tightest memory.max of this process's cgroups (a capped scope).
#![cfg_attr(not(feature = "clang"), allow(dead_code))]

use std::sync::{Condvar, Mutex};
use std::time::{Duration, Instant};

/// a compile's peak, measured: the largest partitions (42 MB of IR) peak at 360 MB in this process, about eight
/// times their IR, hot or cold; a C file far less
pub fn need_ir_mb(ir_bytes: usize) -> u64 {
    64 + 8 * (ir_bytes as u64 >> 20)
}
pub const NEED_C_MB: u64 = 300;

/// how long a compile counts as still growing: its need is held back for it until then
const YOUNG: Duration = Duration::from_secs(3);

pub struct Gate {
    on: bool,
    reserve_mb: u64,
    running: Mutex<Vec<(u64, u64, Instant)>>,
    cv: Condvar,
    next: Mutex<u64>,
}

pub struct Lease<'a> {
    gate: &'a Gate,
    id: u64,
}

impl Drop for Lease<'_> {
    fn drop(&mut self) {
        let mut r = self.gate.running.lock().unwrap();
        r.retain(|x| x.0 != self.id);
        self.gate.cv.notify_all();
    }
}

/// MB free: MemAvailable, or less under a cgroup limit; None when it cannot be read
pub fn free_mb() -> Option<u64> {
    let avail = std::fs::read_to_string("/proc/meminfo")
        .ok()?
        .lines()
        .find(|l| l.starts_with("MemAvailable:"))?
        .split_whitespace()
        .nth(1)?
        .parse::<u64>()
        .ok()?
        / 1024;
    Some(cgroup_room_mb().map_or(avail, |r| r.min(avail)))
}

fn cgroup_room_mb() -> Option<u64> {
    let line = std::fs::read_to_string("/proc/self/cgroup").ok()?;
    let rel = line.lines().find_map(|l| l.strip_prefix("0::"))?.trim().to_string();
    let mut dir = std::path::PathBuf::from("/sys/fs/cgroup").join(rel.trim_start_matches('/'));
    let mut room: Option<u64> = None;
    loop {
        let max = std::fs::read_to_string(dir.join("memory.max")).ok();
        let cur = std::fs::read_to_string(dir.join("memory.current")).ok();
        if let (Some(m), Some(c)) = (max, cur) {
            if let (Ok(m), Ok(c)) = (m.trim().parse::<u64>(), c.trim().parse::<u64>()) {
                let r = m.saturating_sub(c) >> 20;
                room = Some(room.map_or(r, |x| x.min(r)));
            }
        }
        if !dir.pop() || dir.as_os_str() == "/sys/fs" {
            break;
        }
    }
    room
}

impl Gate {
    pub fn new() -> Gate {
        let on = std::env::var("HOOKSHOT_MEM_GATE").map_or(true, |v| v != "0");
        let reserve_gb: f64 = std::env::var("HOOKSHOT_MEM_RESERVE_GB").ok().and_then(|v| v.parse().ok()).unwrap_or(1.5);
        Gate { on, reserve_mb: (reserve_gb * 1024.0) as u64, running: Mutex::new(Vec::new()), cv: Condvar::new(), next: Mutex::new(0) }
    }

    /// waits until a compile needing `need_mb` fits, then holds its place until the lease is dropped
    pub fn enter(&self, need_mb: u64) -> Lease<'_> {
        let id = {
            let mut n = self.next.lock().unwrap();
            *n += 1;
            *n
        };
        let mut r = self.running.lock().unwrap();
        while self.on && !r.is_empty() {
            let owed: u64 = r.iter().filter(|x| x.2.elapsed() < YOUNG).map(|x| x.1).sum();
            match free_mb() {
                Some(free) if free.saturating_sub(owed + self.reserve_mb) < need_mb => {
                    r = self.cv.wait_timeout(r, Duration::from_millis(500)).unwrap().0;
                }
                _ => break,
            }
        }
        r.push((id, need_mb, Instant::now()));
        Lease { gate: self, id }
    }
}

/// the compile threads by default: HOOKSHOT_JOBS, else every CPU (the gate holds compiles back when memory is short)
pub fn default_jobs() -> usize {
    std::env::var("HOOKSHOT_JOBS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or_else(|| std::thread::available_parallelism().map_or(1, |n| n.get()))
}
