//! One partition's IR text (nx2/lift2/game.py lift_part, verify off): its functions, the PATCH_FUNC stubs, the
//! setjmp owners' sjcall_ stubs and the code-pointer-sites trailer.

use crate::build::{build, scope, Prog};
use crate::emit::{sym, Decl, Module};
use crate::export::{Meta, Part};
use crate::ir::Sym;
use std::collections::{BTreeMap, HashSet};
use std::io::Write;

pub struct Lifted {
    /// the text: the module's header (triple, declarations, helpers), then its body
    pub head: Vec<u8>,
    pub body: Vec<u8>,
    pub lifted: usize,
    /// reason -> functions skipped for it
    pub skipped: BTreeMap<String, usize>,
}

pub fn lift_part(meta: &Meta, code_targets: &HashSet<u64>, part: &Part, skip: bool) -> Lifted {
    let insns = part.functions.iter().map(|f| f.pcs.len()).sum();
    let mut m = Module::new(&part.names, false, insns);
    let mut extra: Vec<u8> = Vec::new();
    let mut n_code = 0;
    let mut lifted = 0;
    let mut skipped = BTreeMap::new();
    for f in &part.functions {
        let fname = Sym::Entry(f.entry);
        if skip && f.plt.is_none() {
            if let Some(why) = scope(&f.pcs, &f.words) {
                *skipped.entry(why).or_insert(0) += 1;
                m.defined.insert(fname);
                continue;
            }
        }
        let prog = Prog::new(f, code_targets, part, meta.omit);
        let built = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| build(f, &prog)));
        let fun = match built {
            Ok(fun) => fun,
            Err(e) if skip => {
                let msg = e.downcast_ref::<String>().cloned().or_else(|| e.downcast_ref::<&str>().map(|s| s.to_string()));
                eprintln!("f_{:08x}: {}", f.entry, msg.as_deref().unwrap_or("panic"));
                *skipped.entry(format!("failed: {}", msg.unwrap_or_default().split(" of ").next().unwrap_or(""))).or_insert(0) += 1;
                m.defined.insert(fname);
                continue;
            }
            Err(e) => std::panic::resume_unwind(e),
        };
        n_code += fun.code_sites;
        match &f.func {
            Some(pf) => {
                // (a PATCH_FUNC: the lifted body is named orig_name; f_<entry> tail-calls the host function)
                m.add(&fun, pf.orig_name);
                let mut n = Vec::new();
                sym(&mut n, &part.names, pf.name);
                let n = String::from_utf8(n).expect("utf-8");
                if !extra.is_empty() {
                    extra.push(b'\n');
                }
                write!(
                    extra,
                    "declare void @{n}(ptr captures(none))\ndefine void @f_{a:08x}(ptr %ctx) {{\n  musttail call void @{n}(ptr %ctx)\n  ret void\n}}",
                    a = f.entry
                )
                .unwrap();
            }
            None => m.add(&fun, fname),
        }
        m.defined.insert(fname);
        lifted += 1;
    }
    // (the setjmp owners whose sjcall_ stub the functions call, by address as their declarations sort)
    let mut stubs: Vec<u64> = m
        .decls()
        .filter_map(|d| match d {
            Decl::Fn(Sym::SjCall(e)) => Some(*e),
            _ => None,
        })
        .collect();
    stubs.sort_unstable();
    if !stubs.is_empty() {
        for &e in &stubs {
            m.defined.insert(Sym::SjCall(e));
        }
        if !extra.is_empty() {
            extra.push(b'\n');
        }
        extra.extend_from_slice(b"declare void @hos_sj_call(ptr captures(none), ptr, i64, i64, i64, i64)");
        for &e in &stubs {
            if !m.defined.contains(&Sym::Entry(e)) {
                m.declare(Decl::Fn(Sym::Entry(e)));
            }
            let k = meta.sj_rets.binary_search_by_key(&e, |r| r[0]);
            let r = meta.sj_rets[k.unwrap_or_else(|_| panic!("export: no sj_rets of {e:#x}"))];
            write!(
                extra,
                "\ndefine internal void @sjcall_{e:08x}(ptr %ctx) {{\n  call void @hos_sj_call(ptr %ctx, ptr @f_{e:08x}, i64 {}, i64 {}, i64 {}, i64 {})\n  ret void\n}}",
                r[1], r[2], r[3], r[4]
            )
            .unwrap();
        }
    }
    let (head, mut body) = m.finish();
    body.extend_from_slice(&extra);
    write!(body, "\n; code pointer sites: {n_code}\n").unwrap();
    Lifted { head, body, lifted, skipped }
}
