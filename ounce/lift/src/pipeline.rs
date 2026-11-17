//! The whole lift as a library call: the dump's exefs, the host's C and the game description -> the generated code
//! (gen/), the image, the answers module and one IR module per partition, each handed to an `Out` as it is made, so a
//! caller can compile them in its own process without files in between.

use crate::bounds::Records;
use crate::export::Part;
use crate::gen;
use crate::part::{lift_part, Worker};
use crate::session::{Base, Opts};
use crate::summary::{summarise, Program};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::Instant;

/// What one lift does
#[derive(Clone, Default)]
pub struct Run {
    pub opts: Opts,
    /// lift threads (and the summaries' pool)
    pub jobs: usize,
    /// these partitions only (and lift_answers when named); None: all
    pub only: Option<Vec<String>>,
    /// a function using a template not ported yet is left out and counted ("skipped: template X")
    pub skip: bool,
    /// the hot partitions (lift_h*) lifted first: a caller compiling as they come starts its longest compiles early
    pub hot_first: bool,
}

/// Where the lift's output goes. `part` is called from the lift's threads as each partition is made; it may block
/// (a caller's compile queue), which holds that lift thread.
pub trait Out: Sync {
    /// a file of the lift's folder: gen/<name>, image/image.bin, lift_answers.ll, facts.tsv
    fn file(&self, rel: &str, data: Vec<u8>);
    /// a partition's IR module: `head` then `body`
    fn part(&self, name: &str, head: &[u8], body: &[u8]);
    /// whether facts.tsv (what diffout explains a change of the IR with) is wanted
    fn wants_facts(&self) -> bool {
        true
    }
}

pub struct Stats {
    pub partitions: usize,
    pub lifted: usize,
    pub skipped: usize,
}

/// the answers module's name (the build's lift_answers.o)
pub const ANSWERS: &str = "lift_answers";

/// the generic entries of the answered functions (game.answers_ir): f_<addr> writes the answer to x0 and returns
fn answers_ir(answers: &[(u64, String, u64)]) -> String {
    use std::fmt::Write;
    let mut s = format!("target triple = \"{}\"", crate::emit::TRIPLE);
    for (a, name, v) in answers {
        write!(
            s,
            "\ndefine void @f_{a:08x}(ptr noalias nocapture %ctx) {{   ; {name}\n  %x0 = getelementptr i8, ptr %ctx, i64 {}\n  store i64 {}, ptr %x0, align 8\n  ret void\n}}",
            crate::emit::ctx_off(0),
            *v as i64
        )
        .unwrap();
    }
    s.push('\n');
    s
}

/// facts.tsv: per lifted function its body (size, a hash of its pcs, words and tables), its summary and its call
/// record, one line each by entry: what tools/diffout.py explains a change of the IR with (no compile input)
fn facts_tsv(prog: &Program, sums: &[crate::summary::Summary], rec: &Records) -> String {
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
            crate::summary::reason_names(m.demote).join(","),
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
    s
}

/// the parallel stages' busy seconds so far (crate::busy)
fn busy_s() -> f64 {
    crate::BUSY_NS.load(Ordering::Relaxed) as f64 / 1e9
}

/// (process CPU seconds, user and system; VmHWM in MB)
pub fn usage() -> (f64, f64) {
    let stat = std::fs::read_to_string("/proc/self/stat").unwrap_or_default();
    let f: Vec<&str> = stat.rsplit(')').next().unwrap_or("").split_whitespace().collect();
    let tick = |k: usize| f.get(k).and_then(|x| x.parse::<f64>().ok()).unwrap_or(0.0) / 100.0;
    let hwm = std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| s.lines().find(|l| l.starts_with("VmHWM:")).and_then(|l| l[6..].trim().trim_end_matches(" kB").trim().parse::<f64>().ok()))
        .unwrap_or(0.0);
    (tick(11) + tick(12), hwm / 1024.0)
}

/// The lift (progress lines on stdout)
pub fn lift(r: &Run, out: &dyn Out) -> Result<Stats, String> {
    let (jobs, only, skip) = (r.jobs.max(1), &r.only, r.skip);
    // (the global pool: a second lift in the process keeps the first one's)
    let _ = rayon::ThreadPoolBuilder::new().num_threads(jobs).build_global();
    let t0 = Instant::now();
    // the scan of the host's declarations, the image from the exefs, the discovery and the plan (session::Base)
    let base = Base::new(&r.opts)?;
    let (c_disc, _) = usage();
    let tp = Instant::now();
    // the rows' functions and the lift's checks of them, the PATCH_FUNCs: the patches as the lift reads them
    let pa = base.patches(&base.ps)?;
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
    let (stubs, nstubs) = gen::stubs_ll(&img.hooks, &base.implemented);
    let sizes: Vec<(u64, usize)> = functions.iter().copied().zip(plan.insns.iter().copied()).collect();
    let files: Vec<(&str, Vec<u8>)> = vec![
        ("gen/a64_table.ll", gen::table_ll(&functions, &img.hooks).into_bytes()),
        ("gen/a64_hooks.ll", gen::hooks_ll(&img.hooks, &answered).into_bytes()),
        ("gen/a64_code_slots.ll", gen::code_slots_ll(img, facts, &functions).into_bytes()),
        ("gen/a64_stubs.ll", stubs.into_bytes()),
        ("gen/hos_image.ll", gen::image_ll(img, facts, &base.exefs)?.into_bytes()),
        ("gen/nvn2_exports.ll", gen::nvn_exports_ll(&base.nvn).into_bytes()),
        ("gen/functions.txt", gen::functions_txt(&sizes).into_bytes()),
    ];
    let nfiles = files.len();
    for (name, data) in files {
        out.file(name, data);
    }
    out.file("image/image.bin", img.mem.clone());
    println!("gen: {} files, {} import stubs; {:.2} s wall", nfiles, nstubs, tg.elapsed().as_secs_f64());
    let odd: Vec<&(u64, String)> = pp.census.iter().flat_map(|c| c.2.iter()).collect();
    if !odd.is_empty() {
        return Err(format!("{} code addresses formed in a way code_ptrs does not know, first {:#x}: {}", odd.len(), odd[0].0, odd[0].1));
    }
    let targets: HashSet<u64> = pp.code_targets.iter().copied().collect();
    let parts = &pp.parts;
    let mut names: Vec<String> = parts.iter().map(|p| p.name.clone()).filter(|n| only.as_ref().is_none_or(|o| o.contains(n))).collect();
    if r.hot_first {
        names.sort_by_key(|n| !n.starts_with("lift_h"));
    }
    if only.as_ref().is_none_or(|o| o.iter().any(|n| n == ANSWERS)) {
        out.file(&format!("{ANSWERS}.ll"), answers_ir(answers).into_bytes());
    }
    let ts = Instant::now();
    let mut prog = Program::new(meta, plan, parts, hle_at, &pa.svcs, &facts.names);
    let (sums, st) = summarise(&mut prog);
    let rec = Records::new(&prog, &sums, &plan.sj_rets);
    if out.wants_facts() {
        out.file("facts.tsv", facts_tsv(&prog, &sums, &rec).into_bytes());
    }
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
    let total = Mutex::new((0usize, BTreeMap::<String, usize>::new(), 0f64, 0f64));
    // (hot_first: the hot partitions all handed over before the first cold one, so a compile of a cold one never
    // takes the place of a hot one)
    let nhot = if r.hot_first { names.iter().take_while(|n| n.starts_with("lift_h")).count() } else { 0 };
    for phase in [&names[..nhot], &names[nhot..]] {
        let next = AtomicUsize::new(0);
        std::thread::scope(|s| {
            for _ in 0..jobs.max(1) {
                s.spawn(|| {
                let mut w = Worker::default();
                loop {
                    let k = next.fetch_add(1, Ordering::Relaxed);
                    if k >= phase.len() {
                        break;
                    }
                    let (pk, part) = by_name[phase[k].as_str()];
                    let tr = 0.0;
                    let t = Instant::now();
                    let l = lift_part(&mut w, meta, &plan.sj_rets, &targets, &rec, part, skip);
                    // (every code pointer site of the census translated, and no other: game.prepare's check)
                    assert_eq!(l.sites as u64, pp.census[pk].0, "{}: code pointer sites translated differ from the census", part.name);
                    let tl = t.elapsed().as_secs_f64();
                    out.part(&phase[k], &l.head, &w.body);
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
    }
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
    Ok(Stats { partitions: names.len(), lifted: g.0, skipped: nskip })
}
