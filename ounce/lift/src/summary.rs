//! Whole-program summaries of the guest functions (nx2/lift2/summary.py, summarise_parallel): for every function,
//! over the call graph's strongly connected components callees first, the registers it reads (inputs), may write
//! (clobbers), writes on every path to a return (mustdef), preserves, returns to callers (results), whether it
//! returns, its frame, and the reasons it is demoted to the generic entry. The rules and their reasons are summary.py's
//! docstring; this is the same computation on dense data.
//!
//! Data: a function is its index in the sorted lifted entries (`FnId`); registers are u128 masks over `ir::Var`
//! (the Python's `ir.VARS`, bit for bit). A function's control flow (`Flow`, summary.Func) is built again in each
//! phase into a worker's reused buffers rather than kept for 17 M instructions. Each instruction's register use and
//! definition comes from the semantic templates themselves, run on a scratch builder (summary.def_use), cached per
//! worker by word. The fixed point runs level by level over the components' DAG, a level's components in parallel.

use crate::export::{GuestCalls, Meta, Part};
use crate::ir::{bit, BKey, Builder, Function, ALL};
use crate::sem::{self, Kind};
use ounce_decode::{decode, Fld, Insn, Param};
use rayon::prelude::*;
use std::borrow::Cow;
use std::cell::RefCell;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::OnceLock;

pub type FnId = u32;

pub const fn xm(n: u32) -> u128 {
    1u128 << n
}
const fn range(lo: u32, hi: u32) -> u128 {
    // bits lo..hi (hi exclusive)
    ((1u128 << hi) - 1) & !((1u128 << lo) - 1)
}
pub const SPM: u128 = 1u128 << 31;
pub const NZCV: u128 = range(32, 36);
const fn vm(lo: u32, hi: u32) -> u128 {
    range(36 + lo, 36 + hi)
}
pub const X30: u128 = xm(30);
pub const X0: u128 = xm(0);
pub const CALLEE_SAVED: u128 = range(19, 30) | SPM | vm(8, 16);
/// the registers whose entry values the frame pass tracks: x19-x30, v8-v15
pub const TRACKED: u128 = range(19, 31) | vm(8, 16);
pub const ARGS: u128 = range(0, 9) | SPM | vm(0, 8);
pub const CALLER_SAVED: u128 = range(0, 19) | X30 | NZCV | vm(0, 8) | vm(16, 32);
pub const TMPS: u128 = range(9, 19);
pub const ABI_RESULTS: u128 = xm(0) | xm(1) | vm(0, 4);
/// what guest_orig may write before the body runs
pub const ORIG_ARGS: u128 = range(0, 9);

/// the demotion reasons, in the order Python sorts their names (a set's bits ascending are its sorted list)
pub const REASONS: [&str; 13] = [
    "cs_clobber",
    "dangling_bl",
    "fallthrough_entry",
    "flags_in",
    "inconsistent",
    "lr_data",
    "lr_ret",
    "save_ptr",
    "sp",
    "split_frame",
    "tmp_after_call",
    "tmp_in",
    "unknown callee",
];
pub mod why {
    pub const CS_CLOBBER: u16 = 1 << 0;
    pub const DANGLING_BL: u16 = 1 << 1;
    pub const FALLTHROUGH_ENTRY: u16 = 1 << 2;
    pub const FLAGS_IN: u16 = 1 << 3;
    pub const INCONSISTENT: u16 = 1 << 4;
    pub const LR_DATA: u16 = 1 << 5;
    pub const LR_RET: u16 = 1 << 6;
    pub const SAVE_PTR: u16 = 1 << 7;
    pub const SP: u16 = 1 << 8;
    pub const SPLIT_FRAME: u16 = 1 << 9;
    pub const TMP_AFTER_CALL: u16 = 1 << 10;
    pub const TMP_IN: u16 = 1 << 11;
    pub const UNKNOWN_CALLEE: u16 = 1 << 12;
    /// the reasons that make a function's facts unknown (summary.UNKNOWN_FACTS)
    pub const UNKNOWN_FACTS: u16 = SP | SAVE_PTR | LR_DATA | LR_RET | SPLIT_FRAME;
}

/// a function's summary (summary.Summary)
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Summary {
    pub inputs: u128,
    pub clobbers: u128,
    pub mustdef: u128,
    pub preserved: u128,
    pub results: u128,
    pub frame: i64,
    pub pairs: u32,
    /// demotion reasons (why::*)
    pub demote: u16,
    pub returns: bool,
    pub unknown: bool,
    pub frame_taken: bool,
    pub boundary: bool,
    pub fp: bool,
    pub writes_fpcr: bool,
    pub lr_data: bool,
}

impl Summary {
    /// the optimistic start of the fixed point: nothing read or written, everything preserved
    pub const START: Summary = Summary {
        inputs: 0,
        clobbers: 0,
        mustdef: ALL,
        preserved: CALLEE_SAVED,
        results: 0,
        frame: 0,
        pairs: 0,
        demote: 0,
        returns: false,
        unknown: false,
        frame_taken: false,
        boundary: false,
        fp: false,
        writes_fpcr: false,
        lr_data: false,
    };
    /// what changes the fixed point looks at (summary.Summary.key, and the demotion reasons)
    fn key(&self) -> (u128, u128, u128, u128, bool, bool, bool, bool, u16) {
        (
            self.inputs,
            self.clobbers,
            self.mustdef,
            self.preserved,
            self.returns,
            self.unknown,
            self.boundary,
            self.writes_fpcr,
            self.demote,
        )
    }
}

const UNKNOWN: Summary = Summary {
    inputs: ALL,
    clobbers: ALL,
    mustdef: 0,
    preserved: 0,
    returns: true,
    unknown: true,
    boundary: true,
    ..Summary::START
};
/// a callee the program cannot see: BLR, unresolved BR, an address that is no function
const AAPCS: Summary = Summary {
    inputs: ARGS,
    clobbers: CALLER_SAVED,
    mustdef: X30,
    preserved: CALLEE_SAVED,
    returns: true,
    boundary: true,
    ..Summary::START
};
/// a seeded fatal function: AAPCS facts, never returns
const NORETURN_HOST: Summary = Summary {
    inputs: ARGS,
    clobbers: CALLER_SAVED,
    mustdef: ALL,
    returns: false,
    boundary: true,
    ..Summary::START
};

/// the summary of an HLE'd function (summary.host_summary)
fn host_summary(stops: bool, answer: Option<u64>, clobbering: bool, reads: u128) -> Summary {
    if stops {
        return NORETURN_HOST;
    }
    if answer.is_some() {
        return Summary { clobbers: X0 | X30, mustdef: X0 | X30, results: X0, returns: true, boundary: true, ..Summary::START };
    }
    Summary {
        inputs: if clobbering { ALL } else { reads },
        clobbers: CALLER_SAVED,
        mustdef: X30,
        preserved: CALLEE_SAVED,
        returns: true,
        boundary: true,
        ..Summary::START
    }
}

// ---- the program's facts

/// what a call or branch reaches, resolved once (summary.callee_summary's cases, in its order)
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Cls {
    /// BLR, an unresolved BR (None)
    Indirect,
    /// a lifted function (id: its FnId)
    Fn,
    /// a lifted function replaced by a PATCH_FUNC (id: its FnId, aux: its patch)
    Patch,
    /// an HLE'd function (id: its hook)
    Hook,
    /// an address that is none of these
    Other,
}

#[derive(Clone, Copy, Debug)]
pub struct Tgt {
    pub addr: u64,
    pub cls: Cls,
    pub id: u32,
    pub aux: u32,
    /// a noreturn seed (by name, or by its callers' evidence): set from the second phase on
    pub noret: bool,
}

impl Tgt {
    const INDIRECT: Tgt = Tgt { addr: 0, cls: Cls::Indirect, id: 0, aux: 0, noret: false };
    /// the FnId of a lifted function (with or without a PATCH_FUNC)
    pub fn func(&self) -> Option<FnId> {
        matches!(self.cls, Cls::Fn | Cls::Patch).then_some(self.id)
    }
    /// host code: an HLE'd function or a PATCH_FUNC (summary.is_host)
    fn is_host(&self) -> bool {
        matches!(self.cls, Cls::Hook | Cls::Patch)
    }
}

pub struct Patch {
    pub entry: u64,
    pub reads: u128,
    pub result: u128,
    pub orig: bool,
    pub guest: Vec<u64>,
}

pub struct HookFacts {
    pub addr: u64,
    pub stops: bool,
    pub answer: Option<u64>,
    pub clobbering: bool,
    pub reads: u128,
    pub summary: Summary,
}

/// a pc's rows: their declared reads and writes beside the instruction (summary.rows)
pub struct Rows {
    pub pc: u64,
    pub bu: u128,
    pub au: u128,
    pub bd: u128,
    pub ad: u128,
    /// None: no guest calls; Some(None): "computed"; Some(Some(list))
    pub calls: Option<Option<Vec<u64>>>,
    pub repl: bool,
}

pub struct Body<'a> {
    pub pcs: Cow<'a, [u64]>,
    pub words: Cow<'a, [u32]>,
    /// the jump tables of the body's BRs, by br
    pub jt: &'a [(u64, Vec<u64>)],
}

/// the whole program as the summaries read it (summary.job's input)
pub struct Program<'a> {
    /// the lifted functions' entries, sorted: a FnId is an index here
    pub fns: Vec<u64>,
    pub bodies: Vec<Body<'a>>,
    /// every entry (lifted and hooked), sorted
    pub entries: &'a [u64],
    /// stub -> target
    pub plt: Vec<(u64, u64)>,
    pub hooks: Vec<HookFacts>,
    pub patches: Vec<Patch>,
    /// by pc
    pub rows: Vec<Rows>,
    /// the noreturn seeds, sorted (by name, then with the callers' evidence)
    pub noret: Vec<u64>,
}

impl<'a> Program<'a> {
    pub fn new(meta: &'a Meta, parts: &'a [Part]) -> Program<'a> {
        let mut fs: Vec<(u64, Body<'a>)> = parts
            .iter()
            .flat_map(|p| p.functions.iter())
            .map(|f| (f.entry, Body { pcs: Cow::Borrowed(&f.pcs[..]), words: Cow::Borrowed(&f.words[..]), jt: &f.jt[..] }))
            .collect();
        fs.sort_unstable_by_key(|f| f.0);
        for sb in &meta.summary_bodies {
            // (a body the summaries see as discovered where the lift traps: EXPORT.md summary_bodies)
            let k = fs.binary_search_by_key(&sb.entry, |f| f.0).expect("summary body of no function");
            let (pcs, words) = sb.body();
            fs[k].1.pcs = Cow::Owned(pcs);
            fs[k].1.words = Cow::Owned(words);
        }
        let (fns, bodies): (Vec<u64>, Vec<Body>) = fs.into_iter().unzip();
        let hooks = meta
            .hle
            .iter()
            .map(|h| HookFacts {
                addr: h.0,
                stops: h.2,
                answer: h.3,
                clobbering: h.4,
                reads: h.5,
                summary: host_summary(h.2, h.3, h.4, h.5),
            })
            .collect();
        let patches = meta
            .patch_funcs
            .iter()
            .map(|p| Patch { entry: p.0, reads: p.1, result: p.2, orig: p.3, guest: p.4.clone() })
            .collect();
        let rows = meta
            .row_effects
            .iter()
            .map(|r| Rows {
                pc: r.0,
                bu: r.1,
                au: r.2,
                bd: r.3,
                ad: r.4,
                calls: match &r.5 {
                    GuestCalls::Computed(s) => {
                        assert_eq!(s, "computed");
                        Some(None)
                    }
                    GuestCalls::List(l) if l.is_empty() => None,
                    GuestCalls::List(l) => Some(Some(l.clone())),
                },
                repl: r.6,
            })
            .collect();
        let mut noret: Vec<u64> = meta
            .names
            .iter()
            .filter(|(_, n)| noreturn_seed(n, &meta.noreturn_names, &meta.noreturn_parts))
            .map(|(e, _)| *e)
            .collect();
        noret.sort_unstable();
        noret.dedup();
        Program {
            fns,
            bodies,
            entries: &meta.entries,
            plt: meta.plt.iter().map(|p| (p.0, p.1)).collect(),
            hooks,
            patches,
            rows,
            noret,
        }
    }

    /// the function a call of t reaches: PLT stubs followed, at most 4 steps
    pub fn callee(&self, mut t: u64) -> u64 {
        for _ in 0..4 {
            match self.plt.binary_search_by_key(&t, |p| p.0) {
                Ok(k) => t = self.plt[k].1,
                Err(_) => break,
            }
        }
        t
    }
    pub fn fn_id(&self, a: u64) -> Option<FnId> {
        self.fns.binary_search(&a).ok().map(|k| k as FnId)
    }
    pub fn hook(&self, a: u64) -> Option<usize> {
        self.hooks.binary_search_by_key(&a, |h| h.addr).ok()
    }
    pub fn patch(&self, a: u64) -> Option<usize> {
        self.patches.binary_search_by_key(&a, |p| p.entry).ok()
    }
    fn is_entry(&self, a: u64) -> bool {
        self.entries.binary_search(&a).is_ok()
    }
    /// the target an address is
    pub fn tgt(&self, a: u64) -> Tgt {
        let noret = self.noret.binary_search(&a).is_ok();
        if let Some(h) = self.hook(a) {
            return Tgt { addr: a, cls: Cls::Hook, id: h as u32, aux: 0, noret };
        }
        match self.fn_id(a) {
            Some(i) => match self.patch(a) {
                Some(p) => Tgt { addr: a, cls: Cls::Patch, id: i, aux: p as u32, noret },
                None => Tgt { addr: a, cls: Cls::Fn, id: i, aux: 0, noret },
            },
            None => Tgt { addr: a, cls: Cls::Other, id: 0, aux: 0, noret },
        }
    }
}

/// a function that does not return whatever its code says (summary.noreturn_seed)
fn noreturn_seed(name: &str, names: &[String], parts: &[String]) -> bool {
    if name.is_empty() {
        return false;
    }
    let base = name.split('@').next().unwrap_or("");
    names.iter().any(|n| n == base) || parts.iter().any(|p| base.contains(p.as_str()))
}

// ---- per-instruction register use and definition, from the semantic templates (summary.def_use)

/// a worker's def/use cache: direct-mapped by word, and the scratch function the templates run on
struct DefUse {
    f: Function,
    slots: Vec<(u32, bool, u128, u128)>,
}

const DU_BITS: u32 = 16;

impl DefUse {
    fn new() -> DefUse {
        DefUse { f: Function::new(0), slots: vec![(0, false, 0, 0); 1 << DU_BITS] }
    }
    /// (use, def) of a decoded instruction
    fn get(&mut self, i: &Insn) -> (u128, u128) {
        let w = i.word;
        let h = (w.wrapping_mul(0x9E37_79B1) >> (32 - DU_BITS)) as usize;
        let s = self.slots[h];
        if s.1 && s.0 == w {
            return (s.2, s.3);
        }
        let got = self.compute(i);
        self.slots[h] = (w, true, got.0, got.1);
        got
    }
    fn compute(&mut self, i: &Insn) -> (u128, u128) {
        let run = |f: &mut Function, go: &mut dyn FnMut(&mut Builder)| -> (u128, u128) {
            f.reset(0);
            let blk = f.block(BKey::Pc(0));
            {
                let mut b = Builder::new(f);
                b.at(blk);
                go(&mut b);
            }
            let u = f.b(blk).params.iter().fold(0u128, |m, &(v, _)| m | bit(v));
            (u, f.written)
        };
        match sem::kind(i) {
            Kind::B => (0, if i.f(Fld::op) != 0 { X30 } else { 0 }),
            Kind::Bcond => (run(&mut self.f, &mut |b| {
                sem::int::cond(b, i.f(Fld::cond));
            }).0, 0),
            Kind::Cbz | Kind::Tbz => (run(&mut self.f, &mut |b| {
                sem::int::getx(b, i.f(Fld::Rt), 1, false);
            }).0, 0),
            Kind::Br | Kind::Blr => {
                let rn = i.f(Fld::Rn);
                (if rn != 31 { xm(rn) } else { 0 }, if matches!(sem::kind(i), Kind::Blr) { X30 } else { 0 })
            }
            Kind::Exc => {
                if i.f(Fld::opc) == 0 {
                    (range(0, 8), range(0, 8))
                } else {
                    (0, 0)
                }
            }
            Kind::Ret | Kind::Udf | Kind::Missing => (0, 0),
            Kind::Sem(t, _) => run(&mut self.f, &mut |b| t(b, i, 0x1000)),
        }
    }
}

// ---- facts of each encoding the flow reads, by encoding id

const EF_FP: u8 = 1;
const EF_SYSREG: u8 = 2;

/// per encoding: its template's class is fp or neon (summary.Func's fp), it is a system register access
fn enc_facts() -> &'static [u8] {
    static T: std::sync::OnceLock<Vec<u8>> = std::sync::OnceLock::new();
    T.get_or_init(|| {
        ounce_decode::all_encodings()
            .iter()
            .map(|e| {
                let fp = sem::get(e.template).is_some_and(|t| t.class == "fp" || t.class == "neon");
                (if fp { EF_FP } else { 0 }) | if e.template == "sysreg" { EF_SYSREG } else { 0 }
            })
            .collect()
    })
}

// ---- a function's instructions, with control flow and calls (summary.Func)

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum K {
    Op,
    Goto,
    Call,
    Svc,
    Tail,
    Ret,
    Trap,
}

#[derive(Clone, Copy, Debug)]
pub enum Callee {
    None,
    One(Tgt),
    /// a branch with some edges leaving the function: those edges' targets (Flow::tails[a..b])
    Tails(u32, u32),
}

#[derive(Clone, Copy, Debug)]
enum Guest {
    None,
    Computed,
    /// Flow::guests[a..b]
    List(u32, u32),
}

/// summary.Func, in buffers a worker reuses
#[derive(Default)]
pub struct Flow {
    pub entry: u64,
    pub me: FnId,
    pub n: usize,
    pub kind: Vec<K>,
    pub insn: Vec<Option<Insn>>,
    pub use_: Vec<u128>,
    pub dfn: Vec<u128>,
    succ_at: Vec<u32>,
    succ: Vec<u32>,
    pub callee: Vec<Callee>,
    tails: Vec<Tgt>,
    /// per instruction: the function a call's return falls into (the body ends after the call)
    pub ctail: Vec<Option<Tgt>>,
    guest: Vec<Guest>,
    guests: Vec<Tgt>,
    /// per instruction: (reads, writes) of the after rows of a call
    after: Vec<Option<(u128, u128)>>,
    pub fp: bool,
    pub writes_fpcr: bool,
    /// entries this function falls into without a branch
    pub falls_into: Vec<u64>,
    pub fallthrough_entry: bool,
    // (build scratch)
    local: Vec<Result<u32, u64>>,
    is_ent: Vec<bool>,
    row_k: Vec<u32>,
}

fn sx(v: u32, bits: u32) -> i64 {
    if (v >> (bits - 1)) & 1 != 0 {
        v as i64 - (1i64 << bits)
    } else {
        v as i64
    }
}

impl Flow {
    pub fn succs(&self, k: usize) -> &[u32] {
        &self.succ[self.succ_at[k] as usize..self.succ_at[k + 1] as usize]
    }
    pub fn tails_of(&self, a: u32, b: u32) -> &[Tgt] {
        &self.tails[a as usize..b as usize]
    }
    fn guests_of(&self, k: usize) -> Option<Option<&[Tgt]>> {
        match self.guest[k] {
            Guest::None => None,
            Guest::Computed => Some(None),
            Guest::List(a, b) => Some(Some(&self.guests[a as usize..b as usize])),
        }
    }

    /// builds the function's flow (summary.Func.__init__)
    fn build(&mut self, prog: &Program, du: &mut DefUse, me: FnId) {
        let body = &prog.bodies[me as usize];
        let (pcs, words) = (&body.pcs[..], &body.words[..]);
        let entry = prog.fns[me as usize];
        let n = pcs.len();
        self.entry = entry;
        self.me = me;
        self.n = n;
        self.kind.clear();
        self.kind.resize(n, K::Op);
        self.insn.clear();
        self.use_.clear();
        self.dfn.clear();
        self.succ_at.clear();
        self.succ.clear();
        self.callee.clear();
        self.callee.resize(n, Callee::None);
        self.tails.clear();
        self.ctail.clear();
        self.ctail.resize(n, None);
        self.guest.clear();
        self.guest.resize(n, Guest::None);
        self.guests.clear();
        self.after.clear();
        self.after.resize(n, None);
        self.fp = false;
        self.writes_fpcr = false;
        self.falls_into.clear();
        self.fallthrough_entry = false;
        // which of the body's pcs are entries, and the rows' pcs in it: one walk of each sorted list beside the body's
        let mut is_ent = std::mem::take(&mut self.is_ent);
        is_ent.clear();
        let mut ei = prog.entries.partition_point(|&e| e < pcs.first().copied().unwrap_or(0));
        let mut ri = prog.rows.partition_point(|r| r.pc < pcs.first().copied().unwrap_or(0));
        let mut row_k = std::mem::take(&mut self.row_k);
        row_k.clear();
        for &pc in pcs {
            while ei < prog.entries.len() && prog.entries[ei] < pc {
                ei += 1;
            }
            is_ent.push(ei < prog.entries.len() && prog.entries[ei] == pc);
            while ri < prog.rows.len() && prog.rows[ri].pc < pc {
                ri += 1;
            }
            row_k.push(if ri < prog.rows.len() && prog.rows[ri].pc == pc { ri as u32 } else { u32::MAX });
        }
        // to(tg, None): a local index, or a tail exit to callee(tg); j: tg's index if known
        let to = |tg: u64, j: Option<usize>| -> Result<u32, u64> {
            if let Some(j) = j.or_else(|| pcs.binary_search(&tg).ok()) {
                if tg == entry || !is_ent[j] {
                    return Ok(j as u32);
                }
            }
            Err(prog.callee(tg))
        };
        for k in 0..n {
            let pc = pcs[k];
            let i = decode(words[k]);
            let (mut u, mut d) = match &i {
                Some(i) => du.get(i),
                None => (0, 0),
            };
            let row = (row_k[k] != u32::MAX).then(|| &prog.rows[row_k[k] as usize]);
            let tmpl = i.map(|i| sem::kind(&i));
            if let Some(r) = row {
                match &r.calls {
                    None => {}
                    Some(None) => self.guest[k] = Guest::Computed,
                    Some(Some(l)) => {
                        let a = self.guests.len() as u32;
                        self.guests.extend(l.iter().map(|&g| prog.tgt(g)));
                        self.guest[k] = Guest::List(a, self.guests.len() as u32);
                    }
                }
                let is_br = matches!(tmpl, Some(Kind::Br));
                let is_call = matches!(tmpl, Some(Kind::Blr)) || (matches!(tmpl, Some(Kind::B)) && i.unwrap().f(Fld::op) != 0);
                if r.repl {
                    u = r.bu | r.au;
                    d = r.bd | r.ad | if is_br { 0 } else { X30 };
                } else if is_call {
                    u = r.bu | (u & !r.bd);
                    d |= r.bd | r.ad;
                    if r.au != 0 || r.ad != 0 {
                        self.after[k] = Some((r.au, r.ad));
                    }
                } else {
                    u = r.bu | (u & !r.bd) | (r.au & !(d | r.bd));
                    d |= r.bd | r.ad;
                }
            }
            self.insn.push(i);
            self.use_.push(u);
            self.dfn.push(d);
            self.succ_at.push(self.succ.len() as u32);
            let i = match i {
                None => {
                    self.kind[k] = K::Trap;
                    continue;
                }
                Some(i) => i,
            };
            let t = sem::kind(&i);
            let ef = enc_facts()[i.enc.id as usize];
            if ef & EF_FP != 0 {
                self.fp = true;
            }
            if ef & EF_SYSREG != 0 && i.f(Fld::L) == 0 && i.f(Fld::CRn) == 4 && i.f(Fld::op2) == 0 && i.f(Fld::CRm) == 4 {
                self.writes_fpcr = true;
            }
            // (pc + 4 in the body: the next pc, the body being sorted)
            let next_in = pcs.get(k + 1) == Some(&(pc + 4));
            let nxt = if next_in {
                Some(to(pc + 4, Some(k + 1)))
            } else if prog.is_entry(prog.callee(pc + 4)) {
                Some(to(pc + 4, None))
            } else {
                None
            };
            let repl = row.is_some_and(|r| r.repl);
            let mut local = std::mem::take(&mut self.local);
            local.clear();
            let target = |off: i64| (pc as i64).wrapping_add(off << 2) as u64;
            match t {
                Kind::Sem(..) | Kind::Missing => {
                    local.extend(nxt);
                    self.edge(prog, k, &local, false);
                    if self.kind[k] == K::Tail {
                        if let Callee::One(c) = self.callee[k] {
                            if !self.falls_into.contains(&c.addr) {
                                self.falls_into.push(c.addr);
                            }
                        }
                    }
                }
                Kind::Br if repl => self.kind[k] = K::Ret,
                _ if repl => {
                    local.extend(nxt);
                    self.edge(prog, k, &local, false);
                }
                Kind::B if i.f(Fld::op) != 0 => {
                    let c = prog.callee(target(sx(i.f(Fld::imm26), 26)));
                    self.kind[k] = K::Call;
                    self.callee[k] = Callee::One(prog.tgt(c));
                    local.extend(nxt);
                    self.edge(prog, k, &local, true);
                }
                Kind::B => {
                    local.push(to(target(sx(i.f(Fld::imm26), 26)), None));
                    self.edge(prog, k, &local, false);
                }
                Kind::Bcond | Kind::Cbz | Kind::Tbz => {
                    let tg = if let Kind::Tbz = t { target(sx(i.f(Fld::imm14), 14)) } else { target(sx(i.f(Fld::imm19), 19)) };
                    local.push(to(tg, None));
                    local.extend(nxt);
                    self.edge(prog, k, &local, false);
                }
                Kind::Blr => {
                    self.kind[k] = K::Call;
                    self.callee[k] = Callee::One(Tgt::INDIRECT);
                    local.extend(nxt);
                    self.edge(prog, k, &local, true);
                }
                Kind::Br => match body.jt.binary_search_by_key(&pc, |r| r.0) {
                    Ok(j) if !body.jt[j].1.is_empty() => {
                        // (sorted, unique in the export)
                        local.extend(body.jt[j].1.iter().map(|&x| to(x, None)));
                        self.edge(prog, k, &local, false);
                    }
                    _ => {
                        self.kind[k] = K::Tail;
                        self.callee[k] = Callee::One(Tgt::INDIRECT);
                    }
                },
                Kind::Ret => self.kind[k] = K::Ret,
                Kind::Exc if i.f(Fld::opc) == 0 => {
                    self.kind[k] = K::Svc;
                    local.extend(nxt);
                    self.edge(prog, k, &local, true);
                }
                _ => self.kind[k] = K::Trap,
            }
            self.local = local;
        }
        self.succ_at.push(self.succ.len() as u32);
        self.is_ent = is_ent;
        self.row_k = row_k;
    }

    /// successors of instruction k (summary.Func._edge): local indices; tail exits make it a tail when alone
    fn edge(&mut self, prog: &Program, k: usize, targets: &[Result<u32, u64>], keep_kind: bool) {
        let first_tail = targets.iter().find_map(|t| t.err());
        let ntails = targets.iter().filter(|t| t.is_err()).count();
        let nlocal = targets.len() - ntails;
        if keep_kind {
            self.succ.extend(targets.iter().filter_map(|t| t.ok()));
            if let Some(t) = first_tail {
                self.ctail[k] = Some(prog.tgt(t));
            }
            return;
        }
        self.kind[k] = if nlocal > 0 || ntails == 0 { K::Goto } else { K::Tail };
        if self.kind[k] == K::Tail {
            let t0 = first_tail.unwrap();
            let one = targets.iter().all(|t| *t == Err(t0));
            self.callee[k] = if one { Callee::One(prog.tgt(t0)) } else { Callee::One(Tgt::INDIRECT) };
            return;
        }
        self.succ.extend(targets.iter().filter_map(|t| t.ok()));
        if ntails > 0 {
            let a = self.tails.len() as u32;
            self.tails.extend(targets.iter().filter_map(|t| t.err()).map(|t| prog.tgt(t)));
            self.callee[k] = Callee::Tails(a, self.tails.len() as u32);
        }
    }
}

// ---- the analyses of one function, given its callees' summaries

/// the summaries a component's analysis reads: earlier components' (final), and the component's own (local)
/// the finished summaries: during the fixed point each set once, when its component is done; after it plain
pub enum Done<'s> {
    Once(&'s [OnceLock<Summary>]),
    Plain(&'s [Summary]),
}

pub struct Sums<'s> {
    pub prog: &'s Program<'s>,
    pub done: Done<'s>,
    /// comp_of[f]: f's component
    pub comp_of: &'s [u32],
    pub pos: &'s [u32],
    pub cur: u32,
    pub local: &'s [Summary],
}

impl<'s> Sums<'s> {
    /// sums[f]
    pub fn get(&self, f: FnId) -> &Summary {
        if self.comp_of[f as usize] == self.cur {
            &self.local[self.pos[f as usize] as usize]
        } else {
            match self.done {
                Done::Plain(d) => &d[f as usize],
                Done::Once(d) => d[f as usize].get().expect("a callee's summary before its component ran"),
            }
        }
    }
    /// summary.callee_summary
    pub fn callee(&self, t: &Tgt) -> Cow<'_, Summary> {
        if t.noret {
            return Cow::Borrowed(&NORETURN_HOST);
        }
        match t.cls {
            Cls::Indirect | Cls::Other => Cow::Borrowed(&AAPCS),
            Cls::Hook => Cow::Borrowed(&self.prog.hooks[t.id as usize].summary),
            Cls::Patch => Cow::Owned(self.func_summary(t.aux as usize, t.id, &mut Vec::new())),
            Cls::Fn => {
                let s = self.get(t.id);
                Cow::Borrowed(if s.unknown { &UNKNOWN } else { s })
            }
        }
    }
    fn inputs(&self, t: &Tgt) -> u128 {
        self.callee(t).inputs
    }
    /// summary.func_summary: a PATCH_FUNC's call
    fn func_summary(&self, p: usize, me: FnId, seen: &mut Vec<usize>) -> Summary {
        let d = &self.prog.patches[p];
        let mut s = Summary::START;
        s.inputs = d.reads;
        for &g in &d.guest {
            let t = self.prog.tgt(g);
            if t.cls == Cls::Patch {
                let gp = t.aux as usize;
                if !seen.contains(&gp) && gp != p {
                    seen.push(p);
                    s.inputs |= self.func_summary(gp, t.id, seen).inputs;
                    seen.pop();
                }
            } else {
                s.inputs |= self.inputs(&t);
            }
        }
        s.clobbers = X30 | d.result;
        s.mustdef = X30 | d.result;
        s.preserved = CALLEE_SAVED;
        s.returns = true;
        s.boundary = true;
        if d.orig {
            let b = self.get(me);
            if b.unknown {
                return UNKNOWN;
            }
            s.inputs |= b.inputs;
            s.clobbers |= b.clobbers | ORIG_ARGS;
            s.preserved &= b.preserved;
            s.writes_fpcr = b.writes_fpcr;
        }
        s
    }
    fn guest_reads(&self, g: Option<&[Tgt]>) -> u128 {
        match g {
            None => ALL,
            Some(l) => l.iter().fold(0, |m, t| m | self.inputs(t)),
        }
    }
}

/// live-in per instruction (summary.liveness)
pub fn liveness(f: &Flow, s: &Sums, ret_live: u128, live: &mut Vec<u128>) {
    let n = f.n;
    live.clear();
    live.resize(n, 0);
    let mut changed = true;
    while changed {
        changed = false;
        for k in (0..n).rev() {
            let u = f.use_[k];
            let mut v = match f.kind[k] {
                K::Ret => u & !X30 | ret_live,
                K::Trap => u,
                K::Tail => {
                    let c = match f.callee[k] {
                        Callee::One(t) => s.callee(&t),
                        _ => Cow::Borrowed(&AAPCS),
                    };
                    u | ((c.inputs | (ret_live & !c.mustdef)) & !f.dfn[k])
                }
                kind => {
                    let mut out = 0;
                    for &j in f.succs(k) {
                        out |= live[j as usize];
                    }
                    if let Some(t) = &f.ctail[k] {
                        out |= s.inputs(t) | ret_live;
                    }
                    if let Callee::Tails(a, b) = f.callee[k] {
                        for t in f.tails_of(a, b) {
                            out |= s.inputs(t) | ret_live;
                        }
                    }
                    match kind {
                        K::Svc => u | (out & !f.dfn[k]),
                        K::Call => {
                            let c = match f.callee[k] {
                                Callee::One(t) => s.callee(&t),
                                _ => Cow::Borrowed(&AAPCS),
                            };
                            if !c.returns {
                                out = 0;
                            }
                            if let (Some(a), true) = (f.after[k], c.returns) {
                                out = a.0 | (out & !a.1);
                            }
                            u | c.inputs | (out & !(c.mustdef | X30))
                        }
                        _ => u | (out & !f.dfn[k]),
                    }
                }
            };
            if let Some(g) = f.guests_of(k) {
                v |= s.guest_reads(g);
            }
            if v != live[k] {
                live[k] = v;
                changed = true;
            }
        }
    }
}

fn callee_of<'a>(f: &Flow, k: usize, s: &'a Sums) -> Cow<'a, Summary> {
    match f.callee[k] {
        Callee::One(t) => s.callee(&t),
        _ => Cow::Borrowed(&AAPCS),
    }
}

/// (mustdef at the exits, clobbers, returns) forward (summary.must_and_may)
fn must_and_may(f: &Flow, s: &Sums, must: &mut Vec<Option<u128>>) -> (u128, u128, bool) {
    let n = f.n;
    must.clear();
    must.resize(n, None);
    if n > 0 {
        must[0] = Some(0);
    }
    let (mut exit_must, mut may, mut returns) = (ALL, 0u128, false);
    let mut changed = true;
    while changed {
        changed = false;
        for k in 0..n {
            let m = match must[k] {
                None => continue,
                Some(m) => m,
            };
            let kind = f.kind[k];
            let out = if kind == K::Call {
                let c = callee_of(f, k, s);
                if !c.returns {
                    continue;
                }
                m | f.dfn[k] | c.mustdef | X30
            } else {
                m | f.dfn[k]
            };
            if matches!(kind, K::Call | K::Svc) {
                if let Some(t) = &f.ctail[k] {
                    let s2 = s.callee(t);
                    if s2.returns {
                        exit_must &= out | s2.mustdef;
                        returns = true;
                    }
                }
            }
            if kind == K::Ret {
                exit_must &= out;
                returns = true;
                continue;
            }
            if kind == K::Tail {
                let c = callee_of(f, k, s);
                if c.returns {
                    exit_must &= out | c.mustdef;
                    returns = true;
                }
                continue;
            }
            if let Callee::Tails(a, b) = f.callee[k] {
                for t in f.tails_of(a, b) {
                    let c = s.callee(t);
                    if c.returns {
                        exit_must &= out | c.mustdef;
                        returns = true;
                    }
                }
            }
            for &j in f.succs(k) {
                let nm = match must[j as usize] {
                    None => out,
                    Some(o) => o & out,
                };
                if must[j as usize] != Some(nm) {
                    must[j as usize] = Some(nm);
                    changed = true;
                }
            }
        }
    }
    for k in 0..n {
        may |= f.dfn[k];
        let kind = f.kind[k];
        if let Some(t) = &f.ctail[k] {
            if kind == K::Svc || callee_of(f, k, s).returns {
                may |= s.callee(t).clobbers;
            }
        }
        if matches!(kind, K::Call | K::Tail) {
            may |= callee_of(f, k, s).clobbers | if kind == K::Call { X30 } else { 0 };
        }
        if let Callee::Tails(a, b) = f.callee[k] {
            for t in f.tails_of(a, b) {
                may |= s.callee(t).clobbers;
            }
        }
    }
    (if returns { exit_must } else { ALL }, may, returns)
}

// ---- frame tracking (summary.frame_pass)

/// a register a load or store names: a Var, or x31 (the zero register: never tracked, never a frame register)
const XZR: u8 = 255;

/// (base register, store, [(register, offset delta, size)], offset, writeback) of a load/store with an immediate
/// offset (summary.mem_access)
struct Acc {
    base: u32,
    st: bool,
    regs: [(u8, i64, i64); 4],
    nregs: usize,
    off: i64,
    /// 0 none, 1 pre, 2 post
    wb: u8,
}

fn xreg(n: u32) -> u8 {
    if n == 31 {
        XZR
    } else {
        n as u8
    }
}
fn vreg(n: u32) -> u8 {
    36 + (n % 32) as u8
}
fn pmode(i: &Insn) -> &'static str {
    match i.p("mode") {
        Some(Param::Str(s)) => s,
        _ => "",
    }
}
fn wb_of(mode: &str) -> u8 {
    match mode {
        "pre" => 1,
        "post" => 2,
        _ => 0,
    }
}

fn mem_access(i: &Insn) -> Option<Acc> {
    let f = |x| i.f(x);
    let mut a = Acc { base: 0, st: false, regs: [(0, 0, 0); 4], nregs: 0, off: 0, wb: 0 };
    match i.enc.template {
        "ldst" => {
            let mode = pmode(i);
            if mode == "reg" || (f(Fld::size) == 3 && f(Fld::opc) == 2) {
                return None;
            }
            let size = 1i64 << f(Fld::size);
            a.off = if mode == "uimm" { (f(Fld::imm12) as i64) << f(Fld::size) } else { sx(f(Fld::imm9), 9) };
            a.wb = wb_of(mode);
            a.base = f(Fld::Rn);
            a.st = f(Fld::opc) == 0;
            a.regs[0] = (xreg(f(Fld::Rt)), 0, size);
            a.nregs = 1;
        }
        "ldst_pair" => {
            let sc = if f(Fld::opc) == 2 { 3 } else { 2 };
            a.off = sx(f(Fld::imm7), 7) << sc;
            a.wb = match f(Fld::mode) {
                1 => 2,
                3 => 1,
                _ => 0,
            };
            a.regs[0] = (xreg(f(Fld::Rt)), 0, 1 << sc);
            a.regs[1] = (xreg(f(Fld::Rt2)), 1 << sc, 1 << sc);
            a.nregs = 2;
            a.base = f(Fld::Rn);
            a.st = f(Fld::L) == 0;
        }
        "ldst_v" => {
            let mode = pmode(i);
            if mode == "reg" {
                return None;
            }
            let sc = ounce_decode::info::v_scale(f(Fld::size), f(Fld::opc));
            a.off = if mode == "uimm" { (f(Fld::imm12) as i64) << sc } else { sx(f(Fld::imm9), 9) };
            a.wb = wb_of(mode);
            a.base = f(Fld::Rn);
            a.st = f(Fld::opc) & 1 == 0;
            a.regs[0] = (vreg(f(Fld::Rt)), 0, 1 << sc);
            a.nregs = 1;
        }
        "ldst_mult" => {
            let (nreg, selem) = ounce_decode::info::mult(f(Fld::opcode)).expect("MULT");
            if selem != 1 {
                return None;
            }
            let rb: i64 = if f(Fld::Q) != 0 { 16 } else { 8 };
            for j in 0..nreg {
                a.regs[j as usize] = (vreg(f(Fld::Rt) + j), j as i64 * rb, rb);
            }
            a.nregs = nreg as usize;
            if matches!(i.p("post"), Some(Param::Bool(true))) {
                if f(Fld::Rm) != 31 {
                    return None;
                }
                a.wb = 2;
            }
            a.off = if a.wb != 0 { nreg as i64 * rb } else { 0 };
            a.base = f(Fld::Rn);
            a.st = f(Fld::L) == 0;
        }
        "ldst_v_pair" => {
            let sc = 2 + f(Fld::opc);
            a.off = sx(f(Fld::imm7), 7) << sc;
            a.wb = match f(Fld::mode) {
                1 => 2,
                3 => 1,
                _ => 0,
            };
            a.regs[0] = (vreg(f(Fld::Rt)), 0, 1 << sc);
            a.regs[1] = (vreg(f(Fld::Rt2)), 1 << sc, 1 << sc);
            a.nregs = 2;
            a.base = f(Fld::Rn);
            a.st = f(Fld::L) == 0;
        }
        _ => return None,
    }
    Some(a)
}

fn rbit(r: u8) -> u128 {
    if r == XZR {
        0
    } else {
        1u128 << r
    }
}

/// a frame state (summary.frame_pass): SP's offset, the registers holding entry SP plus a constant (sorted), the
/// registers holding their entry value, the frame slots holding one (offset -> (register, size), sorted)
#[derive(Clone, PartialEq, Eq, Default)]
struct FState {
    sp: Option<i64>,
    fregs: Vec<(u8, i64)>,
    holds: u128,
    slots: Vec<(i64, u8, i64)>,
}

fn fget(fr: &[(u8, i64)], r: u32) -> Option<i64> {
    fr.iter().find(|x| x.0 as u32 == r).map(|x| x.1)
}
fn fset(fr: &mut Vec<(u8, i64)>, r: u32, v: i64) {
    match fr.binary_search_by_key(&(r as u8), |x| x.0) {
        Ok(k) => fr[k].1 = v,
        Err(k) => fr.insert(k, (r as u8, v)),
    }
}
fn fpop(fr: &mut Vec<(u8, i64)>, r: u32) {
    if let Ok(k) = fr.binary_search_by_key(&(r as u8), |x| x.0) {
        fr.remove(k);
    }
}
fn intersect<T: Ord + Copy>(a: &[T], b: &[T]) -> Vec<T> {
    let (mut i, mut j, mut out) = (0, 0, Vec::new());
    while i < a.len() && j < b.len() {
        match a[i].cmp(&b[j]) {
            std::cmp::Ordering::Less => i += 1,
            std::cmp::Ordering::Greater => j += 1,
            std::cmp::Ordering::Equal => {
                out.push(a[i]);
                i += 1;
                j += 1;
            }
        }
    }
    out
}

pub struct FrameOut {
    pub preserved: u128,
    pub reasons: u16,
    pub frame: i64,
    pub taken: bool,
    pub pairs: u32,
    pub lr_data: bool,
}

#[derive(Default)]
pub struct FrameScratch {
    state: Vec<Option<FState>>,
    work: Vec<u32>,
    exits: Vec<(Option<i64>, u128)>,
    saves: Vec<(u8, i64)>,
    restores: Vec<(u8, i64)>,
    other_reads: Vec<i64>,
}

fn frame_pass(f: &Flow, s: &Sums, fs: &mut FrameScratch) -> FrameOut {
    let n = f.n;
    fs.state.clear();
    fs.state.resize(n, None);
    fs.work.clear();
    fs.exits.clear();
    fs.saves.clear();
    fs.restores.clear();
    fs.other_reads.clear();
    if n == 0 {
        return FrameOut { preserved: CALLEE_SAVED, reasons: 0, frame: 0, taken: false, pairs: 0, lr_data: false };
    }
    fs.state[0] = Some(FState { sp: Some(0), fregs: Vec::new(), holds: TRACKED | SPM, slots: Vec::new() });
    fs.work.push(0);
    let mut reasons = 0u16;
    let mut lowest = 0i64;
    let (mut taken, mut lr_data) = (false, false);
    while let Some(k) = fs.work.pop() {
        let k = k as usize;
        let st = fs.state[k].clone().unwrap();
        let (mut sp, mut fregs, mut holds, mut slots) = (st.sp, st.fregs, st.holds, st.slots);
        let kind = f.kind[k];
        let i = f.insn[k];
        if let Some(i) = &i {
            let t = i.enc.template;
            let acc = mem_access(i);
            let mut handled = false;
            if t == "addsub_imm" && i.f(Fld::S) == 0 {
                let mut imm = (i.f(Fld::imm12) as i64) << if i.f(Fld::sh) != 0 { 12 } else { 0 };
                if i.f(Fld::op) != 0 {
                    imm = -imm;
                }
                let (rd, rn) = (i.f(Fld::Rd), i.f(Fld::Rn));
                let src = if rn == 31 { sp } else { fget(&fregs, rn) };
                if rn == 31 || fget(&fregs, rn).is_some() {
                    if rd == 31 {
                        sp = src.map(|v| v + imm);
                    } else {
                        holds &= !xm(rd);
                        match src {
                            Some(v) => fset(&mut fregs, rd, v + imm),
                            None => fpop(&mut fregs, rd),
                        }
                    }
                    handled = true;
                }
            }
            match &acc {
                Some(a) if a.base == 31 || fget(&fregs, a.base).is_some() => {
                    let cur = if a.base == 31 { sp } else { fget(&fregs, a.base) };
                    let addr = cur.map(|c| c + if a.wb != 2 { a.off } else { 0 });
                    for &(r, d, size) in &a.regs[..a.nregs] {
                        let addr = match addr {
                            None => {
                                if !a.st {
                                    holds &= !rbit(r);
                                }
                                continue;
                            }
                            Some(x) => x,
                        };
                        let at = addr + d;
                        if a.st {
                            slots.retain(|&(x, _, xs)| !(x < at + size && at < x + xs));
                            if size >= 8 && holds & rbit(r) & TRACKED != 0 {
                                match slots.binary_search_by_key(&at, |x| x.0) {
                                    Ok(p) => slots[p] = (at, r, size),
                                    Err(p) => slots.insert(p, (at, r, size)),
                                }
                                fs.saves.push((r, at));
                            }
                        } else {
                            let held = slots.binary_search_by_key(&at, |x| x.0).ok().map(|p| slots[p].1);
                            if held == Some(r) && r != XZR && size >= 8 {
                                holds |= rbit(r);
                                fs.restores.push((r, at));
                            } else {
                                holds &= !rbit(r);
                                fs.other_reads.push(at);
                            }
                        }
                    }
                    if a.wb != 0 {
                        if a.base == 31 {
                            sp = sp.map(|v| v + a.off);
                        } else if let Some(v) = fget(&fregs, a.base) {
                            fset(&mut fregs, a.base, v + a.off);
                        }
                    }
                    if !a.st {
                        for &(r, _, _) in &a.regs[..a.nregs] {
                            if r < 32 || r == XZR {
                                fpop(&mut fregs, if r == XZR { 31 } else { r as u32 });
                            }
                        }
                    }
                    handled = true;
                }
                // (a store through another pointer: of an entry value of x19-x30 (setjmp, context captures))
                Some(a) if a.st && a.regs[..a.nregs].iter().any(|&(r, _, _)| holds & rbit(r) & TRACKED != 0) => {
                    reasons |= why::SAVE_PTR;
                }
                _ => {}
            }
            if !handled {
                let d = f.dfn[k];
                if holds & X30 != 0 && f.use_[k] & X30 != 0 && kind != K::Ret && t != "blr" && t != "br" && !acc.as_ref().is_some_and(|a| a.st) {
                    lr_data = true;
                }
                if f.use_[k] & SPM != 0 && t != "addsub_imm" && acc.is_none() && !(t == "ldst" && pmode(i) == "reg") {
                    taken = true;
                }
                holds &= !d;
                if d & SPM != 0 {
                    sp = None;
                }
                fregs.retain(|&(x, _)| d & xm(x as u32) == 0);
            }
            if let Some(v) = sp {
                lowest = lowest.min(v);
            }
        }
        if kind == K::Call || kind == K::Svc {
            let c = if kind == K::Svc { Cow::Borrowed(&AAPCS) } else { callee_of(f, k, s) };
            if kind == K::Call && !c.returns {
                continue;
            }
            holds &= c.preserved | SPM | X30;
            if c.preserved & SPM == 0 {
                sp = None;
            }
            if kind == K::Call {
                holds &= !X30;
                if fregs.iter().any(|&(r, _)| r < 8) {
                    taken = true;
                }
            }
            fregs.retain(|&(x, _)| c.clobbers & xm(x as u32) == 0);
            if f.ctail[k].is_some() {
                continue;
            }
        }
        if kind == K::Ret {
            let i = i.unwrap();
            if holds & X30 == 0 || (i.enc.name == "RET" && i.f(Fld::Rn) != 30) {
                reasons |= why::LR_RET;
            }
            fs.exits.push((sp, holds));
            continue;
        }
        if kind == K::Tail {
            let c = callee_of(f, k, s);
            let indirect = matches!(f.callee[k], Callee::One(Tgt { cls: Cls::Indirect, .. }));
            if sp != Some(0) && !indirect {
                reasons |= why::SPLIT_FRAME;
            }
            if c.returns {
                fs.exits.push((sp, if c.preserved & SPM != 0 { holds & (c.preserved | SPM) } else { 0 }));
            }
            continue;
        }
        if let Callee::Tails(a, b) = f.callee[k] {
            for t in f.tails_of(a, b) {
                let c = s.callee(t);
                if c.returns {
                    fs.exits.push((sp, holds & c.preserved));
                }
            }
        }
        let new = FState { sp, fregs, holds, slots };
        for &j in f.succs(k) {
            let j = j as usize;
            match &fs.state[j] {
                None => {
                    fs.state[j] = Some(new.clone());
                    fs.work.push(j as u32);
                }
                Some(old) => {
                    let m = FState {
                        sp: if old.sp == new.sp { old.sp } else { None },
                        fregs: intersect(&old.fregs, &new.fregs),
                        holds: old.holds & new.holds,
                        slots: intersect(&old.slots, &new.slots),
                    };
                    if &m != old {
                        fs.state[j] = Some(m);
                        fs.work.push(j as u32);
                    }
                }
            }
        }
    }
    let mut preserved = CALLEE_SAVED;
    for &(sp, holds) in &fs.exits {
        let mut p = holds & TRACKED;
        if sp == Some(0) {
            p |= SPM;
        }
        preserved &= p;
    }
    if !fs.exits.is_empty() && preserved & SPM == 0 {
        reasons |= why::SP;
    }
    fs.saves.sort_unstable();
    fs.saves.dedup();
    fs.restores.sort_unstable();
    fs.restores.dedup();
    fs.other_reads.sort_unstable();
    fs.other_reads.dedup();
    let pairs = fs
        .saves
        .iter()
        .filter(|&&(r, a)| {
            fs.restores.binary_search(&(r, a)).is_ok() && fs.other_reads.binary_search(&a).is_err() && preserved & rbit(r) != 0
        })
        .count() as u32;
    FrameOut { preserved, reasons, frame: -lowest, taken, pairs, lr_data }
}

/// a worker's buffers
#[derive(Default)]
pub struct Scratch {
    live: Vec<u128>,
    live2: Vec<u128>,
    must: Vec<Option<u128>>,
    frame: FrameScratch,
}

/// summary.analyse
fn analyse(f: &Flow, s: &Sums, sc: &mut Scratch) -> Summary {
    let mut out = Summary::START;
    liveness(f, s, 0, &mut sc.live);
    let live = &sc.live;
    out.inputs = if f.n > 0 { live[0] } else { 0 };
    let (mustdef, clobbers, returns) = must_and_may(f, s, &mut sc.must);
    out.mustdef = mustdef;
    out.clobbers = clobbers;
    out.returns = returns;
    let fr = frame_pass(f, s, &mut sc.frame);
    let mut reasons = fr.reasons;
    out.frame = fr.frame;
    out.frame_taken = fr.taken;
    out.pairs = fr.pairs;
    out.lr_data = fr.lr_data;
    out.preserved = if out.returns { fr.preserved } else { CALLEE_SAVED };
    if out.lr_data {
        reasons |= why::LR_DATA;
    }
    if out.returns && (CALLEE_SAVED & !SPM) & !out.preserved != 0 {
        reasons |= why::CS_CLOBBER;
    }
    if out.clobbers & out.preserved & !out.inputs & range(19, 30) != 0 {
        reasons |= why::INCONSISTENT;
    }
    if out.inputs & NZCV != 0 {
        reasons |= why::FLAGS_IN;
    }
    if out.inputs & TMPS != 0 {
        reasons |= why::TMP_IN;
    }
    if f.fallthrough_entry {
        reasons |= why::FALLTHROUGH_ENTRY;
    }
    for k in 0..f.n {
        if f.ctail[k].is_some() && f.kind[k] == K::Call && callee_of(f, k, s).returns {
            reasons |= why::DANGLING_BL;
        }
    }
    for k in 0..f.n {
        if f.kind[k] == K::Call {
            let cs = callee_of(f, k, s);
            let mut o = 0;
            for &j in f.succs(k) {
                o |= live[j as usize];
            }
            if let Some(a) = f.after[k] {
                o = a.0 | (o & !a.1);
            }
            if o & !cs.mustdef & cs.clobbers & (TMPS | NZCV) != 0 {
                reasons |= why::TMP_AFTER_CALL;
            }
        }
    }
    let mut boundary = f.guest.iter().any(|g| matches!(g, Guest::Computed));
    for k in 0..f.n {
        let kind = f.kind[k];
        let c = match f.callee[k] {
            Callee::One(t) => Some(t),
            _ => None,
        };
        let callish = matches!(kind, K::Call | K::Tail);
        if kind == K::Svc || (callish && c.is_none_or(|t| t.cls == Cls::Indirect || t.is_host())) {
            boundary = true;
        } else if callish {
            if let Some(g) = c.and_then(|t| t.func()) {
                let cs = s.get(g);
                if cs.boundary || cs.unknown {
                    boundary = true;
                }
                if cs.unknown {
                    reasons |= why::UNKNOWN_CALLEE;
                }
                if cs.writes_fpcr {
                    out.writes_fpcr = true;
                }
            }
        }
    }
    out.boundary = boundary;
    out.fp = f.fp;
    out.writes_fpcr = out.writes_fpcr || f.writes_fpcr;
    out.unknown = reasons & why::UNKNOWN_FACTS != 0;
    out.demote = reasons;
    out
}

// ---- the whole program

thread_local! {
    static DU: RefCell<DefUse> = RefCell::new(DefUse::new());
}

/// the fixed point's schedule: a component runs when its last callee component is done, then releases its callers
struct Wave<'a> {
    prog: &'a Program<'a>,
    comps: &'a [Vec<FnId>],
    comp_of: &'a [u32],
    pos: &'a [u32],
    fallthrough: &'a [bool],
    pending: &'a [AtomicU32],
    dep_at: &'a [u32],
    dep: &'a [u32],
    done: &'a [OnceLock<Summary>],
}

thread_local! {
    static WORKER: RefCell<Worker> = RefCell::new(Worker::default());
}

impl<'a> Wave<'a> {
    fn run<'s>(&'s self, k: u32, sc: &rayon::Scope<'s>) {
        let comp = &self.comps[k as usize];
        let prog = self.prog;
        WORKER.with(|w| {
            let w = &mut *w.borrow_mut();
            let mut local = vec![Summary::START; comp.len()];
            for (p, &e) in comp.iter().enumerate() {
                w.flow(p, prog, e).fallthrough_entry = self.fallthrough[e as usize];
            }
            let self_call = comp.len() == 1 && {
                let f = &w.flows[0];
                (0..f.n).any(|k| matches!(f.callee[k], Callee::One(t) if t.func() == Some(f.me)))
            };
            loop {
                let mut changed = false;
                for p in 0..comp.len() {
                    let s = {
                        let ctx = Sums { prog, done: Done::Once(self.done), comp_of: self.comp_of, pos: self.pos, cur: k, local: &local };
                        analyse(&w.flows[p], &ctx, &mut w.sc)
                    };
                    if s.key() != local[p].key() {
                        changed = true;
                    }
                    local[p] = s;
                }
                if !changed || (comp.len() == 1 && !self_call) {
                    break;
                }
            }
            for (p, s) in local.into_iter().enumerate() {
                assert!(self.done[comp[p] as usize].set(s).is_ok());
            }
        });
        for &d in &self.dep[self.dep_at[k as usize] as usize..self.dep_at[k as usize + 1] as usize] {
            if self.pending[d as usize].fetch_sub(1, Ordering::AcqRel) == 1 {
                sc.spawn(move |sc| self.run(d, sc));
            }
        }
    }
}

/// one worker's state: its flows, def/use cache and analysis buffers
#[derive(Default)]
struct Worker {
    flows: Vec<Flow>,
    sc: Scratch,
}

impl Worker {
    fn flow(&mut self, k: usize, prog: &Program, me: FnId) -> &mut Flow {
        while self.flows.len() <= k {
            self.flows.push(Flow::default());
        }
        DU.with(|du| self.flows[k].build(prog, &mut du.borrow_mut(), me));
        &mut self.flows[k]
    }
}

/// what the first pass takes of a function (summary._graph_part)
struct GraphOut {
    callees: Vec<FnId>,
    falls: Vec<u64>,
    dangling: Vec<u64>,
    tails: Vec<FnId>,
}

fn graph_of(f: &Flow, prog: &Program) -> GraphOut {
    let mut callees = Vec::new();
    for k in 0..f.n {
        match f.callee[k] {
            Callee::Tails(a, b) => callees.extend(f.tails_of(a, b).iter().filter_map(|t| t.func())),
            Callee::One(t) if matches!(f.kind[k], K::Call | K::Tail) => callees.extend(t.func()),
            _ => {}
        }
    }
    callees.extend(f.ctail.iter().flatten().filter_map(|t| t.func()));
    for k in 0..f.n {
        if let Some(Some(l)) = f.guests_of(k) {
            callees.extend(l.iter().filter_map(|t| t.func()));
        }
    }
    if let Some(p) = prog.patch(f.entry) {
        callees.extend(prog.patches[p].guest.iter().filter_map(|&g| prog.fn_id(g)));
    }
    let dangling = (0..f.n)
        .filter(|&k| f.ctail[k].is_some() && f.kind[k] == K::Call)
        .filter_map(|k| match f.callee[k] {
            Callee::One(t) if t.cls != Cls::Indirect => Some(t.addr),
            _ => None,
        })
        .collect();
    let tails = (0..f.n)
        .filter(|&k| f.kind[k] == K::Tail)
        .filter_map(|k| match f.callee[k] {
            Callee::One(t) if t.cls == Cls::Fn => Some(t.id),
            _ => None,
        })
        .collect();
    GraphOut { callees, falls: f.falls_into.clone(), dangling, tails }
}

/// Tarjan's strongly connected components, iteratively, callees before callers (summary.sccs: the same order)
fn sccs(n: usize, at: &[u32], adj: &[FnId]) -> Vec<Vec<FnId>> {
    const NONE: u32 = u32::MAX;
    let mut index = vec![NONE; n];
    let mut low = vec![0u32; n];
    let mut on = vec![false; n];
    let mut stack: Vec<FnId> = Vec::new();
    let mut out = Vec::new();
    let mut counter = 0u32;
    let mut work: Vec<(FnId, u32)> = Vec::new();
    for root in 0..n as FnId {
        if index[root as usize] != NONE {
            continue;
        }
        index[root as usize] = counter;
        low[root as usize] = counter;
        counter += 1;
        stack.push(root);
        on[root as usize] = true;
        work.push((root, at[root as usize]));
        while let Some(&mut (v, ref mut it)) = work.last_mut() {
            let end = at[v as usize + 1];
            let mut pushed = None;
            while *it < end {
                let w = adj[*it as usize];
                *it += 1;
                if index[w as usize] == NONE {
                    index[w as usize] = counter;
                    low[w as usize] = counter;
                    counter += 1;
                    stack.push(w);
                    on[w as usize] = true;
                    pushed = Some(w);
                    break;
                }
                if on[w as usize] {
                    low[v as usize] = low[v as usize].min(index[w as usize]);
                }
            }
            if let Some(w) = pushed {
                work.push((w, at[w as usize]));
                continue;
            }
            work.pop();
            if let Some(&(u, _)) = work.last() {
                low[u as usize] = low[u as usize].min(low[v as usize]);
            }
            if low[v as usize] == index[v as usize] {
                let mut comp = Vec::new();
                loop {
                    let w = stack.pop().unwrap();
                    on[w as usize] = false;
                    comp.push(w);
                    if w == v {
                        break;
                    }
                }
                out.push(comp);
            }
        }
    }
    out
}

pub struct Stats {
    pub functions: usize,
    pub sccs: usize,
    pub largest: usize,
    pub levels: usize,
    pub noreturn_by_callers: usize,
    pub graph_s: f64,
    pub fixed_s: f64,
    pub results_s: f64,
}

/// every lifted function's summary, by FnId (summary.summarise_parallel); prog.noret gains the callers' evidence
pub fn summarise(prog: &mut Program) -> (Vec<Summary>, Stats) {
    let t0 = std::time::Instant::now();
    let n = prog.fns.len();
    // pass A: the call graph
    let graph: Vec<GraphOut> = {
        let p: &Program = prog;
        (0..n as FnId)
            .into_par_iter()
            .map_init(Worker::default, |w, i| {
                let f = w.flow(0, p, i);
                graph_of(f, p)
            })
            .collect()
    };
    let mut at = Vec::with_capacity(n + 1);
    let mut adj = Vec::new();
    let mut by_callers = Vec::new();
    let mut fallthrough = vec![false; n];
    for (e, g) in graph.iter().enumerate() {
        at.push(adj.len() as u32);
        adj.extend_from_slice(&g.callees);
        by_callers.extend_from_slice(&g.dangling);
        for &a in &g.falls {
            if let Some(f) = prog.fn_id(a) {
                if f as usize != e {
                    fallthrough[f as usize] = true;
                }
            }
        }
    }
    at.push(adj.len() as u32);
    by_callers.sort_unstable();
    by_callers.dedup();
    let nbc = by_callers.len();
    prog.noret.extend(by_callers);
    prog.noret.sort_unstable();
    prog.noret.dedup();
    let prog: &Program = prog;
    let t1 = std::time::Instant::now();
    // the fixed point, level by level over the components' DAG
    let comps = sccs(n, &at, &adj);
    let mut comp_of = vec![0u32; n];
    let mut pos = vec![0u32; n];
    for (k, c) in comps.iter().enumerate() {
        for (p, &e) in c.iter().enumerate() {
            comp_of[e as usize] = k as u32;
            pos[e as usize] = p as u32;
        }
    }
    let mut level = vec![0u32; comps.len()];
    for (k, c) in comps.iter().enumerate() {
        let mut lv = 0;
        for &e in c {
            for &g in &adj[at[e as usize] as usize..at[e as usize + 1] as usize] {
                let kk = comp_of[g as usize] as usize;
                if kk != k {
                    lv = lv.max(level[kk] + 1);
                }
            }
        }
        level[k] = lv;
    }
    let nlevels = level.iter().max().map_or(0, |m| m + 1) as usize;
    // each component runs once its callees' components are done (a count of the cross-component edges left), on
    // the pool: no barrier between levels
    let nc = comps.len();
    let mut pending: Vec<u32> = vec![0; nc];
    let mut dep_at = vec![0u32; nc + 1];
    for (e, &k) in comp_of.iter().enumerate() {
        for &g in &adj[at[e] as usize..at[e + 1] as usize] {
            let kk = comp_of[g as usize];
            if kk != k {
                pending[k as usize] += 1;
                dep_at[kk as usize + 1] += 1;
            }
        }
    }
    for k in 0..nc {
        dep_at[k + 1] += dep_at[k];
    }
    let mut dep = vec![0u32; dep_at[nc] as usize];
    let mut fill = dep_at.clone();
    for (e, &k) in comp_of.iter().enumerate() {
        for &g in &adj[at[e] as usize..at[e + 1] as usize] {
            let kk = comp_of[g as usize];
            if kk != k {
                dep[fill[kk as usize] as usize] = k;
                fill[kk as usize] += 1;
            }
        }
    }
    drop(fill);
    let roots: Vec<u32> = (0..nc as u32).filter(|&k| pending[k as usize] == 0).collect();
    let pending: Vec<AtomicU32> = pending.into_iter().map(AtomicU32::new).collect();
    let done: Vec<OnceLock<Summary>> = (0..n).map(|_| OnceLock::new()).collect();
    let wave = Wave { prog, comps: &comps, comp_of: &comp_of, pos: &pos, fallthrough: &fallthrough, pending: &pending, dep_at: &dep_at, dep: &dep, done: &done };
    rayon::scope(|sc| {
        for &k in &roots {
            let w = &wave;
            sc.spawn(move |sc| w.run(k, sc));
        }
    });
    let mut sums: Vec<Summary> = done.into_iter().map(|s| s.into_inner().expect("a component never ran")).collect();
    let t2 = std::time::Instant::now();
    // results: what callers read after their calls, and what callers pass on as their own results
    // (and the calls whose return falls into another function's entry: a fact of the fixed point's returns)
    type Reads = (Vec<(FnId, u128, u128)>, Vec<FnId>);
    let reads: Vec<Reads> = {
        let done: &[Summary] = &sums;
        (0..n as FnId)
            .into_par_iter()
            .map_init(Worker::default, |w, e| {
                w.flow(0, prog, e);
                let f = &w.flows[0];
                let ctx = Sums { prog, done: Done::Plain(done), comp_of: &comp_of, pos: &pos, cur: u32::MAX, local: &[] };
                let falls = (0..f.n)
                    .filter_map(|k| {
                        let c = f.ctail[k]?.func()?;
                        (f.kind[k] == K::Svc || callee_of(f, k, &ctx).returns).then_some(c)
                    })
                    .collect();
                (call_reads(f, &ctx, &mut w.sc), falls)
            })
            .collect()
    };
    let mut passes: Vec<(FnId, FnId, u128)> = Vec::new();
    let mut falls = Vec::new();
    for (e, (rs, fl)) in reads.into_iter().enumerate() {
        for (c, m, pt) in rs {
            sums[c as usize].results |= m;
            passes.push((e as FnId, c, pt));
        }
        falls.extend(fl);
    }
    for (e, g) in graph.iter().enumerate() {
        passes.extend(g.tails.iter().map(|&c| (e as FnId, c, ALL)));
    }
    propagate_results(&mut sums, &passes);
    // a call whose return falls into another function's entry: that entry is entered by falling through
    for c in falls {
        sums[c as usize].demote |= why::FALLTHROUGH_ENTRY;
    }
    let t3 = std::time::Instant::now();
    let stats = Stats {
        functions: n,
        sccs: comps.len(),
        largest: comps.iter().map(|c| c.len()).max().unwrap_or(0),
        levels: nlevels,
        noreturn_by_callers: nbc,
        graph_s: (t1 - t0).as_secs_f64(),
        fixed_s: (t2 - t1).as_secs_f64(),
        results_s: (t3 - t2).as_secs_f64(),
    };
    (sums, stats)
}

/// [(callee, what the caller reads after the call, what may reach the caller's returns unwritten)] of a function's
/// calls of lifted functions (not PATCH_FUNCs: their result is declared) (summary.call_reads)
fn call_reads(f: &Flow, s: &Sums, sc: &mut Scratch) -> Vec<(FnId, u128, u128)> {
    let mut out = Vec::new();
    let mut have = false;
    for k in 0..f.n {
        let c = match (f.kind[k], f.callee[k]) {
            (K::Call, Callee::One(t)) if t.cls == Cls::Fn => t.id,
            _ => continue,
        };
        if !have {
            liveness(f, s, 0, &mut sc.live);
            liveness(f, s, ALL, &mut sc.live2);
            have = true;
        }
        let (mut o, mut p) = (0, 0);
        for &j in f.succs(k) {
            o |= sc.live[j as usize];
            p |= sc.live2[j as usize];
        }
        if let Some(a) = f.after[k] {
            o = a.0 | (o & !a.1);
            p = a.0 | (p & !a.1);
        }
        let g = s.get(c);
        out.push((c, o & g.clobbers, p & g.clobbers));
    }
    out
}

/// the AAPCS64's results for callers the program cannot see; then, to a fixed point, a caller's results that a
/// callee's value may become (summary.propagate_results)
fn propagate_results(sums: &mut [Summary], passes: &[(FnId, FnId, u128)]) {
    for s in sums.iter_mut() {
        s.results = (s.results | (ABI_RESULTS & s.clobbers)) & !s.preserved;
    }
    let mut changed = true;
    while changed {
        changed = false;
        for &(e, c, m) in passes {
            let er = sums[e as usize].results;
            let g = &mut sums[c as usize];
            let r = g.results | (er & m & g.clobbers & !g.preserved);
            if r != g.results {
                g.results = r;
                changed = true;
            }
        }
    }
}

/// the demotion reasons as Python lists them
pub fn reason_names(d: u16) -> Vec<&'static str> {
    (0..REASONS.len()).filter(|&b| d >> b & 1 != 0).map(|b| REASONS[b]).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn masks_are_the_python_ones() {
        assert_eq!(CALLEE_SAVED, 0xff000bff80000);
        assert_eq!(crate::ir::SP as u32, 31);
        assert_eq!(ARGS, 0xff0800001ff);
    }

    #[test]
    fn reasons_sorted() {
        let mut v = REASONS.to_vec();
        v.sort();
        assert_eq!(v, REASONS.to_vec());
    }

    #[test]
    fn def_use_of_templates() {
        let mut du = DefUse::new();
        // add x0, x1, #1
        let i = decode(0x91000420).unwrap();
        assert_eq!(du.get(&i), (xm(1), xm(0)));
        // bl: writes x30
        let i = decode(0x94000001).unwrap();
        assert_eq!(du.get(&i), (0, X30));
        // b.eq reads the flags its condition reads
        let i = decode(0x54000040).unwrap();
        assert_ne!(du.get(&i).0 & NZCV, 0);
    }

    #[test]
    fn tarjan_order() {
        // 0 -> 1 -> 2 -> 1, 3 -> 0
        let at = [0u32, 1, 2, 3, 4];
        let adj = [1u32, 2, 1, 0];
        let c = sccs(4, &at, &adj);
        assert_eq!(c, vec![vec![2, 1], vec![0], vec![3]]);
    }
}
