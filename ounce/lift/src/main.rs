//! ounce-lift <export dir> <out dir> [-j N (default: the CPUs)] [--parts a,b,...] [--skip] [--check]: one .ll per
//! partition of the export, its summaries and call-boundary records computed first (summary, bounds).
//! --skip: a function using a template not ported yet is left out and counted ("skipped: template X").
//! --check: compare every summary and record with Python's (the export's check/) and stop: the diagnostic gate.

use ounce_lift::bounds::Records;
use ounce_lift::export::{read_meta, read_part, Part};
use ounce_lift::summary::{summarise, Program};
use rayon::prelude::*;
use ounce_lift::part::{lift_part, Worker};
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

/// the answers module's name (the build's lift_answers.o)
const ANSWERS: &str = "lift_answers";

/// the generic entries of the answered functions (game.answers_ir): f_<addr> writes the answer to x0 and returns
fn answers_ir(meta: &ounce_lift::export::Meta) -> String {
    use std::fmt::Write;
    let mut s = format!("target triple = \"{}\"", ounce_lift::emit::TRIPLE);
    for (a, name, v) in &meta.answers {
        write!(
            s,
            "\ndefine void @f_{a:08x}(ptr noalias nocapture %ctx) {{   ; {name}\n  %x0 = getelementptr i8, ptr %ctx, i64 {}\n  store i64 {}, ptr %x0, align 8\n  ret void\n}}",
            ounce_lift::emit::ctx_off(0),
            *v as i64
        )
        .unwrap();
    }
    s.push('\n');
    s
}

fn main() {
    let mut args = std::env::args().skip(1);
    let mut pos = Vec::new();
    let mut jobs = std::thread::available_parallelism().map_or(1, |n| n.get());
    let (mut only, mut skip, mut check) = (None::<Vec<String>>, false, false);
    while let Some(a) = args.next() {
        match a.as_str() {
            "-j" => jobs = args.next().unwrap().parse().unwrap(),
            "--parts" => only = Some(args.next().unwrap().split(',').map(String::from).collect()),
            "--skip" => skip = true,
            "--check" => check = true,
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
        .filter(|n| only.as_ref().is_none_or(|o| o.contains(n)))
        .collect();
    if only.as_ref().is_none_or(|o| o.iter().any(|n| n == ANSWERS)) {
        std::fs::write(out.join(format!("{ANSWERS}.ll")), answers_ir(&meta)).unwrap();
    }
    rayon::ThreadPoolBuilder::new().num_threads(jobs.max(1)).build_global().unwrap();
    let t0 = Instant::now();
    // every partition's functions: the summaries read the whole program
    let all: Vec<String> = meta.partitions.iter().map(|p| p.name.clone()).collect();
    let parts: Vec<Part> = all.par_iter().map(|n| read_part(dir, n).unwrap_or_else(|e| panic!("{e}"))).collect();
    let t_read = t0.elapsed().as_secs_f64();
    let ts = Instant::now();
    let mut prog = Program::new(&meta, &parts);
    let (sums, st) = summarise(&mut prog);
    let rec = Records::new(&prog, &sums, &meta.sj_rets);
    println!(
        "summaries: {} functions, {} components (largest {}) in {} levels, {} noreturn by callers; graph {:.2} s, fixed point {:.2} s, results {:.2} s; {:.2} s in all (read {:.2} s)",
        st.functions, st.sccs, st.largest, st.levels, st.noreturn_by_callers, st.graph_s, st.fixed_s, st.results_s,
        ts.elapsed().as_secs_f64(), t_read
    );
    if check {
        let (a, b) = ounce_lift::check::check(dir, &prog, &sums, &rec, 40);
        std::process::exit(if a + b == 0 { 0 } else { 1 });
    }
    let by_name: std::collections::HashMap<&str, &Part> = parts.iter().map(|p| (p.name.as_str(), p)).collect();
    let next = AtomicUsize::new(0);
    let total = Mutex::new((0usize, BTreeMap::<String, usize>::new(), 0f64, 0f64));
    std::thread::scope(|s| {
        for _ in 0..jobs.max(1) {
            s.spawn(|| {
            let mut w = Worker::default();
            loop {
                let k = next.fetch_add(1, Ordering::Relaxed);
                if k >= names.len() {
                    break;
                }
                let part = by_name[names[k].as_str()];
                let tr = 0.0;
                let t = Instant::now();
                let l = lift_part(&mut w, &meta, &targets, &rec, part, skip);
                let tl = t.elapsed().as_secs_f64();
                {
                    use std::io::Write;
                    let mut file = std::fs::File::create(out.join(format!("{}.ll", names[k]))).unwrap();
                    file.write_all(&l.head).unwrap();
                    file.write_all(&w.body).unwrap();
                }
                let mut g = total.lock().unwrap();
                g.0 += l.lifted;
                for (why, n) in l.skipped {
                    *g.1.entry(why).or_insert(0) += n;
                }
                g.2 += tr;
                g.3 += tl;
            }
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
    by.sort_by_key(|b| std::cmp::Reverse(b.1));
    for (why, n) in by.iter().take(40) {
        println!("  skipped: {why}: {n}");
    }
}
