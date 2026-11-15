//! One partition's IR text (nx2/lift2/game.py lift_part, verify off): its functions, the PATCH_FUNC stubs, the
//! setjmp owners' sjcall_ stubs and the code-pointer-sites trailer.

use crate::build::{build, scope, Prog};
use crate::emit::{no_capture, Module};
use crate::export::{Calls, Meta, Part};
use std::collections::{BTreeMap, HashMap, HashSet};

pub struct Lifted {
    pub text: String,
    pub lifted: usize,
    /// reason -> functions skipped for it
    pub skipped: BTreeMap<String, usize>,
}

pub fn lift_part(meta: &Meta, code_targets: &HashSet<u64>, part: &Part, skip: bool) -> Lifted {
    let calls = Calls::new(part);
    let sj: HashMap<u64, [u64; 5]> = meta.sj_rets.iter().map(|r| (r[0], *r)).collect();
    let mut m = Module::new(false);
    let mut extra: Vec<String> = Vec::new();
    let mut n_code = 0;
    let mut lifted = 0;
    let mut skipped = BTreeMap::new();
    for f in &part.functions {
        let (pcs, words) = f.body();
        let fname = format!("f_{:08x}", f.entry);
        if skip && f.plt.is_none() {
            if let Some(why) = scope(&pcs, &words) {
                *skipped.entry(why).or_insert(0) += 1;
                m.defined.insert(fname);
                continue;
            }
        }
        let prog = Prog::new(f, &pcs, code_targets, &calls, meta.omit);
        let mut fun = build(f, &pcs, &words, &prog);
        n_code += fun.code_sites;
        fun.name = fname.clone();
        if let Some(pf) = &f.func {
            fun.name = pf.orig_name.clone();
            extra.push(format!(
                "declare void @{n}(ptr)\ndefine void @f_{a:08x}(ptr %ctx) {{\n  musttail call void @{n}(ptr %ctx)\n  ret void\n}}",
                n = pf.name,
                a = f.entry
            ));
        }
        m.add(&fun);
        m.defined.insert(fname);
        lifted += 1;
    }
    let stubs: Vec<u64> = m
        .decls
        .iter()
        .filter_map(|d| d.strip_prefix("declare void @sjcall_"))
        .map(|s| u64::from_str_radix(&s[..8], 16).unwrap())
        .collect();
    if !stubs.is_empty() {
        for e in &stubs {
            m.defined.insert(format!("sjcall_{e:08x}"));
        }
        extra.push("declare void @hos_sj_call(ptr, ptr, i64, i64, i64, i64)".into());
        for e in &stubs {
            let t = format!("f_{e:08x}");
            if !m.defined.contains(&t) {
                m.decls.insert(format!("declare void @{t}(ptr)"));
            }
            let r = sj.get(e).unwrap_or_else(|| panic!("export: no sj_rets of {e:#x}"));
            extra.push(format!(
                "define internal void @sjcall_{e:08x}(ptr %ctx) {{\n  call void @hos_sj_call(ptr %ctx, ptr @{t}, i64 {}, i64 {}, i64 {}, i64 {})\n  ret void\n}}",
                r[1], r[2], r[3], r[4]
            ));
        }
    }
    let mut text = m.text();
    text.push_str(&no_capture(&extra.join("\n")));
    text.push_str(&format!("\n; code pointer sites: {n_code}\n"));
    Lifted { text, lifted, skipped }
}
