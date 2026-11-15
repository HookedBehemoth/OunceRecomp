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
    /// sorted by addr
    pub bounds: Vec<Bound>,
    /// sorted by t
    pub redirect: Vec<(u64, u64)>,
    pub names: Names,
}

/// a call-boundary record (bounds.Bound)
#[derive(Debug, Clone)]
pub struct Bound {
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

#[derive(Debug, Clone)]
pub struct ValueRow {
    pub pc: u64,
    pub word: u32,
    pub func: Sym,
    pub args: Vec<String>,
    pub result: String,
}

#[derive(Debug, Clone)]
pub struct AtRow {
    pub pc: u64,
    pub word: u32,
    pub func: Sym,
    pub mode: String,
    pub args: Vec<String>,
    pub result: Option<String>,
    pub io: Vec<String>,
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
    pub own: Bound,
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
    #[serde(borrow)]
    bounds: Vec<RawBound<'a>>,
    redirect: Vec<(u64, u64)>,
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
    #[serde(borrow)]
    own: RawBound<'a>,
}

impl RawBound<'_> {
    fn typed(&self, names: &mut Names) -> Bound {
        Bound {
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
        let n: u64 = self.pcs.iter().map(|r| r.1).sum();
        let mut pcs = Vec::with_capacity(n as usize);
        for &(a, n) in &self.pcs {
            pcs.extend((0..n).map(|k| a + 4 * k));
        }
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
                .map(|r| ValueRow { pc: r.pc, word: r.word, func: names.sym(&r.func), args: r.args, result: r.result })
                .collect(),
            rows: self
                .rows
                .into_iter()
                .map(|r| AtRow {
                    pc: r.pc,
                    word: r.word,
                    func: names.sym(&r.func),
                    mode: r.mode,
                    args: r.args,
                    result: r.result,
                    io: r.io,
                    guest: r.guest.is_some(),
                })
                .collect(),
            func: self.func.map(|p| PatchFunc { name: names.sym(&p.name), orig_name: names.sym(&p.orig_name) }),
            own: self.own.typed(names),
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
    let bounds: Vec<Bound> = raw.bounds.iter().map(|b| b.typed(&mut names)).collect();
    assert!(bounds.windows(2).all(|w| w[0].addr < w[1].addr), "export: bounds not sorted");
    assert!(raw.redirect.windows(2).all(|w| w[0].0 < w[1].0), "export: redirect not sorted");
    let functions = raw.functions.into_iter().map(|f| f.typed(&mut names)).collect();
    Ok(Part { name: raw.name.into_owned(), functions, bounds, redirect: raw.redirect, names })
}

impl Part {
    /// callee(t): the PLT stubs followed
    pub fn callee(&self, t: u64) -> u64 {
        match self.redirect.binary_search_by_key(&t, |r| r.0) {
            Ok(k) => self.redirect[k].1,
            Err(_) => t,
        }
    }
    /// bound(c): the record of a callee
    pub fn bound(&self, c: u64) -> &Bound {
        match self.bounds.binary_search_by_key(&c, |b| b.addr) {
            Ok(k) => &self.bounds[k],
            Err(_) => panic!("export: no record of {c:#x}"),
        }
    }
}
