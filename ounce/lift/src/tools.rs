//! The patch tools and the per-function dump (nx2/patch.py and nx2/lift2/dump.py before them): what a patch site is,
//! whether the host's declarations fit the plan and what each costs, and a function's guest code, IR and LLVM IR. Each
//! runs the lift's analysis from the dump (session::Base, ~6 s) instead of reading a saved plan; nothing is written
//! beside the lift. `scan` writes the host's declarations as the lift reads them, as JSON, for the host's tests.

use ounce_lift::bounds::{Records, KEEP_ALL};
use ounce_lift::build::target;
use ounce_lift::export::{Bound, Part};
use ounce_lift::ir::ALL;
use ounce_lift::part::{lift_part, Worker};
use ounce_lift::patch::{self, Guest, PatchSet, Row, Where};
use ounce_lift::session::{host_texts, Base, Opts, Patches};
use ounce_lift::summary::{self, analyse_alone, def_use, flow_of, live_in_out, regs, rows_from, summarise, Plain, Program, Summary, K};
use serde_json::json;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::path::PathBuf;

use crate::{fail, opt_arg, OPTS};

// ---- the common part: arguments, labels, the analysis

/// a line into the tool's text
macro_rules! p {
    ($o:expr) => { $o.push('\n') };
    ($o:expr, $($t:tt)*) => {{ use std::fmt::Write as _; writeln!($o, $($t)*).unwrap() }};
}

struct Args {
    o: Opts,
    pos: Vec<String>,
    flags: HashSet<String>,
    vals: HashMap<String, String>,
}

/// OPTS, the flags in `flags` (no value) and `vals` (one value), the rest positional
fn args(argv: Vec<String>, flags: &[&str], vals: &[&str], usage: &str) -> Args {
    let mut a = Args { o: Opts::default(), pos: Vec::new(), flags: HashSet::new(), vals: HashMap::new() };
    let mut it = argv.into_iter();
    while let Some(x) = it.next() {
        if flags.contains(&x.as_str()) {
            a.flags.insert(x);
        } else if vals.contains(&x.as_str()) {
            let v = it.next().unwrap_or_else(|| fail(format!("{x} wants a value; usage: {usage}")));
            a.vals.insert(x, v);
        } else if opt_arg(&mut a.o, &x, &mut it) {
        } else if x.starts_with("--") {
            fail(format!("what is {x}? usage: {usage} {OPTS}"));
        } else {
            a.pos.push(x);
        }
    }
    a
}

/// guest address -> label of the game's label file
fn labels(base: &Base) -> BTreeMap<u64, String> {
    let text = std::fs::read_to_string(&base.game.labels).unwrap_or_default();
    text.lines()
        .filter_map(|l| {
            let mut w = l.split_whitespace();
            let a = u64::from_str_radix(w.next()?, 16).ok()?;
            let n = w.next().filter(|n| !n.starts_with('#'))?;
            Some((a, n.to_string()))
        })
        .collect()
}

/// an address (0x.., f_.., hex) or a label
fn resolve_addr(arg: &str, labels: &BTreeMap<u64, String>) -> u64 {
    if let Some((&a, _)) = labels.iter().find(|(_, l)| l.as_str() == arg) {
        return a;
    }
    let h = arg.strip_prefix("0x").or_else(|| arg.strip_prefix("f_")).unwrap_or(arg);
    u64::from_str_radix(h, 16).unwrap_or_else(|_| fail(format!("{arg} is neither an address nor a label")))
}

fn base(o: &Opts) -> Base {
    Base::new(o).unwrap_or_else(|e| fail(e))
}

fn patches(b: &Base, ps: &PatchSet) -> Patches {
    b.patches(ps).unwrap_or_else(|e| fail(e))
}

/// an LLVM tool: OUNCE_LLVM_BIN/<name>, else the PATH's
fn llvm_tool(name: &str) -> PathBuf {
    if let Some(d) = std::env::var_os("OUNCE_LLVM_BIN").filter(|v| !v.is_empty()) {
        let p = PathBuf::from(d).join(name);
        if p.exists() {
            return p;
        }
    }
    PathBuf::from(name)
}

const MC_ATTRS: &str = "+v8.5a,+lse,+rcpc,+pauth,+dotprod,+fullfp16,+fp16fml,+i8mm,+bf16,+crc,+mte,+aes,+sha2";

/// llvm-mc's text of each word ("mnemonic operands"), a branch's target as an address, "(invalid)" for a word it
/// does not decode
fn disasm(pcs: &[u64], words: &[u32]) -> Vec<String> {
    use std::io::Write;
    use std::process::{Command, Stdio};
    let mut src = String::new();
    for w in words {
        src += &format!("0x{:02x} 0x{:02x} 0x{:02x} 0x{:02x}\n", w & 255, w >> 8 & 255, w >> 16 & 255, w >> 24);
    }
    let child = Command::new(llvm_tool("llvm-mc"))
        .args(["--disassemble", "-triple=aarch64", &format!("-mattr={MC_ATTRS}")])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn();
    let Ok(mut child) = child else { return words.iter().map(|w| format!(".word {w:#010x}   (no llvm-mc)")).collect() };
    let mut stdin = child.stdin.take().unwrap();
    let feed = std::thread::spawn(move || stdin.write_all(src.as_bytes()));
    let out = child.wait_with_output().unwrap_or_else(|e| fail(e));
    let _ = feed.join();
    let (stdout, stderr) = (String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
    let bad: HashSet<usize> = stderr
        .lines()
        .filter(|l| l.contains("invalid instruction encoding"))
        .filter_map(|l| l.strip_prefix("<stdin>:")?.split(':').next()?.parse::<usize>().ok())
        .map(|n| n - 1)
        .collect();
    let mut lines = stdout.lines().filter(|l| l.starts_with('\t')).map(|l| {
        let t = l.split("//").next().unwrap_or("").trim();
        let (mn, ops) = t.split_once('\t').unwrap_or((t, ""));
        format!("{mn} {}", ops.split_whitespace().collect::<Vec<_>>().join(" ")).trim().to_string()
    });
    (0..words.len())
        .map(|k| {
            if bad.contains(&k) {
                return "(invalid)".to_string();
            }
            let t = lines.next().unwrap_or_default();
            match ounce_decode::decode(words[k]).and_then(|i| target(&i, pcs[k])) {
                Some(tg) => format!("{t}   ; {tg:#x}"),
                None => t,
            }
        })
        .collect()
}

fn imm26_target(pc: u64, w: u32) -> u64 {
    let off = ((w & 0x3FF_FFFF) as i64) - (((w & 0x200_0000) as i64) << 1);
    (pc as i64 + off * 4) as u64
}

/// {callee: BL sites to it} over every body
fn callers(b: &Base, prog: &Program) -> HashMap<u64, u64> {
    let mut n = HashMap::new();
    for body in &b.plan.bodies {
        for (&pc, &w) in body.pcs.iter().zip(&body.words) {
            if w & 0xFC00_0000 == 0x9400_0000 {
                *n.entry(prog.callee(imm26_target(pc, w))).or_insert(0) += 1;
            }
        }
    }
    n
}

/// {callee: tail branches (B out of the body) to it} over every body
fn tail_sites(b: &Base, prog: &Program) -> HashMap<u64, u64> {
    let entries: HashSet<u64> = b.plan.entries.iter().copied().collect();
    let mut n = HashMap::new();
    for body in &b.plan.bodies {
        for (&pc, &w) in body.pcs.iter().zip(&body.words) {
            if w & 0xFC00_0000 == 0x1400_0000 {
                let t = imm26_target(pc, w);
                if body.pcs.binary_search(&t).is_err() || (t != body.entry && entries.contains(&t)) {
                    *n.entry(prog.callee(t)).or_insert(0) += 1;
                }
            }
        }
    }
    n
}

fn record(b: &Bound) -> String {
    format!(
        "{}: reads {} | clobbers {} | keeps {} | must {} | results {}{}",
        b.kind.name(),
        regs(b.reads),
        regs(b.clobbers),
        regs(b.keeps),
        regs(b.must),
        regs(b.results),
        if b.noreturn { " | noreturn" } else { "" }
    )
}

/// a summary with every fact known and no demotion, of a function that returns (bounds.full)
fn full(s: Option<&Summary>) -> bool {
    s.is_some_and(|s| s.demote == 0 && !s.unknown && s.returns)
}

fn wh(w: &Where) -> String {
    format!("{}:{}", w.0, w.1)
}

/// the analysis of one set of declarations, built on the caller's stack
macro_rules! analysis {
    ($b:expr, $pa:expr, $prog:ident, $sums:ident) => {
        let mut $prog = Program::new(&$pa.meta, &$b.plan, &$pa.parts.parts, &$pa.hle_at, &$pa.svcs, &$b.facts.names);
        let ($sums, _) = summarise(&mut $prog);
    };
}

struct Names<'a>(&'a BTreeMap<u64, String>);
impl Names<'_> {
    fn of(&self, e: u64) -> String {
        match self.0.get(&e) {
            Some(l) => format!("f_{e:08x} ({l})"),
            None => format!("f_{e:08x}"),
        }
    }
}

// ---- scan

/// `ounce-lift scan [--game] [--root] [<out.json>]`: the host's declarations as the lift reads them (no dump)
pub fn scan(argv: Vec<String>) {
    let a = args(argv, &[], &[], "ounce-lift scan [<out.json>]");
    let game = ounce_lift::game::Game::load(&a.o.game).unwrap_or_else(|e| fail(e));
    let ps = patch::scan_texts(&host_texts(&a.o.root, &game).unwrap_or_else(|e| fail(e))).unwrap_or_else(|e| fail(e));
    let rows = patch::rows(&ps.sites).unwrap_or_else(|e| fail(e));
    let guest = |g: &Option<Guest>| match g {
        None => serde_json::Value::Null,
        Some(Guest::Computed) => json!("computed"),
        Some(Guest::List(l)) => json!(l),
    };
    let rows: Vec<_> = rows
        .iter()
        .map(|r| {
            json!({"where": [r.at.0, r.at.1], "value": r.value, "pc": r.pc, "word": r.word, "fn": r.fname, "args": r.args,
                   "result": r.result, "mode": r.mode, "io": r.io, "guest": guest(&r.guest)})
        })
        .collect();
    let funcs: Vec<_> = ps
        .funcs
        .iter()
        .map(|f| {
            let reads: std::collections::BTreeSet<String> = f.args.iter().map(|x| patch::var(x)).collect();
            json!({"where": [f.at.0, f.at.1], "kind": f.kind, "addr": f.addr, "symbol": f.symbol, "name": f.name,
                   "orig_name": f.orig_name, "fn": f.fname, "result": f.result, "args": f.args, "reads": reads,
                   "orig": f.orig, "guest": f.guest})
        })
        .collect();
    let replaced = |m: &BTreeMap<String, patch::Replaced>| -> serde_json::Value {
        m.values()
            .map(|r| (r.symbol.clone(), json!({"where": [r.at.0, r.at.1], "kind": r.kind, "reads": r.reads, "clobbers": r.clobbers})))
            .collect::<serde_json::Map<_, _>>()
            .into()
    };
    let answers: serde_json::Map<_, _> =
        ps.answers.values().map(|x| (x.symbol.clone(), json!({"where": [x.at.0, x.at.1], "value": x.value.to_string()}))).collect();
    let out = json!({"rows": rows, "funcs": funcs, "hle": replaced(&ps.hle), "nv": replaced(&ps.nv),
                     "declared": replaced(&ps.declared), "answers": answers});
    let text = serde_json::to_string_pretty(&out).unwrap();
    match a.pos.first() {
        Some(p) => std::fs::write(p, text).unwrap_or_else(|e| fail(e)),
        None => println!("{text}"),
    }
}

// ---- site

/// (mode, result, operands, why) of a PATCH_AT suggested at instruction k of the function (patch.suggest)
fn suggest(w: u32, pc: u64, li: u128, lo: u128, rec: &Records, prog: &Program, n: &Names) -> (String, String, Vec<String>, String) {
    let gpr = |m: u128| (0..31u32).filter(move |r| m >> r & 1 != 0);
    if patch::is_call(w) {
        let i = ounce_decode::decode(w).unwrap();
        if i.enc.template == "b" {
            let c = prog.callee(target(&i, pc).unwrap());
            let b = rec.bound(c);
            let args: Vec<u32> = if b.reads != ALL { gpr(b.reads).filter(|&r| r < 8).collect() } else { (0..8).filter(|r| li >> r & 1 != 0).collect() };
            let res = gpr(b.results & lo).find(|&r| r < 8).map_or("none".to_string(), |r| format!("x{r}"));
            let why = if b.reads != ALL {
                format!("the callee {}: reads {}, results {}", n.of(c), regs(b.reads), regs(b.results))
            } else {
                format!("the callee {}: {}, its record unknown: the live arguments", n.of(c), b.kind.name())
            };
            return ("instead".into(), res, args.iter().take(8).map(|r| format!("x{r}")).collect(), why);
        }
        let rn = i.f(ounce_decode::Fld::Rn);
        let mut args: Vec<u32> = (0..8).filter(|r| li >> r & 1 != 0).collect();
        if (8..31).contains(&rn) {
            args.push(rn);
        }
        let res = if lo & 1 != 0 { "x0" } else { "none" };
        return (
            "instead".into(),
            res.into(),
            args.iter().take(8).map(|r| format!("x{r}")).collect(),
            format!("an indirect call (x{rn}): the AAPCS arguments live into it and its target"),
        );
    }
    let (us, df) = def_use(w);
    let (mut res, mut order) = ("none".to_string(), Vec::new());
    if let Some((r, bits)) = patch::dest(w) {
        if lo >> r & 1 != 0 {
            res = format!("{}{r}", if bits == 32 { "w" } else { "x" });
            order.push(r);
        }
    }
    for r in gpr(us | df).chain(gpr(li)) {
        if !order.contains(&r) {
            order.push(r);
        }
    }
    ("after".into(), res, order.iter().take(6).map(|r| format!("x{r}")).collect(), "the instruction's registers, then those live into it".into())
}

/// the build's checks of one declaration's text at its function e
fn verdict(b: &Base, e: u64, decl: &str) -> Vec<String> {
    let ps = match patch::scan_texts(&[("<site>".into(), format!("{decl} {{}}"))]) {
        Ok(ps) => ps,
        Err(m) => return vec![m],
    };
    let mut rows = match patch::rows(&ps.sites) {
        Ok(r) => r,
        Err(m) => return vec![m],
    };
    let Some(r) = rows.first_mut() else { return vec!["no declaration".into()] };
    r.func = e;
    let entries: HashSet<u64> = b.plan.entries.iter().copied().collect();
    b.row_errors(r, &entries).unwrap_or_else(|m| vec![m])
}

fn prototype(decl: &str) -> String {
    patch::declarations(&format!("{decl} {{}}"), "<site>")
        .and_then(|d| d.first().map(patch::prototype).unwrap_or_else(|| Err("no declaration".into())))
        .unwrap_or_else(|e| format!("(no prototype: {e})"))
}

/// `ounce-lift site <address|label>... OPTS`
pub fn site(argv: Vec<String>) {
    let a = args(argv, &[], &[], "ounce-lift site <address|label>...");
    if a.pos.is_empty() {
        fail(format!("usage: ounce-lift site <address|label>... {OPTS}"));
    }
    let b = base(&a.o);
    let lab = labels(&b);
    let pcs: Vec<u64> = a.pos.iter().map(|x| resolve_addr(x, &lab)).collect();
    match site_text(&b, &lab, &pcs) {
        Ok(t) => print!("{t}"),
        Err(e) => fail(e),
    }
}

/// what site prints of each pc
fn site_text(b: &Base, lab: &BTreeMap<u64, String>, pcs: &[u64]) -> Result<String, String> {
    let mut o = String::new();
    let n = Names(lab);
    let pa = b.patches(&b.usable(&b.ps)?)?;
    analysis!(b, pa, prog, sums);
    let rec = Records::new(&prog, &sums, &b.plan.sj_rets);
    let plain = Plain::new(prog.fns.len());
    let s = plain.sums(&prog, &sums);
    let calls = callers(b, &prog);
    for (q, &pc) in pcs.iter().enumerate() {
        if q > 0 {
            p!(o);
        }
        let hits = b.containing(pc);
        if hits.is_empty() {
            return Err(format!("{pc:#x} is in no lifted function{}", if b.plan.hooks.contains(&pc) { " (an HLE'd function's entry)" } else { "" }));
        }
        let e = hits[0];
        let body = b.body(e).unwrap();
        let k = body.pcs.binary_search(&pc).unwrap();
        let w = body.words[k];
        let lo_k = k.saturating_sub(3);
        let hi_k = (k + 4).min(body.pcs.len());
        let text = disasm(&body.pcs[lo_k..hi_k], &body.words[lo_k..hi_k]);
        let to = ounce_decode::decode(w).and_then(|i| target(&i, pc)).map(|t| {
            let c = prog.callee(t);
            if b.plan.entries.binary_search(&c).is_ok() { format!("   (to {})", n.of(c)) } else { String::new() }
        });
        p!(o, "{pc:#x}  {w:08x}  {}{}", text[k - lo_k], to.unwrap_or_default());
        p!(o, 
            "in {}, +{:#x} of {} instructions{}",
            n.of(e),
            pc - e,
            body.pcs.len(),
            if hits.len() == 1 { String::new() } else { format!("; also in {}", hits[1..].iter().map(|h| n.of(*h)).collect::<Vec<_>>().join(", ")) }
        );
        for j in lo_k..hi_k {
            p!(o, "  {} {:#x}  {:08x}  {}", if j == k { "->" } else { "  " }, body.pcs[j], body.words[j], text[j - lo_k]);
        }
        // (the paths into the instruction: a before row runs on every one of them)
        let mut into = Vec::new();
        for (&q, &wq) in body.pcs.iter().zip(&body.words) {
            if let Some(i) = ounce_decode::decode(wq) {
                if target(&i, q) == Some(pc) && q != pc.wrapping_sub(4) && !patch::is_call(wq) {
                    into.push(format!("a branch at {q:#x}"));
                }
            }
        }
        let mut jts: Vec<_> = b.plan.jt.iter().filter(|(_, t)| t.contains(&pc)).map(|(br, _)| *br).collect();
        jts.sort_unstable();
        into.extend(jts.iter().map(|br| format!("the jump table of the BR at {br:#x}")));
        if b.plan.entries.binary_search(&pc).is_ok() {
            into.push(if pc == e { "the function's entry (its callers)".into() } else { "the entry of a function of its own".into() });
        }
        if hits.len() > 1 {
            into.push(format!("the code of {} functions (split or shared: rows here run in each)", hits.len()));
        }
        p!(o, "paths into it other than from the instruction before: {}", if into.is_empty() { "none".into() } else { into.join("; ") });
        let fid = prog.fn_id(e);
        let sum = fid.map(|f| sums[f as usize]);
        let (mut li, mut lo) = (ALL, ALL);
        let mut is_call = false;
        if let Some(f) = fid {
            let flow = flow_of(&prog, f);
            let pcs = &prog.bodies[f as usize].pcs;
            if let Ok(kk) = pcs.binary_search(&pc) {
                let (lin, lout) = live_in_out(&flow, &s, sum.map_or(summary::ABI_RESULTS, |s| s.results));
                li = lin[kk];
                lo = lout[kk];
                is_call = matches!(flow.kind[kk], K::Call);
            }
        }
        let (us, df) = def_use(w);
        p!(o, "live in:  {}", regs(li));
        p!(o, "live out: {}", regs(lo));
        p!(o, "reads {}; writes {}{}", regs(us), regs(df), if is_call { " (a call: and the callee's clobbers)" } else { "" });
        let bd = rec.bound(e);
        let nc = calls.get(&e).copied().unwrap_or(0);
        if bd.keeps == 0 && bd.reads == ALL {
            let why = match sum {
                Some(s) if s.demote != 0 => format!("demoted ({})", summary::reason_names(s.demote).join(", ")),
                _ => bd.kind.name().to_string(),
            };
            p!(o, "record of {}: {why}: callers reload everything; a row here cannot make it worse ({nc} BL sites)", n.of(e));
        } else {
            p!(o, "record of {} ({} summary, {nc} BL sites): {}", n.of(e), if full(sum.as_ref()) { "full" } else { "no full" }, record(&bd));
        }
        let mut near = 0;
        for d in b.ps.sites.iter().filter(|d| body.pcs.binary_search(&d.pc).is_ok()) {
            near += 1;
            p!(o, "declared: {} {} {:#x} {} {}{}", wh(&d.at), d.kind, d.pc, d.mode, d.fname, if d.pc == pc { "  <- this instruction" } else { "" });
        }
        for f in b.ps.funcs.iter().filter(|f| f.addr == Some(e) || f.symbol.as_ref().is_some_and(|s| b.facts.exports.iter().any(|(n, x)| n == s && *x == e))) {
            near += 1;
            p!(o, "declared: {} {} of {}", wh(&f.at), f.kind, n.of(e));
        }
        if near == 0 {
            p!(o, "declared: nothing in {}", n.of(e));
        }
        let (mode, res, ops, why) = suggest(w, pc, li, lo, &rec, &prog, &n);
        let decl = format!("PATCH_AT({pc:#x}, word({w:#010x}), {mode}, {res}, my_fn{})", ops.iter().map(|o| format!(", {o}")).collect::<String>());
        let errs = verdict(b, e, &decl);
        p!(o);
        p!(o, "/* suggested operands: {why} */");
        p!(o, "/* in {} */", n.of(e));
        p!(o, "{decl} {{   /* {} */", text[k - lo_k]);
        p!(o, "}}");
        p!(o, "/* generates: {} */", prototype(&decl));
        p!(o, "/* the build's checks: {} */", if errs.is_empty() { "it passes".into() } else { errs.join("; ") });
        if pc == e {
            let args: Vec<String> = (0..9).filter(|r| sum.is_some_and(|s| s.inputs >> r & 1 != 0)).map(|r| format!("x{r}")).collect();
            p!(o, "/* or, the function replaced: orig its body, run with the call's registers; the operands are those the");
            p!(o, "   host function uses (the body reads {}: what a call of it passes) */", if args.is_empty() { "none of x0-x8".into() } else { args.join(", ") });
            p!(o, "PATCH_FUNC({pc:#x}, word({w:#010x}), body({:#018x}), aapcs, x0, my_fn, orig) {{", patch::body_hash(&body.words));
            p!(o, "    return guest_orig(orig);");
            p!(o, "}}");
            p!(o, "/* generates: static uint64_t my_fn(GuestFn *orig), called by hos_func_{pc:#x}(Ctx *) */");
        }
    }
    Ok(o)
}

// ---- check

/// the summaries with `after` against those with `before`: functions whose record fields change, demotions, call
/// sites whose kept registers or reads change (patch.diff)
fn cost(b: &Base, before: &PatchSet, after: &PatchSet, n: &Names, o: &mut String) -> Result<(usize, u64), String> {
    let (pa, pb) = (b.patches(before)?, b.patches(after)?);
    analysis!(b, pa, prog_a, sums_a);
    analysis!(b, pb, prog_b, sums_b);
    let (ra, rb) = (Records::new(&prog_a, &sums_a, &b.plan.sj_rets), Records::new(&prog_b, &sums_b, &b.plan.sj_rets));
    let calls = callers(b, &prog_a);
    let mut changed: BTreeMap<&str, u64> = BTreeMap::new();
    let mut rows: Vec<(u64, Vec<String>)> = Vec::new();
    for (k, &e) in prog_a.fns.iter().enumerate() {
        let x = sums_a[k];
        let Some(y) = prog_b.fn_id(e).map(|f| sums_b[f as usize]) else { continue };
        if x == y {
            continue;
        }
        let mut what = Vec::new();
        for (name, va, vb) in [("inputs", x.inputs, y.inputs), ("clobbers", x.clobbers, y.clobbers), ("must", x.mustdef, y.mustdef), ("results", x.results, y.results)] {
            if va != vb {
                *changed.entry(name).or_insert(0) += 1;
                let (plus, minus) = (vb & !va, va & !vb);
                what.push(format!(
                    "{name}{}{}",
                    if plus != 0 { format!(" +{}", regs(plus)) } else { String::new() },
                    if minus != 0 { format!(" -{}", regs(minus)) } else { String::new() }
                ));
            }
        }
        let (fa, fb) = (full(Some(&x)), full(Some(&y)));
        if fa && !fb {
            *changed.entry("demoted").or_insert(0) += 1;
            what.push(if y.demote != 0 { format!("demoted ({})", summary::reason_names(y.demote).join(", ")) } else { "demoted".into() });
        } else if fb && !fa {
            *changed.entry("promoted").or_insert(0) += 1;
            what.push("now full".into());
        }
        if !what.is_empty() {
            rows.push((e, what));
        }
    }
    let fset = |p: &Patches| p.funcs.iter().copied().collect::<HashSet<u64>>();
    let mut cands: HashSet<u64> = rows.iter().map(|r| r.0).collect();
    cands.extend(fset(&pa).symmetric_difference(&fset(&pb)));
    let (mut sites, mut funcs) = (0, 0);
    for c in cands {
        let (x, y) = (ra.bound(c), rb.bound(c));
        if x.keeps != y.keeps || x.reads != y.reads {
            sites += calls.get(&c).copied().unwrap_or(0);
            funcs += 1;
        }
    }
    let ch: Vec<String> = changed.iter().map(|(k, v)| format!("{k} {v}")).collect();
    p!(o, "functions whose summary changes: {} ({})", rows.len(), if ch.is_empty() { "-".into() } else { ch.join(", ") });
    p!(o, "call sites whose kept registers or reads change: {sites} (calls of {funcs} functions)");
    rows.sort_by_key(|r| std::cmp::Reverse(calls.get(&r.0).copied().unwrap_or(0)));
    for (e, what) in rows.iter().take(20) {
        p!(o, "  {} ({} BL sites): {}", n.of(*e), calls.get(e).copied().unwrap_or(0), what.join("; "));
    }
    Ok((rows.len(), sites))
}

/// the PatchSet without the declaration at w
fn without(texts: &[(String, String)], w: &Where) -> PatchSet {
    let mut ps = patch::scan_texts(texts).unwrap_or_else(|e| fail(e));
    ps.sites.retain(|d| d.at != *w);
    ps.funcs.retain(|d| d.at != *w);
    for m in [&mut ps.hle, &mut ps.nv, &mut ps.declared] {
        m.retain(|_, d| d.at != *w);
    }
    ps.answers.retain(|_, d| d.at != *w);
    ps
}

fn declared_at(ps: &PatchSet, w: &Where) -> Option<&'static str> {
    ps.sites
        .iter()
        .find(|d| d.at == *w)
        .map(|d| d.kind)
        .or_else(|| ps.funcs.iter().find(|d| d.at == *w).map(|d| d.kind))
        .or_else(|| [&ps.hle, &ps.nv, &ps.declared].iter().flat_map(|m| m.values()).find(|d| d.at == *w).map(|d| d.kind))
        .or_else(|| ps.answers.values().find(|d| d.at == *w).map(|_| "PATCH_ANSWER"))
}

fn count(ps: &PatchSet) -> usize {
    ps.sites.len() + ps.funcs.len() + ps.hle.len() + ps.nv.len() + ps.declared.len()
}

/// `ounce-lift check [<file:line> | - | --all] OPTS`
pub fn check(argv: Vec<String>) {
    let a = args(argv, &["--all", "-"], &[], "ounce-lift check [<file:line> | - | --all]");
    let b = base(&a.o);
    let lab = labels(&b);
    let n = Names(&lab);
    let texts = host_texts(&a.o.root, &b.game).unwrap_or_else(|e| fail(e));
    if a.flags.contains("-") {
        let mut stdin = String::new();
        std::io::Read::read_to_string(&mut std::io::stdin(), &mut stdin).unwrap_or_else(|e| fail(e));
        let mut all = texts.clone();
        all.push(("<stdin>".into(), stdin));
        let after = patch::scan_texts(&all).unwrap_or_else(|e| fail(e));
        let errs: Vec<_> = b.errors(&after).unwrap_or_else(|e| fail(e)).into_iter().filter(|(w, _)| w.0 == "<stdin>").collect();
        for (w, m) in &errs {
            println!("{}: {m}", wh(w));
        }
        if !errs.is_empty() {
            std::process::exit(1);
        }
        println!("proposed: {} declarations, no error the build would stop on", count(&after) - count(&b.ps) + after.answers.len() - b.ps.answers.len());
        let mut o = String::new();
        cost(&b, &b.ps, &after, &n, &mut o).unwrap_or_else(|e| fail(e));
        print!("{o}");
        return;
    }
    let only = match a.pos.first() {
        Some(x) if !a.flags.contains("--all") => {
            let (f, l) = x.rsplit_once(':').unwrap_or_else(|| fail(format!("{x}: file:line")));
            let w = Where(f.to_string(), l.parse().unwrap_or_else(|_| fail(format!("{x}: file:line"))));
            if declared_at(&b.ps, &w).is_none() {
                fail(format!("no declaration at {x}"));
            }
            Some(w)
        }
        _ => None,
    };
    let errs: Vec<_> = b.errors(&b.ps).unwrap_or_else(|e| fail(e)).into_iter().filter(|(w, _)| only.as_ref().is_none_or(|o| o == w)).collect();
    for (w, m) in &errs {
        println!("{}: {m}", wh(w));
    }
    println!(
        "{} declarations checked against the plan, {} errors{}",
        if only.is_some() { 1 } else { count(&b.ps) },
        errs.len(),
        if only.is_some() {
            ""
        } else {
            "; a guest function a host function calls without declaring it is not checkable here (the check host_reads compiles the host for that)"
        }
    );
    if let (Some(w), true) = (&only, errs.is_empty()) {
        println!("cost of {} {} (the summaries computed again without it):", declared_at(&b.ps, w).unwrap(), wh(w));
        let mut o = String::new();
        cost(&b, &without(&texts, w), &b.ps, &n, &mut o).unwrap_or_else(|e| fail(e));
        print!("{o}");
    }
    if !errs.is_empty() {
        std::process::exit(1);
    }
}

// ---- report

/// (what a BL keeps, what it reads) of a callee with that local summary: bounds' rule for a lifted callee
fn kept(s: &Summary) -> (u128, u128) {
    if s.returns && s.demote == 0 && !s.unknown {
        (s.preserved & KEEP_ALL, s.inputs)
    } else {
        (0, ALL)
    }
}

/// `ounce-lift report OPTS`: one line per declaration with its local cost, costliest first
pub fn report(argv: Vec<String>) {
    let a = args(argv, &[], &[], "ounce-lift report");
    let b = base(&a.o);
    let lab = labels(&b);
    let lines = report_lines(&b, &lab).unwrap_or_else(|e| fail(e));
    println!("{:<34} {:<22} {:<11} {:<40} {:>7}  cost", "declaration", "kind", "site", "function", "sites");
    for (sites, _, _, w, kind, at, name, what) in &lines {
        println!("{:<34} {:<22} {:<11} {:<40} {:>7}  {}", wh(w), kind, at, name.chars().take(40).collect::<String>(), sites, what);
    }
    println!(
        "(sites: BL sites whose call record (keeps, reads) differs from the function's without the declaration; record: its \
         function's summary analysed again without it, the callees' summaries the plan's; `check <file:line>` gives the whole \
         program's difference. Context stores kept by a declaration are not measured here)"
    );
}

/// (sites, fields changed, extra registers, where, kind, at, function, what) of a report line
type Line = (u64, usize, u32, Where, String, String, String, String);

/// every declaration's line, costliest first
fn report_lines(b: &Base, lab: &BTreeMap<u64, String>) -> Result<Vec<Line>, String> {
    let n = Names(lab);
    let ps = &b.ps;
    let pa = b.patches(&b.usable(ps)?)?;
    analysis!(b, pa, prog, sums);
    let calls = callers(b, &prog);
    let plain = Plain::new(prog.fns.len());
    let mut lines: Vec<Line> = Vec::new();
    for (w, m) in b.errors(ps)? {
        lines.push((1 << 30, 0, 0, w, "error".into(), "-".into(), "?".into(), format!("{m}: check")));
    }
    // the rows: the function analysed alone with all of them and without each (its callees' summaries the plan's)
    let all_rows: Vec<Row> = pa.rows.clone();
    let mut base_local: HashMap<u64, Summary> = HashMap::new();
    for r in &all_rows {
        let e = r.func;
        let Some(fid) = prog.fn_id(e) else {
            lines.push((1 << 30, 0, 0, r.at.clone(), "site".into(), format!("{:#x}", r.pc), "?".into(), "not in one function: check".into()));
            continue;
        };
        let (us, df) = def_use(r.word);
        let (rd, wr) = r.effects();
        let m = |s: &std::collections::BTreeSet<String>| s.iter().map(|v| patch::var_bit(v)).fold(0u128, |a, x| a | x);
        let mut extra_r = m(&rd) & !us;
        let extra_w = m(&wr) & !df;
        if !r.value && r.mode == "after" && !patch::is_call(r.word) {
            extra_r &= !df; // (an after row's reads of what its instruction wrote)
        }
        let bl = *base_local.entry(e).or_insert_with(|| {
            let s = plain.sums(&prog, &sums);
            analyse_alone(&flow_of(&prog, fid), &s)
        });
        let rest: Vec<Row> = all_rows.iter().filter(|x| x.at != r.at).cloned().collect();
        let saved = std::mem::replace(&mut prog.rows, rows_from(&patch::row_effects(&rest)));
        let alone = {
            let s = plain.sums(&prog, &sums);
            analyse_alone(&flow_of(&prog, fid), &s)
        };
        prog.rows = saved;
        let mut what = Vec::new();
        for (name, x, y) in [("inputs", bl.inputs, alone.inputs), ("clobbers", bl.clobbers, alone.clobbers), ("must", bl.mustdef, alone.mustdef)] {
            if x != y {
                what.push(format!("{name} {}", regs(x ^ y)));
            }
        }
        if alone.demote != bl.demote {
            what.push(format!("demotion {}", summary::reason_names(bl.demote).join(",")));
        }
        let sites = if kept(&bl) != kept(&alone) { calls.get(&e).copied().unwrap_or(0) } else { 0 };
        let mode = if r.value { "instead" } else { r.mode.as_str() };
        lines.push((
            sites,
            what.len(),
            (extra_r | extra_w).count_ones(),
            r.at.clone(),
            format!("PATCH_AT {mode}"),
            format!("{:#x}", r.pc),
            n.of(e),
            format!(
                "reads {}, writes {} beyond the instruction; record: {}",
                if extra_r != 0 { regs(extra_r) } else { "-".into() },
                if extra_w != 0 { regs(extra_w) } else { "-".into() },
                if what.is_empty() { "the same without it".into() } else { what.join("; ") }
            ),
        ));
    }
    let rec = Records::new(&prog, &sums, &b.plan.sj_rets);
    let exported = |s: &str| b.facts.exports.iter().find(|(x, _)| x == s).map(|(_, a)| *a);
    for f in &ps.funcs {
        let e = match &f.symbol {
            None => f.addr,
            Some(s) => exported(s),
        };
        let Some(e) = e.filter(|e| b.body(*e).is_some()) else {
            lines.push((0, 0, 0, f.at.clone(), f.kind.into(), f.symbol.clone().unwrap_or_else(|| format!("{:#x}", f.addr.unwrap_or(0))), "?".into(), "no lifted function: check".into()));
            continue;
        };
        let (bd, own) = (rec.bound(e), rec.own(e));
        let nc = calls.get(&e).copied().unwrap_or(0);
        let sites = if (bd.keeps, bd.reads) != (own.keeps, own.reads) { nc } else { 0 };
        lines.push((
            sites,
            0,
            f.args.len() as u32,
            f.at.clone(),
            f.kind.into(),
            format!("{e:#x}"),
            n.of(e),
            format!(
                "record {} reads {}, keeps {} at {nc} BL sites; the body's own: {} reads {}, keeps {}",
                bd.kind.name(),
                regs(bd.reads),
                regs(bd.keeps),
                own.kind.name(),
                regs(own.reads),
                regs(own.keeps)
            ),
        ));
    }
    let mut by_sym: HashMap<&str, Vec<u64>> = HashMap::new();
    for h in &b.img.hooks {
        if let Some(s) = h.c.strip_prefix("imp_") {
            by_sym.entry(s).or_default().push(h.addr);
        }
    }
    for r in ps.hle.values().chain(ps.nv.values()).chain(ps.declared.values()) {
        if r.reads.is_empty() && r.clobbers.is_none() {
            continue;
        }
        let addrs = by_sym.get(r.symbol.as_str()).cloned().unwrap_or_default();
        let nc: u64 = addrs.iter().map(|x| calls.get(x).copied().unwrap_or(0)).sum();
        let fns: Vec<String> = addrs.iter().take(2).map(|x| n.of(*x)).collect();
        lines.push((
            if r.clobbers.is_some() { nc } else { 0 },
            0,
            r.reads.len() as u32,
            r.at.clone(),
            r.kind.into(),
            r.symbol.clone(),
            if fns.is_empty() { "-".into() } else { fns.join(", ") },
            if r.clobbers.is_some() { format!("clobbers: {nc} BL sites keep nothing") } else { format!("reads {} at {nc} BL sites", r.reads.join(",")) },
        ));
    }
    let tails = tail_sites(b, &prog);
    let mut answers: Vec<_> = ps.answers.values().collect();
    answers.sort_by(|x, y| (&x.at.0, x.at.1).cmp(&(&y.at.0, y.at.1)));
    for x in answers {
        let addrs = by_sym.get(x.symbol.as_str()).cloned().unwrap_or_default();
        let nc: u64 = addrs.iter().map(|a| calls.get(a).copied().unwrap_or(0)).sum();
        let nt: u64 = addrs.iter().map(|a| tails.get(a).copied().unwrap_or(0)).sum();
        let rc = addrs.first().map_or("no hooked address".to_string(), |a| record(&rec.bound(*a)));
        lines.push((
            0,
            0,
            0,
            x.at.clone(),
            "PATCH_ANSWER".into(),
            x.symbol.chars().take(22).collect(),
            addrs.first().map_or("-".into(), |a| n.of(*a)),
            format!("x0 = {:#x} at {nc} BL and {nt} tail-branch sites (no call); {rc}", x.value),
        ));
    }
    lines.sort_by(|x, y| (std::cmp::Reverse(x.0), std::cmp::Reverse(x.1), std::cmp::Reverse(x.2), (&x.3 .0, x.3 .1)).cmp(&(std::cmp::Reverse(y.0), std::cmp::Reverse(y.1), std::cmp::Reverse(y.2), (&y.3 .0, y.3 .1))));
    Ok(lines)
}

// ---- dump

/// `ounce-lift dump <address|label>... [--tree] [--opt O1|O2] [--asm] [-o <dir>] OPTS`: for each function, in
/// <dir>/<address>/ (default <work>/lift/dump: game-derived, never committed): guest.s, ir.txt, lift.ll and with --opt
/// lift.opt.ll, with --asm lift.s
pub fn dump(argv: Vec<String>) {
    let usage = "ounce-lift dump <address|label>... [--tree] [--opt O1|O2] [--asm] [-o <dir>]";
    let a = args(argv, &["--tree", "--asm"], &["--opt", "-o"], usage);
    if a.pos.is_empty() {
        fail(format!("usage: {usage} {OPTS}"));
    }
    let b = base(&a.o);
    let lab = labels(&b);
    let n = Names(&lab);
    let pa = patches(&b, &b.usable(&b.ps).unwrap_or_else(|e| fail(e)));
    analysis!(b, pa, prog, sums);
    let rec = Records::new(&prog, &sums, &b.plan.sj_rets);
    let mut want: Vec<u64> = a.pos.iter().map(|x| resolve_addr(x, &lab)).collect();
    if a.flags.contains("--tree") {
        for e in want.clone() {
            if let Some(body) = b.body(e) {
                for (&pc, &w) in body.pcs.iter().zip(&body.words) {
                    if w & 0x7C00_0000 == 0x1400_0000 {
                        let t = prog.callee(imm26_target(pc, w));
                        if (w >> 31 == 1 || body.pcs.binary_search(&t).is_err()) && b.body(t).is_some() && !want.contains(&t) {
                            want.push(t);
                        }
                    }
                }
            }
        }
    }
    let dir = a.vals.get("-o").map(PathBuf::from).unwrap_or_else(|| a.o.work.join("lift").join("dump"));
    let targets: HashSet<u64> = pa.parts.code_targets.iter().copied().collect();
    let mut w = Worker::default();
    for e in want {
        let Some(part) = pa.parts.parts.iter().find(|p| p.functions.iter().any(|f| f.entry == e)) else {
            eprintln!("{}: no lifted function there", n.of(e));
            continue;
        };
        let f = part.functions.iter().find(|f| f.entry == e).unwrap().clone();
        let one = Part { name: format!("f_{e:08x}"), functions: vec![f.clone()], names: part.names.clone() };
        let l = lift_part(&mut w, &pa.meta, &b.plan.sj_rets, &targets, &rec, &one, false);
        let out = dir.join(format!("{e:08x}"));
        std::fs::create_dir_all(&out).unwrap_or_else(|x| fail(x));
        let guest: Vec<String> = disasm(&f.pcs, &f.words).into_iter().zip(f.pcs.iter().zip(&f.words)).map(|(t, (p, w))| format!("{p:#x}  {w:08x}  {t}")).collect();
        std::fs::write(out.join("guest.s"), guest.join("\n") + "\n").unwrap();
        std::fs::write(out.join("ir.txt"), w.fun.text()).unwrap();
        let mut ll = l.head.clone();
        ll.extend_from_slice(&w.body);
        let llp = out.join("lift.ll");
        std::fs::write(&llp, &ll).unwrap();
        let level = a.vals.get("--opt").map(String::as_str);
        let clang = |extra: &[&str], to: &str| {
            let mut c = std::process::Command::new(llvm_tool("clang"));
            c.args(["-march=x86-64-v3", "-ffp-contract=off", "-g0", "-Wno-everything", "-x", "ir"]).arg(&llp).args(extra).arg("-o").arg(out.join(to));
            match c.status() {
                Ok(s) if s.success() => {}
                _ => eprintln!("{}: clang failed for {to}", n.of(e)),
            }
        };
        if let Some(lv) = level {
            clang(&[&format!("-{lv}"), "-S", "-emit-llvm"], "lift.opt.ll");
        }
        if a.flags.contains("--asm") {
            clang(&[&format!("-{}", level.unwrap_or("O1")), "-S"], "lift.s");
        }
        let s = prog.fn_id(e).map(|k| sums[k as usize]);
        println!(
            "{}: {} instructions -> {}{}",
            n.of(e),
            f.pcs.len(),
            out.display(),
            match s {
                Some(s) => format!(
                    "; inputs {}, clobbers {}, preserved {}, results {}{}",
                    regs(s.inputs),
                    regs(s.clobbers),
                    regs(s.preserved),
                    regs(s.results),
                    if s.demote != 0 { format!(", demoted ({})", summary::reason_names(s.demote).join(", ")) } else { String::new() }
                ),
                None => String::new(),
            }
        );
    }
}

#[cfg(test)]
mod tests {
    //! the tools on a small program of their own (no game): site, check, report, the summaries' difference, live-out

    use super::*;

    const F: u64 = 0x8000_1000;
    const C: u64 = 0x8000_2000;
    const DECLS: &str = "PATCH_AT(0x80001004, word(0x8b020020), after, x0, t_after, x0, x1) { return x0 + x1; }
PATCH_AT(0x80001008, word(0x940003fe), before, none, t_before, x0) {}
PATCH_AT(0x80001004, word(0x8b020021), after, none, t_word, x1) {}
PATCH_AT(0x80003000, word(0xd503201f), after, none, t_nowhere, x1) {}
PATCH_AT(0x80001004, word(0x8b020020), after, x19, t_x19, x1) { return x1; }
";

    fn code() -> Vec<(u64, Vec<u32>)> {
        vec![
            // cbz x0, +8; add x0, x1, x2; bl C; ret
            (F, vec![0xb400_0040, 0x8b02_0020, 0x9400_0000 | (((C - (F + 8)) / 4) as u32 & 0x3FF_FFFF), 0xd65f_03c0]),
            // add x0, x0, x1; ret
            (C, vec![0x8b01_0000, 0xd65f_03c0]),
        ]
    }

    fn root() -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
    }

    fn base(decls: &str) -> Base {
        Base::synthetic(&code(), &[("runtime/t.c".into(), decls.into())], &root()).unwrap()
    }

    fn labels() -> BTreeMap<u64, String> {
        BTreeMap::from([(F, "t_function".to_string())])
    }

    #[test]
    fn site() {
        let b = base(DECLS);
        let t = site_text(&b, &labels(), &[0x8000_1008]).unwrap();
        assert!(t.contains("in f_80001000 (t_function)"), "{t}");
        assert!(t.contains("a branch at 0x80001000"), "{t}");
        assert!(t.contains("(to f_80002000)"), "{t}");
        assert!(t.contains("instead, x0, my_fn, x0, x1)"), "{t}"); // (the callee's reads and result)
        assert!(t.contains("runtime/t.c:2 PATCH_AT 0x80001008 before t_before  <- this instruction"), "{t}");
        let t = site_text(&b, &labels(), &[0x8000_1004]).unwrap();
        assert!(t.contains("paths into it other than from the instruction before: none"), "{t}");
        assert!(t.contains("live in:  x1-x2"), "{t}"); // (the add reads x1, x2; the row t_after its x0)
        assert!(t.contains("live out: x0-x1"), "{t}");
        assert!(t.contains("PATCH_AT(0x80001004, word(0x8b020020), after, x0, my_fn, x0, x1, x2) {"), "{t}");
        assert!(t.contains("generates: uint64_t my_fn(uint64_t x0, uint64_t x1, uint64_t x2)"), "{t}");
        assert!(t.contains("the build's checks: it passes"), "{t}");
        // (at an entry: the function replaced, its body hash, the argument registers it reads)
        let t = site_text(&b, &labels(), &[F]).unwrap();
        let h = patch::body_hash(&code()[0].1);
        assert!(t.contains(&format!("PATCH_FUNC(0x80001000, word(0xb4000040), body({h:#018x}), aapcs, x0, my_fn, orig) {{")), "{t}");
        assert!(t.contains("the body reads x0, x1, x2"), "{t}");
        assert!(site_text(&b, &labels(), &[0x8000_3000]).is_err());
    }

    #[test]
    fn check() {
        let b = base(DECLS);
        let errs: Vec<String> = b.errors(&b.ps).unwrap().into_iter().map(|(w, m)| format!("{}: {m}", wh(&w))).collect();
        assert_eq!(errs.len(), 4, "{errs:?}");
        assert!(errs[0].starts_with("runtime/t.c:2: ") && errs[0].contains("a branch target"), "{}", errs[0]);
        assert!(errs[1].starts_with("runtime/t.c:3: ") && errs[1].contains("another game version"), "{}", errs[1]);
        assert!(errs[2].starts_with("runtime/t.c:4: ") && errs[2].contains("no lifted function"), "{}", errs[2]);
        assert!(errs[3].starts_with("runtime/t.c:5: ") && errs[3].contains("x19"), "{}", errs[3]);
        assert_eq!(b.usable(&b.ps).unwrap().sites.len(), 1);
    }

    #[test]
    fn report() {
        let b = base(DECLS);
        let lines = report_lines(&b, &labels()).unwrap();
        let first = lines.iter().find(|l| l.3 == Where("runtime/t.c".into(), 1)).unwrap();
        assert!(first.7.contains("reads -, writes - beyond the instruction"), "{}", first.7); // (it reads the add's x0 and x1)
        assert_eq!(lines.iter().filter(|l| l.4 == "error").count(), 4);
    }

    #[test]
    fn diff() {
        // (a row at C's add that reads x5: C, and F that calls it, read x5; C's one BL site keeps other registers)
        let b = base("");
        let after = patch::scan_texts(&[("runtime/t.c".into(), "PATCH_AT(0x80002000, word(0x8b010000), after, none, t_c, x5) {}\n".into())]).unwrap();
        let mut o = String::new();
        let (n, sites) = cost(&b, &b.ps, &after, &Names(&labels()), &mut o).unwrap();
        assert!(n >= 1 && sites == 1, "{n} {sites}\n{o}");
        assert!(o.contains("inputs +x5"), "{o}");
    }

    #[test]
    fn live_out() {
        let b = base("");
        let pa = b.patches(&b.ps).unwrap();
        analysis!(b, pa, prog, sums);
        let plain = Plain::new(prog.fns.len());
        let s = plain.sums(&prog, &sums);
        let flow = flow_of(&prog, prog.fn_id(F).unwrap());
        let (li, lo) = live_in_out(&flow, &s, 0);
        for k in 0..flow.n {
            if matches!(flow.kind[k], K::Op) {
                // (live-in = what it reads, and what is live after it that it does not write)
                assert_eq!(li[k], flow.use_[k] | (lo[k] & !flow.dfn[k]), "at {k}");
            }
        }
    }

    #[test]
    fn prototype_of_operands() {
        assert_eq!(prototype("PATCH_AT(0x1000, word(0x8b020020), after, w0, f, w1, s2, q3, ld(x4, 8))"), "uint32_t f(uint32_t w1, float s2, hos_q q3, uint64_t ld_x4_8)");
    }
}
