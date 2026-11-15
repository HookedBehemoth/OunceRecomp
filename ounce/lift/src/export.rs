//! The lift export (ounce/lift/EXPORT.md): meta.json and parts/<name>.json, read into typed records.
//!
//! A partition's JSON is parsed borrowing from the file's bytes, then turned into the records the lift reads: the
//! words decoded once, the register masks as u128, every name a `Sym` (the `f_`/`sjcall_` ones as their address, the
//! others interned in the partition's `Names`).

use crate::ir::Sym;
use serde::Deserialize;
use std::borrow::Cow;
use std::collections::HashMap;
use std::path::Path;

#[derive(Deserialize, Debug)]
pub struct Meta {
    pub format: u32,
    #[serde(default)]
    pub commit: String,
    pub verify: bool,
    pub omit: bool,
    pub code_targets: Vec<u64>,
    pub sj_rets: Vec<[u64; 5]>,
    /// [entry, symbol, value] of the answered functions (PATCH_ANSWER), by entry: the answers module
    #[serde(default)]
    pub answers: Vec<(u64, String, u64)>,
    pub partitions: Vec<PartInfo>,
    // ---- what the summaries and the call-boundary records read (format 3)
    /// every function entry of the program (lifted and hooked), sorted
    pub entries: Vec<u64>,
    /// [stub, target, slot] of the PLT stubs, by stub
    pub plt: Vec<(u64, u64, u64)>,
    /// [entry, symbol] of the image's symbols: the noreturn seeds match them
    pub names: Vec<(u64, String)>,
    pub noreturn_names: Vec<String>,
    pub noreturn_parts: Vec<String>,
    /// the HLE'd functions, by address
    pub hle: Vec<Hle>,
    /// the patch rows' declared effects, by pc
    pub row_effects: Vec<RowEffect>,
    /// the PATCH_FUNCs' declarations, by entry
    pub patch_funcs: Vec<PatchDecl>,
    /// the bodies the summaries see where they differ from the lifted ones
    pub summary_bodies: Vec<SummaryBody>,
}

/// an HLE'd function: [address, symbol, stops, answer, declared clobbering, reads mask]
#[derive(Deserialize, Debug)]
pub struct Hle(pub u64, pub String, pub bool, pub Option<u64>, pub bool, #[serde(deserialize_with = "de_mask")] pub u128);

/// the guest functions host code calls: "computed" (any), or their addresses
#[derive(Deserialize, Debug, Clone)]
#[serde(untagged)]
pub enum GuestCalls {
    Computed(String),
    List(Vec<u64>),
}

/// a pc's rows' declared effects: [pc, before reads, after reads, before writes, after writes, guest calls, replace]
#[derive(Deserialize, Debug)]
pub struct RowEffect(
    pub u64,
    #[serde(deserialize_with = "de_mask")] pub u128,
    #[serde(deserialize_with = "de_mask")] pub u128,
    #[serde(deserialize_with = "de_mask")] pub u128,
    #[serde(deserialize_with = "de_mask")] pub u128,
    pub GuestCalls,
    pub bool,
);

/// a PATCH_FUNC: [entry, reads mask, result mask, orig, guest callees]
#[derive(Deserialize, Debug)]
pub struct PatchDecl(
    pub u64,
    #[serde(deserialize_with = "de_mask")] pub u128,
    #[serde(deserialize_with = "de_mask")] pub u128,
    pub bool,
    pub Vec<u64>,
);

#[derive(Deserialize, Debug)]
pub struct SummaryBody {
    pub entry: u64,
    pub pcs: Vec<(u64, u64)>,
    pub words: String,
}

impl SummaryBody {
    /// (pcs, words)
    pub fn body(&self) -> (Vec<u64>, Vec<u32>) {
        let pcs = expand(&self.pcs);
        let w = self.words.as_bytes();
        assert_eq!(w.len(), 8 * pcs.len(), "export: summary body f_{:08x}", self.entry);
        (pcs, w.as_chunks::<8>().0.iter().map(|h| hex_word(h)).collect())
    }
}

fn de_mask<'de, D: serde::Deserializer<'de>>(d: D) -> Result<u128, D::Error> {
    let s: Cow<str> = Deserialize::deserialize(d)?;
    u128::from_str_radix(&s, 16).map_err(serde::de::Error::custom)
}

fn expand(runs: &[(u64, u64)]) -> Vec<u64> {
    let n: u64 = runs.iter().map(|r| r.1).sum();
    let mut pcs = Vec::with_capacity(n as usize);
    for &(a, n) in runs {
        pcs.extend((0..n).map(|k| a + 4 * k));
    }
    pcs
}

#[derive(Deserialize, Debug)]
pub struct PartInfo {
    pub name: String,
    pub tag: String,
    pub index: u32,
    pub functions: u32,
    pub insns: u64,
}

/// the names of a partition's symbols other than `f_<addr>` and `sjcall_<addr>`
#[derive(Default, Debug)]
pub struct Names {
    list: Vec<Box<str>>,
    index: HashMap<Box<str>, u32>,
}

impl Names {
    /// the symbol of a name: `f_` or `sjcall_` and the address as `%08x` print it are the address
    pub fn sym(&mut self, s: &str) -> Sym {
        fn addr(s: &str) -> Option<u64> {
            let a = u64::from_str_radix(s, 16).ok()?;
            (s.len() >= 8 && format!("{a:08x}") == s).then_some(a)
        }
        if let Some(a) = s.strip_prefix("f_").and_then(addr) {
            return Sym::Entry(a);
        }
        if let Some(a) = s.strip_prefix("sjcall_").and_then(addr) {
            return Sym::SjCall(a);
        }
        if let Some(&k) = self.index.get(s) {
            return Sym::Name(k);
        }
        let k = self.list.len() as u32;
        self.list.push(s.into());
        self.index.insert(s.into(), k);
        Sym::Name(k)
    }
    pub fn name(&self, k: u32) -> &str {
        &self.list[k as usize]
    }
}

pub struct Part {
    pub name: String,
    pub functions: Vec<Func>,
    pub names: Names,
}

/// why a call-boundary record is what it is (bounds.Bound.kind)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BoundKind {
    Lifted,
    Noreturn,
    Hle,
    PatchFunc,
    SetjmpOwner,
    NoSummary,
    Demoted,
    HleClobbering,
    PatchFuncUnproven,
    Answer,
    Stop,
}

impl BoundKind {
    pub fn name(self) -> &'static str {
        match self {
            BoundKind::Lifted => "lifted",
            BoundKind::Noreturn => "noreturn",
            BoundKind::Hle => "HLE",
            BoundKind::PatchFunc => "PATCH_FUNC",
            BoundKind::SetjmpOwner => "setjmp owner",
            BoundKind::NoSummary => "no summary",
            BoundKind::Demoted => "demoted",
            BoundKind::HleClobbering => "HLE declared clobbering",
            BoundKind::PatchFuncUnproven => "PATCH_FUNC with orig, body not proven",
            BoundKind::Answer => "answer",
            BoundKind::Stop => "stop",
        }
    }
    fn parse(s: &str) -> BoundKind {
        use BoundKind::*;
        [Lifted, Noreturn, Hle, PatchFunc, SetjmpOwner, NoSummary, Demoted, HleClobbering, PatchFuncUnproven, Answer, Stop]
            .into_iter()
            .find(|k| k.name() == s)
            .unwrap_or_else(|| panic!("export: record kind {s}"))
    }
}

/// a call-boundary record (bounds.Bound)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Bound {
    pub kind: BoundKind,
    pub addr: u64,
    pub reads: u128,
    pub clobbers: u128,
    pub keeps: u128,
    pub must: u128,
    pub results: u128,
    pub noreturn: bool,
    /// kind "stop": a call of it is a noreturn call
    pub stop: bool,
    pub symbol: Sym,
    pub tail: Sym,
    /// kind "answer" (PATCH_ANSWER): the constant x0 is answered with; a call of it is no call
    pub answer: Option<u64>,
}

#[derive(Deserialize, Debug)]
pub struct Plt {
    pub target: u64,
    pub slot: u64,
    pub target_is_code: bool,
}

#[derive(Debug)]
pub struct PatchFunc {
    pub name: Sym,
    pub orig_name: Sym,
}

/// a register a patch row names: Xn, Wn (its low 32 bits), Sn (f32 of Vn), Qn (Vn as <2 x i64>)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Reg {
    X(u32),
    W(u32),
    S(u32),
    Q(u32),
}

impl Reg {
    fn parse(s: &str) -> Reg {
        let n = || s[1..].parse().unwrap_or_else(|_| panic!("export: register {s}"));
        match s.as_bytes().first() {
            Some(b'x') => Reg::X(n()),
            Some(b'w') => Reg::W(n()),
            Some(b's') => Reg::S(n()),
            Some(b'q') => Reg::Q(n()),
            _ => panic!("export: register {s}"),
        }
    }
}

/// an argument of a Value row: `xN` (i64), `xN+K`, `sN` (f32)
#[derive(Debug, Clone, Copy)]
pub enum ValueArg {
    X(u32, Option<i128>),
    S(u32),
}

/// an argument of an At row: `#K` (i32), `sp`, `[xN+K]` / `[sp+K]` (the i64 there; base None: sp), or a register
#[derive(Debug, Clone, Copy)]
pub enum AtArg {
    Imm(i128),
    Sp,
    Load(Option<u32>, i128),
    Reg(Reg),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    Before,
    After,
    Instead,
}

/// K decimal or 0x hex
fn int0(s: &str) -> i128 {
    match s.strip_prefix("0x") {
        Some(h) => i128::from_str_radix(h, 16),
        None => s.parse(),
    }
    .unwrap_or_else(|_| panic!("export: constant {s}"))
}

impl ValueArg {
    fn parse(a: &str) -> ValueArg {
        match a.strip_prefix('x') {
            Some(r) => match r.split_once('+') {
                Some((n, o)) => ValueArg::X(n.parse().expect(a), Some(o.parse().expect(a))),
                None => ValueArg::X(r.parse().expect(a), None),
            },
            None => match Reg::parse(a) {
                Reg::S(n) => ValueArg::S(n),
                _ => panic!("export: value argument {a}"),
            },
        }
    }
}

impl AtArg {
    fn parse(a: &str) -> AtArg {
        if let Some(k) = a.strip_prefix('#') {
            return AtArg::Imm(int0(k));
        }
        if a == "sp" {
            return AtArg::Sp;
        }
        if let Some(m) = a.strip_prefix('[') {
            let (base, off) = m.trim_end_matches(']').split_once('+').expect(a);
            let base = if base == "sp" {
                None
            } else {
                match Reg::parse(base) {
                    Reg::X(n) => Some(n),
                    _ => panic!("export: at argument {a}"),
                }
            };
            return AtArg::Load(base, int0(off));
        }
        AtArg::Reg(Reg::parse(a))
    }
}

#[derive(Debug, Clone)]
pub struct ValueRow {
    pub pc: u64,
    pub word: u32,
    pub func: Sym,
    pub args: Vec<ValueArg>,
    /// Xn or Wn
    pub result: Reg,
}

#[derive(Debug, Clone)]
pub struct AtRow {
    pub pc: u64,
    pub word: u32,
    pub func: Sym,
    pub mode: Mode,
    pub args: Vec<AtArg>,
    pub result: Option<Reg>,
    /// Xn, Wn or Qn
    pub io: Vec<Reg>,
    /// the row's host function calls guest code (`guest` not null): it takes the context
    pub guest: bool,
}

#[derive(Debug)]
pub struct Func {
    pub entry: u64,
    /// the body's pcs, ascending
    pub pcs: Vec<u64>,
    /// the instruction words, parallel to pcs
    pub words: Vec<u32>,
    pub plt: Option<Plt>,
    pub body_entries: Vec<u64>,
    pub jt: Vec<(u64, Vec<u64>)>,
    pub jt_index: Vec<(u64, u64, u32, u32, Vec<u64>)>,
    pub code_sites: Vec<(u64, u64, Option<u64>)>,
    pub values: Vec<ValueRow>,
    pub rows: Vec<AtRow>,
    pub func: Option<PatchFunc>,
}

// ---- the JSON as written, borrowed from the file's bytes

fn mask(s: &str) -> u128 {
    u128::from_str_radix(s, 16).unwrap_or_else(|e| panic!("export: mask {s}: {e}"))
}

#[derive(Deserialize)]
struct RawPart<'a> {
    #[serde(borrow)]
    name: Cow<'a, str>,
    #[serde(borrow)]
    functions: Vec<RawFunc<'a>>,
}

#[derive(Deserialize)]
struct RawBound<'a> {
    addr: u64,
    #[serde(borrow)]
    kind: Cow<'a, str>,
    #[serde(borrow)]
    reads: Cow<'a, str>,
    #[serde(borrow)]
    clobbers: Cow<'a, str>,
    #[serde(borrow)]
    keeps: Cow<'a, str>,
    #[serde(borrow)]
    must: Cow<'a, str>,
    #[serde(borrow)]
    results: Cow<'a, str>,
    noreturn: bool,
    #[serde(borrow)]
    symbol: Cow<'a, str>,
    #[serde(borrow)]
    tail: Cow<'a, str>,
    #[serde(default)]
    answer: Option<u64>,
}

#[derive(Deserialize)]
struct RawPatchFunc<'a> {
    #[serde(borrow)]
    name: Cow<'a, str>,
    #[serde(borrow)]
    orig_name: Cow<'a, str>,
}

#[derive(Deserialize)]
struct RawValueRow<'a> {
    pc: u64,
    word: u32,
    #[serde(rename = "fn", borrow)]
    func: Cow<'a, str>,
    args: Vec<String>,
    result: String,
}

#[derive(Deserialize)]
struct RawAtRow<'a> {
    pc: u64,
    word: u32,
    #[serde(rename = "fn", borrow)]
    func: Cow<'a, str>,
    mode: String,
    args: Vec<String>,
    result: Option<String>,
    #[serde(default)]
    io: Vec<String>,
    #[serde(default)]
    guest: Option<serde::de::IgnoredAny>,
}

#[derive(Deserialize)]
struct RawFunc<'a> {
    entry: u64,
    pcs: Vec<(u64, u64)>,
    #[serde(borrow)]
    words: Cow<'a, str>,
    plt: Option<Plt>,
    body_entries: Vec<u64>,
    jt: Vec<(u64, Vec<u64>)>,
    jt_index: Vec<(u64, u64, u32, u32, Vec<u64>)>,
    code_sites: Vec<(u64, u64, Option<u64>)>,
    #[serde(borrow)]
    values: Vec<RawValueRow<'a>>,
    #[serde(borrow)]
    rows: Vec<RawAtRow<'a>>,
    #[serde(borrow)]
    func: Option<RawPatchFunc<'a>>,
}

impl RawBound<'_> {
    fn typed(&self, names: &mut Names) -> Bound {
        Bound {
            kind: BoundKind::parse(&self.kind),
            addr: self.addr,
            reads: mask(&self.reads),
            clobbers: mask(&self.clobbers),
            keeps: mask(&self.keeps),
            must: mask(&self.must),
            results: mask(&self.results),
            noreturn: self.noreturn,
            stop: self.kind == "stop",
            symbol: names.sym(&self.symbol),
            tail: names.sym(&self.tail),
            answer: self.answer,
        }
    }
}

fn hex_word(h: &[u8]) -> u32 {
    h.iter().fold(0u32, |w, &c| {
        let d = match c {
            b'0'..=b'9' => c - b'0',
            b'a'..=b'f' => c - b'a' + 10,
            _ => panic!("export: word digit {}", c as char),
        };
        w << 4 | d as u32
    })
}

impl RawFunc<'_> {
    fn typed(self, names: &mut Names) -> Func {
        let pcs = expand(&self.pcs);
        let w = self.words.as_bytes();
        assert_eq!(w.len(), 8 * pcs.len(), "export: f_{:08x}'s words", self.entry);
        let words = w.as_chunks::<8>().0.iter().map(|h| hex_word(h)).collect();
        Func {
            entry: self.entry,
            pcs,
            words,
            plt: self.plt,
            body_entries: self.body_entries,
            jt: self.jt,
            jt_index: self.jt_index,
            code_sites: self.code_sites,
            values: self
                .values
                .into_iter()
                .map(|r| ValueRow {
                    pc: r.pc,
                    word: r.word,
                    func: names.sym(&r.func),
                    args: r.args.iter().map(|a| ValueArg::parse(a)).collect(),
                    result: match Reg::parse(&r.result) {
                        x @ (Reg::X(_) | Reg::W(_)) => x,
                        _ => panic!("export: value result {}", r.result),
                    },
                })
                .collect(),
            rows: self
                .rows
                .into_iter()
                .map(|r| AtRow {
                    pc: r.pc,
                    word: r.word,
                    func: names.sym(&r.func),
                    mode: match &*r.mode {
                        "before" => Mode::Before,
                        "after" => Mode::After,
                        "instead" => Mode::Instead,
                        m => panic!("export: row mode {m}"),
                    },
                    args: r.args.iter().map(|a| AtArg::parse(a)).collect(),
                    result: r.result.as_deref().map(Reg::parse),
                    io: r
                        .io
                        .iter()
                        .map(|s| match Reg::parse(s) {
                            Reg::S(_) => panic!("export: io register {s}"),
                            x => x,
                        })
                        .collect(),
                    guest: r.guest.is_some(),
                })
                .collect(),
            func: self.func.map(|p| PatchFunc { name: names.sym(&p.name), orig_name: names.sym(&p.orig_name) }),
        }
    }
}

pub fn read_meta(dir: &Path) -> Result<Meta, String> {
    let p = dir.join("meta.json");
    let s = std::fs::read(&p).map_err(|e| format!("{}: {e}", p.display()))?;
    serde_json::from_slice(&s).map_err(|e| format!("{}: {e}", p.display()))
}

pub fn read_part(dir: &Path, name: &str) -> Result<Part, String> {
    let p = dir.join("parts").join(format!("{name}.json"));
    let s = std::fs::read(&p).map_err(|e| format!("{}: {e}", p.display()))?;
    let raw: RawPart = serde_json::from_slice(&s).map_err(|e| format!("{}: {e}", p.display()))?;
    let mut names = Names::default();
    let functions = raw.functions.into_iter().map(|f| f.typed(&mut names)).collect();
    Ok(Part { name: raw.name.into_owned(), functions, names })
}

/// the diagnostic's reference (check/bounds.json): Python's records of every callee the partitions read, and the
/// PATCH_FUNCs' own records; the symbols as names
pub struct CheckBounds {
    pub bounds: Vec<Bound>,
    pub own: Vec<Bound>,
    pub names: Names,
}

pub fn read_check_bounds(dir: &Path) -> Result<CheckBounds, String> {
    #[derive(Deserialize)]
    struct Raw<'a> {
        #[serde(borrow)]
        bounds: Vec<RawBound<'a>>,
        #[serde(borrow)]
        own: Vec<RawBound<'a>>,
    }
    let p = dir.join("check").join("bounds.json");
    let s = std::fs::read(&p).map_err(|e| format!("{}: {e}", p.display()))?;
    let raw: Raw = serde_json::from_slice(&s).map_err(|e| format!("{}: {e}", p.display()))?;
    let mut names = Names::default();
    let bounds = raw.bounds.iter().map(|b| b.typed(&mut names)).collect();
    let own = raw.own.iter().map(|b| b.typed(&mut names)).collect();
    Ok(CheckBounds { bounds, own, names })
}

/// a packed summary as check/summaries.json has it (summary.pack): [entry, inputs, clobbers, mustdef, preserved,
/// results, returns, demote, unknown, frame, pairs, boundary, fp, writes_fpcr]
#[derive(Deserialize, Debug)]
pub struct PackedSummary(
    pub u64,
    #[serde(deserialize_with = "de_mask")] pub u128,
    #[serde(deserialize_with = "de_mask")] pub u128,
    #[serde(deserialize_with = "de_mask")] pub u128,
    #[serde(deserialize_with = "de_mask")] pub u128,
    #[serde(deserialize_with = "de_mask")] pub u128,
    pub bool,
    pub Vec<String>,
    pub bool,
    pub i64,
    pub u32,
    pub bool,
    pub bool,
    pub bool,
);

pub fn read_check_summaries(dir: &Path) -> Result<Vec<PackedSummary>, String> {
    let p = dir.join("check").join("summaries.json");
    let s = std::fs::read(&p).map_err(|e| format!("{}: {e}", p.display()))?;
    serde_json::from_slice(&s).map_err(|e| format!("{}: {e}", p.display()))
}
