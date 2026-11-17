//! The progress lines a front end reads: `@@plan <to compile> <modules>`, `@@file <name> <seconds>
//! <new|cached>` as each module ends, `@@eta <seconds>` after it. The estimate is the seconds each module took in the
//! last build, kept in the cache folder (seconds.txt).
#![cfg_attr(not(feature = "clang"), allow(dead_code))]

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Mutex;

pub struct Progress {
    on: bool,
    jobs: usize,
    record: PathBuf,
    /// the last build's seconds of the modules still to come
    pending: Mutex<HashMap<String, f64>>,
    /// this build's
    seen: Mutex<Vec<(String, f64)>>,
}

impl Progress {
    pub fn new(on: bool, jobs: usize, cache_dir: &std::path::Path) -> Progress {
        let record = cache_dir.join("seconds.txt");
        let pending: HashMap<String, f64> = std::fs::read_to_string(&record)
            .unwrap_or_default()
            .lines()
            .filter_map(|l| l.split_once(' ').and_then(|(n, s)| Some((n.to_string(), s.parse().ok()?))))
            .collect();
        let p = Progress { on, jobs: jobs.max(1), record, pending: Mutex::new(pending), seen: Mutex::new(Vec::new()) };
        if on {
            let n = p.pending.lock().unwrap().len();
            println!("@@plan {n} {n}");
            p.eta();
        }
        p
    }

    /// the rest, with this many compilers: the work spread over them, the longest module the floor
    fn eta(&self) {
        let p = self.pending.lock().unwrap();
        let (sum, max) = p.values().fold((0f64, 0f64), |a, &s| (a.0 + s, a.1.max(s)));
        println!("@@eta {:.0}", max.max(sum / self.jobs as f64));
    }

    pub fn done(&self, name: &str, secs: f64, cached: bool) {
        self.seen.lock().unwrap().push((name.to_string(), if cached { 0.0 } else { secs }));
        self.pending.lock().unwrap().remove(name);
        if self.on {
            println!("@@file {name} {secs:.1} {}", if cached { "cached" } else { "new" });
            self.eta();
        }
    }

    /// keeps this build's seconds for the next one's estimate (a cached module keeps its last real time)
    pub fn save(&self) {
        let old: HashMap<String, f64> = std::fs::read_to_string(&self.record)
            .unwrap_or_default()
            .lines()
            .filter_map(|l| l.split_once(' ').and_then(|(n, s)| Some((n.to_string(), s.parse().ok()?))))
            .collect();
        let text: String = self
            .seen
            .lock()
            .unwrap()
            .iter()
            .map(|(n, s)| format!("{n} {:.2}\n", if *s > 0.0 { *s } else { old.get(n).copied().unwrap_or(0.0) }))
            .collect();
        let _ = std::fs::write(&self.record, text);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// the estimate: the work spread over the compilers, the longest module the floor; a module done leaves it
    #[test]
    fn eta_from_the_last_build() {
        let dir = std::env::temp_dir().join(format!("hk-progress-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("seconds.txt"), "a 10\nb 2\nc 2\nd 2\n").unwrap();
        let p = Progress::new(false, 2, &dir);
        let rest = |p: &Progress| {
            let m = p.pending.lock().unwrap();
            let (s, x) = m.values().fold((0f64, 0f64), |a, &v| (a.0 + v, a.1.max(v)));
            x.max(s / p.jobs as f64)
        };
        assert_eq!(rest(&p), 10.0);
        p.done("a", 9.0, false);
        assert_eq!(rest(&p), 3.0);
        p.done("b", 0.0, true);
        p.save();
        let saved = std::fs::read_to_string(dir.join("seconds.txt")).unwrap();
        assert!(saved.contains("a 9.00") && saved.contains("b 2.00"), "{saved}");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
