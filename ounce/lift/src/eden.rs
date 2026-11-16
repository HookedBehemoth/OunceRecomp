//! The guest image built from the exefs (nx2/image.py EdenImage over a64-kit hos/nx_image.py, as nx2/lift.py
//! --standalone builds it): rtld, main, subsdk*, sdk back to back from 0x80000000 (Eden's layout, ASLR 0), relocated,
//! every import bound (a hook at an HLE'd symbol's definition, a trampoline for an unresolved one), and the entry
//! candidates, symbol names and the image.json facts. And standalone.hle_set: which symbols are HLE'd or stubbed.

use crate::image::{Hook, Image, Module, Segment};
use crate::nso::{Nso, RelType, DT_FINI, DT_INIT, DT_INIT_ARRAY, DT_INIT_ARRAYSZ, STB_WEAK, STT_FUNC};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

pub const CODE_39BIT: u64 = 0x8000_0000;
pub const TRAMP_BASE: u64 = 0x0700_0000;
const PAGE: u64 = 0x1000;

/// what the image stage gives beyond the image itself
pub struct Facts {
    /// the entry candidates (nx_image._entries), sorted
    pub entries: Vec<u64>,
    /// the .eh_frame_hdr function starts, sorted
    pub eh_starts: Vec<u64>,
    /// each module's .eh_frame_hdr, absolute: (module, start, end)
    pub eh_frame_hdr: Vec<(String, u64, u64)>,
    /// address -> the first defined function symbol there (symbol_names), sorted by address
    pub names: Vec<(u64, String)>,
    /// module -> its init array's (relocated) values
    pub init_arrays: Vec<(String, Vec<u64>)>,
    /// defined function exports: name -> address, in the order Python's dict has them
    pub exports: Vec<(String, u64)>,
    /// non-weak imports bound to a trampoline: symbol -> references, in Python's dict order
    pub unresolved: Vec<(String, u64)>,
    /// every relocated slot (absolute address), in module and relocation order
    pub reloc_slots: Vec<u64>,
}

/// nx_image.c_ident
pub fn c_ident(sym: &str) -> String {
    let mut s = String::from("imp_");
    s.extend(sym.chars().map(|c| if c.is_alphanumeric() || c == '_' { c } else { '_' }));
    s
}

fn page_align(v: u64) -> u64 {
    (v + PAGE - 1) & !(PAGE - 1)
}

fn eh_starts(img: &Image, hdrs: &[(String, u64, u64)]) -> Vec<u64> {
    let mut starts: Vec<u64> = hdrs.iter().flat_map(|(_, s, e)| img.eh_funcs(*s, *e)).collect();
    starts.sort_unstable();
    starts
}

/// builds the image (EdenImage(exefs, 0, hle=hle)): hle the symbols the host implements, stubbed those it stops on
pub fn build(mut mods: Vec<Nso>, implemented: &BTreeSet<String>, stubbed: &BTreeSet<String>) -> Result<(Image, Facts), String> {
    let all: BTreeSet<String> = implemented.union(stubbed).cloned().collect();
    let hle = &all;
    let mut base = CODE_39BIT;
    let mut bases = Vec::new();
    for m in mods.iter_mut() {
        let (data_off, data_size, _) = m.segments[2];
        let size = page_align(data_off + data_size + m.bss_size);
        if (m.image.len() as u64) < size {
            m.image.resize(size as usize, 0);
        }
        bases.push(base);
        base += size;
    }
    let end = base;
    let mut mem = vec![0u8; (end - CODE_39BIT) as usize];
    for (m, &b) in mods.iter().zip(&bases) {
        let o = (b - CODE_39BIT) as usize;
        mem[o..o + m.image.len()].copy_from_slice(&m.image);
    }
    // exports: the first definition of each name, in module order
    let mut exports: HashMap<&str, (usize, usize)> = HashMap::new();
    let mut export_order: Vec<&str> = Vec::new();
    for (mi, m) in mods.iter().enumerate() {
        for (si, s) in m.symbols.iter().enumerate() {
            if s.defined && !s.name.is_empty() && !exports.contains_key(s.name.as_str()) {
                exports.insert(&s.name, (mi, si));
                export_order.push(&s.name);
            }
        }
    }
    let addr_of = |(mi, si): (usize, usize)| bases[mi] + mods[mi].symbols[si].value;
    let mut hooks: BTreeMap<u64, String> = BTreeMap::new();
    let mut hook_syms: HashMap<String, String> = HashMap::new();
    // (symbols at one address: an implemented one before a stubbed one, the first stubbed of several; two
    // implemented ones are an error: which one the guest's calls reach would be a choice)
    for sym in implemented.iter().chain(stubbed.iter().filter(|s| !implemented.contains(*s))) {
        if let Some(&hit) = exports.get(sym.as_str()) {
            let a = addr_of(hit);
            if let Some(o) = hooks.get(&a) {
                let other = &hook_syms[o];
                if implemented.contains(sym) {
                    return Err(format!("host/ replaces both {other} and {sym}, one function at {a:#x}: replace one of them"));
                }
                continue;
            }
            let name = c_ident(sym);
            if let Some(o) = hook_syms.get(&name) {
                return Err(format!("{o} and {sym} have one C name, {name}"));
            }
            hooks.insert(a, name.clone());
            hook_syms.insert(name, sym.clone());
        }
    }
    // relocation and binding
    let mut tramp: HashMap<String, u64> = HashMap::new();
    let mut unresolved: Vec<(String, u64)> = Vec::new();
    let mut reloc_slots = Vec::new();
    for (mi, m) in mods.iter().enumerate() {
        let mut bind: HashMap<&str, u8> = HashMap::new();
        for s in &m.symbols {
            bind.insert(&s.name, s.bind);
        }
        for r in &m.relocs {
            let at = bases[mi] + r.offset;
            let v: u64 = match r.typ {
                RelType::Relative => (bases[mi] as i64).wrapping_add(r.addend) as u64,
                RelType::Abs64 | RelType::GlobDat | RelType::JumpSlot => {
                    let sym = m.rel_sym(r).ok_or_else(|| format!("{}: a symbol relocation without a symbol at {:#x}", m.name, r.offset))?;
                    let weak = bind.get(sym) == Some(&STB_WEAK);
                    let a = match exports.get(sym) {
                        Some(&hit) => addr_of(hit),
                        None if weak && !hle.contains(sym) => 0,
                        None => {
                            if !hle.contains(sym) {
                                match unresolved.iter_mut().find(|u| u.0 == sym) {
                                    Some(u) => u.1 += 1,
                                    None => unresolved.push((sym.to_string(), 1)),
                                }
                            }
                            match tramp.get(sym) {
                                Some(&a) => a,
                                None => {
                                    let a = TRAMP_BASE + 16 * tramp.len() as u64;
                                    tramp.insert(sym.to_string(), a);
                                    let name = c_ident(sym);
                                    if let Some(o) = hook_syms.get(&name) {
                                        return Err(format!("{o} and {sym} have one C name, {name}"));
                                    }
                                    hooks.insert(a, name.clone());
                                    hook_syms.insert(name, sym.to_string());
                                    a
                                }
                            }
                        }
                    };
                    (a as i64).wrapping_add(if r.typ == RelType::Abs64 { r.addend } else { 0 }) as u64
                }
                RelType::Other(t) => return Err(format!("{}: relocation type {t} at {:#x} not supported", m.name, r.offset)),
            };
            let o = (at - CODE_39BIT) as usize;
            mem[o..o + 8].copy_from_slice(&v.to_le_bytes());
            reloc_slots.push(at);
        }
    }
    let modules: Vec<Module> = mods
        .iter()
        .zip(&bases)
        .map(|(m, &b)| Module {
            name: m.name.clone(),
            base: b,
            size: m.image.len() as u64,
            build_id: m.build_id.clone(),
            init: m.dyn_first(DT_INIT).map_or(0, |v| b + v),
            fini: m.dyn_first(DT_FINI).map_or(0, |v| b + v),
            segments: m.segments.iter().map(|&(o, s, n)| Segment { name: n.to_string(), addr: b + o, size: s }).collect(),
            bss: [b + m.bss.0, b + m.bss.1],
        })
        .collect();
    let code: Vec<(u64, u64)> = mods.iter().zip(&bases).map(|(m, &b)| (b + m.code_range().0, b + m.code_range().1)).collect();
    let hook_list: Vec<Hook> = hooks.iter().map(|(&a, n)| Hook { addr: a, c: n.clone(), sym: hook_syms[n].clone() }).collect();
    let img = Image { base: CODE_39BIT, end, mem, modules, hooks: hook_list, code };
    let eh_frame_hdr: Vec<(String, u64, u64)> = mods
        .iter()
        .zip(&bases)
        .map(|(m, &b)| (m.name.clone(), b + m.eh_frame_hdr.0, b + m.eh_frame_hdr.1))
        .collect();
    let eh_starts = eh_starts(&img, &eh_frame_hdr);
    // entry candidates
    let mut e: HashSet<u64> = HashSet::new();
    let mut init_arrays = Vec::new();
    for (mi, m) in mods.iter().enumerate() {
        let b = bases[mi];
        let (lo, hi) = m.code_range();
        e.insert(b + lo);
        for k in [DT_INIT, DT_FINI] {
            if let Some(v) = m.dyn_first(k) {
                e.insert(b + v);
            }
        }
        e.extend(m.symbols.iter().filter(|s| s.defined && s.typ == STT_FUNC && s.value != 0).map(|s| b + s.value));
        for r in &m.relocs {
            if matches!(r.typ, RelType::Relative | RelType::Abs64) {
                let at = b + r.offset;
                let v = u64::from_le_bytes(img.read(at, 8).unwrap().try_into().unwrap());
                if img.in_code(v) {
                    e.insert(v);
                }
            }
        }
        // (BL targets; not ADR / ADRP+ADD: those come from adr_entries)
        let mut pc = b + lo;
        while pc + 4 <= b + hi {
            let w = img.word(pc);
            if w & 0xFC000000 == 0x94000000 {
                let off = (w & 0x3FFFFFF) as i64;
                let off = if off & (1 << 25) != 0 { off - (1 << 26) } else { off };
                e.insert((pc as i64 + 4 * off) as u64);
            }
            pc += 4;
        }
        let start = m.dyn_first(DT_INIT_ARRAY).unwrap_or(0);
        let size = m.dyn_first(DT_INIT_ARRAYSZ).unwrap_or(0);
        let arr: Vec<u64> = (0..size / 8).map(|i| u64::from_le_bytes(img.read(b + start + 8 * i, 8).unwrap().try_into().unwrap())).collect();
        init_arrays.push((m.name.clone(), arr));
    }
    e.extend(eh_starts.iter().copied());
    for (_, arr) in &init_arrays {
        e.extend(arr.iter().copied().filter(|&a| img.in_code(a)));
    }
    let mut entries: Vec<u64> = e.into_iter().filter(|&a| img.in_code(a)).collect();
    entries.sort_unstable();
    let mut names: BTreeMap<u64, String> = BTreeMap::new();
    for (m, &b) in mods.iter().zip(&bases) {
        for s in &m.symbols {
            if s.defined && s.typ == STT_FUNC && !s.name.is_empty() {
                names.entry(b + s.value).or_insert_with(|| s.name.clone());
            }
        }
    }
    let exports_out = export_order
        .iter()
        .filter_map(|n| {
            let (mi, si) = exports[n];
            (mods[mi].symbols[si].typ == STT_FUNC).then(|| (n.to_string(), bases[mi] + mods[mi].symbols[si].value))
        })
        .collect();
    let facts = Facts {
        entries,
        eh_starts,
        eh_frame_hdr,
        names: names.into_iter().collect(),
        init_arrays,
        exports: exports_out,
        unresolved,
        reloc_slots,
    };
    Ok((img, facts))
}

// ---- standalone.hle_set

/// the components of an Itanium nested name (_ZN[K]<len><id>...E), or None (standalone.nested_name)
pub fn nested_name(sym: &str) -> Option<Vec<&str>> {
    let rest = sym.strip_prefix("_ZNK").or_else(|| sym.strip_prefix("_ZN"))?;
    let b = rest.as_bytes();
    let mut i = 0;
    let mut parts = Vec::new();
    while i < b.len() && b[i].is_ascii_digit() {
        let mut j = i;
        while j < b.len() && b[j].is_ascii_digit() {
            j += 1;
        }
        let n: usize = rest[i..j].parse().ok()?;
        let e = (j + n).min(b.len());
        parts.push(rest.get(j..e)?);
        i = j + n;
    }
    (i < b.len() && b[i] == b'E').then_some(parts)
}

/// a free function of an OS-facing namespace (standalone.os_facing)
pub fn os_facing(sym: &str, namespaces: &[String]) -> bool {
    match nested_name(sym) {
        Some(p) => {
            p.first() == Some(&"nn")
                && p.len() >= 2
                && namespaces.iter().any(|n| n == p[1])
                && (p.len() == 3 || (p.len() == 4 && p[2] == "sdk"))
        }
        None => false,
    }
}

/// the NVN proc a symbol is: a C name (nvnFoo), or a C++ one (_Z<len>nvnFoo<args>) (horizon.nvn_proc_name)
pub fn nvn_proc_name(sym: &str) -> Option<&str> {
    if sym.starts_with("nvn") {
        return Some(sym);
    }
    let rest = sym.strip_prefix("_Z")?;
    let d = rest.bytes().take_while(|c| c.is_ascii_digit()).count();
    if d == 0 {
        return None;
    }
    let n: usize = rest[..d].parse().ok()?;
    let tail = &rest[d..];
    if !tail.starts_with("nvn") {
        return None;
    }
    let w = tail.char_indices().take_while(|&(_, c)| c.is_alphanumeric() || c == '_').last().map_or(0, |(k, c)| k + c.len_utf8());
    if w <= 3 {
        return None;
    }
    let word = &tail[..w];
    Some(&word[..n.min(word.len())])
}

/// the sdk's NVN functions the host does not implement, sorted (standalone.nvn_exports)
pub fn nvn_exports(mods: &[Nso], implemented: &BTreeSet<String>) -> Vec<String> {
    let sdk = mods.iter().find(|m| m.name == "sdk").expect("no sdk module");
    let mut v: Vec<String> = sdk
        .symbols
        .iter()
        .filter(|s| s.defined && !s.name.is_empty() && nvn_proc_name(&s.name).is_some() && s.typ == STT_FUNC && !implemented.contains(&s.name))
        .map(|s| s.name.clone())
        .collect();
    v.sort();
    v
}

/// (HLE'd symbols, stubbed symbols) (standalone.hle_set); implemented: the host's HLE and PATCH_ANSWER symbols
pub fn hle_set(mods: &[Nso], implemented: &BTreeSet<String>, namespaces: &[String]) -> Result<(BTreeSet<String>, BTreeSet<String>), String> {
    let known: HashSet<&str> = mods.iter().flat_map(|m| m.symbols.iter().filter(|s| !s.name.is_empty()).map(|s| s.name.as_str())).collect();
    let missing: Vec<&String> = implemented.iter().filter(|s| !known.contains(s.as_str())).collect();
    if !missing.is_empty() {
        return Err(format!("host/ replaces {} functions no module of this image has (HLE / PATCH_ANSWER): {:?}", missing.len(), missing));
    }
    let mut kind: HashMap<&str, u8> = HashMap::new();
    for m in mods {
        for s in &m.symbols {
            if s.defined && !s.name.is_empty() {
                kind.insert(&s.name, s.typ);
            }
        }
    }
    let mut stubbed = BTreeSet::new();
    for m in mods {
        if m.name == "sdk" || m.name == "rtld" {
            continue;
        }
        for s in &m.symbols {
            if !s.defined && !s.name.is_empty() && os_facing(&s.name, namespaces) && kind.get(s.name.as_str()).is_none_or(|&t| t == STT_FUNC) {
                stubbed.insert(s.name.clone());
            }
        }
    }
    let mut hle = implemented.clone();
    hle.extend(nvn_exports(mods, implemented));
    let stubbed = stubbed.into_iter().filter(|s| !implemented.contains(s)).collect();
    Ok((hle, stubbed))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unwind_starts_come_from_image_bytes() {
        let base = 0x1000;
        let start = 0x1010;
        let target = 0x1028;
        let mut mem = vec![0; 0x40];
        mem[0x10..0x14].copy_from_slice(&[1, 0x03, 0x03, 0x3b]);
        mem[0x18..0x1c].copy_from_slice(&1u32.to_le_bytes());
        mem[0x1c..0x20].copy_from_slice(&((target - start) as i32).to_le_bytes());
        let img = Image { base, end: base + mem.len() as u64, mem, modules: Vec::new(), hooks: Vec::new(), code: Vec::new() };
        let hdrs = vec![("main".to_string(), start, start + 0x20)];

        assert_eq!(eh_starts(&img, &hdrs), vec![target]);
    }

    fn module(syms: &[(&str, u64)]) -> Nso {
        let symbols = syms
            .iter()
            .map(|&(n, v)| crate::nso::Symbol { name: n.into(), typ: STT_FUNC, bind: 1, defined: true, value: v, size: 4 })
            .collect();
        Nso {
            name: "main".into(),
            build_id: String::new(),
            segments: vec![(0, 0x1000, ".text"), (0x1000, 0x1000, ".rodata"), (0x2000, 0x1000, ".data")],
            bss_size: 0,
            image: vec![0; 0x3000],
            mod0: 0,
            bss: (0x3000, 0x3000),
            eh_frame_hdr: (0, 0),
            dynamic: HashMap::new(),
            symbols,
            relocs: Vec::new(),
        }
    }

    #[test]
    fn hooks_at_one_address() {
        let set = |v: &[&str]| v.iter().map(|s| s.to_string()).collect::<BTreeSet<String>>();
        let mods = || vec![module(&[("alias_a", 0x100), ("alias_b", 0x100)])];
        // (an implemented symbol wins over a stubbed one at its address, whichever sorts first)
        let (img, _) = build(mods(), &set(&["alias_b"]), &set(&["alias_a"])).unwrap();
        assert_eq!(img.hooks.len(), 1);
        assert_eq!(img.hooks[0].sym, "alias_b");
        // (two implemented ones: which the guest reaches would be a choice)
        let e = build(mods(), &set(&["alias_a", "alias_b"]), &set(&[])).err().unwrap();
        assert!(e.contains("alias_a") && e.contains("alias_b"), "{e}");
    }

    #[test]
    fn names() {
        assert_eq!(nested_name("_ZN2nn2oe10InitializeEv"), Some(vec!["nn", "oe", "Initialize"]));
        assert_eq!(nested_name("_Z3foov"), None);
        assert!(os_facing("_ZN2nn2oe10InitializeEv", &["oe".to_string()]));
        assert_eq!(nvn_proc_name("_Z24nvnNativeImageInitializeP9NVNdevice"), Some("nvnNativeImageInitialize"));
        assert_eq!(nvn_proc_name("nvnDeviceInitialize"), Some("nvnDeviceInitialize"));
        assert_eq!(c_ident("a.b$c"), "imp_a_b_c");
    }
}
