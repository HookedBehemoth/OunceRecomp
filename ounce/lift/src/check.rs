//! The diagnostic gate: every summary and call-boundary record the Rust side computes against Python's (the export's
//! check/summaries.json and check/bounds.json), field by field.

use crate::bounds::Records;
use crate::export::{read_check_bounds, read_check_summaries, Bound, Names};
use crate::ir::Sym;
use crate::summary::{reason_names, Program, Summary};
use std::path::Path;

fn sym_text(s: Sym, names: Option<&Names>) -> String {
    match s {
        Sym::Entry(a) => format!("f_{a:08x}"),
        Sym::SjCall(a) => format!("sjcall_{a:08x}"),
        Sym::Name(k) => names.map_or(format!("#{k}"), |n| n.name(k).to_string()),
    }
}

fn bound_diff(a: &Bound, b: &Bound, names: &Names) -> Vec<String> {
    let mut d = Vec::new();
    macro_rules! f {
        ($x:ident, $fmt:literal) => {
            if a.$x != b.$x {
                d.push(format!(concat!(stringify!($x), " rust ", $fmt, " python ", $fmt), a.$x, b.$x));
            }
        };
    }
    if a.kind != b.kind {
        d.push(format!("kind rust {} python {}", a.kind.name(), b.kind.name()));
    }
    f!(reads, "{:x}");
    f!(clobbers, "{:x}");
    f!(keeps, "{:x}");
    f!(must, "{:x}");
    f!(results, "{:x}");
    f!(noreturn, "{}");
    f!(answer, "{:?}");
    let (sa, sb) = (sym_text(a.symbol, None), sym_text(b.symbol, Some(names)));
    if sa != sb {
        d.push(format!("symbol rust {sa} python {sb}"));
    }
    let (ta, tb) = (sym_text(a.tail, None), sym_text(b.tail, Some(names)));
    if ta != tb {
        d.push(format!("tail rust {ta} python {tb}"));
    }
    d
}

/// (functions whose summary differs, records that differ); prints the first `show` of each with their fields
pub fn check(dir: &Path, prog: &Program, sums: &[Summary], rec: &Records, show: usize) -> (usize, usize) {
    let py = read_check_summaries(dir).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(py.len(), prog.fns.len(), "check: {} Python summaries, {} functions", py.len(), prog.fns.len());
    let mut nsum = 0;
    for p in &py {
        let i = prog.fn_id(p.0).unwrap_or_else(|| panic!("check: f_{:08x} has no body", p.0)) as usize;
        let s = &sums[i];
        let mut d = Vec::new();
        macro_rules! m {
            ($name:literal, $a:expr, $b:expr) => {
                if $a != $b {
                    d.push(format!(concat!($name, " rust {:x} python {:x} (rust-only {:x}, python-only {:x})"), $a, $b, $a & !$b, $b & !$a));
                }
            };
        }
        m!("inputs", s.inputs, p.1);
        m!("clobbers", s.clobbers, p.2);
        m!("mustdef", s.mustdef, p.3);
        m!("preserved", s.preserved, p.4);
        m!("results", s.results, p.5);
        let names: Vec<String> = reason_names(s.demote).iter().map(|x| x.to_string()).collect();
        for (n, a, b) in [
            ("returns", s.returns, p.6),
            ("unknown", s.unknown, p.8),
            ("boundary", s.boundary, p.11),
            ("fp", s.fp, p.12),
            ("writes_fpcr", s.writes_fpcr, p.13),
        ] {
            if a != b {
                d.push(format!("{n} rust {a} python {b}"));
            }
        }
        if names != p.7 {
            d.push(format!("demote rust {names:?} python {:?}", p.7));
        }
        if s.frame != p.9 {
            d.push(format!("frame rust {} python {}", s.frame, p.9));
        }
        if s.pairs != p.10 {
            d.push(format!("pairs rust {} python {}", s.pairs, p.10));
        }
        if !d.is_empty() {
            nsum += 1;
            if nsum <= show {
                println!("summary f_{:08x}: {}", p.0, d.join("; "));
            }
        }
    }
    let cb = read_check_bounds(dir).unwrap_or_else(|e| panic!("{e}"));
    let mut nb = 0;
    for (what, list) in [("bound", &cb.bounds), ("own", &cb.own)] {
        for b in list {
            let r = if what == "own" { rec.own(b.addr) } else { rec.bound(b.addr) };
            let d = bound_diff(&r, b, &cb.names);
            if !d.is_empty() {
                nb += 1;
                if nb <= show {
                    println!("{what} {:08x}: {}", b.addr, d.join("; "));
                }
            }
        }
    }
    println!(
        "check: summaries {} of {} differ; records {} of {} differ",
        nsum,
        py.len(),
        nb,
        cb.bounds.len() + cb.own.len()
    );
    (nsum, nb)
}

// ---- the discovery and the plan against Python's (check/plan.json)

/// a body's words as check/plan.json keys them: sha256 of their little-endian bytes, 16 hex digits
fn words_key(words: impl Iterator<Item = u32>) -> String {
    use sha2::{Digest, Sha256};
    let mut h = Sha256::new();
    for w in words {
        h.update(w.to_le_bytes());
    }
    h.finalize().iter().take(8).map(|b| format!("{b:02x}")).collect()
}

fn runs(pcs: &[u64]) -> Vec<(u64, u64)> {
    let mut out: Vec<(u64, u64)> = Vec::new();
    for &pc in pcs {
        match out.last_mut() {
            Some(r) if r.0 + 4 * r.1 == pc => r.1 += 1,
            _ => out.push((pc, 1)),
        }
    }
    out
}

/// one field's comparison: prints the first differences, returns their count
fn field<T: PartialEq + std::fmt::Debug>(what: &str, rust: &[T], py: &[T], show: usize) -> usize {
    let mut n = 0;
    for k in 0..rust.len().max(py.len()) {
        let (a, b) = (rust.get(k), py.get(k));
        if a != b {
            n += 1;
            if n <= show {
                println!("{what}[{k}]: rust {a:?} python {b:?}");
            }
        }
    }
    if rust.len() != py.len() {
        println!("{what}: rust {} python {}", rust.len(), py.len());
    }
    n
}

/// (entry, pcs as runs, words key)
type BodyKey = (u64, Vec<(u64, u64)>, String);

#[derive(serde::Deserialize)]
struct PyPlan {
    entries: Vec<u64>,
    bodies: Vec<BodyKey>,
    raw_bodies: Vec<BodyKey>,
    insns: Vec<(u64, usize)>,
    jt: Vec<(u64, Vec<u64>)>,
    jt_index: Vec<(u64, u64, u32, u32, Vec<u64>)>,
    plt: Vec<(u64, u64, u64)>,
    no_call: Vec<u64>,
    sj_rets: Vec<[u64; 5]>,
    hooks: Vec<u64>,
    why: Vec<(String, u32)>,
    hot: Vec<u64>,
    code_excluded: Vec<u64>,
    partitions: Vec<(String, Vec<u64>)>,
    census: Vec<crate::plan::Census>,
}

/// differences of the discovery and the plan from Python's
pub fn check_plan(dir: &Path, plan: &crate::plan::Plan, parts: &crate::plan::Parts, img: &crate::image::Image, show: usize) -> usize {
    use rayon::prelude::*;
    let p = dir.join("check").join("plan.json");
    let py: PyPlan = serde_json::from_slice(&std::fs::read(&p).unwrap_or_else(|e| panic!("{}: {e}", p.display())))
        .unwrap_or_else(|e| panic!("{}: {e}", p.display()));
    let mut n = 0;
    n += field("entries", &plan.entries, &py.entries, show);
    let bodies: Vec<BodyKey> =
        plan.bodies.par_iter().map(|b| (b.entry, runs(&b.pcs), words_key(b.words.iter().copied()))).collect();
    n += field("bodies", &bodies, &py.bodies, show);
    let raw: Vec<BodyKey> =
        plan.raw.iter().map(|(e, pcs)| (*e, runs(pcs), words_key(pcs.iter().map(|&p| img.word(p))))).collect();
    // (Python lists the discovered bodies that differ from the lifted ones; the Rust side keeps those of the trapped
    // and cut functions: the same unless a trapped body's discovered words are its trap's)
    n += field("raw_bodies", &raw, &py.raw_bodies, show);
    let insns: Vec<(u64, usize)> = plan.bodies.iter().zip(&plan.insns).map(|(b, &k)| (b.entry, k)).collect();
    n += field("insns", &insns, &py.insns, show);
    let mut jt: Vec<(u64, Vec<u64>)> = plan.jt.iter().map(|(k, v)| (*k, v.clone())).collect();
    jt.sort_unstable();
    n += field("jt", &jt, &py.jt, show);
    let mut ji: Vec<(u64, u64, u32, u32, Vec<u64>)> = plan.jt_index.iter().map(|(k, v)| (*k, v.0, v.1, v.2, v.3.clone())).collect();
    ji.sort_unstable();
    n += field("jt_index", &ji, &py.jt_index, show);
    n += field("plt", &plan.plt, &py.plt, show);
    n += field("no_call", &plan.no_call, &py.no_call, show);
    n += field("sj_rets", &plan.sj_rets, &py.sj_rets, show);
    n += field("hooks", &plan.hooks, &py.hooks, show);
    let why: Vec<(String, u32)> = plan.why.iter().map(|(k, v)| (k.clone(), *v)).collect();
    n += field("why", &why, &py.why, show);
    n += field("hot", &plan.hot, &py.hot, show);
    n += field("code_excluded", &plan.code_excluded, &py.code_excluded, show);
    let pt: Vec<(String, Vec<u64>)> = parts
        .parts
        .iter()
        .map(|p| (p.name[5..6].to_string(), p.functions.iter().map(|f| f.entry).collect()))
        .collect();
    n += field("partitions", &pt, &py.partitions, show);
    n += field("census", &parts.census, &py.census, show);
    println!("check: discovery and plan: {n} differences");
    n
}

// ---- the image against the lift's (check/image.json, check/image_entries.json, meta.image_sha256)

/// differences of the image the Rust side built from the lift's
pub fn check_image(dir: &Path, meta: &crate::export::Meta, img: &crate::image::Image, facts: &crate::eden::Facts, show: usize) -> usize {
    use sha2::{Digest, Sha256};
    #[derive(serde::Deserialize)]
    struct PyImage {
        base: u64,
        end: u64,
        tramp_base: u64,
        modules: Vec<crate::image::Module>,
        skipped: Vec<String>,
        hooks: Vec<crate::image::Hook>,
        unresolved: serde_json::Map<String, serde_json::Value>,
        init_arrays: serde_json::Map<String, serde_json::Value>,
        exports: serde_json::Map<String, serde_json::Value>,
    }
    let p = dir.join("check").join("image.json");
    let py: PyImage = serde_json::from_slice(&std::fs::read(&p).unwrap_or_else(|e| panic!("{}: {e}", p.display())))
        .unwrap_or_else(|e| panic!("{}: {e}", p.display()));
    let p = dir.join("check").join("image_entries.json");
    let py_entries: Vec<u64> = serde_json::from_slice(&std::fs::read(&p).unwrap_or_else(|e| panic!("{}: {e}", p.display()))).unwrap();
    let mut n = 0;
    let sha: String = Sha256::digest(&img.mem).iter().map(|b| format!("{b:02x}")).collect();
    if sha != meta.image_sha256 {
        n += 1;
        println!("image.bin: sha256 rust {sha} python {}", meta.image_sha256);
    }
    n += field("base/end/tramp", &[img.base, img.end, crate::eden::TRAMP_BASE], &[py.base, py.end, py.tramp_base], show);
    n += field("skipped", &[] as &[String], &py.skipped, show);
    n += field("modules", &img.modules, &py.modules, show);
    n += field("hooks", &img.hooks, &py.hooks, show);
    let as_list = |m: &serde_json::Map<String, serde_json::Value>| -> Vec<(String, serde_json::Value)> {
        m.iter().map(|(k, v)| (k.clone(), v.clone())).collect()
    };
    // (by key: serde_json's map is sorted; the order of Python's dicts is not a fact the build reads)
    let sorted = |mut v: Vec<(String, serde_json::Value)>| {
        v.sort_by(|a, b| a.0.cmp(&b.0));
        v
    };
    let unresolved: Vec<(String, serde_json::Value)> = facts.unresolved.iter().map(|(k, v)| (k.clone(), (*v).into())).collect();
    let unresolved = sorted(unresolved);
    n += field("unresolved", &unresolved, &as_list(&py.unresolved), show);
    let init: Vec<(String, serde_json::Value)> = facts.init_arrays.iter().map(|(k, v)| (k.clone(), serde_json::json!(v))).collect();
    let init = sorted(init);
    n += field("init_arrays", &init, &as_list(&py.init_arrays), show);
    let exports: Vec<(String, serde_json::Value)> = facts.exports.iter().map(|(k, v)| (k.clone(), (*v).into())).collect();
    let exports = sorted(exports);
    n += field("exports", &exports, &as_list(&py.exports), show);
    n += field("entries", &facts.entries, &py_entries, show);
    println!("check: image: {n} differences");
    n
}

// ---- the patch scan against Python's (the export's meta.json)

/// differences of the Rust side's patch scan, rows and effects from Python's
pub fn check_scan(rust: &crate::export::Meta, py: &crate::export::Meta, show: usize) -> usize {
    let mut n = 0;
    n += field("hle_symbols", &rust.hle_symbols, &py.hle_symbols, show);
    let mut a = rust.answers.clone();
    a.sort();
    let mut b = py.answers.clone();
    b.sort();
    n += field("answers", &a, &b, show);
    n += field("hle", &rust.hle, &py.hle, show);
    n += field("values", &rust.values, &py.values, show);
    n += field("rows", &rust.rows, &py.rows, show);
    n += field("func_names", &rust.func_names, &py.func_names, show);
    n += field("row_effects", &rust.row_effects, &py.row_effects, show);
    n += field("patch_funcs", &rust.patch_funcs, &py.patch_funcs, show);
    n += field("os_namespaces", &rust.os_namespaces, &py.os_namespaces, show);
    n += field("setjmp_names", &rust.setjmp_names, &py.setjmp_names, show);
    n += field("noreturn", &[&rust.noreturn_names, &rust.noreturn_parts], &[&py.noreturn_names, &py.noreturn_parts], show);
    n += field("part_insns", &[rust.part_insns], &[py.part_insns], show);
    println!("check: patch scan: {n} differences");
    n
}
