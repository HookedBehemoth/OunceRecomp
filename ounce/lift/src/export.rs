//! The lift's typed records, in the shapes the export had them (ounce/lift/EXPORT.md): the scan's products (`Meta`),
//! the partitions' functions with the words decoded once, the call-boundary records with the register masks as u128,
//! every name a `Sym` (the `f_`/`sjcall_` ones as their address, the others interned in the partition's `Names`).

use crate::ir::Sym;
use serde::Deserialize;
use std::borrow::Cow;
use std::collections::HashMap;

#[derive(Deserialize, Debug)]
pub struct Meta {
    pub format: u32,
    #[serde(default)]
    pub commit: String,
    pub verify: bool,
    pub omit: bool,
    /// [symbol, value] of the answered functions (PATCH_ANSWER): the answers module
    pub answers: Vec<(String, u64)>,
    /// the lift's image.bin's sha256 (the image the Rust side builds must be the same)
    pub image_sha256: String,
    // ---- the patch scan's results the image stage reads (format 5)
    /// the symbols the host's HLE(...) and PATCH_ANSWER(...) replace
    pub hle_symbols: Vec<String>,
    /// nx2/horizon OS_NAMESPACES: an import of a free function of one is stubbed
    pub os_namespaces: Vec<String>,
    pub setjmp_names: Vec<String>,
    pub part_insns: usize,
    /// the Value probes and At rows, each with its function, in declaration order
    pub values: Vec<MetaValue>,
    pub rows: Vec<MetaAt>,
    /// [entry, adapter name, orig name] of the PATCH_FUNCs, by entry
    pub func_names: Vec<(u64, String, String)>,
    // ---- what the summaries and the call-boundary records read (format 3)
    pub noreturn_names: Vec<String>,
    pub noreturn_parts: Vec<String>,
    /// the HLE'd functions' declarations, by symbol
    pub hle: Vec<HleFact>,
    /// the patch rows' declared effects, by pc
    pub row_effects: Vec<RowEffect>,
    /// the PATCH_FUNCs' declarations, by entry
    pub patch_funcs: Vec<PatchDecl>,
}

#[derive(Deserialize, Debug, PartialEq)]
pub struct MetaValue {
    pub pc: u64,
    pub word: u32,
    #[serde(rename = "fn")]
    pub fname: String,
    pub args: Vec<String>,
    pub result: String,
    pub func: u64,
}

#[derive(Deserialize, Debug, PartialEq)]
pub struct MetaAt {
    pub pc: u64,
    pub word: u32,
    #[serde(rename = "fn")]
    pub fname: String,
    pub mode: String,
    pub args: Vec<String>,
    pub result: Option<String>,
    #[serde(default)]
    pub io: Vec<String>,
    #[serde(default)]
    pub guest: Option<serde_json::Value>,
    pub func: u64,
}

/// a Value probe as the lift reads it
pub fn value_row(r: &MetaValue, names: &mut Names) -> ValueRow {
    ValueRow {
        pc: r.pc,
        word: r.word,
        func: names.sym(&r.fname),
        args: r.args.iter().map(|a| ValueArg::parse(a)).collect(),
        result: match Reg::parse(&r.result) {
            x @ (Reg::X(_) | Reg::W(_)) => x,
            _ => panic!("export: value result {}", r.result),
        },
    }
}

/// an At row as the lift reads it
pub fn at_row(r: &MetaAt, names: &mut Names) -> AtRow {
    AtRow {
        pc: r.pc,
        word: r.word,
        func: names.sym(&r.fname),
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
        guest: r.guest.as_ref().is_some_and(|g| !g.is_null()),
    }
}

/// an HLE'd function's declared facts: [symbol (its hook's C name less imp_), stops, answer, declared clobbering,
/// reads mask]
#[derive(Deserialize, Debug, Clone, PartialEq)]
pub struct HleFact(pub String, pub bool, pub Option<u64>, pub bool, #[serde(deserialize_with = "de_mask")] pub u128);

/// an HLE'd function at its address: [address, symbol, stops, answer, declared clobbering, reads mask]
#[derive(Debug)]
pub struct Hle(pub u64, pub String, pub bool, pub Option<u64>, pub bool, pub u128);

/// the guest functions host code calls: "computed" (any), or their addresses
#[derive(Deserialize, Debug, Clone, PartialEq)]
#[serde(untagged)]
pub enum GuestCalls {
    Computed(String),
    List(Vec<u64>),
}

/// a pc's rows' declared effects: [pc, before reads, after reads, before writes, after writes, guest calls, replace]
#[derive(Deserialize, Debug, PartialEq)]
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
#[derive(Deserialize, Debug, PartialEq)]
pub struct PatchDecl(
    pub u64,
    #[serde(deserialize_with = "de_mask")] pub u128,
    #[serde(deserialize_with = "de_mask")] pub u128,
    pub bool,
    pub Vec<u64>,
);

fn de_mask<'de, D: serde::Deserializer<'de>>(d: D) -> Result<u128, D::Error> {
    let s: Cow<str> = Deserialize::deserialize(d)?;
    u128::from_str_radix(&s, 16).map_err(serde::de::Error::custom)
}

pub fn expand(runs: &[(u64, u64)]) -> Vec<u64> {
    let n: u64 = runs.iter().map(|r| r.1).sum();
    let mut pcs = Vec::with_capacity(n as usize);
    for &(a, n) in runs {
        pcs.extend((0..n).map(|k| a + 4 * k));
    }
    pcs
}

/// the names of a partition's symbols other than `f_<addr>` and `sjcall_<addr>`
#[derive(Default, Debug, Clone)]
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

#[derive(Clone)]
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

#[derive(Deserialize, Debug, Clone)]
pub struct Plt {
    pub target: u64,
    pub slot: u64,
    pub target_is_code: bool,
}

#[derive(Debug, Clone)]
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

#[derive(Debug, Clone)]
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
