//! The shader pack: the GPU programs a game's romfs can draw with, each stage's bytes once, for the renderer to
//! translate and compile ahead of the game (the game's `--precompile`, docs/builder-plan.md "Shaders").
//!
//! Two sources make it up. The models (bfres): each material names a shading model of a shader archive (bfsha) and the
//! static options it sets; the programs of that model whose static key is the material's are the ones it can draw with
//! (gsys's own static options taken from the material's render infos where gsys sets them, any choice otherwise; the
//! dynamic ones, the pass and the skinning, are chosen per draw). The other shader archives (agl's BAHS, its NDSR
//! networks, the movie upscaler's NDVE, the BNSH inside particle sets and layouts) are taken whole. `Scope::All` takes
//! every program of every archive instead, bfsha included (the shader census's seed). A game whose patches make a kind
//! of archive or a file unreachable (OoT's and DKB's movies play without NDVE, OoT's NDSR stage runs one network of
//! three) leaves it out (`make`'s `left_out`).
//!
//! The pack is the seed format the shader tools read (`HKSEED01`): u32 programs, u32 stages; each stage {u64 code key,
//! u32 NVN stage, u32 control bytes, u32 code bytes, the control section (0xd8 bytes), the code area its +0x44 sizes};
//! each program {u32 count, u32 stage indices}; then the codes left out, each {u64 code key, the archive's magic (4
//! bytes)}, and their u32 count last (the renderer reads them from the end, to name one that is set all the same). A stage's code key is the renderer's: FNV-1a
//! 64 over the control section, then the code area. Each stage's origin (its archive and file) is `Pack::origins`.
//!
//! The layouts read here (nn::g3d2, bfres 10 and bfsha 9; offsets from each structure's start):
//! - bfres FMDL +0x38 its materials (0xb0 each), +0x6c their count; FMAT +0x10 the static shader variation, +0x40 / +0x48 /
//!   +0x50 the render info values, their counts and their offsets (u16 each).
//! - the variation: +0x00 the reflection, +0x28 the bool options (a bit each), +0x30 the other options' strings, +0x38 u16
//!   each option's index in the reflection's option dict (bools first), +0x46 u16 bool options, +0x48 u16 all options.
//! - the reflection: +0x00 the archive's name, +0x08 the shading model's, +0x10 the render infos (0x10 each: name, u8
//!   type, 2 string), +0x40 the static option dict, +0x48 u16 the render info count.
//! - Names are string offsets (u16 length, bytes); OoT's reflections give a 64-bit hash instead, which
//!   Shader/ExternalBinaryString.bfres resolves (+0xb8 its hashes, +0xc0 a dict of the names in the same order).
//! - bfsha +0x38 the archive's name, +0x48 its shading models (0x100 each), +0x78 u16 their count. A shading model: +0x00
//!   name, +0x08 / +0x18 its static / dynamic options (0x28 each: name, +0x08 the choice dict, +0x1a u16 default choice,
//!   +0x20 u32 mask, +0x24 u8 key word, +0x25 u8 shift), +0x88 the programs (0x40 each: +0x20 its BNSH variation), +0x90
//!   the key table (static words then dynamic, per program), +0xa8 its BNSH, +0xe4 / +0xe6 / +0xe8 u16 static options,
//!   dynamic options, programs, +0xea / +0xeb u8 the static / dynamic key's words.
//! - BNSH: +0x1c u32 size, +0x7c u32 variations, +0x80 their offset (0x40 each: +0x10 the program); a program's stage
//!   slots (+0x08 .. +0x48) each {+0x08 control offset, +0x10 code offset, +0x18 u32 code bytes, +0x1c u32 control bytes}.

use ounce_tex::romfs::{sarc_unpack, Dicts};
use rayon::prelude::*;
use std::collections::{BTreeSet, HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

const CONTROL: usize = 0xd8;
const CONTROL_MAGIC: [u8; 4] = [0x34, 0x12, 0x76, 0x98];
/// a variation's program's stage slots
const SLOTS: [usize; 9] = [8, 16, 24, 32, 40, 48, 56, 64, 72];

/// What goes into the pack.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Scope {
    /// the bfsha programs the models' materials reach, and every program of the other archives
    Reached,
    /// every program of every archive (the census's seed)
    All,
}

/// The kinds of shader archive a program comes from
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash, PartialOrd, Ord)]
pub enum Archive {
    /// a bfsha shading model's program
    Bfsha,
    /// agl's shader archive
    Bahs,
    /// agl's networks (the game's upscaler)
    Ndsr,
    /// the movie player's enhancement network
    Ndve,
    /// a BNSH of a particle set or a layout (or, for `Scope::All`, a bfsha's)
    Bnsh,
}

impl Archive {
    pub const ALL: [Archive; 5] = [Archive::Bfsha, Archive::Bahs, Archive::Ndsr, Archive::Ndve, Archive::Bnsh];
    /// its magic, as game.toml and the pack name it
    pub fn magic(self) -> &'static str {
        match self {
            Archive::Bfsha => "FSHA",
            Archive::Bahs => "BAHS",
            Archive::Ndsr => "NDSR",
            Archive::Ndve => "NDVE",
            Archive::Bnsh => "BNSH",
        }
    }
    pub fn from_magic(s: &str) -> Option<Archive> {
        Archive::ALL.into_iter().find(|a| a.magic() == s)
    }
}

/// What a game leaves out of its pack (game.toml's `[assets].shaders_left_out`): a kind of archive by its magic, or a
/// romfs file (`<file>:<member>` for a SARC's member), with every member inside it
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum LeftOut {
    Kind(Archive),
    File(String),
}

impl LeftOut {
    /// a magic, or a path (it has a `/`)
    pub fn parse(s: &str) -> Option<LeftOut> {
        Archive::from_magic(s).map(LeftOut::Kind).or_else(|| s.contains('/').then(|| LeftOut::File(s.to_string())))
    }
    pub fn text(&self) -> &str {
        match self {
            LeftOut::Kind(a) => a.magic(),
            LeftOut::File(f) => f,
        }
    }
    pub fn covers(&self, o: &Origin) -> bool {
        match self {
            LeftOut::Kind(a) => o.archive == *a,
            LeftOut::File(f) => o.file.strip_prefix(f.as_str()).is_some_and(|rest| rest.is_empty() || rest.starts_with(':')),
        }
    }
}

/// Where a program was found: the kind of archive and the romfs file (`<file>:<member>` inside a SARC)
#[derive(Clone, Debug)]
pub struct Origin {
    pub archive: Archive,
    pub file: String,
}

/// The renderer's code key of a stage: FNV-1a 64 over the control section, then the code area.
pub fn code_key(control: &[u8], area: &[u8]) -> u64 {
    let mut h = 0xcbf2_9ce4_8422_2325u64;
    for &b in control.iter().chain(area) {
        h = (h ^ b as u64).wrapping_mul(0x100_0000_01b3);
    }
    h
}

fn rd<const N: usize>(d: &[u8], o: u64) -> Option<[u8; N]> {
    let o = usize::try_from(o).ok()?;
    d.get(o..o.checked_add(N)?).map(|s| s.try_into().unwrap())
}
fn u8_(d: &[u8], o: u64) -> Option<u8> {
    rd::<1>(d, o).map(|b| b[0])
}
fn u16_(d: &[u8], o: u64) -> Option<u16> {
    rd(d, o).map(u16::from_le_bytes)
}
fn u32_(d: &[u8], o: u64) -> Option<u32> {
    rd(d, o).map(u32::from_le_bytes)
}
fn u64_(d: &[u8], o: u64) -> Option<u64> {
    rd(d, o).map(u64::from_le_bytes)
}
fn slice(d: &[u8], o: u64, n: u64) -> Option<&[u8]> {
    let (o, n) = (usize::try_from(o).ok()?, usize::try_from(n).ok()?);
    d.get(o..o.checked_add(n)?)
}

/// A file's strings: offsets to a u16 length and the bytes, or (OoT) a 64-bit hash of a name `names` holds.
struct Strs<'a> {
    d: &'a [u8],
    names: Option<&'a HashMap<u64, String>>,
}

impl Strs<'_> {
    fn at(&self, o: u64) -> Option<String> {
        if o == 0 {
            return None;
        }
        if o >= self.d.len() as u64 - 2 {
            return Some(self.names.and_then(|n| n.get(&o).cloned()).unwrap_or_else(|| format!("#{o:016x}")));
        }
        let n = u16_(self.d, o)? as u64;
        Some(slice(self.d, o + 2, n)?.iter().map(|&b| b as char).collect())
    }
    /// the string a pointer at `o` points to
    fn ptr(&self, o: u64) -> Option<String> {
        self.at(u64_(self.d, o)?)
    }
    /// a dict's keys in order
    fn dict_keys(&self, o: u64) -> Vec<String> {
        if o == 0 {
            return Vec::new();
        }
        let n = u32_(self.d, o + 4).unwrap_or(0) as u64;
        (0..n).map(|i| self.ptr(o + 8 + 16 * (i + 1) + 8).unwrap_or_default()).collect()
    }
}

/// A stage: its NVN stage, control section (0xd8 bytes) and code area.
#[derive(Clone)]
pub struct Stage {
    pub kind: u32,
    pub control: Vec<u8>,
    pub area: Vec<u8>,
}

/// The control section at `c` and the code area that follows `code` for its size, when they are one.
fn stage_at(d: &[u8], c: u64, code: u64) -> Option<Stage> {
    let control = slice(d, c, CONTROL as u64)?;
    if control[..4] != CONTROL_MAGIC {
        return None;
    }
    let kind = u16::from_le_bytes([control[0x56], control[0x57]]) as u32;
    let end = u32::from_le_bytes(control[0x44..0x48].try_into().unwrap()) as u64;
    if kind > 7 {
        return None;
    }
    Some(Stage { kind, control: control.to_vec(), area: slice(d, code, end)?.to_vec() })
}

// ---- the shader archives that are no bfsha (and, for Scope::All, the bfsha's BNSH too) ----

/// A BNSH's variations (its offsets are its own), each a program
fn bnsh_programs(d: &[u8], out: &mut Vec<Vec<Stage>>) -> Option<()> {
    if d.get(..4)? != b"BNSH" || d.get(12..14)? != [0xff, 0xfe] || d.get(96..100)? != b"grsc" {
        return None;
    }
    let size = u32_(d, 28)? as u64;
    let d = d.get(..size as usize)?;
    let (count, variations) = (u32_(d, 124)? as u64, u64_(d, 128)?);
    for i in 0..count {
        let Some(program) = u64_(d, variations + i * 64 + 16) else { continue };
        if program == 0 || program + 80 > size {
            continue;
        }
        let mut stages = Vec::new();
        for rel in SLOTS {
            let Some(at) = u64_(d, program + rel as u64).filter(|&o| o != 0 && o + 32 <= size) else { continue };
            let (Some(c), Some(x), Some(xn), Some(cn)) = (u64_(d, at + 8), u64_(d, at + 16), u32_(d, at + 24), u32_(d, at + 28)) else { continue };
            if !(0x60..=0x1000).contains(&cn) || c + cn as u64 > size || x + xn as u64 > size {
                continue;
            }
            // (the code area is what the control section sizes; one that runs past the stage's code is not read)
            if let Some(s) = stage_at(d, c, x).filter(|s| s.area.len() <= xn as usize) {
                stages.push(s);
            }
        }
        if !stages.is_empty() {
            out.push(stages);
        }
    }
    Some(())
}

/// agl's shader archive (BAHS): +0x14 the data start, +0x1c the code blob's start; at data +0x150 the program table's
/// offset (from there) and the stage count, the stages from data +0x158 ({u32 bytes to the next, .., u64 the code's
/// offset in the blob, u32, s32 its control section from +0x10}); the program table {u32 bytes, u32 programs, each {u32
/// bytes, u32, u32 kinds (a bit each), u32 first stage, ..}}: a program's variations are its run of stages up to the next
/// program's first, one stage of each of its kinds per variation.
fn bahs_programs(d: &[u8], out: &mut Vec<Vec<Stage>>) -> Option<()> {
    let (base, blob) = (u32_(d, 0x14)? as u64, u32_(d, 0x1c)? as u64);
    let (table, count) = (u32_(d, base + 0x150)? as u64, u32_(d, base + 0x154)? as u64);
    let mut at = base + 0x158;
    let mut stages = Vec::with_capacity(count as usize);
    for _ in 0..count {
        let step = u32_(d, at)? as u64;
        if step < 32 {
            return None;
        }
        let offset = u64_(d, at + 16)?;
        let rel = u32_(d, at + 28)? as i32 as i64;
        stages.push(stage_at(d, (at as i64 + 16 + rel) as u64, blob + offset)?);
        at += step;
    }
    let table = table + base + 0x150;
    let mut at = table + 8;
    let mut heads = Vec::new();
    for _ in 0..u32_(d, table + 4)? {
        let (size, kinds, first) = (u32_(d, at)? as u64, u32_(d, at + 8)?, u32_(d, at + 12)? as usize);
        heads.push((first, kinds.count_ones() as usize));
        at += size;
    }
    heads.sort();
    for (i, &(first, width)) in heads.iter().enumerate() {
        let last = heads.get(i + 1).map_or(count as usize, |h| h.0);
        let mut v = first;
        while v < last {
            if let Some(s) = stages.get(v..(v + width).min(stages.len())).filter(|s| !s.is_empty()) {
                out.push(s.to_vec());
            }
            v += width.max(1);
        }
    }
    Some(())
}

/// agl's networks (NDSR, at `base`): +0x0c the item count, items from +0x14 {u32 offset, u32 bytes << 4 | type}; version
/// 0x300 has no count and its items from +0x10 up to a zero one. Types 0 / 3 / 6 are a vertex / fragment / compute control
/// section, the item after each its code; a vertex and the fragment after it are a program, a compute stage is one alone.
fn ndsr_programs(d: &[u8], base: u64, out: &mut Vec<Vec<Stage>>) -> Option<()> {
    let mut items = Vec::new();
    let len = d.len() as u64;
    if u32_(d, base + 4)? == 0x300 {
        let mut at = base + 0x10;
        while at + 8 <= len {
            let o = u32_(d, at)? as u64;
            if o == 0 || o >= len - base {
                break;
            }
            items.push((o, u32_(d, at + 4)?));
            at += 8;
        }
    } else {
        for i in 0..u32_(d, base + 0x0c)? as u64 {
            items.push((u32_(d, base + 0x14 + 8 * i)? as u64, u32_(d, base + 0x18 + 8 * i)?));
        }
    }
    let mut graphics: Vec<Stage> = Vec::new();
    for w in items.windows(2) {
        let ((c, ct), (x, xt)) = (w[0], w[1]);
        if !matches!(ct & 15, 0 | 3 | 6) || xt & 15 != (ct & 15) + 1 {
            continue;
        }
        let Some(s) = stage_at(d, base + c, base + x) else { continue };
        match ct & 15 {
            6 => out.push(vec![s]),
            0 => {
                if !graphics.is_empty() {
                    out.push(std::mem::take(&mut graphics));
                }
                graphics = vec![s];
            }
            _ if !graphics.is_empty() => graphics.push(s),
            _ => {}
        }
    }
    if !graphics.is_empty() {
        out.push(graphics);
    }
    Some(())
}

/// The movie upscaler's networks (NDVE): an NDSR at +0x28, then tables {u32 stages, u32 blobs, blobs x {u32, u32}, stages
/// x {u32 control section from the table, u32 code from the control, u32 code bytes, u32 0}}; each control section's
/// entry is found from its code bytes (the pair "bytes, 0") and checked against its table's header.
fn ndve_programs(d: &[u8], out: &mut Vec<Vec<Stage>>) {
    if d.get(0x28..0x2c) == Some(b"NDSR") {
        ndsr_programs(d, 0x28, out);
    }
    for c in memchr::memmem::find_iter(d, &CONTROL_MAGIC) {
        let c = c as u64;
        let Some(end) = slice(d, c, CONTROL as u64).filter(|s| u16::from_le_bytes([s[0x56], s[0x57]]) <= 7).and_then(|_| u32_(d, c + 0x44)) else { continue };
        let mut pair = [0u8; 8];
        pair[..4].copy_from_slice(&end.to_le_bytes());
        for e in memchr::memmem::find_iter(d, &pair) {
            let Some(entry) = (e as u64).checked_sub(8).filter(|e| e % 4 == 0) else { continue };
            let (Some(to_control), Some(to_code)) = (u32_(d, entry), u32_(d, entry + 4)) else { continue };
            let (to_control, to_code) = (to_control as u64, to_code as u64);
            let Some(table) = c.checked_sub(to_control).filter(|t| to_control != 0 && t % 4 == 0) else { continue };
            if c + to_code + end as u64 > d.len() as u64 {
                continue;
            }
            let (Some(n), Some(blobs)) = (u32_(d, table), u32_(d, table + 4)) else { continue };
            let first = table + 8 + 8 * blobs as u64;
            if 0 < n && n < 256 && blobs < 16 && entry >= first && (entry - first) % 16 == 0 && (entry - first) / 16 < n as u64 {
                if let Some(s) = stage_at(d, c, c + to_code) {
                    out.push(vec![s]);
                }
                break;
            }
        }
    }
}

/// Every BNSH inside the data (a bfsha's, a particle set's inside its byml, a layout's)
fn bnsh_all(d: &[u8], out: &mut Vec<Vec<Stage>>) {
    for m in memchr::memmem::find_iter(d, b"BNSH") {
        if d.get(m + 96..m + 100) == Some(b"grsc") {
            if let Some(size) = u32_(d, m as u64 + 28) {
                if let Some(b) = d.get(m..m + size as usize) {
                    bnsh_programs(b, out);
                }
            }
        }
    }
}

/// The programs of one file (`file` its name), each with its origin
fn programs_in(d: &[u8], file: &str, out: &mut Vec<(Origin, Vec<Stage>)>) {
    if d.len() < 0x100 {
        return;
    }
    let mut found = Vec::new();
    let archive = match &d[..4] {
        b"BAHS" => {
            let _ = bahs_programs(d, &mut found);
            Archive::Bahs
        }
        b"NDSR" => {
            let _ = ndsr_programs(d, 0, &mut found);
            Archive::Ndsr
        }
        b"NDVE" => {
            ndve_programs(d, &mut found);
            Archive::Ndve
        }
        _ => {
            bnsh_all(d, &mut found);
            Archive::Bnsh
        }
    };
    out.extend(found.into_iter().map(|s| (Origin { archive, file: file.to_string() }, s)));
}

/// The data itself, or each member when it is an archive (SARCs inside SARCs, each member decompressed)
fn blobs(dicts: &Dicts, d: &[u8], file: &str, out: &mut Vec<(Origin, Vec<Stage>)>, depth: u32) {
    if d.starts_with(b"SARC") && depth < 4 {
        if let Ok(members) = sarc_unpack(d) {
            for (name, m) in members {
                if let Ok(m) = dicts.decompress(m) {
                    blobs(dicts, &m, &format!("{file}:{name}"), out, depth + 1);
                }
            }
        }
    } else {
        programs_in(d, file, out);
    }
}

/// The files that hold shaders: Shader/ (bfsha, agl archives), Layout2Shader/ and Layout/ (ui2d's BNSH), Lib/ (agl, gsys
/// and terrain archives, agl's networks), Movie/ (the upscaler's networks), Effect/ (particle sets)
fn archive_paths(romfs: &Path) -> Vec<PathBuf> {
    fn walk(d: &Path, out: &mut Vec<PathBuf>) {
        let Ok(rd) = std::fs::read_dir(d) else { return };
        let mut es: Vec<_> = rd.flatten().collect();
        es.sort_by_key(|e| e.file_name());
        for e in es {
            let p = e.path();
            if e.file_type().is_ok_and(|t| t.is_dir()) {
                walk(&p, out);
            } else {
                let n = e.file_name().to_string_lossy().into_owned();
                if n.ends_with(".zs") || n.contains(".bfsha") || n.contains(".sarc") || n.contains(".pck") {
                    out.push(p);
                }
            }
        }
    }
    let mut out = Vec::new();
    for f in ["Shader", "Layout2Shader", "Layout", "Lib", "Movie", "Effect"] {
        walk(&romfs.join(f), &mut out);
    }
    out
}

// ---- the models' materials and the bfsha programs they reach ----

struct Opt {
    name: String,
    stat: bool,
    choices: Vec<String>,
    default: u32,
    word: usize,
    shift: u32,
    mask: u32,
}

/// A bfsha shading model: its options and its programs' keys.
struct Model {
    /// its bfsha in the romfs
    source: String,
    file: Arc<Vec<u8>>,
    at: u64,
    opts: Vec<Opt>,
    by_name: HashMap<String, usize>,
    sk: usize,
    keys: Vec<Vec<u32>>,
    by_static: HashMap<Vec<u32>, Vec<u32>>,
    /// (by the mask of the options that match any choice: the programs by their static key outside it)
    wild: Mutex<HashMap<Vec<u32>, Arc<HashMap<Vec<u32>, Vec<u32>>>>>,
}

impl Model {
    fn read(file: &Arc<Vec<u8>>, m: u64) -> Option<Model> {
        let d = &file[..];
        let s = Strs { d, names: None };
        let mut opts = Vec::new();
        for (stat, arr, count) in [(true, 0x08, u16_(d, m + 0xe4)?), (false, 0x18, u16_(d, m + 0xe6)?)] {
            for i in 0..count as u64 {
                let o = u64_(d, m + arr)? + 0x28 * i;
                opts.push(Opt {
                    name: s.ptr(o).unwrap_or_default(),
                    stat,
                    choices: s.dict_keys(u64_(d, o + 8)?),
                    default: u16_(d, o + 0x1a)? as u32,
                    word: u8_(d, o + 0x24)? as usize,
                    shift: u8_(d, o + 0x25)? as u32,
                    mask: u32_(d, o + 0x20)?,
                });
            }
        }
        let mut by_name = HashMap::new();
        for (i, o) in opts.iter().enumerate() {
            by_name.insert(o.name.clone(), i);
        }
        let (count, sk, dk) = (u16_(d, m + 0xe8)? as u64, u8_(d, m + 0xea)? as usize, u8_(d, m + 0xeb)? as usize);
        let (kt, w) = (u64_(d, m + 0x90)?, (sk + dk) as u64);
        let mut keys = Vec::with_capacity(count as usize);
        let mut by_static: HashMap<Vec<u32>, Vec<u32>> = HashMap::new();
        for i in 0..count {
            let k: Vec<u32> = (0..w).map(|j| u32_(d, kt + 4 * (w * i + j))).collect::<Option<_>>()?;
            by_static.entry(k[..sk].to_vec()).or_default().push(i as u32);
            keys.push(k);
        }
        Some(Model { source: String::new(), file: file.clone(), at: m, opts, by_name, sk, keys, by_static, wild: Mutex::new(HashMap::new()) })
    }

    fn name(&self) -> String {
        Strs { d: &self.file, names: None }.ptr(self.at).unwrap_or_default()
    }

    /// The programs whose static key is the values' (option -> choice name); the options in `wild` match any choice, and so
    /// does an option whose value no choice has
    fn matching(&self, values: &HashMap<String, String>, wild: &[&str]) -> Vec<u32> {
        let mut key = vec![0u32; self.sk];
        let mut wmask = vec![0u32; self.sk];
        for o in self.opts.iter().filter(|o| o.stat && o.word < self.sk) {
            key[o.word] |= (o.default << o.shift) & o.mask;
        }
        for (name, value) in values {
            let Some(o) = self.by_name.get(name).map(|&i| &self.opts[i]).filter(|o| o.stat && o.word < self.sk) else { continue };
            match o.choices.iter().position(|c| c == value) {
                Some(c) => key[o.word] = (key[o.word] & !o.mask) | (((c as u32) << o.shift) & o.mask),
                None => wmask[o.word] |= o.mask,
            }
        }
        for name in wild {
            if let Some(o) = self.by_name.get(*name).map(|&i| &self.opts[i]).filter(|o| o.stat && o.word < self.sk) {
                wmask[o.word] |= o.mask;
            }
        }
        if wmask.iter().all(|&m| m == 0) {
            return self.by_static.get(&key).cloned().unwrap_or_default();
        }
        let index = {
            let mut w = self.wild.lock().unwrap();
            w.entry(wmask.clone())
                .or_insert_with(|| {
                    let mut idx: HashMap<Vec<u32>, Vec<u32>> = HashMap::new();
                    for (i, k) in self.keys.iter().enumerate() {
                        idx.entry(k[..self.sk].iter().zip(&wmask).map(|(a, m)| a & !m).collect()).or_default().push(i as u32);
                    }
                    Arc::new(idx)
                })
                .clone()
        };
        let masked: Vec<u32> = key.iter().zip(&wmask).map(|(a, m)| a & !m).collect();
        index.get(&masked).cloned().unwrap_or_default()
    }

    /// A program's stages
    fn stages(&self, program: u32) -> Vec<Stage> {
        let d = &self.file[..];
        let f = || -> Option<Vec<Stage>> {
            let var = u64_(d, u64_(d, self.at + 0x88)? + 0x40 * program as u64 + 0x20)?;
            let bnsh = u64_(d, self.at + 0xa8)?;
            let prog = bnsh + u64_(d, var + 16)?;
            let mut out = Vec::new();
            for slot in SLOTS {
                let st = u64_(d, prog + slot as u64)?;
                if st == 0 {
                    continue;
                }
                let st = st + bnsh;
                let (c, x, cn) = (u64_(d, st + 8)?, u64_(d, st + 16)?, u32_(d, st + 28)?);
                if !(0x60..=0x1000).contains(&cn) {
                    continue;
                }
                if let Some(s) = stage_at(d, bnsh + c, bnsh + x) {
                    out.push(s);
                }
            }
            Some(out)
        };
        f().unwrap_or_default()
    }
}

/// gsys's static options, from the material's render infos: option, render info, its value -> choice
const RULES: [(&str, &str, &[(&str, &str)]); 4] = [
    ("gsys_alpha_test_enable", "gsys_alpha_test_enable", &[("true", "1"), ("false", "0")]),
    (
        "gsys_alpha_test_func",
        "gsys_alpha_test_func",
        &[("never", "0"), ("less", "1"), ("equal", "2"), ("lequal", "3"), ("greater", "4"), ("notequal", "5"), ("gequal", "6"), ("always", "7")],
    ),
    ("gsys_display_face_type", "gsys_render_state_display_face", &[("both", "0"), ("front", "1"), ("back", "2"), ("none", "3")]),
    ("gsys_renderstate", "gsys_render_state_mode", &[("opaque", "0"), ("mask", "1"), ("translucent", "2"), ("custom", "3")]),
];

/// A material: the shading model it names, its options' values and the render infos gsys's rules read.
struct Material {
    archive: String,
    model: String,
    options: HashMap<String, String>,
    render: HashMap<String, String>,
}

fn read_bfres(d: &[u8], names: &HashMap<u64, String>) -> Option<Vec<Material>> {
    if !d.starts_with(b"FRES") {
        return Some(Vec::new());
    }
    let s = Strs { d, names: Some(names) };
    let rule_infos: HashSet<&str> = RULES.iter().map(|r| r.1).collect();
    let mut out = Vec::new();
    let (models, count) = (u64_(d, 0x28)?, u16_(d, 0xdc)? as u64);
    for mi in 0..count {
        let fm = models + 0x78 * mi;
        if slice(d, fm, 4)? != b"FMDL" {
            break;
        }
        let (nmat, mat) = (u16_(d, fm + 0x6c)? as u64, u64_(d, fm + 0x38)?);
        for m in 0..nmat {
            let f = mat + 0xb0 * m;
            let var = u64_(d, f + 0x10)?;
            if var == 0 {
                continue;
            }
            let refl = u64_(d, var)?;
            let (Some(archive), Some(model)) = (s.ptr(refl), s.ptr(refl + 8)) else { continue };
            let onames = s.dict_keys(u64_(d, refl + 0x40)?);
            let (nb, nt) = (u16_(d, var + 0x46)? as u64, u16_(d, var + 0x48)? as u64);
            let (bits, strs, idx) = (u64_(d, var + 0x28)?, u64_(d, var + 0x30)?, u64_(d, var + 0x38)?);
            let mut options = HashMap::new();
            for j in 0..nt {
                let k = if idx != 0 { u16_(d, idx + 2 * j)? as usize } else { j as usize };
                let Some(name) = onames.get(k) else { continue };
                let v = if j < nb { ((u32_(d, bits + 4 * (j / 32))? >> (j % 32)) & 1).to_string() } else { s.ptr(strs + 8 * (j - nb)).unwrap_or_default() };
                options.insert(name.clone(), v);
            }
            // (the render infos: only the string ones gsys's rules read, their first value)
            let mut render = HashMap::new();
            let (rcount, arr) = (u16_(d, refl + 0x48)? as u64, u64_(d, refl + 0x10)?);
            let (vals, counts, offs) = (u64_(d, f + 0x40)?, u64_(d, f + 0x48)?, u64_(d, f + 0x50)?);
            if vals != 0 {
                for r in 0..rcount {
                    let (Some(name), Some(t)) = (s.ptr(arr + 16 * r), u8_(d, arr + 16 * r + 8)) else { continue };
                    if t != 2 || !rule_infos.contains(name.as_str()) {
                        continue;
                    }
                    let (c, off) = (u16_(d, counts + 2 * r)?, u16_(d, offs + 2 * r)? as u64);
                    if c > 0 {
                        if let Some(v) = s.ptr(vals + off) {
                            render.insert(name, v);
                        }
                    }
                }
            }
            out.push(Material { archive, model, options, render });
        }
    }
    Some(out)
}

/// The programs a material reaches: each shading model of its name, gsys's options from its render infos where its
/// rules give them, any choice for the other gsys options
fn resolve(mat: &Material, models: &[Model], shaders: &HashMap<String, HashMap<String, Vec<usize>>>, out: &mut BTreeSet<(usize, u32)>) -> bool {
    let Some(ids) = shaders.get(&mat.archive).and_then(|m| m.get(&mat.model)) else { return false };
    for &id in ids {
        let sm = &models[id];
        let mut values = mat.options.clone();
        let mut ruled = Vec::new();
        for (opt, info, table) in RULES {
            let v = mat.render.get(info).and_then(|v| table.iter().find(|t| t.0 == v));
            if let (true, Some(v)) = (sm.by_name.contains_key(opt), v) {
                values.insert(opt.to_string(), v.1.to_string());
                ruled.push(opt);
            }
        }
        let wild: Vec<&str> = sm.opts.iter().filter(|o| o.stat && o.name.starts_with("gsys_") && !ruled.contains(&o.name.as_str())).map(|o| o.name.as_str()).collect();
        for p in sm.matching(&values, &wild) {
            out.insert((id, p));
        }
    }
    true
}

/// What making a pack found.
#[derive(Default, Debug)]
pub struct Stats {
    pub models: usize,
    pub materials: usize,
    pub resolved: usize,
    pub bfsha_programs: usize,
    pub bfsha_reached: usize,
    pub other_programs: usize,
    /// the programs of each kind of archive the pack took, and of those it left out
    pub by_archive: std::collections::BTreeMap<Archive, usize>,
    pub left_out_programs: usize,
    pub unread: Vec<String>,
}

/// The programs, each stage once.
#[derive(Default)]
pub struct Pack {
    pub stages: Vec<Stage>,
    /// each stage's origin: where it was found first
    pub origins: Vec<Origin>,
    keys: HashMap<u64, u32>,
    pub programs: Vec<Vec<u32>>,
    seen: HashSet<Vec<u32>>,
    /// the codes of the archives left out that no program of the pack has, with their origin
    pub left_out: Vec<(u64, Origin)>,
}

impl Pack {
    /// adds a program (its stages); false when the pack has it already
    pub fn add(&mut self, origin: &Origin, stages: Vec<Stage>) -> bool {
        let mut ids: Vec<u32> = stages
            .into_iter()
            .map(|s| {
                let k = code_key(&s.control, &s.area);
                *self.keys.entry(k).or_insert_with(|| {
                    self.stages.push(s);
                    self.origins.push(origin.clone());
                    (self.stages.len() - 1) as u32
                })
            })
            .collect();
        ids.sort();
        ids.dedup();
        if ids.is_empty() || !self.seen.insert(ids.clone()) {
            return false;
        }
        self.programs.push(ids);
        true
    }

    /// notes a program of an archive left out: its codes that the pack does not hold (`leave_out` after every `add`)
    fn leave_out(&mut self, origin: &Origin, stages: &[Stage]) {
        for s in stages {
            let k = code_key(&s.control, &s.area);
            if !self.keys.contains_key(&k) && !self.left_out.iter().any(|l| l.0 == k) {
                self.left_out.push((k, origin.clone()));
            }
        }
    }

    pub fn bytes(&self) -> u64 {
        self.stages.iter().map(|s| (20 + s.control.len() + s.area.len()) as u64).sum::<u64>()
            + self.programs.iter().map(|p| 4 + 4 * p.len() as u64).sum::<u64>()
            + 4
            + 12 * self.left_out.len() as u64
    }

    /// Each code's origin, a line each: `<code key> <NVN stage> <archive> <file>`, then the codes left out
    /// (`<code key> - <archive> <file> left out`)
    pub fn origins_text(&self) -> String {
        let mut o = String::new();
        for (s, g) in self.stages.iter().zip(&self.origins) {
            o += &format!("{:016x} {} {} {}\n", code_key(&s.control, &s.area), s.kind, g.archive.magic(), g.file);
        }
        for (k, g) in &self.left_out {
            o += &format!("{k:016x} - {} {} left out\n", g.archive.magic(), g.file);
        }
        o
    }

    /// The pack in the seed format (module docs), written beside and renamed over `path`
    pub fn write(&self, path: &Path) -> Result<(), String> {
        let mut o = Vec::with_capacity(self.bytes() as usize + 16);
        o.extend_from_slice(b"HKSEED01");
        o.extend_from_slice(&(self.programs.len() as u32).to_le_bytes());
        o.extend_from_slice(&(self.stages.len() as u32).to_le_bytes());
        for s in &self.stages {
            o.extend_from_slice(&code_key(&s.control, &s.area).to_le_bytes());
            for v in [s.kind, s.control.len() as u32, s.area.len() as u32] {
                o.extend_from_slice(&v.to_le_bytes());
            }
            o.extend_from_slice(&s.control);
            o.extend_from_slice(&s.area);
        }
        for p in &self.programs {
            o.extend_from_slice(&(p.len() as u32).to_le_bytes());
            for i in p {
                o.extend_from_slice(&i.to_le_bytes());
            }
        }
        for (k, g) in &self.left_out {
            o.extend_from_slice(&k.to_le_bytes());
            o.extend_from_slice(g.archive.magic().as_bytes());
        }
        o.extend_from_slice(&(self.left_out.len() as u32).to_le_bytes());
        let tmp = path.with_extension("tmp");
        std::fs::write(&tmp, &o).map_err(|e| format!("{}: {e}", tmp.display()))?;
        std::fs::rename(&tmp, path).map_err(|e| format!("{}: {e}", path.display()))
    }
}

fn read(dicts: &Dicts, p: &Path) -> Result<Vec<u8>, String> {
    let raw = std::fs::read(p).map_err(|e| format!("{}: {e}", p.display()))?;
    dicts.decompress(&raw).map(|c| c.into_owned()).map_err(|e| format!("{}: {e}", p.display()))
}

/// Shader/ExternalBinaryString.bfres: OoT's name hashes
fn external_names(dicts: &Dicts, romfs: &Path) -> HashMap<u64, String> {
    let Ok(d) = read(dicts, &romfs.join("Shader/ExternalBinaryString.bfres.zs")) else { return HashMap::new() };
    let s = Strs { d: &d, names: None };
    let (Some(hashes), Some(dict)) = (u64_(&d, 0xb8), u64_(&d, 0xc0)) else { return HashMap::new() };
    s.dict_keys(dict).into_iter().enumerate().filter_map(|(i, n)| Some((u64_(&d, hashes + 8 * i as u64)?, n))).collect()
}

/// Shader/*.bfsha: the shading models, and archive name -> shading model name -> models (a model's programs can be
/// split over files: MKW's)
fn bfsha_models(dicts: &Dicts, romfs: &Path, unread: &mut Vec<String>) -> (Vec<Model>, HashMap<String, HashMap<String, Vec<usize>>>) {
    let (mut models, mut by) = (Vec::new(), HashMap::<String, HashMap<String, Vec<usize>>>::new());
    let mut names: Vec<_> = std::fs::read_dir(romfs.join("Shader")).into_iter().flatten().flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
    names.sort();
    for name in names.into_iter().filter(|n| n.ends_with(".bfsha") || n.ends_with(".bfsha.zs")) {
        let file = match read(dicts, &romfs.join("Shader").join(&name)) {
            Ok(d) => Arc::new(d),
            Err(e) => {
                unread.push(e);
                continue;
            }
        };
        let arch = Strs { d: &file, names: None }.ptr(0x38).unwrap_or_default();
        let first = name.split('.').next().unwrap_or("").to_string();
        let (Some(n), Some(at)) = (u16_(&file, 0x78), u64_(&file, 0x48)) else { continue };
        for i in 0..n as u64 {
            let Some(mut m) = Model::read(&file, at + 0x100 * i) else {
                unread.push(format!("Shader/{name}: shading model {i}"));
                continue;
            };
            m.source = format!("Shader/{name}");
            let sm = m.name();
            let id = models.len();
            models.push(m);
            // (materials name the archive, or the file's first part: "Model" for Model.Product.110...)
            for key in [&arch, &first].into_iter().collect::<BTreeSet<_>>() {
                by.entry(key.clone()).or_default().entry(sm.clone()).or_default().push(id);
            }
        }
    }
    (models, by)
}

fn bfres_paths(romfs: &Path) -> Vec<PathBuf> {
    fn walk(d: &Path, out: &mut Vec<PathBuf>) {
        let Ok(rd) = std::fs::read_dir(d) else { return };
        let mut es: Vec<_> = rd.flatten().collect();
        es.sort_by_key(|e| e.file_name());
        for e in es {
            let n = e.file_name().to_string_lossy().into_owned();
            if e.file_type().is_ok_and(|t| t.is_dir()) {
                walk(&e.path(), out);
            } else if n.contains(".bfres") && !n.contains(".anim.") && !n.starts_with("ExternalBinaryString") {
                out.push(e.path());
            }
        }
    }
    let mut out = Vec::new();
    walk(romfs, &mut out);
    out
}

/// The pack's format and what this crate reads: a change to either changes it
const VERSION: u64 = 2;

/// A key of what a pack is made from: the files it reads (their paths, sizes and modification times) and VERSION
pub fn inputs_key(romfs: &Path, scope: Scope, left_out: &[LeftOut]) -> u64 {
    let mut files = archive_paths(romfs);
    files.extend(bfres_paths(romfs));
    files.push(romfs.join("Shader/ExternalBinaryString.bfres.zs"));
    files.sort();
    let mut h = code_key(&VERSION.to_le_bytes(), &[scope as u8]);
    for l in left_out {
        h = code_key(&h.to_le_bytes(), l.text().as_bytes());
    }
    for f in files {
        let m = std::fs::metadata(&f).ok();
        let t = m.as_ref().and_then(|m| m.modified().ok()).and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map_or(0, |d| d.as_nanos() as u64);
        let line = format!("{}\0{}\0{t}\n", f.strip_prefix(romfs).unwrap_or(&f).display(), m.map_or(0, |m| m.len()));
        h = code_key(&h.to_le_bytes(), line.as_bytes());
    }
    h
}

/// The pack of a romfs; `dictionaries` is the romfs file of the game's zstd dictionaries (game.toml's
/// `[assets].zstd_dictionaries`), `left_out` the kinds of archive and the files the game's patches make unreachable
/// (game.toml's `[assets].shaders_left_out`: their programs not taken, their codes listed; one that covers no program
/// is an error), `progress(done, of)` is told as files are read.
pub fn make(romfs: &Path, dictionaries: &Path, scope: Scope, left_out: &[LeftOut], progress: &(dyn Fn(usize, usize) + Sync)) -> Result<(Pack, Stats), String> {
    let dicts = Dicts::load(dictionaries)?;
    let mut st = Stats::default();
    let mut pack = Pack::default();
    let archives: Vec<PathBuf> = archive_paths(romfs).into_iter().filter(|p| scope == Scope::All || !p.to_string_lossy().contains(".bfsha")).collect();
    let bfres = if scope == Scope::Reached { bfres_paths(romfs) } else { Vec::new() };
    let total = archives.len() + bfres.len();
    let done = std::sync::atomic::AtomicUsize::new(0);
    let tick = || progress(done.fetch_add(1, std::sync::atomic::Ordering::Relaxed) + 1, total);
    if scope == Scope::Reached {
        let names = external_names(&dicts, romfs);
        let (models, shaders) = bfsha_models(&dicts, romfs, &mut st.unread);
        st.models = models.len();
        st.bfsha_programs = models.iter().map(|m| m.keys.len()).sum();
        let found: Vec<(usize, usize, BTreeSet<(usize, u32)>)> = bfres
            .par_iter()
            .map(|p| {
                let mut reached = BTreeSet::new();
                let (mut n, mut ok) = (0, 0);
                if let Ok(d) = read(&dicts, p) {
                    for m in read_bfres(&d, &names).unwrap_or_default() {
                        n += 1;
                        ok += resolve(&m, &models, &shaders, &mut reached) as usize;
                    }
                }
                tick();
                (n, ok, reached)
            })
            .collect();
        let mut reached = BTreeSet::new();
        for (n, ok, r) in found {
            st.materials += n;
            st.resolved += ok;
            reached.extend(r);
        }
        st.bfsha_reached = reached.len();
        for (id, p) in reached {
            let origin = Origin { archive: Archive::Bfsha, file: models[id].source.clone() };
            if pack.add(&origin, models[id].stages(p)) {
                *st.by_archive.entry(Archive::Bfsha).or_default() += 1;
            }
        }
    }
    let found: Vec<Vec<(Origin, Vec<Stage>)>> = archives
        .par_iter()
        .map(|p| {
            let mut out = Vec::new();
            if let Ok(d) = read(&dicts, p) {
                blobs(&dicts, &d, &p.strip_prefix(romfs).unwrap_or(p).to_string_lossy(), &mut out, 0);
            }
            tick();
            out
        })
        .collect();
    let mut out = Vec::new();
    let mut covered = vec![false; left_out.len()];
    for (origin, p) in found.into_iter().flatten() {
        let by: Vec<usize> = (0..left_out.len()).filter(|&i| left_out[i].covers(&origin)).collect();
        if !by.is_empty() {
            by.into_iter().for_each(|i| covered[i] = true);
            out.push((origin, p));
        } else if pack.add(&origin, p) {
            st.other_programs += 1;
            *st.by_archive.entry(origin.archive).or_default() += 1;
        }
    }
    if let Some(i) = covered.iter().position(|c| !c) {
        return Err(format!("shaders_left_out: {} holds no program of the romfs", left_out[i].text()));
    }
    st.left_out_programs = out.len();
    for (origin, p) in &out {
        pack.leave_out(origin, p);
    }
    Ok((pack, st))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn code_key_is_fnv1a_over_control_then_area() {
        assert_eq!(code_key(b"", b""), 0xcbf2_9ce4_8422_2325);
        assert_eq!(code_key(b"a", b""), 0xaf63_dc4c_8601_ec8c);
        assert_eq!(code_key(b"a", b"b"), code_key(b"ab", b""));
    }

    #[test]
    fn a_program_is_kept_once_and_its_stages_once() {
        let st = |b: u8| Stage { kind: 0, control: vec![b; CONTROL], area: vec![b; 16] };
        let o = Origin { archive: Archive::Bahs, file: "a".into() };
        let mut p = Pack::default();
        assert!(p.add(&o, vec![st(1), st(2)]));
        assert!(!p.add(&o, vec![st(2), st(1)]));
        assert!(p.add(&o, vec![st(1)]));
        assert_eq!((p.stages.len(), p.programs.len()), (2, 2));
        // (a left-out program's codes are listed unless the pack holds them)
        p.leave_out(&Origin { archive: Archive::Ndve, file: "b".into() }, &[st(1), st(3), st(3)]);
        assert_eq!(p.left_out.len(), 1);
        assert_eq!(p.left_out[0].0, code_key(&st(3).control, &st(3).area));
    }

    #[test]
    fn a_file_left_out_covers_itself_and_its_members() {
        let o = |f: &str| Origin { archive: Archive::Ndsr, file: f.into() };
        let l = LeftOut::parse("Lib/a.sarc.zs:x.pck").unwrap();
        assert!(l.covers(&o("Lib/a.sarc.zs:x.pck")));
        assert!(!l.covers(&o("Lib/a.sarc.zs:x.pck2")));
        assert!(!l.covers(&o("Lib/a.sarc.zs:y.pck")));
        assert!(LeftOut::parse("Lib/a.sarc.zs").unwrap().covers(&o("Lib/a.sarc.zs:y.pck")));
        assert_eq!(LeftOut::parse("NDVE"), Some(LeftOut::Kind(Archive::Ndve)));
        assert_eq!(LeftOut::parse("NDXX"), None);
    }
}
