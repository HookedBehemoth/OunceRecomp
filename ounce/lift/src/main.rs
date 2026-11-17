//! ounce-lift [<export dir>] <out dir> [-j N] [--parts a,b,...] [--skip] [--check] [--golden <dir>] [--game <dir>]
//! [--work <dir>] [--exefs <dir>] [--root <dir>]: the whole lift from the dump's exefs, the host's C and the game
//! description: gen/, rt64/, image/image.bin and one .ll per partition (EXPORT.md).
//! --skip: a function using a template not ported yet is left out and counted ("skipped: template X").
//! --check: compare the scan, the image, the plan, every summary and record with Python's (the export) and stop.
//! --golden: gen/, rt64/ and image.bin against the golden's sha256.

use ounce_lift::bounds::Records;
use ounce_lift::export::Part;
use ounce_lift::gen;
use ounce_lift::part::{lift_part, Worker};
use ounce_lift::session::{Base, Opts};
use ounce_lift::summary::{summarise, Program};
use ounce_lift::game::Game;
use ounce_lift::patch;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::Instant;

mod tools;

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
fn answers_ir(answers: &[(u64, String, u64)]) -> String {
    use std::fmt::Write;
    let mut s = format!("target triple = \"{}\"", ounce_lift::emit::TRIPLE);
    for (a, name, v) in answers {
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

/// the generated files and image.bin against the golden's sha256 (golden/meta.json "files"): the differing count
fn check_golden(golden: &std::path::Path, files: &[(String, Vec<u8>)], image: &[u8]) -> usize {
    use sha2::{Digest, Sha256};
    let meta: serde_json::Value = serde_json::from_slice(&std::fs::read(golden.join("meta.json")).expect("golden meta.json")).unwrap();
    let want = meta["files"].as_object().expect("golden files");
    let hex = |d: &[u8]| Sha256::digest(d).iter().map(|b| format!("{b:02x}")).collect::<String>();
    let mut bad = 0;
    let mut seen = 0;
    for (name, data) in files.iter().map(|(n, d)| (n.as_str(), d.as_slice())).chain([("image/image.bin", image)]) {
        match want.get(name).and_then(|v| v.as_str()) {
            Some(w) => {
                seen += 1;
                if hex(data) != w {
                    bad += 1;
                    println!("golden: {name} differs");
                }
            }
            None => println!("golden: {name} not in the golden"),
        }
    }
    println!("golden: {} of {} files equal (gen/, rt64/, image.bin)", seen - bad, want.len());
    bad + want.len() - seen
}

/// facts.tsv: per lifted function its body (size, a hash of its pcs, words and tables), its summary and its call
/// record, one line each by entry: what tools/diffout.py explains a change of the IR with (no compile input)
fn write_facts(path: &std::path::Path, prog: &Program, sums: &[ounce_lift::summary::Summary], rec: &Records) {
    use std::fmt::Write as _;
    let fnv = |h: &mut u64, v: u64| {
        for b in v.to_le_bytes() {
            *h = (*h ^ b as u64).wrapping_mul(0x100_0000_01b3);
        }
    };
    let mut s = String::from("entry\tinsns\tbody\tinputs\tclobbers\tmustdef\tpreserved\tresults\treturns\tdemote\tunknown\tboundary\trecord\n");
    for (k, &e) in prog.fns.iter().enumerate() {
        let b = &prog.bodies[k];
        let mut h = 0xcbf2_9ce4_8422_2325u64;
        for (&p, &w) in b.pcs.iter().zip(b.words.iter()) {
            fnv(&mut h, p);
            fnv(&mut h, w as u64);
        }
        for (br, t) in b.jt.iter() {
            fnv(&mut h, *br);
            for &x in t {
                fnv(&mut h, x);
            }
        }
        let m = &sums[k];
        let r = rec.bound(e);
        writeln!(
            s,
            "f_{e:08x}\t{}\t{h:016x}\t{:x}\t{:x}\t{:x}\t{:x}\t{:x}\t{}\t{}\t{}\t{}\t{:?}/{:x}/{:x}/{:x}/{:x}/{:x}/{}",
            b.pcs.len(),
            m.inputs,
            m.clobbers,
            m.mustdef,
            m.preserved,
            m.results,
            m.returns as u8,
            ounce_lift::summary::reason_names(m.demote).join(","),
            m.unknown as u8,
            m.boundary as u8,
            r.kind,
            r.reads,
            r.clobbers,
            r.keeps,
            r.must,
            r.results,
            r.noreturn as u8
        )
        .unwrap();
    }
    std::fs::write(path, s).unwrap();
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

/// `ounce-lift records [--game <dir>] [--root <dir>] <object>...`: the host objects' hos_patch records against the scan
fn records(argv: Vec<String>) {
    let (mut game_dir, mut root, mut objs) = (PathBuf::from("games/oot"), PathBuf::from("."), Vec::new());
    let mut args = argv.into_iter();
    while let Some(a) = args.next() {
        match a.as_str() {
            "--game" => game_dir = PathBuf::from(args.next().unwrap()),
            "--root" => root = PathBuf::from(args.next().unwrap()),
            _ => objs.push(PathBuf::from(a)),
        }
    }
    let game = Game::load(&game_dir).unwrap_or_else(|e| fail(e));
    match patch::check_records(&root, &game.host, &objs) {
        Ok(line) => println!("{line}"),
        Err(e) => fail(e),
    }
}

/// `ounce-lift rt64 <dir> [--root <dir>]`: the guest model (a64.h, a64_rt.c), which needs no dump
fn rt64(argv: Vec<String>) {
    let (mut root, mut out) = (PathBuf::from("."), None);
    let mut args = argv.into_iter();
    while let Some(a) = args.next() {
        match a.as_str() {
            "--root" => root = PathBuf::from(args.next().unwrap()),
            _ => out = Some(PathBuf::from(a)),
        }
    }
    let out = out.unwrap_or_else(|| fail("usage: ounce-lift rt64 <dir> [--root <dir>]"));
    std::fs::create_dir_all(&out).unwrap_or_else(|e| fail(e));
    for (name, text) in gen::rt64(&root).unwrap_or_else(|e| fail(e)) {
        std::fs::write(out.join(name), text).unwrap_or_else(|e| fail(e));
    }
}

/// the options every subcommand that lifts takes
const OPTS: &str = "[--game <dir>] [--work <dir>] [--root <dir>] [--exefs <dir>]";

/// takes one of OPTS (and its value) into o: false when a is none of them
fn opt_arg(o: &mut Opts, a: &str, args: &mut impl Iterator<Item = String>) -> bool {
    let mut val = || PathBuf::from(args.next().unwrap_or_else(|| fail(format!("{a} wants a value"))));
    match a {
        "--game" => o.game = val(),
        "--work" => o.work = val(),
        "--root" => o.root = val(),
        "--exefs" => o.exefs = Some(val()),
        "--max-func" => o.max_func = val().to_string_lossy().parse().unwrap_or_else(|_| fail("--max-func N")),
        _ => return false,
    }
    true
}

/// the message on stderr, exit status 1
fn fail(msg: impl std::fmt::Display) -> ! {
    eprintln!("ounce-lift: {msg}");
    std::process::exit(1)
}

const HELP: &str = "ounce-lift: the lifter. Subcommands (each but rt64, records and scan reads the dump; OPTS: --game <dir> (games/oot), --work <dir> (work), --root <dir> (.), --exefs <dir>):
  [lift] <out> [-j N] [--parts a,b] [--skip] [--golden <dir>] OPTS   the lift: <out>/gen, rt64, image/image.bin, lift_*.ll
  rt64 <dir> [--root <dir>]                                         the guest model (a64.h, a64_rt.c)
  records [--game] [--root] <object>...                             the host objects' hos_patch records against the scan
  scan [--game] [--root] [<out.json>]                               the host's declarations as the lift reads them, as JSON
  site <address|label>... OPTS                                      what a patch site is: paths, liveness, record, a PATCH_AT to paste
  check [<file:line> | - | --all] OPTS                              the declarations against the plan; one: its cost (the summaries' difference)
  report OPTS                                                       every declaration's local cost, costliest first
  dump <address|label>... [--tree] [--opt O1|O2] [--asm] [-o <dir>] OPTS   a function's guest code, IR and LLVM IR";

fn main() {
    let mut argv: Vec<String> = std::env::args().skip(1).collect();
    match argv.first().map(String::as_str) {
        Some("records") => records(argv.split_off(1)),
        Some("rt64") => rt64(argv.split_off(1)),
        Some("scan") => tools::scan(argv.split_off(1)),
        Some("site") => tools::site(argv.split_off(1)),
        Some("check") => tools::check(argv.split_off(1)),
        Some("report") => tools::report(argv.split_off(1)),
        Some("dump") => tools::dump(argv.split_off(1)),
        Some("-h" | "--help" | "help") => println!("{}", HELP),
        Some("lift") => lift(argv.split_off(1)),
        _ => lift(argv),
    }
}

/// `ounce-lift [lift] ...`: the whole lift (the module's description)
fn lift(argv: Vec<String>) {
    let mut args = argv.into_iter();
    let mut out = None::<PathBuf>;
    let mut jobs = std::thread::available_parallelism().map_or(1, |n| n.get());
    let (mut only, mut skip, mut golden) = (None::<Vec<String>>, false, None::<PathBuf>);
    let mut o = Opts::default();
    while let Some(a) = args.next() {
        match a.as_str() {
            "-j" => jobs = args.next().unwrap().parse().unwrap(),
            "--parts" => only = Some(args.next().unwrap().split(',').map(String::from).collect()),
            "--skip" => skip = true,
            "--golden" => golden = Some(PathBuf::from(args.next().unwrap())),
            _ if opt_arg(&mut o, &a, &mut args) => {}
            _ if out.is_none() && !a.starts_with('-') => out = Some(PathBuf::from(a)),
            _ => fail(format!("lift: what is {a}? usage: ounce-lift [lift] <out dir> [-j N] [--parts a,b] [--skip] [--golden <dir>] {OPTS}")),
        }
    }
    let out = &out.unwrap_or_else(|| fail(format!("usage: ounce-lift [lift] <out dir> [-j N] [--parts a,b] [--skip] [--golden <dir>] {OPTS}")));
    std::fs::create_dir_all(out).unwrap();
    rayon::ThreadPoolBuilder::new().num_threads(jobs.max(1)).build_global().unwrap();
    let t0 = Instant::now();
    // the scan of the host's declarations, the image from the exefs, the discovery and the plan (session::Base)
    let base = Base::new(&o).unwrap_or_else(|e| fail(e));
    let (c_disc, _) = usage();
    let tp = Instant::now();
    // the rows' functions and the lift's checks of them, the PATCH_FUNCs: the patches as the lift reads them
    let pa = base.patches(&base.ps).unwrap_or_else(|e| fail(e));
    let (game, ps, img, facts, plan) = (&base.game, &base.ps, &base.img, &base.facts, &base.plan);
    let (meta, pp, hle_at, answers) = (&pa.meta, &pa.parts, &pa.hle_at, &pa.answers);
    let b_plan = busy_s();
    let (c_plan, m_plan) = usage();
    let t_plan = tp.elapsed().as_secs_f64();
    let tm = base.times;
    println!("patch scan: {} files, {} sites, {} functions, {} HLE / {} NV / {} effects, {} answers; {:.3} s", game.host.len(), ps.sites.len(), ps.funcs.len(), ps.hle.len(), ps.nv.len(), ps.declared.len(), ps.answers.len(), tm.scan);
    println!(
        "image: exefs loaded in {:.2} s, built in {:.2} s; {} hooks ({} HLE, {} stubbed), {} entries, {} more from ADR / ADRP+ADD; {:.2} s wall",
        tm.load,
        tm.build,
        img.hooks.len(),
        base.hle.len(),
        base.stubbed.len(),
        facts.entries.len(),
        base.entries.len() - facts.entries.len(),
        tm.image,
    );
    let f = base.found;
    println!(
        "discovery: {} functions, {} jump tables ({} on their index) in {} rounds, {} PLT stubs, {} setjmp owners; {:.2} s wall",
        f.0, f.1, f.2, f.5, f.3, f.4, tm.discovery
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
    // the generated sources and the image
    let tg = Instant::now();
    let functions: Vec<u64> = plan.bodies.iter().map(|b| b.entry).collect();
    let answered: BTreeSet<u64> = answers.iter().map(|a| a.0).collect();
    let (table_c, hooks_c) = gen::tables(&functions, &img.hooks, &answered);
    let (stubs, nstubs) = gen::stubs_c(&img.hooks, &base.implemented);
    let sizes: Vec<(u64, usize)> = functions.iter().copied().zip(plan.insns.iter().copied()).collect();
    let mut files: Vec<(String, Vec<u8>)> = vec![
        ("gen/a64_table.c".into(), table_c.into_bytes()),
        ("gen/a64_hooks.c".into(), hooks_c.into_bytes()),
        ("gen/a64_code_slots.c".into(), gen::code_slots(img, facts, &functions).into_bytes()),
        ("gen/a64_stubs.c".into(), stubs.into_bytes()),
        ("gen/hos_image.c".into(), gen::image_c(img, facts, &base.exefs).unwrap_or_else(|e| fail(e)).into_bytes()),
        ("gen/nvn2_exports.c".into(), gen::nvn_exports_c(&base.nvn).into_bytes()),
        ("gen/functions.txt".into(), gen::functions_txt(&sizes).into_bytes()),
    ];
    for (name, text) in gen::rt64(&o.root).unwrap_or_else(|e| fail(e)) {
        files.push((format!("rt64/{name}"), text.into_bytes()));
    }
    for (name, data) in &files {
        let p = out.join(name);
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(&p, data).unwrap();
    }
    // (the same definitions as IR, for the player builder, which has no C compiler; not in the golden)
    let lls = [
        ("gen/a64_table.ll", gen::table_ll(&functions, &img.hooks)),
        ("gen/hos_image.ll", gen::image_ll(img, facts, &base.exefs).unwrap_or_else(|e| fail(e))),
        ("gen/a64_code_slots.ll", gen::code_slots_ll(img, facts, &functions)),
    ];
    for (name, text) in &lls {
        std::fs::write(out.join(name), text).unwrap();
    }
    std::fs::create_dir_all(out.join("image")).unwrap();
    std::fs::write(out.join("image/image.bin"), &img.mem).unwrap();
    println!("gen: {} files, {} import stubs; {:.2} s wall", files.len(), nstubs, tg.elapsed().as_secs_f64());
    let gen_bad = golden.as_ref().map_or(0, |g| check_golden(g, &files, &img.mem));
    let odd: Vec<&(u64, String)> = pp.census.iter().flat_map(|c| c.2.iter()).collect();
    if !odd.is_empty() {
        fail(format!("{} code addresses formed in a way code_ptrs does not know, first {:#x}: {}", odd.len(), odd[0].0, odd[0].1));
    }
    let targets: HashSet<u64> = pp.code_targets.iter().copied().collect();
    let parts = &pp.parts;
    let names: Vec<String> = parts.iter().map(|p| p.name.clone()).filter(|n| only.as_ref().is_none_or(|o| o.contains(n))).collect();
    if only.as_ref().is_none_or(|o| o.iter().any(|n| n == ANSWERS)) {
        std::fs::write(out.join(format!("{ANSWERS}.ll")), answers_ir(answers)).unwrap();
    }
    let ts = Instant::now();
    let mut prog = Program::new(meta, plan, parts, hle_at, &pa.svcs, &facts.names);
    let (sums, st) = summarise(&mut prog);
    let rec = Records::new(&prog, &sums, &plan.sj_rets);
    write_facts(&out.join("facts.tsv"), &prog, &sums, &rec);
    let b_sum = busy_s() - b_plan;
    let (c_sum, m_sum) = usage();
    println!(
        "summaries: {} functions, {} components (largest {}) in {} levels, {} noreturn by callers; graph {:.2} s, fixed point {:.2} s, results {:.2} s; {:.2} s wall, {:.2} s CPU ({:.2} s busy); peak {:.0} MB",
        st.functions, st.sccs, st.largest, st.levels, st.noreturn_by_callers, st.graph_s, st.fixed_s, st.results_s,
        ts.elapsed().as_secs_f64(), c_sum - c_plan, b_sum, m_sum
    );
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
                let l = lift_part(&mut w, meta, &plan.sj_rets, &targets, &rec, part, skip);
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
        c_disc + busy_s() + g.3
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
    if gen_bad > 0 {
        std::process::exit(1);
    }
}
