//! The plan (nx2/lift2/game.py write_plan, partitions; nx2/lift2/code_ptrs.py): which discovered body each function is
//! lifted with (scope's refusals become a trap at the entry, after the reachable() retry), the hot set, the partitions,
//! the code-pointer targets, sites and census, and each lifted function's records as the lift reads them.

use crate::build::target;
use crate::discover::Discovery;
use crate::export::{at_row, value_row, Func, Meta, Names, Part, PatchFunc, Plt};
use crate::image::Image;
use crate::sem::{self, Kind};
use crate::summary::def_use;
use ounce_decode::{decode, Fld};
use rayon::prelude::*;
use std::collections::{BTreeMap, HashMap, HashSet};

/// a partition is cut after the function that brings it to this many instructions or more (game.PART_INSNS: a build
/// parameter)
pub const PART_INSNS: usize = 120000;

/// a lifted function's body: sorted pcs and their words
pub struct PBody {
    pub entry: u64,
    pub pcs: Vec<u64>,
    pub words: Vec<u32>,
}

pub struct Plan {
    /// every entry (lifted, hooked, setjmp returns), sorted
    pub entries: Vec<u64>,
    /// the lifted bodies, by entry
    pub bodies: Vec<PBody>,
    /// the discovered bodies (the summaries' view, their words the image's) where the lifted ones differ: trapped or
    /// cut to their reachable part, by entry
    pub raw: BTreeMap<u64, Vec<u64>>,
    /// the lifted bodies' sizes (plan["insns"]), by entry as bodies
    pub insns: Vec<usize>,
    pub jt: HashMap<u64, Vec<u64>>,
    pub jt_index: HashMap<u64, (u64, u32, u32, Vec<u64>)>,
    /// (stub, target, slot), by stub
    pub plt: Vec<(u64, u64, u64)>,
    pub no_call: Vec<u64>,
    pub sj_rets: Vec<[u64; 5]>,
    pub hooks: Vec<u64>,
    pub why: BTreeMap<String, u32>,
    pub hot: Vec<u64>,
    pub code_excluded: Vec<u64>,
}

fn is_term(k: Kind) -> bool {
    k.is_term()
}

/// a branch to t stays in the function (build_ir.inside)
fn inside(entries: &HashSet<u64>, entry: u64, body: &[u64], t: u64) -> bool {
    body.binary_search(&t).is_ok() && (t == entry || !entries.contains(&t))
}

/// None if the function can be lifted, else why not (build_ir.scope with the plan's Program)
fn scope(d: &Discovery, entry: u64, pcs: &[u64], img: &Image) -> Option<String> {
    if d.plt.contains_key(&entry) {
        return None;
    }
    for &pc in pcs {
        let w = img.word(pc);
        let i = match decode(w) {
            Some(i) => i,
            None => return Some(format!("word {w:08x} at {pc:#x} not decoded")),
        };
        let k = sem::kind(&i);
        if let Kind::Missing = k {
            return Some(format!("{} at {pc:#x}", i.enc.name));
        }
        let tg = target(&i, pc);
        if let Kind::B = k {
            if i.f(Fld::op) != 0 {
                if !d.entries.contains(&d.callee(tg.unwrap())) {
                    return Some(format!("BL to a non-entry at {pc:#x}"));
                }
                continue;
            }
        }
        if let Some(t) = tg {
            if !inside(&d.entries, entry, pcs, t) && !d.entries.contains(&d.callee(t)) {
                return Some(format!("branch out of the body at {pc:#x}"));
            }
        }
        let falls = !is_term(k) || matches!(k, Kind::Bcond | Kind::Cbz | Kind::Tbz | Kind::Blr) || (matches!(k, Kind::Exc) && i.f(Fld::opc) == 0);
        if falls && pcs.binary_search(&(pc + 4)).is_err() && !d.entries.contains(&d.callee(pc + 4)) {
            return Some(format!("falls out of the body at {pc:#x}"));
        }
    }
    None
}

/// the instructions of a body reachable from its entry (game.reachable)
fn reachable(entry: u64, pcs: &[u64], img: &Image, jt: &HashMap<u64, Vec<u64>>) -> Vec<u64> {
    let mut seen: HashSet<u64> = HashSet::new();
    let mut todo = vec![entry];
    while let Some(pc) = todo.pop() {
        if seen.contains(&pc) || pcs.binary_search(&pc).is_err() {
            continue;
        }
        seen.insert(pc);
        let i = match decode(img.word(pc)) {
            Some(i) => i,
            None => continue,
        };
        let k = sem::kind(&i);
        if let Some(t) = target(&i, pc) {
            todo.push(t);
        }
        if let Some(t) = jt.get(&pc) {
            todo.extend(t);
        }
        if !is_term(k)
            || matches!(k, Kind::Bcond | Kind::Cbz | Kind::Tbz | Kind::Blr)
            || (matches!(k, Kind::Exc) && i.f(Fld::opc) == 0)
            || (matches!(k, Kind::B) && i.f(Fld::op) != 0)
        {
            todo.push(pc + 4);
        }
    }
    pcs.iter().copied().filter(|p| seen.contains(p)).collect()
}

/// the `why` key of a trap's reason: re.sub(r" \(.*| at .*| [0-9a-f]{8}.*|0x[0-9a-f]+", "", r)
pub fn why_key(r: &str) -> String {
    let b = r.as_bytes();
    let hex = |c: u8| c.is_ascii_digit() || (b'a'..=b'f').contains(&c);
    let mut out = String::new();
    let mut i = 0;
    while i < b.len() {
        let rest = &b[i..];
        if rest.starts_with(b" (") || rest.starts_with(b" at ") || (rest.len() >= 9 && rest[0] == b' ' && rest[1..9].iter().all(|&c| hex(c))) {
            break;
        }
        if rest.starts_with(b"0x") && rest.len() > 2 && hex(rest[2]) {
            let mut j = 2;
            while j < rest.len() && hex(rest[j]) {
                j += 1;
            }
            i += j;
            continue;
        }
        out.push(b[i] as char);
        i += 1;
    }
    out
}

/// a function's lifted body, its discovered body where that differs, the trap's reason
type Chosen = (PBody, Option<Vec<u64>>, Option<String>);

impl Plan {
    /// game.write_plan over a finished discovery; hot: the hot list's functions
    pub fn new(d: &Discovery, img: &Image, hot: &HashSet<u64>) -> Plan {
        let hooks: HashSet<u64> = d.hooks.clone();
        let mut es: Vec<u64> = d.funcs.keys().copied().collect();
        es.sort_unstable();
        let jt = &d.jt;
        let chosen: Vec<Chosen> = es
            .par_iter()
            .map(|&e| crate::busy(|| {
                let raw = &d.funcs[&e].order;
                let mut pcs = raw.clone();
                pcs.sort_unstable();
                let mut r = scope(d, e, &pcs, img);
                if r.as_deref().is_some_and(|r| r.contains("not decoded")) {
                    let live = reachable(e, &pcs, img, jt);
                    if live.len() < pcs.len() && scope(d, e, &live, img).is_none() {
                        pcs = live;
                        r = None;
                    }
                }
                let raw_sorted = {
                    let mut x = raw.clone();
                    x.sort_unstable();
                    x
                };
                if let Some(r) = r {
                    // (a trap at its entry; an entry whose first word is no instruction (a module's MOD0 header) is
                    // lifted as UDF #0, the others counted by their reason)
                    let why = if decode(img.word(e)).is_none() { None } else { Some(why_key(&r)) };
                    return (PBody { entry: e, pcs: vec![e], words: vec![0] }, Some(raw_sorted), why);
                }
                let words = pcs.iter().map(|&p| img.word(p)).collect();
                let over = (pcs != raw_sorted).then_some(raw_sorted);
                (PBody { entry: e, pcs, words }, over, None)
            }))
            .collect();
        let mut why: BTreeMap<String, u32> = BTreeMap::new();
        let n_hooked = es.iter().filter(|e| hooks.contains(e)).count() as u32;
        if n_hooked > 0 {
            why.insert("hooked".into(), n_hooked);
        }
        let mut bodies = Vec::with_capacity(chosen.len());
        let mut raw = BTreeMap::new();
        let mut insns = Vec::with_capacity(chosen.len());
        for (b, over, w) in chosen {
            if let Some(o) = over {
                raw.insert(b.entry, o);
            }
            if let Some(w) = w {
                *why.entry(w).or_insert(0) += 1;
            }
            insns.push(b.pcs.len());
            bodies.push(b);
        }
        let mut entries: Vec<u64> = d.entries.iter().copied().collect();
        entries.sort_unstable();
        // (check_complete: every entry has a body or is hooked)
        let lost = entries.iter().find(|e| es.binary_search(e).is_err() && !hooks.contains(e));
        if let Some(l) = lost {
            panic!("plan: an entry has no body and no HLE, first f_{l:08x}");
        }
        let mut plt: Vec<(u64, u64, u64)> = d.plt.iter().map(|(&s, &(t, slot))| (s, t, slot)).collect();
        plt.sort_unstable();
        let mut no_call: Vec<u64> = d.setjmp_owner.iter().map(|o| o.0).collect();
        no_call.sort_unstable();
        let mut sj_rets: Vec<[u64; 5]> = d
            .setjmp_owner
            .iter()
            .map(|(e, r)| {
                let g = |k: usize| r.get(k).copied().unwrap_or(0);
                [*e, g(0), g(1), g(2), g(3)]
            })
            .collect();
        sj_rets.sort_unstable();
        let mut hk: Vec<u64> = hooks.iter().copied().collect();
        hk.sort_unstable();
        let mut hot: Vec<u64> = hot.iter().copied().filter(|e| es.binary_search(e).is_ok()).collect();
        hot.sort_unstable();
        let mut code_excluded: Vec<u64> = img.modules.iter().map(|m| m.base).collect();
        code_excluded.sort_unstable();
        Plan {
            entries,
            bodies,
            raw,
            insns,
            jt: d.jt.clone(),
            jt_index: d.jt_index.clone(),
            plt,
            no_call,
            sj_rets,
            hooks: hk,
            why,
            hot,
            code_excluded,
        }
    }

    /// the partitions (game.partitions): cold then hot, each by entry, cut after the function that brings it to
    /// part_insns or more; (tag, indices into bodies)
    pub fn partitions(&self, part_insns: usize) -> Vec<(char, Vec<usize>)> {
        let mut parts = Vec::new();
        for (tag, want_hot) in [('c', false), ('h', true)] {
            let (mut cur, mut n) = (Vec::new(), 0);
            for (k, b) in self.bodies.iter().enumerate() {
                if (self.hot.binary_search(&b.entry).is_ok()) != want_hot {
                    continue;
                }
                cur.push(k);
                n += b.pcs.len();
                if n >= part_insns {
                    parts.push((tag, std::mem::take(&mut cur)));
                    n = 0;
                }
            }
            if !cur.is_empty() {
                parts.push((tag, cur));
            }
        }
        parts
    }

    /// code_ptrs.targets: the function table less the modules' bases, sorted
    pub fn code_targets(&self) -> Vec<u64> {
        let mut t: Vec<u64> = self.bodies.iter().map(|b| b.entry).chain(self.hooks.iter().copied()).collect();
        t.retain(|a| self.code_excluded.binary_search(a).is_err());
        t.sort_unstable();
        t.dedup();
        t
    }
}

// ---- code pointers (code_ptrs.py)

fn imm21(w: u32) -> i64 {
    let v = ((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)) as i64;
    if v >> 20 != 0 {
        v - (1 << 21)
    } else {
        v
    }
}

pub fn branch_target(pc: u64, w: u32) -> Option<u64> {
    let p = pc as i64;
    if w & 0x7C000000 == 0x14000000 {
        let o = (w & 0x3FFFFFF) as i64;
        return Some((p + 4 * if o >> 25 != 0 { o - (1 << 26) } else { o }) as u64);
    }
    if w & 0xFF000010 == 0x54000000 || w & 0x7E000000 == 0x34000000 {
        let o = ((w >> 5) & 0x7FFFF) as i64;
        return Some((p + 4 * if o >> 18 != 0 { o - (1 << 19) } else { o }) as u64);
    }
    if w & 0x7E000000 == 0x36000000 {
        let o = ((w >> 5) & 0x3FFF) as i64;
        return Some((p + 4 * if o >> 13 != 0 { o - (1 << 14) } else { o }) as u64);
    }
    None
}

fn ends_block(w: u32) -> bool {
    branch_target(0, w).is_some() || w & 0xFE000000 == 0xD6000000 || w & 0xFF000000 == 0xD4000000
}

fn leaders(pcs: &[u64], words: &[u32], jt: &HashMap<u64, Vec<u64>>) -> HashSet<u64> {
    let inb = |t: u64| pcs.binary_search(&t).is_ok();
    let mut lead = HashSet::new();
    for (&pc, &w) in pcs.iter().zip(words) {
        if let Some(t) = branch_target(pc, w) {
            if inb(t) {
                lead.insert(t);
            }
        }
        if ends_block(w) {
            lead.insert(pc + 4);
            if let Some(ts) = jt.get(&pc) {
                lead.extend(ts.iter().copied().filter(|&x| inb(x)));
            }
        }
    }
    lead
}

fn xbit(n: u32) -> u128 {
    1u128 << n
}

/// {pc: (entry, page)} of a body's code pointer sites (code_ptrs.sites), sorted by pc
pub fn sites(pcs: &[u64], words: &[u32], tg: &[u64], jt: &HashMap<u64, Vec<u64>>) -> Vec<(u64, u64, Option<u64>)> {
    let is_tg = |a: u64| tg.binary_search(&a).is_ok();
    let mut out = Vec::new();
    let mut pos: Option<HashSet<u64>> = None;
    for (k, (&pc, &w)) in pcs.iter().zip(words).enumerate() {
        if w & 0x9F000000 == 0x10000000 {
            let t = (pc as i64 + imm21(w)) as u64;
            if is_tg(t) {
                out.push((pc, t, None));
            }
            continue;
        }
        if w & 0xFFC00000 != 0x91000000 {
            continue;
        }
        let n = (w >> 5) & 31;
        if n == 31 {
            continue;
        }
        for d in 1..=4usize {
            if k < d || pcs[k - d] != pc - 4 * d as u64 {
                break;
            }
            let p = words[k - d];
            if p & 0x9F00001F == 0x90000000 | n {
                let page = (((pc - 4 * d as u64) & !0xFFF) as i64 + (imm21(p) << 12)) as u64;
                let t = page + ((w >> 10) & 0xFFF) as u64;
                if is_tg(t) {
                    let lead = pos.get_or_insert_with(|| leaders(pcs, words, jt));
                    let mut q = pc - 4 * d as u64 + 4;
                    let mut cut = false;
                    while q < pc + 4 {
                        if lead.contains(&q) {
                            cut = true;
                            break;
                        }
                        q += 4;
                    }
                    if !cut {
                        out.push((pc, t, Some(t - ((w >> 10) & 0xFFF) as u64)));
                    }
                }
                break;
            }
            if ends_block(p) || def_use(p).1 & xbit(n) != 0 {
                break;
            }
        }
    }
    out
}

/// the census's other code addresses: ({kind: count}, [(pc, why)]) (code_ptrs.audit)
#[allow(clippy::too_many_arguments)]
pub fn audit(
    pcs: &[u64],
    words: &[u32],
    tg: &[u64],
    excluded: &[u64],
    texts: &[(u64, u64)],
    jt: &HashMap<u64, Vec<u64>>,
    continuations: &HashMap<u64, u32>,
    kinds: &mut BTreeMap<String, u64>,
    odd: &mut Vec<(u64, String)>,
) {
    let in_text = |a: u64| texts.iter().any(|&(lo, hi)| lo <= a && a < hi);
    let mut pos: Option<HashSet<u64>> = None;
    for (k, (&pc, &w)) in pcs.iter().zip(words).enumerate() {
        if w & 0x9F000000 == 0x10000000 {
            let t = (pc as i64 + imm21(w)) as u64;
            if tg.binary_search(&t).is_ok() || !in_text(t) {
                continue;
            }
            let n = w & 31;
            let kind = if excluded.contains(&t) || excluded.contains(&(t.wrapping_sub(1))) {
                Some("module base")
            } else if continuations.get(&pc) == Some(&w) {
                Some("local continuation")
            } else {
                let mut kind = None;
                for &x in &words[k + 1..(k + 6).min(pcs.len())] {
                    let (u, d) = def_use(x);
                    if u & xbit(n) != 0 {
                        if matches!(x & 0xFF200000, 0x8B000000 | 0x8B200000) {
                            kind = Some("jump-table base");
                        }
                        break;
                    }
                    if d & xbit(n) != 0 {
                        break;
                    }
                }
                kind
            };
            match kind {
                None => odd.push((pc, format!("ADR of {t:#x}, not an entry"))),
                Some(kd) => *kinds.entry(kd.into()).or_insert(0) += 1,
            }
            continue;
        }
        if w & 0x9F000000 != 0x90000000 || w & 31 == 31 {
            continue;
        }
        let n = w & 31;
        let page = ((pc & !0xFFF) as i64 + (imm21(w) << 12)) as u64;
        if !texts.iter().any(|&(lo, hi)| (lo & !0xFFF) <= page && page < hi) {
            continue;
        }
        let lead = pos.get_or_insert_with(|| leaders(pcs, words, jt));
        let mut why: Option<String> = None;
        let mut j = k + 1;
        loop {
            if j >= pcs.len() || pcs[j] != pcs[j - 1] + 4 || lead.contains(&pcs[j]) {
                why = Some("its page is live at a block's end".into());
                break;
            }
            let u = words[j];
            let (us, ds) = def_use(u);
            if us & xbit(n) != 0 && !((u & 0xFFC00000 == 0x91000000 || u & 0x3B000000 == 0x39000000) && (u >> 5) & 31 == n) {
                why = Some(format!("its page is read by {u:08x} at {:#x}", pcs[j]));
            } else if ds & xbit(n) != 0 {
                break;
            } else if ends_block(u) {
                why = Some("its page is live at a branch".into());
            }
            if why.is_some() {
                break;
            }
            j += 1;
        }
        match why {
            Some(wy) => odd.push((pc, format!("ADRP of the text page {page:#x}: {wy}"))),
            None => *kinds.entry("text page consumed".into()).or_insert(0) += 1,
        }
    }
}

/// one partition's census: (sites, {kind: count}, odd) (code_ptrs.count)
pub type Census = (u64, BTreeMap<String, u64>, Vec<(u64, String)>);

/// the partitions as the lift reads them, and their census
pub struct Parts {
    pub parts: Vec<Part>,
    pub census: Vec<Census>,
    pub code_targets: Vec<u64>,
}

/// every partition's functions with the records the lift reads of them (as the export's parts were), and the census
pub fn parts(plan: &Plan, meta: &Meta, img: &Image, continuations: &HashMap<u64, u32>) -> Parts {
    let tg = plan.code_targets();
    let entries: HashSet<u64> = plan.entries.iter().copied().collect();
    let texts: Vec<(u64, u64)> = img
        .modules
        .iter()
        .flat_map(|m| m.segments.iter().filter(|s| s.name == ".text").map(|s| (s.addr, s.addr + s.size)))
        .collect();
    let mut values_by: HashMap<u64, Vec<usize>> = HashMap::new();
    for (k, v) in meta.values.iter().enumerate() {
        values_by.entry(v.func).or_default().push(k);
    }
    let mut rows_by: HashMap<u64, Vec<usize>> = HashMap::new();
    for (k, v) in meta.rows.iter().enumerate() {
        rows_by.entry(v.func).or_default().push(k);
    }
    let parts = plan.partitions(meta.part_insns);
    let built: Vec<(Part, Census)> = parts
        .par_iter()
        .enumerate()
        .map(|(k, (tag, idx))| crate::busy(|| {
            let mut names = Names::default();
            let mut fns = Vec::with_capacity(idx.len());
            let (mut nsites, mut kinds, mut odd) = (0u64, BTreeMap::new(), Vec::new());
            for &i in idx {
                let b = &plan.bodies[i];
                let e = b.entry;
                let (pcs, words) = (&b.pcs, &b.words);
                let inb = |t: u64| pcs.binary_search(&t).is_ok();
                let mut jt: Vec<(u64, Vec<u64>)> = pcs
                    .iter()
                    .filter_map(|pc| {
                        plan.jt.get(pc).map(|t| {
                            let mut t = t.clone();
                            t.sort_unstable();
                            t.dedup();
                            (*pc, t)
                        })
                    })
                    .collect();
                jt.sort_unstable_by_key(|r| r.0);
                // (the tables the lift switches on by their index: the load in the body, no probe or row from the
                // load to the BR; the summaries read the same)
                let rowed = |lo: u64, hi: u64| {
                    let at = |pc: u64| lo <= pc && pc <= hi;
                    values_by.get(&e).is_some_and(|v| v.iter().any(|&j| at(meta.values[j].pc)))
                        || rows_by.get(&e).is_some_and(|v| v.iter().any(|&j| meta.rows[j].mode != "instead" && at(meta.rows[j].pc)))
                };
                let jt_index: Vec<(u64, u64, u32, u32, Vec<u64>)> = pcs
                    .iter()
                    .filter_map(|pc| plan.jt_index.get(pc).map(|v| (*pc, v.0, v.1, v.2, v.3.clone())))
                    .filter(|r| inb(r.1) && !rowed(r.1, r.0))
                    .collect();
                let code_sites = sites(pcs, words, &tg, &plan.jt);
                nsites += code_sites.len() as u64;
                audit(pcs, words, &tg, &plan.code_excluded, &texts, &plan.jt, continuations, &mut kinds, &mut odd);
                let plt = plan.plt.binary_search_by_key(&e, |p| p.0).ok().map(|j| {
                    let (_, t, slot) = plan.plt[j];
                    Plt { target: t, slot, target_is_code: tg.binary_search(&t).is_ok() }
                });
                let body_entries: Vec<u64> = pcs.iter().copied().filter(|p| entries.contains(p)).collect();
                let values = values_by.get(&e).map_or(Vec::new(), |v| v.iter().map(|&j| value_row(&meta.values[j], &mut names)).collect());
                let rows = rows_by.get(&e).map_or(Vec::new(), |v| v.iter().map(|&j| at_row(&meta.rows[j], &mut names)).collect());
                let func = meta
                    .func_names
                    .binary_search_by_key(&e, |f| f.0)
                    .ok()
                    .map(|j| PatchFunc { name: names.sym(&meta.func_names[j].1), orig_name: names.sym(&meta.func_names[j].2) });
                fns.push(Func {
                    entry: e,
                    pcs: pcs.clone(),
                    words: words.clone(),
                    plt,
                    body_entries,
                    jt,
                    jt_index,
                    code_sites,
                    values,
                    rows,
                    func,
                });
            }
            let name = format!("lift_{tag}{k:03}");
            (Part { name, functions: fns, names }, (nsites, kinds, odd))
        }))
        .collect();
    let (parts, census) = built.into_iter().unzip();
    Parts { parts, census, code_targets: tg }
}
