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
