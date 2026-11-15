//! ounce-lift <export dir> <out dir> [-j N] [--parts a,b,...] [--skip]: one .ll per partition of the export.
//! --skip: a function using a template not ported yet is left out and counted ("skipped: template X").

use ounce_lift::export::{read_meta, read_part};
use ounce_lift::part::lift_part;
use std::collections::{BTreeMap, HashSet};
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::Instant;

#[cfg(feature = "count-alloc")]
mod count {
    use std::alloc::{GlobalAlloc, Layout, System};
    use std::sync::atomic::{AtomicU64, Ordering::Relaxed};
    pub static N: AtomicU64 = AtomicU64::new(0);
    pub static BYTES: AtomicU64 = AtomicU64::new(0);
    pub struct Counting;
    unsafe impl GlobalAlloc for Counting {
        unsafe fn alloc(&self, l: Layout) -> *mut u8 {
            N.fetch_add(1, Relaxed);
            BYTES.fetch_add(l.size() as u64, Relaxed);
            System.alloc(l)
        }
        unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
            System.dealloc(p, l)
        }
        unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
            N.fetch_add(1, Relaxed);
            BYTES.fetch_add(n.saturating_sub(l.size()) as u64, Relaxed);
            System.realloc(p, l, n)
        }
    }
    #[global_allocator]
    static A: Counting = Counting;
}

fn main() {
    let mut args = std::env::args().skip(1);
    let mut pos = Vec::new();
    let (mut jobs, mut only, mut skip) = (6usize, None::<Vec<String>>, false);
    while let Some(a) = args.next() {
        match a.as_str() {
            "-j" => jobs = args.next().unwrap().parse().unwrap(),
            "--parts" => only = Some(args.next().unwrap().split(',').map(String::from).collect()),
            "--skip" => skip = true,
            _ => pos.push(PathBuf::from(a)),
        }
    }
    if pos.len() != 2 {
        eprintln!("usage: ounce-lift <export dir> <out dir> [-j N] [--parts a,b] [--skip]");
        std::process::exit(2);
    }
    let (dir, out) = (&pos[0], &pos[1]);
    let meta = read_meta(dir).unwrap_or_else(|e| panic!("{e}"));
    std::fs::create_dir_all(out).unwrap();
    let targets: HashSet<u64> = meta.code_targets.iter().copied().collect();
    let names: Vec<String> = meta
        .partitions
        .iter()
        .map(|p| p.name.clone())
        .filter(|n| only.as_ref().map_or(true, |o| o.contains(n)))
        .collect();
    let t0 = Instant::now();
    let next = AtomicUsize::new(0);
    let total = Mutex::new((0usize, BTreeMap::<String, usize>::new(), 0f64, 0f64));
    std::thread::scope(|s| {
        for _ in 0..jobs.max(1) {
            s.spawn(|| loop {
                let k = next.fetch_add(1, Ordering::Relaxed);
                if k >= names.len() {
                    break;
                }
                let t = Instant::now();
                let part = read_part(dir, &names[k]).unwrap_or_else(|e| panic!("{e}"));
                let tr = t.elapsed().as_secs_f64();
                let t = Instant::now();
                let l = lift_part(&meta, &targets, &part, skip);
                let tl = t.elapsed().as_secs_f64();
                std::fs::write(out.join(format!("{}.ll", names[k])), &l.text).unwrap();
                let mut g = total.lock().unwrap();
                g.0 += l.lifted;
                for (why, n) in l.skipped {
                    *g.1.entry(why).or_insert(0) += n;
                }
                g.2 += tr;
                g.3 += tl;
            });
        }
    });
    let g = total.into_inner().unwrap();
    let nskip: usize = g.1.values().sum();
    println!(
        "{} partitions: {} functions lifted, {} skipped; {:.2} s wall (read {:.2} s, lift+emit {:.2} s CPU)",
        names.len(),
        g.0,
        nskip,
        t0.elapsed().as_secs_f64(),
        g.2,
        g.3
    );
    let hwm = std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| s.lines().find(|l| l.starts_with("VmHWM:")).map(|l| l[6..].trim().to_string()))
        .unwrap_or_default();
    println!("peak memory (VmHWM): {hwm}");
    #[cfg(feature = "count-alloc")]
    println!(
        "allocations: {} ({:.1} MB requested)",
        count::N.load(std::sync::atomic::Ordering::Relaxed),
        count::BYTES.load(std::sync::atomic::Ordering::Relaxed) as f64 / 1e6
    );
    let mut by: Vec<_> = g.1.into_iter().collect();
    by.sort_by(|a, b| b.1.cmp(&a.1));
    for (why, n) in by.iter().take(40) {
        println!("  skipped: {why}: {n}");
    }
}
