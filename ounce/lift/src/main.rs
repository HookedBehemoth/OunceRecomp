//! ounce-lift <export dir> <out dir> [-j N (default: the CPUs)] [--parts a,b,...] [--skip] [--check]: one .ll per
//! partition of the export, its summaries and call-boundary records computed first (summary, bounds).
//! --skip: a function using a template not ported yet is left out and counted ("skipped: template X").
//! --check: compare every summary and record with Python's (the export's check/) and stop: the diagnostic gate.

use ounce_lift::bounds::Records;
use ounce_lift::discover::Discovery;
use ounce_lift::export::{read_meta, Part};
use ounce_lift::game::Game;
use ounce_lift::image::Image;
use ounce_lift::plan::{self, Plan};
use std::collections::HashMap;
use ounce_lift::summary::{summarise, Program};
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

/// the parallel stages' busy seconds so far (ounce_lift::busy)
fn busy_s() -> f64 {
    ounce_lift::BUSY_NS.load(Ordering::Relaxed) as f64 / 1e9
}

/// (process CPU seconds, user and system; VmHWM in MB)
fn usage() -> (f64, f64) {
    let stat = std::fs::read_to_string("/proc/self/stat").unwrap_or_default();
    let f: Vec<&str> = stat.rsplit(')').next().unwrap_or("").split_whitespace().collect();
    let tick = |k: usize| f.get(k).and_then(|x| x.parse::<f64>().ok()).unwrap_or(0.0) / 100.0;
    let hwm = std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| s.lines().find(|l| l.starts_with("VmHWM:")).and_then(|l| l[6..].trim().trim_end_matches(" kB").trim().parse::<f64>().ok()))
        .unwrap_or(0.0);
    (tick(11) + tick(12), hwm / 1024.0)
}

fn main() {
    let mut args = std::env::args().skip(1);
    let mut pos = Vec::new();
    let mut jobs = std::thread::available_parallelism().map_or(1, |n| n.get());
    let (mut only, mut skip, mut check) = (None::<Vec<String>>, false, false);
    let (mut game_dir, mut work, mut max_func) = (PathBuf::from("games/oot"), PathBuf::from("work"), 0usize);
    while let Some(a) = args.next() {
        match a.as_str() {
            "-j" => jobs = args.next().unwrap().parse().unwrap(),
            "--parts" => only = Some(args.next().unwrap().split(',').map(String::from).collect()),
            "--skip" => skip = true,
            "--check" => check = true,
            "--game" => game_dir = PathBuf::from(args.next().unwrap()),
            "--work" => work = PathBuf::from(args.next().unwrap()),
            "--max-func" => max_func = args.next().unwrap().parse().unwrap(),
            _ => pos.push(PathBuf::from(a)),
        }
    }
    if pos.len() != 2 {
        eprintln!("usage: ounce-lift <export dir> <out dir> [-j N] [--parts a,b] [--skip]");
        std::process::exit(2);
    }
    let (dir, out) = (&pos[0], &pos[1]);
    let meta = read_meta(dir).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(meta.format, 4, "export format");
    std::fs::create_dir_all(out).unwrap();
    rayon::ThreadPoolBuilder::new().num_threads(jobs.max(1)).build_global().unwrap();
    let t0 = Instant::now();
    let game = Game::load(&game_dir).unwrap_or_else(|e| panic!("{e}"));
    // the image and its entries (nx2/lift.py: the image's, then those ADR / ADRP+ADD form outside unwind-covered code)
    let img = Image::load(&dir.join("image")).unwrap_or_else(|e| panic!("{e}"));
    let mut eh: Vec<u64> = meta.eh_frame_hdr.iter().flat_map(|(_, s, e)| img.eh_funcs(*s, *e)).collect();
    eh.sort_unstable();
    let spans = img.fde_spans(&meta.eh_frame_hdr);
    let mut entries = meta.image_entries.clone();
    let have: HashSet<u64> = entries.iter().copied().collect();
    entries.extend(img.code_address_refs(&spans).into_iter().filter(|&e| img.in_code(e) && !have.contains(&e)));
    let (c_img, _) = usage();
    let t_img = t0.elapsed().as_secs_f64();
    // discovery (sequential: Python's order), then the plan
    let td = Instant::now();
    let mut d = Discovery::new(&img, &entries, &meta.names, &meta.setjmp_names, eh);
    d.discover(max_func);
    let (c_disc, _) = usage();
    let t_disc = td.elapsed().as_secs_f64();
    let tp = Instant::now();
    let hot_path = game.hot_path(&work).unwrap_or_else(|e| panic!("{e}"));
    let hot: HashSet<u64> = std::fs::read_to_string(&hot_path)
        .unwrap_or_else(|e| panic!("{}: {e}", hot_path.display()))
        .lines()
        .filter_map(|l| l.split_whitespace().next())
        .filter_map(|w| w.strip_prefix("f_").map(|h| u64::from_str_radix(h, 16).unwrap()))
        .collect();
    let mut plan = Plan::new(&d, &img, &hot);
    let continuations: HashMap<u64, u32> = game.local_continuations.iter().copied().collect();
    let pp = plan::parts(&plan, &meta, &img, &continuations);
    let b_plan = busy_s();
    let (c_plan, m_plan) = usage();
    let t_plan = tp.elapsed().as_secs_f64();
    println!(
        "image: {} entries, {} more from ADR / ADRP+ADD; {:.2} s wall, {:.2} s CPU",
        meta.image_entries.len(),
        entries.len() - meta.image_entries.len(),
        t_img,
        c_img
    );
    println!(
        "discovery: {} functions, {} jump tables ({} on their index), {} PLT stubs, {} setjmp owners; {:.2} s wall, {:.2} s CPU (one thread)",
        d.funcs.len(),
        d.jt.len(),
        d.jt_index.len(),
        d.plt.len(),
        d.setjmp_owner.len(),
        t_disc,
        c_disc - c_img
    );
    println!(
        "plan: {} bodies ({} traps by reason), {} partitions, census {} sites; {:.2} s wall, {:.2} s CPU ({:.2} s busy); peak {:.0} MB",
        plan.bodies.len(),
        plan.why.values().sum::<u32>(),
        pp.parts.len(),
        pp.census.iter().map(|c| c.0).sum::<u64>(),
        t_plan,
        c_plan - c_disc,
        b_plan,
        m_plan
    );
    drop(d);
    if !check {
        // (the partitions hold the bodies now; the check compares the plan's)
        for b in &mut plan.bodies {
            b.pcs = Vec::new();
            b.words = Vec::new();
        }
    }
    let odd: Vec<&(u64, String)> = pp.census.iter().flat_map(|c| c.2.iter()).collect();
    if !odd.is_empty() && !check {
        panic!("{} code addresses formed in a way code_ptrs does not know, first {:#x}: {}", odd.len(), odd[0].0, odd[0].1);
    }
    let targets: HashSet<u64> = pp.code_targets.iter().copied().collect();
    let parts = &pp.parts;
    let names: Vec<String> = parts.iter().map(|p| p.name.clone()).filter(|n| only.as_ref().is_none_or(|o| o.contains(n))).collect();
    if only.as_ref().is_none_or(|o| o.iter().any(|n| n == ANSWERS)) {
        std::fs::write(out.join(format!("{ANSWERS}.ll")), answers_ir(&meta)).unwrap();
    }
    let ts = Instant::now();
    let mut prog = Program::new(&meta, &plan, parts, &img);
    let (sums, st) = summarise(&mut prog);
    let rec = Records::new(&prog, &sums, &plan.sj_rets);
    let b_sum = busy_s() - b_plan;
    let (c_sum, m_sum) = usage();
    println!(
        "summaries: {} functions, {} components (largest {}) in {} levels, {} noreturn by callers; graph {:.2} s, fixed point {:.2} s, results {:.2} s; {:.2} s wall, {:.2} s CPU ({:.2} s busy); peak {:.0} MB",
        st.functions, st.sccs, st.largest, st.levels, st.noreturn_by_callers, st.graph_s, st.fixed_s, st.results_s,
        ts.elapsed().as_secs_f64(), c_sum - c_plan, b_sum, m_sum
    );
    if check {
        let n = ounce_lift::check::check_plan(dir, &plan, &pp, &img, 20);
        let (a, b) = ounce_lift::check::check(dir, &prog, &sums, &rec, 40);
        std::process::exit(if a + b + n == 0 { 0 } else { 1 });
    }
    let by_name: HashMap<&str, (usize, &Part)> = parts.iter().enumerate().map(|(k, p)| (p.name.as_str(), (k, p))).collect();
    let (c_lift0, _) = usage();
    let tl = Instant::now();
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
                let (pk, part) = by_name[names[k].as_str()];
                let tr = 0.0;
                let t = Instant::now();
                let l = lift_part(&mut w, &meta, &plan.sj_rets, &targets, &rec, part, skip);
                // (every code pointer site of the census translated, and no other: game.prepare's check)
                assert_eq!(l.sites as u64, pp.census[pk].0, "{}: code pointer sites translated differ from the census", part.name);
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
    let (c_lift, _) = usage();
    println!(
        "lift: {} partitions, {} functions lifted, {} skipped; {:.2} s wall, {:.2} s CPU (lift+emit {:.2} s in the workers)",
        names.len(),
        g.0,
        nskip,
        tl.elapsed().as_secs_f64(),
        c_lift - c_lift0,
        g.3
    );
    println!(
        "whole pipeline: {:.2} s wall, {:.2} s CPU ({:.2} s of it busy in tasks: discovery, plan, summaries, lift)",
        t0.elapsed().as_secs_f64(),
        c_lift,
        c_disc - c_img + busy_s() + g.3 + c_img
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
