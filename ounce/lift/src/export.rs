//! The lift export (ounce/lift/EXPORT.md): meta.json and parts/<name>.json, read into plain structs.

use serde::Deserialize;
use std::collections::HashMap;
use std::path::Path;

fn mask<'de, D: serde::Deserializer<'de>>(d: D) -> Result<u128, D::Error> {
    let s = String::deserialize(d)?;
    u128::from_str_radix(&s, 16).map_err(serde::de::Error::custom)
}

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

#[derive(Deserialize, Debug)]
pub struct Part {
    pub name: String,
    pub functions: Vec<Func>,
    pub bounds: Vec<Bound>,
    pub redirect: Vec<(u64, u64)>,
}

/// a call-boundary record (bounds.Bound)
#[derive(Deserialize, Debug, Clone)]
pub struct Bound {
    pub addr: u64,
    pub kind: String,
    #[serde(deserialize_with = "mask")]
    pub reads: u128,
    #[serde(deserialize_with = "mask")]
    pub clobbers: u128,
    #[serde(deserialize_with = "mask")]
    pub keeps: u128,
    #[serde(deserialize_with = "mask")]
    pub must: u128,
    #[serde(deserialize_with = "mask")]
    pub results: u128,
    pub noreturn: bool,
    pub symbol: String,
    pub tail: String,
    /// kind "answer" (PATCH_ANSWER): the constant x0 is answered with; a call of it is no call
    #[serde(default)]
    pub answer: Option<u64>,
}

#[derive(Deserialize, Debug)]
pub struct Plt {
    pub target: u64,
    pub slot: u64,
    pub target_is_code: bool,
}

#[derive(Deserialize, Debug)]
pub struct PatchFunc {
    pub name: String,
    pub orig_name: String,
}

#[derive(Deserialize, Debug, Clone)]
pub struct ValueRow {
    pub pc: u64,
    pub word: u32,
    #[serde(rename = "fn")]
    pub func: String,
    pub args: Vec<String>,
    pub result: String,
}

#[derive(Deserialize, Debug, Clone)]
pub struct AtRow {
    pub pc: u64,
    pub word: u32,
    #[serde(rename = "fn")]
    pub func: String,
    pub mode: String,
    pub args: Vec<String>,
    pub result: Option<String>,
    #[serde(default)]
    pub io: Vec<String>,
    #[serde(default)]
    pub guest: serde_json::Value,
}

#[derive(Deserialize, Debug)]
pub struct Func {
    pub entry: u64,
    /// runs [first pc, count]
    pub pcs: Vec<(u64, u64)>,
    pub words: String,
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

impl Func {
    /// (pcs, words) of the body
    pub fn body(&self) -> (Vec<u64>, Vec<u32>) {
        let mut pcs = Vec::new();
        for &(a, n) in &self.pcs {
            pcs.extend((0..n).map(|k| a + 4 * k));
        }
        let w = self.words.as_bytes();
        let words = (0..pcs.len())
            .map(|k| u32::from_str_radix(std::str::from_utf8(&w[8 * k..8 * k + 8]).unwrap(), 16).unwrap())
            .collect();
        (pcs, words)
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
    serde_json::from_slice(&s).map_err(|e| format!("{}: {e}", p.display()))
}

/// the partition's records by address, and callee(t)
pub struct Calls {
    pub bounds: HashMap<u64, Bound>,
    pub redirect: HashMap<u64, u64>,
}

impl Calls {
    pub fn new(p: &Part) -> Calls {
        Calls {
            bounds: p.bounds.iter().map(|b| (b.addr, b.clone())).collect(),
            redirect: p.redirect.iter().copied().collect(),
        }
    }
    pub fn callee(&self, t: u64) -> u64 {
        self.redirect.get(&t).copied().unwrap_or(t)
    }
    pub fn bound(&self, c: u64) -> &Bound {
        self.bounds.get(&c).unwrap_or_else(|| panic!("export: no record of {c:#x}"))
    }
}
