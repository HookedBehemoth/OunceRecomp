//! Function discovery (nx2/lift2/discover.py): function bodies from the entries, jump tables (and those switched on
//! their index), PLT stubs, setjmp owners and the split of very large functions.
//!
//! A function is an entry plus everything reachable from it without crossing another entry. What a jump table
//! resolves to depends on the tables known (another switch's edges reach the table's constants), so the tables are
//! found in rounds against the tables of the round before, to a fixed point (Discovery::discover): the walks and
//! the resolutions of a round read only that round's fixed state and run in parallel, in any order.

use crate::image::Image;
use rayon::prelude::*;
use std::collections::{HashMap, HashSet};

type Map<K, V> = HashMap<K, V>;

pub fn sx(v: u64, bits: u32) -> i64 {
    if v & (1 << (bits - 1)) != 0 {
        v as i64 - (1i64 << bits)
    } else {
        v as i64
    }
}

/// discover.classify: the control-flow kind of a word
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Ck {
    Ret,
    Br(u32),
    Blr(u32),
    B(u64),
    Bl(u64),
    Bcond(u64),
    Cbz(u64),
    Tbz(u64),
    Svc,
    Trap,
    Isb,
    Ic,
    Op,
}

fn at(pc: u64, off: i64) -> u64 {
    (pc as i64).wrapping_add(off) as u64
}

pub fn classify(w: u32, pc: u64) -> Ck {
    let w64 = w as u64;
    if w == 0xD65F0BFF || w == 0xD65F0FFF {
        return Ck::Ret;
    }
    if w & 0xFEFFF800 == 0xD61F0800 {
        return Ck::Br((w >> 5) & 31);
    }
    if w & 0xFEFFF800 == 0xD63F0800 {
        return Ck::Blr((w >> 5) & 31);
    }
    if w & 0x7C000000 == 0x14000000 {
        let t = at(pc, 4 * sx(w64 & 0x3FFFFFF, 26));
        return if w & 0x80000000 != 0 { Ck::Bl(t) } else { Ck::B(t) };
    }
    if w & 0xFF000010 == 0x54000000 {
        return Ck::Bcond(at(pc, 4 * sx((w64 >> 5) & 0x7FFFF, 19)));
    }
    if w & 0x7E000000 == 0x34000000 {
        return Ck::Cbz(at(pc, 4 * sx((w64 >> 5) & 0x7FFFF, 19)));
    }
    if w & 0x7E000000 == 0x36000000 {
        return Ck::Tbz(at(pc, 4 * sx((w64 >> 5) & 0x3FFF, 14)));
    }
    if w & 0xFFFFFC1F == 0xD61F0000 {
        return Ck::Br((w >> 5) & 31);
    }
    if w & 0xFFFFFC1F == 0xD63F0000 {
        return Ck::Blr((w >> 5) & 31);
    }
    if w & 0xFFFFFC1F == 0xD65F0000 {
        return Ck::Ret;
    }
    if w & 0xFFE0001F == 0xD4000001 {
        return Ck::Svc;
    }
    if matches!(w & 0xFFE0001F, 0xD4200000 | 0xD4400000) {
        return Ck::Trap;
    }
    if w & 0xFFFFF0FF == 0xD50330DF {
        return Ck::Isb;
    }
    if w & 0xFFFFFFE0 == 0xD50B7520 {
        return Ck::Ic;
    }
    Ck::Op
}

fn auth_end(w: u32) -> bool {
    w == 0xD65F0BFF || w == 0xD65F0FFF || w & 0xFFFFF81F == 0xD61F081F || w & 0xFFFFF800 == 0xD71F0800
}

/// discover.reads_rt_only: bits 0-4 are not a destination (stores, compares, branches)
pub fn reads_rt_only(w: u32) -> bool {
    (w & 0x3A000000 == 0x38000000 && (w >> 22) & 1 == 0)
        || w & 0x1C000000 == 0x14000000
        || matches!(w & 0x7E000000, 0x34000000 | 0x36000000)
        || ((w >> 27) & 7 == 5 && (w >> 22) & 1 == 0)
}

/// discover.writes: the instruction may write X`reg`
pub fn writes(w: u32, reg: u32) -> bool {
    if (w >> 25) & 7 == 7 {
        // (of the SIMD and FP class only FMOV / FCVT* to a general register, and UMOV / SMOV: imm4 0111 / 0101)
        return w & 31 == reg && (w & 0x5F20FC00 == 0x1E200000 || w & 0xBFE0EC00 == 0x0E002C00);
    }
    if w & 0x0A000000 == 0x08000000 && (w >> 26) & 1 != 0 {
        return false;
    }
    if w & 31 == reg && !reads_rt_only(w) {
        return true;
    }
    (w >> 27) & 7 == 5 && (w >> 22) & 1 != 0 && (w >> 26) & 1 == 0 && (w >> 10) & 31 == reg
}

fn adrp_page(w: u32, pc: u64) -> u64 {
    let imm = (((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)) as u64;
    ((pc & !0xFFF) as i64 + (sx(imm, 21) << 12)) as u64
}
fn adr_target(w: u32, pc: u64) -> u64 {
    let imm = (((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)) as u64;
    (pc as i64 + sx(imm, 21)) as u64
}
fn add_imm(w: u32) -> u64 {
    (((w >> 10) & 0xFFF) as u64) << if w & (1 << 22) != 0 { 12 } else { 0 }
}

/// a function's body: its pcs in the walk's order (Python's dict order)
#[derive(Clone, Default, Debug)]
pub struct Body {
    pub order: Vec<u64>,
}

/// a symbolic value of the jump-table window (discover._jump_table's R)
#[derive(Clone, Debug, PartialEq)]
enum V {
    C(u64),
    Reg(u32),
    RegOff(u32, u64),
    Ld(Box<Ld>),
    Add(Box<Add>),
}

#[derive(Clone, Debug, PartialEq)]
struct Ld {
    v: V,
    m: u32,
    size: u64,
    signed: bool,
    pc: u64,
    opc: u32,
    opt: u32,
    n: u32,
    s: u32,
}

#[derive(Clone, Debug, PartialEq)]
struct Add {
    a: Option<V>,
    b: Option<V>,
    k: u32,
    ext: Option<u32>,
    pc: u64,
    n: u32,
}

/// the shape of a table BR as _jump_table read it, or why it has none (_jt_cand)
#[derive(Clone, Debug)]
enum Cand {
    Why(&'static str),
    Shape { load: u64, idx: u32, opt: u32, s: u32, size: u64, opc: u32, treg: u32, table: u64, add: u64, areg: Option<u32>, k: u32, ext: Option<u32>, count: Option<u64> },
}

/// The walk of the functions against a table map fixed for the round: a function's body is the code reachable from
/// its entry without crossing another entry, a BR's successors its table's targets in that map.
pub struct Discovery<'a> {
    img: &'a Image,
    pub entries: HashSet<u64>,
    pub hooks: HashSet<u64>,
    /// the functions whose symbol is a setjmp
    setjmp_syms: HashSet<u64>,
    /// entry -> body; the functions by entry
    pub funcs: Map<u64, Body>,
    pub order: Vec<u64>,
    pub jt: Map<u64, Vec<u64>>,
    /// br -> (load pc, index register, LDR extend option, targets)
    pub jt_index: Map<u64, (u64, u32, u32, Vec<u64>)>,
    /// the shape of each BR's table as its last resolution read it
    jt_cand: Map<u64, Cand>,
    pub plt: Map<u64, (u64, u64)>,
    /// owner -> the return addresses of its setjmp calls
    pub setjmp_owner: Vec<(u64, Vec<u64>)>,
    eh_starts: Vec<u64>,
    /// the rounds the tables took to reach their fixed point
    pub rounds: u32,
}

/// a walked function: its body and the BRs it holds (pc, register)
struct Walked {
    body: Body,
    brs: Vec<(u64, u32)>,
}

impl<'a> Discovery<'a> {
    pub fn new(img: &'a Image, entries: &[u64], names: &[(u64, String)], setjmp_names: &[String], eh_starts: Vec<u64>) -> Discovery<'a> {
        let hooks: HashSet<u64> = img.hooks.iter().map(|h| h.addr).collect();
        let mut all: HashSet<u64> = entries.iter().copied().collect();
        all.extend(&hooks);
        let setjmp_syms = names.iter().filter(|(_, n)| setjmp_names.iter().any(|s| s == n)).map(|(a, _)| *a).collect();
        let mut eh = eh_starts;
        if eh.is_empty() {
            eh = all.iter().copied().collect();
        }
        eh.sort_unstable();
        Discovery {
            img,
            entries: all,
            hooks,
            setjmp_syms,
            funcs: Map::default(),
            order: Vec::new(),
            jt: Map::default(),
            jt_index: Map::default(),
            jt_cand: Map::default(),
            plt: Map::default(),
            setjmp_owner: Vec::new(),
            eh_starts: eh,
            rounds: 0,
        }
    }

    fn function_extent(&self, pc: u64) -> (u64, u64) {
        extent(&self.eh_starts, pc)
    }

    fn resolver(&self) -> Resolver<'_> {
        Resolver { img: self.img, eh_starts: &self.eh_starts, entries: &self.entries, jt: &self.jt, preds: Map::default(), redirect: None, cand: Cand::Why("unusual shape") }
    }

    /// The tables to their fixed point, in rounds: each round walks the functions against the tables of the round
    /// before and resolves every BR the bodies hold against them too (a table found through another switch's edges
    /// needs those edges known), until a round changes no table. Each walk and each resolution reads only the
    /// round's fixed state: the result is independent of the order and the thread count they run in.
    pub fn discover(&mut self, max_insns: usize) {
        let mut es: Vec<u64> = self.entries.iter().copied().filter(|e| !self.hooks.contains(e)).collect();
        es.sort_unstable();
        let walk_all = |d: &Self, ks: &[usize]| -> Vec<Walked> {
            ks.par_iter().map_init(HashSet::new, |seen, &k| crate::busy(|| d.walk_in(es[k], seen))).collect()
        };
        let mut walked: Vec<Walked> = walk_all(self, &(0..es.len()).collect::<Vec<_>>());
        // (a BR's resolution reads the tables of its own extent only, through that extent's predecessor map: a round
        // resolves again the BRs of the extents a changed table is in, and the BRs new to the bodies, each extent's
        // in one task with its map built once)
        let mut res: Map<u64, (Option<Vec<u64>>, Cand)> = Map::default();
        let mut dirty: HashSet<u64> = HashSet::new();
        loop {
            let mut brs: Vec<(u64, u32)> = walked.iter().flat_map(|w| w.brs.iter().copied()).collect();
            brs.sort_unstable();
            brs.dedup();
            let live: HashSet<u64> = brs.iter().map(|b| b.0).collect();
            res.retain(|b, _| live.contains(b));
            let mut groups: Vec<(u64, Vec<(u64, u32)>)> = Vec::new();
            for &(br, reg) in &brs {
                let lo = self.function_extent(br).0;
                if res.contains_key(&br) && !dirty.contains(&lo) {
                    continue;
                }
                match groups.last_mut() {
                    Some(g) if g.0 == lo => g.1.push((br, reg)),
                    _ => groups.push((lo, vec![(br, reg)])),
                }
            }
            let got: Vec<Vec<(u64, Option<Vec<u64>>, Cand)>> = groups
                .par_iter()
                .map(|(_, g)| {
                    crate::busy(|| {
                        let mut r = self.resolver();
                        g.iter()
                            .map(|&(br, reg)| {
                                let t = r.jump_table(br, reg);
                                (br, t, std::mem::replace(&mut r.cand, Cand::Why("unusual shape")))
                            })
                            .collect()
                    })
                })
                .collect();
            for (br, t, c) in got.into_iter().flatten() {
                res.insert(br, (t, c));
            }
            let jt: Map<u64, Vec<u64>> = res.iter().filter_map(|(b, (t, _))| t.clone().map(|t| (*b, t))).collect();
            if jt == self.jt {
                break;
            }
            self.rounds += 1;
            assert!(self.rounds < 64, "discovery: the jump tables reach no fixed point in {} rounds", self.rounds);
            let changed: HashSet<u64> =
                jt.iter().filter(|(b, t)| self.jt.get(b) != Some(t)).map(|(b, _)| *b).chain(self.jt.keys().filter(|b| !jt.contains_key(b)).copied()).collect();
            dirty = changed.iter().map(|&b| self.function_extent(b).0).collect();
            self.jt = jt;
            let again: Vec<usize> = (0..walked.len()).filter(|&k| walked[k].brs.iter().any(|b| changed.contains(&b.0))).collect();
            for (k, w) in again.iter().zip(walk_all(self, &again)) {
                walked[*k] = w;
            }
        }
        self.jt_cand = res.into_iter().map(|(b, (_, c))| (b, c)).collect();
        for (e, w) in es.iter().zip(walked) {
            self.funcs.insert(*e, w.body);
        }
        self.order = es;
        if max_insns > 0 {
            self.split_large(max_insns);
        }
        let plt: Vec<(u64, (u64, u64))> = self.order.iter().filter_map(|&e| self.plt_target(e).map(|t| (e, t))).collect();
        self.plt.extend(plt);
        self.find_setjmp_owners();
        self.index_tables();
    }

    fn set_func(&mut self, e: u64, b: Body) {
        if self.funcs.insert(e, b).is_none() {
            self.order.push(e);
        }
    }

    pub fn callee(&self, mut t: u64) -> u64 {
        for _ in 0..4 {
            match self.plt.get(&t) {
                Some(p) => t = p.0,
                None => break,
            }
        }
        t
    }

    fn find_setjmp_owners(&mut self) {
        let mut owners: Vec<(u64, Vec<u64>)> = Vec::new();
        for e in &self.order {
            for &pc in &self.funcs[e].order {
                if let Ck::Bl(t) = classify(self.img.word(pc), pc) {
                    if self.setjmp_syms.contains(&self.callee(t)) {
                        match owners.iter_mut().find(|o| o.0 == *e) {
                            Some(o) => o.1.push(pc + 4),
                            None => owners.push((*e, vec![pc + 4])),
                        }
                    }
                }
            }
        }
        for (_, rets) in owners.clone() {
            for r in rets {
                if !self.funcs.contains_key(&r) {
                    self.entries.insert(r);
                    let b = self.walk(r).body;
                    self.set_func(r, b);
                }
            }
        }
        self.setjmp_owner = owners;
    }

    /// extra entries in functions longer than max_insns, about every max_insns / 2 instructions, where no branch
    /// crosses the cut (discover.split_large; the standalone lift runs without it)
    fn split_large(&mut self, max_insns: usize) {
        let mut new: Vec<u64> = Vec::new();
        for e in &self.order {
            let body = &self.funcs[e].order;
            if body.len() <= max_insns {
                continue;
            }
            let mut pcs = body.clone();
            pcs.sort_unstable();
            let inb = |t: u64| pcs.binary_search(&t).is_ok();
            let mut diff = vec![0i64; pcs.len() + 1];
            for &pc in body {
                let targets: Vec<u64> = match classify(self.img.word(pc), pc) {
                    Ck::B(t) | Ck::Bcond(t) | Ck::Cbz(t) | Ck::Tbz(t) => vec![t],
                    Ck::Br(_) => self.jt.get(&pc).cloned().unwrap_or_default(),
                    _ => vec![],
                };
                for t in targets {
                    if !inb(t) || t == pc {
                        continue;
                    }
                    let (lo, hi) = if t > pc {
                        (pcs.partition_point(|&x| x <= pc), pcs.partition_point(|&x| x < t))
                    } else {
                        (pcs.partition_point(|&x| x <= t), pcs.partition_point(|&x| x <= pc))
                    };
                    if lo < hi {
                        diff[lo] += 1;
                        diff[hi] -= 1;
                    }
                }
            }
            let step = max_insns / 2;
            let (mut last, mut cover) = (0usize, 0i64);
            for (i, &pc) in pcs.iter().enumerate() {
                cover += diff[i];
                if i - last >= step && cover == 0 && i > 0 && pcs[i - 1] == pc - 4 {
                    new.push(pc);
                    last = i;
                }
            }
        }
        new.retain(|a| !self.entries.contains(a));
        new.sort_unstable();
        new.dedup();
        if new.is_empty() {
            return;
        }
        self.entries.extend(&new);
        let newset: HashSet<u64> = new.iter().copied().collect();
        let mut todo: Vec<u64> = new.clone();
        for e in &self.order {
            if self.funcs[e].order.iter().any(|p| newset.contains(p)) {
                todo.push(*e);
            }
        }
        todo.sort_unstable();
        todo.dedup();
        for a in todo {
            let b = self.walk(a).body;
            self.set_func(a, b);
        }
    }

    /// the body from an entry, a BR's successors its table in the current map (the walk reads nothing else that
    /// changes)
    fn walk(&self, entry: u64) -> Walked {
        self.walk_in(entry, &mut HashSet::new())
    }

    /// walk with a scratch set
    fn walk_in(&self, entry: u64, seen: &mut HashSet<u64>) -> Walked {
        seen.clear();
        let mut order = Vec::new();
        let mut brs = Vec::new();
        let mut todo = vec![entry];
        let visit = |t: u64, todo: &mut Vec<u64>| {
            if (t == entry || !self.entries.contains(&t)) && self.img.in_code(t) {
                todo.push(t);
            }
        };
        while let Some(mut pc) = todo.pop() {
            while !seen.contains(&pc) {
                if (pc != entry && self.entries.contains(&pc)) || !self.img.in_code(pc) {
                    break;
                }
                let k = classify(self.img.word(pc), pc);
                seen.insert(pc);
                order.push(pc);
                match k {
                    Ck::B(t) => {
                        visit(t, &mut todo);
                        break;
                    }
                    Ck::Bcond(t) | Ck::Cbz(t) | Ck::Tbz(t) => visit(t, &mut todo),
                    Ck::Br(r) => {
                        brs.push((pc, r));
                        for &t in self.jt.get(&pc).map(|v| v.as_slice()).unwrap_or(&[]) {
                            visit(t, &mut todo);
                        }
                        break;
                    }
                    Ck::Ret | Ck::Trap => break,
                    _ => {}
                }
                pc += 4;
            }
        }
        Walked { body: Body { order }, brs }
    }

    // ---- PLT stubs

    fn plt_target(&self, e: u64) -> Option<(u64, u64)> {
        let img = self.img;
        let w: Vec<u32> = (0..4).map(|i| img.try_word(e + 4 * i)).collect::<Option<_>>()?;
        if w[0] & 0x9F00001F != 0x90000010 || w[1] & 0xFFC003FF != 0xF9400211 || w[2] & 0xFFC003FF != 0x91000210 || w[3] != 0xD61F0220 {
            return None;
        }
        let slot = adrp_page(w[0], e) + ((w[1] >> 10) & 0xFFF) as u64 * 8;
        let t = u64::from_le_bytes(img.read(slot, 8).expect("PLT slot outside the image").try_into().unwrap());
        self.entries.contains(&t).then_some((t, slot))
    }

    // ---- jump tables on their index

    fn index_tables(&mut self) {
        let mut brs: Vec<u64> = self.jt.keys().copied().collect();
        brs.sort_unstable();
        let mut r = self.resolver();
        let mut out = Map::default();
        for br in brs {
            let c = self.jt_cand.get(&br).cloned().unwrap_or(Cand::Why("unusual shape"));
            if let Cand::Shape { .. } = c {
                if let Some(v) = r.index_table(br, &c) {
                    out.insert(br, v);
                }
            }
        }
        self.jt_index = out;
    }
}

/// the unwind-described function a pc is in: [the start at or before it, the next start)
fn extent(eh_starts: &[u64], pc: u64) -> (u64, u64) {
    let i = eh_starts.partition_point(|&s| s <= pc);
    let lo = if i > 0 { eh_starts[i - 1] } else { 0 };
    let hi = if i < eh_starts.len() { eh_starts[i] } else { u64::MAX };
    (lo, hi)
}

/// A BR's table resolved against the round's fixed table map: the predecessor maps of the function extents (built
/// from that map, cached for the resolver's life), the constants reaching the table's address and index.
struct Resolver<'r> {
    img: &'r Image,
    eh_starts: &'r [u64],
    entries: &'r HashSet<u64>,
    jt: &'r Map<u64, Vec<u64>>,
    preds: Map<u64, Map<u64, Vec<u64>>>,
    /// jump_table's redirection of the resolvers to the table load: (br, table register, load pc)
    redirect: Option<(u64, u32, u64)>,
    /// the shape of the table the last jump_table read, or why it has none
    cand: Cand,
}

impl<'r> Resolver<'r> {
    fn function_extent(&self, pc: u64) -> (u64, u64) {
        extent(self.eh_starts, pc)
    }
    /// hi - lo > 0x40000 (Python's hi is 1 << 64 past the last start)
    fn too_big(lo: u64, hi: u64) -> bool {
        hi == u64::MAX || hi - lo > 0x40000
    }

    fn jump_table(&mut self, br_pc: u64, reg: u32) -> Option<Vec<u64>> {
        let mut window = 32u64;
        for k in 1..=window {
            let pc = br_pc.wrapping_sub(4 * k);
            if self.img.in_code(pc) && auth_end(self.img.word(pc)) {
                window = k - 1;
                break;
            }
        }
        match self.table_load(br_pc, window) {
            None => self.jump_table_window(br_pc, reg, window),
            Some((load_pc, treg)) => {
                self.redirect = Some((br_pc, treg, load_pc));
                let out = self.jump_table_window(br_pc, reg, window);
                self.redirect = None;
                out
            }
        }
    }
    fn redirected(&self, at: u64, r: u32) -> u64 {
        match self.redirect {
            Some((br, treg, load)) if (at, r) == (br, treg) => load,
            _ => at,
        }
    }

    fn table_load(&self, br_pc: u64, window: u64) -> Option<(u64, u32)> {
        let img = self.img;
        let mut pc = br_pc.wrapping_sub(4);
        let stop = br_pc.wrapping_sub(4 * window);
        while (pc as i128) > (stop as i128) {
            if !img.in_code(pc) {
                return None;
            }
            let w = img.word(pc);
            if w & 0xFC000000 == 0x14000000 || matches!(w & 0xFFFFFC1F, 0xD65F0000 | 0xD61F0000) {
                return None;
            }
            if w & 0x3B200C00 == 0x38200800 && (w >> 26) & 1 == 0 && (w >> 22) & 3 != 0 {
                let n = (w >> 5) & 31;
                let mut q = pc + 4;
                while q < br_pc {
                    if writes(img.word(q), n) {
                        return Some((pc, n));
                    }
                    q += 4;
                }
                return None;
            }
            pc = pc.wrapping_sub(4);
        }
        None
    }

    fn jump_table_window(&mut self, br_pc: u64, reg: u32, window: u64) -> Option<Vec<u64>> {
        let img = self.img;
        self.cand = Cand::Why("unusual shape");
        let mut r: Map<u32, Option<V>> = Map::default();
        // (bound: the last CMP / CCMP's immediate, and the count a B.HI / B.HS after it gives)
        let mut bound: Option<(Option<u64>, Option<u64>)> = None;
        let mut bounds: Map<u32, u64> = Map::default();
        let mut cmp: Option<(u32, u64)> = None;
        let mut pc = br_pc.wrapping_sub(4 * window);
        while pc < br_pc {
            let p = pc;
            pc += 4;
            if !img.in_code(p) {
                r.clear();
                bounds.clear();
                cmp = None;
                continue;
            }
            let w = img.word(p);
            if w & 0xFC000000 == 0x14000000 || matches!(w & 0xFFFFFC1F, 0xD65F0000 | 0xD61F0000) {
                r.clear();
                bounds.clear();
                cmp = None;
                continue;
            }
            let d = w & 31;
            let n = (w >> 5) & 31;
            let m = (w >> 16) & 31;
            let is_cmp = w & 0x7F80001F == 0x7100001F;
            let is_ccmp = w & 0x7FE00C10 == 0x7A400800;
            let is_bhi = matches!(w & 0xFF00001F, 0x54000008 | 0x54000002);
            if is_cmp || is_ccmp {
                cmp = Some((n, if is_cmp { ((w >> 10) & 0xFFF) as u64 } else { ((w >> 16) & 31) as u64 }));
            } else if is_bhi && cmp.is_some() {
                let (rr, imm) = cmp.unwrap();
                bounds.insert(rr, if w & 15 == 8 { imm + 1 } else { imm });
            } else if w & 0x7F800000 == 0x12000000 && (w >> 16) & 63 == 0 {
                let (imms, nbit) = ((w >> 10) & 63, (w >> 22) & 1);
                if (nbit != 0 || w >> 31 == 0) && imms < 31 {
                    bounds.insert(d, 1u64 << (imms + 1));
                } else {
                    bounds.remove(&d);
                }
            } else if w & 0x7FE0FFE0 == 0x2A0003E0 {
                match bounds.get(&m).copied() {
                    Some(b) => {
                        bounds.insert(d, b);
                    }
                    None => {
                        bounds.remove(&d);
                    }
                }
            } else if d != 31 && writes(w, d) {
                bounds.remove(&d);
            }
            let get_or_reg = |r: &Map<u32, Option<V>>, x: u32| -> Option<V> {
                match r.get(&x) {
                    None => Some(V::Reg(x)),
                    Some(v) => v.clone(),
                }
            };
            let get = |r: &Map<u32, Option<V>>, x: u32| -> Option<V> { r.get(&x).cloned().flatten() };
            if w & 0x9F000000 == 0x90000000 {
                r.insert(d, Some(V::C(adrp_page(w, p))));
            } else if w & 0x9F000000 == 0x10000000 {
                r.insert(d, Some(V::C(adr_target(w, p))));
            } else if w & 0xFF800000 == 0x91000000 {
                let v = get_or_reg(&r, n);
                let imm = add_imm(w);
                match v {
                    Some(V::C(c)) => {
                        r.insert(d, Some(V::C(c.wrapping_add(imm))));
                    }
                    Some(V::Reg(_)) | Some(V::RegOff(..)) if d == n => {
                        r.remove(&d);
                    }
                    Some(V::Reg(_)) => {
                        r.insert(d, Some(V::RegOff(n, imm)));
                    }
                    _ => {
                        r.insert(d, None);
                    }
                }
            } else if w & 0x3B200C00 == 0x38200800 && (w >> 26) & 1 == 0 {
                let (size, opc, s) = (w >> 30, (w >> 22) & 3, (w >> 12) & 1);
                let v = get_or_reg(&r, n);
                let val = match v {
                    Some(v) if opc != 0 => Some(V::Ld(Box::new(Ld {
                        v,
                        m,
                        size: 1 << size,
                        signed: opc >= 2,
                        pc: p,
                        opc,
                        opt: (w >> 13) & 7,
                        n,
                        s,
                    }))),
                    _ => None,
                };
                r.insert(d, val);
            } else if w & 0xFF200000 == 0x8B000000 && (w >> 22) & 3 == 0 {
                let a = Add { a: get(&r, n), b: get(&r, m), k: (w >> 10) & 63, ext: None, pc: p, n };
                r.insert(d, Some(V::Add(Box::new(a))));
            } else if w & 0xFFE00000 == 0x8B200000 {
                let a = Add { a: get(&r, n), b: get(&r, m), k: (w >> 10) & 7, ext: Some((w >> 13) & 7), pc: p, n };
                r.insert(d, Some(V::Add(Box::new(a))));
            } else if is_cmp {
                bound = Some((Some(((w >> 10) & 0xFFF) as u64), None));
            } else if is_ccmp {
                bound = Some((Some(((w >> 16) & 31) as u64), None));
            } else if is_bhi && bound.is_some_and(|b| b.1.is_none()) {
                let imm = bound.unwrap().0.unwrap();
                bound = Some((Some(imm), Some(if w & 15 == 8 { imm + 1 } else { imm })));
            } else if writes(w, d) {
                r.insert(d, None);
            }
            let rt2 = (w >> 10) & 31;
            if (w >> 27) & 7 == 5 && (w >> 22) & 1 != 0 && (w >> 26) & 1 == 0 {
                r.insert(rt2, None);
                bounds.remove(&rt2);
            }
        }
        let v = r.get(&reg).cloned().flatten();
        if let Some(V::C(c)) = v {
            // (br to a code constant: `adr x19, label ... br x19`, a continuation in a register)
            let (lo, hi) = self.function_extent(br_pc);
            if lo <= c && c < hi && img.in_code(c) {
                self.cand = Cand::Why("no table (a constant)");
                return Some(vec![c]);
            }
            return None;
        }
        let add = match v {
            Some(V::Add(a)) => a,
            _ => {
                self.cand = Cand::Why("no table (ADR candidates)");
                return self.adr_targets(br_pc, reg);
            }
        };
        let (mut a, mut b, k) = (add.a.clone(), add.b.clone(), add.k);
        let mut ld_shifted = true;
        let mut swapped = false;
        if matches!(a, Some(V::Ld(_))) {
            std::mem::swap(&mut a, &mut b);
            ld_shifted = false;
            swapped = true;
        }
        let (base, ld) = match (&a, &b) {
            (Some(V::C(c)), Some(V::Ld(l))) => (*c, l.clone()),
            _ => {
                return None;
            }
        };
        let (table, idx, size, signed) = (&ld.v, ld.m, ld.size, ld.signed);
        if let Some(&c) = bounds.get(&idx) {
            bound = Some((None, Some(c)));
        }
        let tables: Vec<u64> = match table {
            V::Reg(_) | V::RegOff(..) => {
                let (reg_t, off) = match table {
                    V::Reg(x) => (*x, 0),
                    V::RegOff(x, o) => (*x, *o),
                    _ => unreachable!(),
                };
                let mut ts: Vec<u64> = self.reaching_constants(br_pc, reg_t);
                let mut guess = self.hoisted_constant(br_pc, reg_t, 1024);
                if guess.is_none() {
                    guess = self.function_constant(br_pc, reg_t);
                }
                ts.extend(guess);
                let mut ts: Vec<u64> = ts.into_iter().map(|t| t.wrapping_add(off)).collect();
                ts.sort_unstable();
                ts.dedup();
                if ts.is_empty() {
                    return None;
                }
                if ts.len() > 1 {
                    self.cand = Cand::Why("several tables");
                }
                ts
            }
            V::C(c) => vec![*c],
            other => panic!("jump table at {br_pc:#x}: a table address of shape {other:?} (Python fails here too)"),
        };
        let (lo, hi) = self.function_extent(br_pc);
        let count = bound.and_then(|b| b.1).filter(|&c| c > 0 && c <= 4096);
        let read = |table: u64, limit: u64, strict: bool| -> Option<Vec<u64>> {
            let mut out = Vec::new();
            for i in 0..limit {
                let bytes = match img.read(table.wrapping_add(i * size), size) {
                    Some(b) => b,
                    None => break,
                };
                let mut raw = [0u8; 8];
                raw[..size as usize].copy_from_slice(bytes);
                let u = u64::from_le_bytes(raw);
                let e: i128 = if signed && (u >> (8 * size - 1)) & 1 != 0 { u as i128 - (1i128 << (8 * size)) } else { u as i128 };
                let t = ((base as i128 + (e << k)) & 0xFFFF_FFFF_FFFF_FFFF) as u64;
                if !(lo <= t && t < hi && img.in_code(t)) {
                    if strict {
                        return None;
                    }
                    break;
                }
                out.push(t);
            }
            Some(out)
        };
        let mut targets: Vec<u64> = Vec::new();
        for &t in &tables {
            let got = match count {
                Some(c) => read(t, c, true),
                None => None,
            };
            let got = match got {
                Some(g) => g,
                None => read(t, if size == 1 { 256 } else { 1024 }, false).unwrap(),
            };
            let before = targets.len();
            for x in got {
                if !targets[..before].contains(&x) {
                    targets.push(x);
                }
            }
        }
        if targets.is_empty() {
            return None;
        }
        if tables.len() == 1 && matches!(self.cand, Cand::Why("unusual shape")) {
            if !ld_shifted {
                self.cand = Cand::Why("entry not the ADD's shifted operand");
            } else {
                self.cand = Cand::Shape {
                    load: ld.pc,
                    idx,
                    opt: ld.opt,
                    s: ld.s,
                    size,
                    opc: ld.opc,
                    treg: ld.n,
                    table: tables[0],
                    add: add.pc,
                    areg: if !swapped { Some(add.n) } else { None },
                    k,
                    ext: add.ext,
                    count,
                };
            }
        }
        Some(targets)
    }

    fn preds(&mut self, lo: u64, hi: u64) -> &Map<u64, Vec<u64>> {
        let img = self.img;
        let jt = self.jt;
        self.preds.entry(lo).or_insert_with(|| {
            let mut p: Map<u64, Vec<u64>> = Map::default();
            let mut pc = lo;
            while pc < hi {
                if img.in_code(pc) {
                    let succ: Vec<u64> = match classify(img.word(pc), pc) {
                        Ck::B(t) => vec![t],
                        Ck::Bcond(t) | Ck::Cbz(t) | Ck::Tbz(t) => vec![t, pc + 4],
                        Ck::Br(_) => jt.get(&pc).cloned().unwrap_or_default(),
                        Ck::Ret | Ck::Trap => vec![],
                        _ => vec![pc + 4],
                    };
                    for s in succ {
                        p.entry(s).or_default().push(pc);
                    }
                }
                pc += 4;
            }
            p
        })
    }

    fn reaching_constants(&mut self, at: u64, reg: u32) -> Vec<u64> {
        let at = self.redirected(at, reg);
        let mut v: Vec<u64> = self.reaching(at, Loc::Reg(reg), 50000, 0).into_iter().collect();
        v.sort_unstable();
        v
    }

    fn reaching(&mut self, at: u64, loc: Loc, limit: usize, depth: u32) -> HashSet<u64> {
        let img = self.img;
        let (lo, hi) = self.function_extent(at);
        if Self::too_big(lo, hi) || depth > 4 {
            return HashSet::new();
        }
        self.preds(lo, hi);
        let mut found = HashSet::new();
        let mut seen: HashSet<u64> = HashSet::new();
        let mut todo = vec![at];
        while !todo.is_empty() && seen.len() < limit {
            let q = todo.pop().unwrap();
            let ps: Vec<u64> = self.preds[&lo].get(&q).cloned().unwrap_or_default();
            for p in ps {
                if !seen.insert(p) {
                    continue;
                }
                let w = img.word(p);
                match loc {
                    Loc::Reg(x) => {
                        if matches!(classify(w, p), Ck::Bl(_) | Ck::Blr(_)) && (x <= 18 || x == 30) {
                            continue;
                        }
                        if !writes(w, x) {
                            todo.push(p);
                            continue;
                        }
                        if w & 0x9F000000 == 0x90000000 {
                            found.insert(adrp_page(w, p));
                        } else if w & 0xFF800000 == 0x91000000 {
                            if let Some(v) = self.hoisted_constant(p, (w >> 5) & 31, 64) {
                                found.insert(v.wrapping_add(add_imm(w)));
                            }
                        } else if w & 0xFFE0FFE0 == 0xAA0003E0 {
                            found.extend(self.reaching(p, Loc::Reg((w >> 16) & 31), limit, depth + 1));
                        } else if w & 0xFFC003E0 == 0xF94003E0 {
                            found.extend(self.reaching(p, Loc::Slot((((w >> 10) & 0xFFF) * 8) as i64), limit, depth + 1));
                        } else if w & 0xFFC003E0 == 0xA94003E0 {
                            let off = sx(((w >> 15) & 0x7F) as u64, 7) * 8 + if w & 31 != x { 8 } else { 0 };
                            found.extend(self.reaching(p, Loc::Slot(off), limit, depth + 1));
                        }
                        continue;
                    }
                    Loc::Slot(x) => {
                        if w & 0xFFC003E0 == 0xF90003E0 && (((w >> 10) & 0xFFF) * 8) as i64 == x {
                            found.extend(self.reaching(p, Loc::Reg(w & 31), limit, depth + 1));
                            continue;
                        }
                        if w & 0xFFC003E0 == 0xA90003E0 {
                            let off = sx(((w >> 15) & 0x7F) as u64, 7) * 8;
                            if x == off || x == off + 8 {
                                let r = if x == off { w & 31 } else { (w >> 10) & 31 };
                                found.extend(self.reaching(p, Loc::Reg(r), limit, depth + 1));
                                continue;
                            }
                        }
                        if (w & 31 == 31 && matches!(w & 0x7F800000, 0x51000000 | 0x11000000))
                            || ((w >> 5) & 31 == 31 && (matches!(w & 0x3B200C00, 0x38000C00 | 0x38000400) || w & 0x3A800000 == 0x28800000))
                        {
                            continue;
                        }
                        todo.push(p);
                    }
                }
            }
        }
        found
    }

    fn function_constant(&mut self, br_pc: u64, reg: u32) -> Option<u64> {
        let br_pc = self.redirected(br_pc, reg);
        if let Some(v) = self.function_constant_adrp(br_pc, reg) {
            return Some(v);
        }
        let img = self.img;
        let (lo, hi) = self.function_extent(br_pc);
        if Self::too_big(lo, hi) {
            return None;
        }
        let mut page: Map<u32, u64> = Map::default();
        let mut found: HashSet<u64> = HashSet::new();
        let mut pc = lo;
        while pc < hi {
            if img.in_code(pc) {
                let w = img.word(pc);
                let d = w & 31;
                if w & 0x9F000000 == 0x90000000 {
                    page.insert(d, adrp_page(w, pc));
                } else if w & 0xFF800000 == 0x91000000 && page.contains_key(&((w >> 5) & 31)) {
                    if d == reg {
                        found.insert(page[&((w >> 5) & 31)].wrapping_add(add_imm(w)));
                    }
                } else if w & 0x9F000000 == 0x10000000 && d == reg {
                    found.insert(adr_target(w, pc));
                }
            }
            pc += 4;
        }
        if found.len() == 1 {
            return found.into_iter().next();
        }
        None
    }

    fn function_constant_adrp(&self, br_pc: u64, reg: u32) -> Option<u64> {
        let img = self.img;
        let (lo, hi) = self.function_extent(br_pc);
        if Self::too_big(lo, hi) {
            return None;
        }
        let mut page: Map<u32, u64> = Map::default();
        let mut found: HashSet<u64> = HashSet::new();
        let mut pc = lo;
        while pc < hi {
            let w = img.word(pc);
            let d = w & 31;
            if w & 0x9F000000 == 0x90000000 {
                page.insert(d, adrp_page(w, pc));
            } else if w & 0xFF800000 == 0x91000000 && page.contains_key(&((w >> 5) & 31)) {
                let v = page[&((w >> 5) & 31)].wrapping_add(add_imm(w));
                if d == reg {
                    found.insert(v);
                }
            }
            pc += 4;
        }
        if found.len() == 1 {
            found.into_iter().next()
        } else {
            None
        }
    }

    fn adr_targets(&self, br_pc: u64, reg: u32) -> Option<Vec<u64>> {
        let img = self.img;
        let (lo, hi) = self.function_extent(br_pc);
        if Self::too_big(lo, hi) {
            return None;
        }
        let mut targets = Vec::new();
        let mut pc = lo;
        while pc < hi {
            let w = img.word(pc);
            if w & 0x9F00001F == 0x10000000 | reg {
                let t = adr_target(w, pc);
                if lo <= t && t < hi && img.in_code(t) {
                    targets.push(t);
                }
            }
            pc += 4;
        }
        if targets.is_empty() {
            None
        } else {
            Some(targets)
        }
    }

    fn hoisted_constant(&self, before: u64, reg: u32, scan: u32) -> Option<u64> {
        let before = self.redirected(before, reg);
        let img = self.img;
        let mut pc = before.wrapping_sub(4);
        for _ in 0..scan {
            if !img.in_code(pc) {
                return None;
            }
            let w = img.word(pc);
            if w & 31 == reg && !reads_rt_only(w) {
                if w & 0x9F000000 == 0x90000000 {
                    return Some(adrp_page(w, pc));
                }
                if w & 0xFF800000 == 0x91000000 {
                    return self.hoisted_constant(pc, (w >> 5) & 31, 64).map(|v| v.wrapping_add(add_imm(w)));
                }
                return None;
            }
            pc = pc.wrapping_sub(4);
        }
        None
    }

    fn index_table(&mut self, br: u64, c: &Cand) -> Option<(u64, u32, u32, Vec<u64>)> {
        let Cand::Shape { load, idx, opt, s, size, opc, treg, table, add, areg, k, ext, count } = *c else {
            return None;
        };
        let img = self.img;
        if !matches!(opt, 2 | 3 | 6 | 7) || idx == 31 {
            return None;
        }
        if size == 8 && opc != 1 {
            return None;
        }
        if size > 1 && s == 0 {
            return None;
        }
        let (lo, hi) = self.function_extent(br);
        self.preds(lo, hi);
        let mut q = load;
        while q < br {
            if !matches!(classify(img.word(q), q), Ck::Op | Ck::Isb | Ck::Ic) {
                return None;
            }
            let ok = self.preds[&lo].get(&(q + 4)).is_some_and(|p| p.as_slice() == [q]);
            if self.entries.contains(&(q + 4)) || !ok {
                return None;
            }
            q += 4;
        }
        let t = self.exact_constant(load, treg, 0)?;
        if t != table {
            return None;
        }
        let areg = areg?;
        let base = self.exact_constant(add, areg, 0)?;
        if k > 4 && ext.is_some() {
            return None;
        }
        let known: HashSet<u64> = self.jt[&br].iter().copied().collect();
        let mut out = Vec::new();
        let n = count.unwrap_or(if size == 1 { 256 } else { 1024 });
        for i in 0..n {
            let a = t.wrapping_add(i * size);
            if !img.read_only(a, size) {
                break;
            }
            let bytes = img.read(a, size).unwrap();
            let mut raw = [0u8; 8];
            raw[..size as usize].copy_from_slice(bytes);
            let mut e = u64::from_le_bytes(raw) as i128;
            let bits = 8 * size as u32;
            if opc >= 2 && (e >> (bits - 1)) & 1 != 0 {
                e -= 1i128 << bits;
            }
            e &= (1i128 << if opc == 3 { 32 } else { 64 }) - 1;
            if let Some(x) = ext {
                let w = [8u32, 16, 32, 64][(x & 3) as usize];
                e &= (1i128 << w) - 1;
                if x & 4 != 0 && (e >> (w - 1)) & 1 != 0 {
                    e -= 1i128 << w;
                }
            }
            let tg = ((base as i128 + (e << k)) & 0xFFFF_FFFF_FFFF_FFFF) as u64;
            if !known.contains(&tg) {
                break;
            }
            out.push(tg);
        }
        if out.is_empty() {
            return None;
        }
        Some((load, idx, opt, out))
    }

    fn exact_constant(&mut self, at: u64, reg: u32, depth: u32) -> Option<u64> {
        let img = self.img;
        let (lo, hi) = self.function_extent(at);
        if Self::too_big(lo, hi) || depth > 6 || reg == 31 {
            return None;
        }
        self.preds(lo, hi);
        let mut found: HashSet<u64> = HashSet::new();
        let mut seen: HashSet<u64> = HashSet::from([at]);
        let mut todo = vec![at];
        while let Some(q) = todo.pop() {
            let ps: Vec<u64> = self.preds[&lo].get(&q).cloned().unwrap_or_default();
            if self.entries.contains(&q) || ps.is_empty() || seen.len() > 50000 {
                return None;
            }
            for p in ps {
                if !seen.insert(p) {
                    continue;
                }
                let w = img.word(p);
                if matches!(classify(w, p), Ck::Bl(_) | Ck::Blr(_) | Ck::Svc) && (reg <= 18 || reg == 30) {
                    return None;
                }
                if !writes(w, reg) {
                    todo.push(p);
                    continue;
                }
                if w & 0x9F000000 == 0x90000000 {
                    found.insert(adrp_page(w, p));
                } else if w & 0x9F000000 == 0x10000000 {
                    found.insert(adr_target(w, p));
                } else if w & 0xFF800000 == 0x91000000 {
                    let v = self.exact_constant(p, (w >> 5) & 31, depth + 1)?;
                    found.insert(v.wrapping_add(add_imm(w)));
                } else if w & 0xFFE0FFE0 == 0xAA0003E0 {
                    let v = self.exact_constant(p, (w >> 16) & 31, depth + 1)?;
                    found.insert(v);
                } else {
                    return None;
                }
                if found.len() > 1 {
                    return None;
                }
            }
        }
        if found.len() == 1 {
            found.into_iter().next()
        } else {
            None
        }
    }
}

#[derive(Clone, Copy)]
enum Loc {
    Reg(u32),
    Slot(i64),
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn classify_words() {
        assert_eq!(classify(0xD65F03C0, 0x1000), Ck::Ret);
        assert_eq!(classify(0x94000002, 0x1000), Ck::Bl(0x1008));
        assert_eq!(classify(0x17FFFFFF, 0x1000), Ck::B(0xFFC));
        assert_eq!(classify(0xD61F0200, 0), Ck::Br(16));
    }

    #[test]
    fn writes_rules() {
        // add x0, x1, #1 writes x0; str x0, [x1] does not
        assert!(writes(0x91000420, 0));
        assert!(!writes(0xF9000020, 0));
        // ldp x2, x3, [sp] writes x3 (Rt2)
        assert!(writes(0xA9400FE2, 3));
        // umov w3, v1.s[1]; smov x3, v1.h[0]; mov x3, v1.d[0] (UMOV); not ins v3.s[1], w1
        assert!(writes(0x0E0C3C23, 3));
        assert!(writes(0x4E022C23, 3));
        assert!(writes(0x4E083C23, 3));
        assert!(!writes(0x4E0C1C23, 3));
    }
}
