//! What the host reads of the guest context, against what it declares (its patch declarations, runtime/patch.h, as
//! the lifter scans them; the rules in ounce/lift/src/patch.rs): runtime/*.c, runtime/media/*.c and the game's patches
//! compiled to LLVM IR (-O1, no inlining), every load from a context pointer traced through the functions it is passed
//! to, and every way into guest code followed through the host's calls.
//!
//! Guest code is entered through guest_call, guest_call8, hos_call_guest, a64_icall, a64_ijump or a call of an
//! f_<addr> / body_<addr>; a guest function's host pointer (CODE_PTR(f_<addr>)) is a constant target. A guest function
//! reaches host code by its name or from a64_lookup / a64_fn_of, which only the boot and the thread start call, and no
//! static holds one (checked), so any other indirect call is of host code (orig's, through its GuestFn, is the body's).

use ounce_tests::*;
use regex::Regex;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Mutex, OnceLock};

/// (lr comparisons that only label or count: allowed)
const LR_LABELS: &[(&str, &str)] = &[("games/oot/patches/layer_ahead.c", "jobs recorded from another caller (a count for the log)")];

fn re(s: &str) -> Regex {
    Regex::new(s).unwrap()
}

/// splits an argument or parameter list at its commas outside parentheses (a parameter's attribute such as
/// initializes((0, 12), (16, 40)) stays whole)
fn split_args(s: &str) -> Vec<&str> {
    let (mut out, mut depth, mut start) = (vec![], 0i32, 0);
    for (i, c) in s.char_indices() {
        match c {
            '(' => depth += 1,
            ')' => depth -= 1,
            ',' if depth <= 0 => {
                out.push(&s[start..i]);
                start = i + 1;
            }
            _ => {}
        }
    }
    out.push(&s[start..]);
    out
}

fn field(off: i64) -> String {
    if off < 248 {
        format!("x{}", off.div_euclid(8))
    } else if (248..256).contains(&off) {
        "sp".into()
    } else if (0x110..0x310).contains(&off) {
        format!("v{}", (off - 0x110) / 16)
    } else {
        format!("ctx+{off:#x}")
    }
}

/// a function's context reads: per pointer parameter the fields it loads, the calls it passes a pointer derived from
/// one to (param, offset, callee, argument), whether it enters guest code
struct Traced {
    params: Vec<Option<String>>,
    r: Vec<BTreeSet<String>>,
    calls: Vec<(usize, i64, String, usize)>,
}

const GUEST: &[&str] = &["a64_icall", "a64_ijump", "hos_call_guest", "hos_sj_call"];

fn trace(files: &[(String, String)]) -> HashMap<String, Traced> {
    let def = re(r"^define [^@]*@([\w.$]+)\((.*)\)[^{]*\{");
    let last = re(r"(%[\w.]+)\s*$");
    let gep = re(r"^\s*(%[\w.]+) = getelementptr (?:inbounds )?(?:nuw )?i8, ptr (%[\w.]+), i64 (-?\d+)");
    let load = re(r"^\s*%[\w.]+ = load \S+, ptr (%[\w.]+)");
    let copy = re(r"call void @llvm\.mem(?:cpy|move)\.[^(]*\(ptr[^,]*, ptr[^%]*(%[\w.]+)");
    let call = re(r"call [^@%]*?(?:@([\w.$]+)|(%[\w.]+))\(([^)]*)");
    let mut funcs: HashMap<String, Traced> = HashMap::new();
    for (_, text) in files {
        let mut cur: Option<String> = None;
        let mut ptr: HashMap<String, (usize, i64)> = HashMap::new();
        for line in text.lines() {
            if let Some(m) = def.captures(line) {
                let names: Vec<Option<String>> = split_args(&m[2])
                    .iter()
                    .map(|p| {
                        let p = p.trim();
                        if p.starts_with("ptr") {
                            last.captures(p).map(|c| c[1].to_string())
                        } else {
                            None
                        }
                    })
                    .collect();
                let name = m[1].to_string();
                // (a name defined again, a static of another file: the first one's record, as setdefault keeps it)
                funcs.entry(name.clone()).or_insert_with(|| Traced {
                    r: vec![BTreeSet::new(); names.len()],
                    params: names.clone(),
                    calls: vec![],
                });
                ptr = names.iter().enumerate().filter_map(|(i, n)| n.clone().map(|n| (n, (i, 0)))).collect();
                cur = Some(name);
                continue;
            }
            let Some(name) = &cur else { continue };
            if line.starts_with('}') {
                cur = None;
                continue;
            }
            let f = funcs.get_mut(name).unwrap();
            if let Some(g) = gep.captures(line) {
                if let Some(&(i, o)) = ptr.get(&g[2]) {
                    ptr.insert(g[1].to_string(), (i, o + g[3].parse::<i64>().unwrap()));
                    continue;
                }
            }
            if let Some(g) = load.captures(line) {
                if let Some(&(i, o)) = ptr.get(&g[1]) {
                    f.r[i].insert(field(o));
                    continue;
                }
            }
            if let Some(g) = copy.captures(line) {
                if let Some(&(i, _)) = ptr.get(&g[1]) {
                    f.r[i].insert("a copy of the context".into());
                    continue;
                }
            }
            for c in call.captures_iter(line) {
                let callee = c.get(1).map_or("<indirect>".to_string(), |m| m.as_str().to_string());
                if callee.starts_with("llvm.") {
                    continue;
                }
                for (k, a) in c[3].split(',').enumerate() {
                    let v = a.split_whitespace().last().unwrap_or("");
                    if let Some(&(i, o)) = ptr.get(v) {
                        f.calls.push((i, o, callee.clone(), k));
                    }
                }
            }
        }
    }
    funcs
}

/// the context fields function name reads through parameter k (the guest-call functions it reaches into api)
fn reads(funcs: &HashMap<String, Traced>, name: &str, k: usize, seen: &mut HashSet<(String, usize)>, api: &mut BTreeSet<String>) -> BTreeSet<String> {
    let Some(f) = funcs.get(name) else { return BTreeSet::new() };
    if seen.contains(&(name.to_string(), k)) || k >= f.params.len() {
        return BTreeSet::new();
    }
    seen.insert((name.to_string(), k));
    let mut out = f.r[k].clone();
    for (i, o, callee, arg) in &f.calls {
        if *i != k {
            continue;
        }
        if callee == "guest_call" || callee == "guest_call8" {
            api.insert(callee.clone());
            continue;
        }
        if GUEST.contains(&callee.as_str()) || callee == "<indirect>" {
            out.insert(format!("everything ({callee})"));
            continue;
        }
        if *o == 0 {
            out.extend(reads(funcs, callee, *arg, seen, api));
        }
    }
    out
}

fn reads_of(funcs: &HashMap<String, Traced>, name: &str) -> BTreeSet<String> {
    reads(funcs, name, 0, &mut HashSet::new(), &mut BTreeSet::new())
}

/// {function: (its parameters' names, its lines)} (the last definition of a name: static ones recur)
fn bodies(files: &[(String, String)]) -> HashMap<String, (Vec<Option<String>>, Vec<String>)> {
    let def = re(r"^define [^@]*@([\w.$]+)\((.*)\)[^{]*\{");
    let last = re(r"(%[\w.]+)\s*$");
    let mut out = HashMap::new();
    for (_, text) in files {
        let mut cur: Option<String> = None;
        for line in text.lines() {
            if let Some(m) = def.captures(line) {
                let params = split_args(&m[2]).iter().map(|p| last.captures(p.trim()).map(|c| c[1].to_string())).collect();
                out.insert(m[1].to_string(), (params, vec![]));
                cur = Some(m[1].to_string());
                continue;
            }
            let Some(name) = &cur else { continue };
            if line.starts_with('}') {
                cur = None;
                continue;
            }
            out.get_mut(name).unwrap().1.push(line.to_string());
        }
    }
    out
}

// ---- the guest code host code enters: every call, with constant arguments followed through the host's own calls

/// a function of the call graph: a global by its name, an internal one by (file, name), as the same static name recurs
#[derive(Clone, PartialEq, Eq, Hash, PartialOrd, Ord, Debug)]
enum Key {
    Global(String),
    Local(String, String),
}

impl Key {
    fn name(&self) -> &str {
        match self {
            Key::Global(n) | Key::Local(_, n) => n,
        }
    }
}

struct Node {
    file: String,
    params: Vec<String>,
    /// (callee: a name, or a %value for an indirect call; its argument texts)
    calls: Vec<(String, Vec<String>)>,
    lines: Vec<String>,
}

fn call_graph(files: &[(String, String)]) -> HashMap<Key, Node> {
    let def = re(r"^define ([^@]*)@([\w.$]+)\((.*)\)[^{]*\{");
    let local = re(r"\b(internal|private)\b");
    let call = re(r"call [^@%(]*?(?:@([\w.$]+)|(%[\w.]+))\((.*)\)");
    let mut funcs = HashMap::new();
    for (file, text) in files {
        let mut cur: Option<Key> = None;
        for line in text.lines() {
            if let Some(m) = def.captures(line) {
                let params = split_args(&m[3]).iter().map(|p| p.split_whitespace().last().unwrap_or("").to_string()).collect();
                let key = if local.is_match(&m[1]) { Key::Local(file.clone(), m[2].to_string()) } else { Key::Global(m[2].to_string()) };
                funcs.insert(key.clone(), Node { file: file.clone(), params, calls: vec![], lines: vec![] });
                cur = Some(key);
                continue;
            }
            let Some(key) = &cur else { continue };
            if line.starts_with('}') {
                cur = None;
                continue;
            }
            let f = funcs.get_mut(key).unwrap();
            f.lines.push(line.to_string());
            for c in call.captures_iter(line) {
                let args = split_args(&c[3]).iter().map(|a| a.trim().to_string()).collect();
                let callee = c.get(1).or(c.get(2)).unwrap().as_str().to_string();
                f.calls.push((callee, args));
            }
        }
    }
    funcs
}

#[derive(Clone, PartialEq, Eq, Hash, PartialOrd, Ord, Debug)]
enum Val {
    Int(u64),
    /// a host function's name, with its @
    Name(String),
}

/// the call that enters guest code: the index of its target argument
fn enter(callee: &str) -> Option<usize> {
    match callee {
        "guest_call" | "guest_call8" => Some(2),
        "hos_call_guest" | "a64_icall" | "a64_ijump" => Some(1),
        _ => None,
    }
}

/// [(how, target or None (not a constant), path)]: the guest code root enters, through every host function it calls
/// (a constant argument, or a host function's address, followed into the callee's parameters)
fn guest_entries(funcs: &HashMap<Key, Node>, root: &Key) -> Vec<(String, Option<u64>, Vec<String>)> {
    struct W<'a> {
        funcs: &'a HashMap<Key, Node>,
        seen: HashSet<(Key, Vec<(String, Val)>)>,
        out: Vec<(String, Option<u64>, Vec<String>)>,
        guest_fn: Regex,
        int: Regex,
        body: Regex,
    }
    impl W<'_> {
        fn key_of(&self, name: &str, file: &str) -> Option<Key> {
            let l = Key::Local(file.to_string(), name.to_string());
            if self.funcs.contains_key(&l) {
                return Some(l);
            }
            let g = Key::Global(name.to_string());
            self.funcs.contains_key(&g).then_some(g)
        }
        /// an argument's value: a guest function's address (an f_ / body_ reference: CODE_PTR), an integer, a host
        /// function's name, or what the parameter it names was given
        fn val(&self, text: &str, env: &BTreeMap<String, Val>) -> Option<Val> {
            if let Some(g) = self.guest_fn.captures(text) {
                return Some(Val::Int(u64::from_str_radix(&g[1], 16).unwrap()));
            }
            let v = text.split_whitespace().last().unwrap_or("");
            if self.int.is_match(v) {
                return Some(Val::Int(v.parse::<i128>().unwrap() as u64));
            }
            if v.starts_with('@') {
                return Some(Val::Name(v.to_string()));
            }
            env.get(v).cloned()
        }
        fn walk(&mut self, key: &Key, env: BTreeMap<String, Val>, path: &[String]) {
            let sig = (key.clone(), env.clone().into_iter().collect());
            if !self.seen.insert(sig) {
                return;
            }
            let f = &self.funcs[key];
            let mut path = path.to_vec();
            path.push(key.name().to_string());
            for (callee, args) in &f.calls {
                let mut callee = callee.clone();
                if callee.starts_with('%') {
                    // (an indirect call: of a host function whose address is known, or host code)
                    match self.val(&callee, &env) {
                        Some(Val::Name(n)) => callee = n,
                        _ => continue,
                    }
                }
                let callee = callee.trim_start_matches('@');
                if callee.starts_with("llvm.") {
                    continue;
                }
                if let Some(k) = enter(callee) {
                    let t = args.get(k).and_then(|a| self.val(a, &env));
                    let t = match t {
                        Some(Val::Int(i)) => Some(i),
                        _ => None,
                    };
                    self.out.push((callee.to_string(), t, path.clone()));
                } else if self.body.is_match(callee) {
                    self.out.push((callee.to_string(), Some(u64::from_str_radix(&callee[callee.len() - 8..], 16).unwrap()), path.clone()));
                } else if let Some(k) = self.key_of(callee, &f.file) {
                    let params = &self.funcs[&k].params;
                    let next: BTreeMap<String, Val> =
                        params.iter().zip(args).filter_map(|(p, a)| self.val(a, &env).map(|v| (p.clone(), v))).collect();
                    self.walk(&k, next, &path);
                }
            }
        }
    }
    let mut w = W {
        funcs,
        seen: HashSet::new(),
        out: vec![],
        guest_fn: re(r"@(?:f|body)_([0-9a-f]{8})\b"),
        int: re(r"^-?\d+$"),
        body: re(r"^(f|body)_[0-9a-f]{8}$"),
    };
    w.walk(root, BTreeMap::new(), &[]);
    w.out
}

struct Host {
    funcs: HashMap<String, Traced>,
    graph: HashMap<Key, Node>,
    bodies: HashMap<String, (Vec<Option<String>>, Vec<String>)>,
}

fn sources() -> Vec<PathBuf> {
    let mut out = vec![];
    let mut add = |dir: &Path| {
        if let Ok(rd) = std::fs::read_dir(dir) {
            for e in rd {
                let p = e.unwrap().path();
                if p.extension().is_some_and(|x| x == "c") && p.is_file() {
                    out.push(p);
                }
            }
        }
    };
    add(&path("runtime"));
    add(&path("runtime/media"));
    for g in std::fs::read_dir(path("games")).unwrap() {
        add(&g.unwrap().path().join("patches"));
    }
    out.sort();
    out
}

/// the host's C as LLVM IR, traced (once a run)
fn host() -> &'static Host {
    static H: OnceLock<Host> = OnceLock::new();
    H.get_or_init(|| {
        let t = Tmp::new("host-reads");
        let rt = t.join("rt64");
        rt64(&rt);
        let mut flags: Vec<String> = ["-S", "-emit-llvm", "-O1", "-fno-inline", "-g0", "-Wno-everything", "-D_GNU_SOURCE"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        flags.extend([inc("runtime/os"), inc("runtime"), format!("-I{}", rt.display()), inc("renderer/api"), inc("runtime/media"), inc("third_party")]);
        let srcs = sources();
        let next = Mutex::new(0usize);
        let results = Mutex::new(Vec::new());
        let jobs = std::thread::available_parallelism().map_or(8, |n| n.get());
        std::thread::scope(|s| {
            for _ in 0..jobs {
                s.spawn(|| loop {
                    let i = {
                        let mut n = next.lock().unwrap();
                        *n += 1;
                        *n - 1
                    };
                    let Some(src) = srcs.get(i) else { break };
                    let stem = src.file_stem().unwrap().to_string_lossy().into_owned();
                    let out = t.join(&format!("{stem}.ll"));
                    let o = output(Command::new("clang").args(&flags).arg(src).arg("-o").arg(&out));
                    results.lock().unwrap().push((src.clone(), o.status.success(), text(&o.stderr), out));
                });
            }
        });
        let mut res = results.into_inner().unwrap();
        res.sort();
        // (a file whose third-party headers this PC lacks is left out: the check covers what compiles)
        let bad: Vec<_> = res.iter().filter(|(_, ok, err, _)| !ok && !err.contains("file not found")).collect();
        assert!(bad.is_empty(), "{}: {}", bad[0].0.display(), bad[0].2);
        let files: Vec<(String, String)> = res
            .iter()
            .filter(|(_, ok, _, _)| *ok)
            .map(|(_, _, _, out)| {
                (out.file_name().unwrap().to_string_lossy().into_owned(), String::from_utf8_lossy(&std::fs::read(out).unwrap()).into_owned())
            })
            .collect();
        let mut files = files;
        files.sort();
        Host { funcs: trace(&files), graph: call_graph(&files), bodies: bodies(&files) }
    })
}

/// A replaced function's adapter (PATCH_FUNC: hos_func_0x*, hos_func_sym_*) reads only its declared operands of the
/// call's context (the host function takes them as values; its GuestCall goes only to guest_call); each one is declared.
#[test]
fn funcs() {
    let h = host();
    let name = re(r"^hos_func_(0x[0-9a-f]{8}|sym_\w+)$");
    let mut funcs: Vec<&String> = h.funcs.keys().filter(|n| name.is_match(n)).collect();
    funcs.sort();
    let decl: BTreeMap<&str, &Func> = scan().ps.funcs.iter().map(|f| (f.name.as_str(), f)).collect();
    assert!(funcs.len() > 20, "{} replaced functions", funcs.len());
    assert_eq!(funcs.iter().map(|s| s.as_str()).collect::<Vec<_>>(), decl.keys().copied().collect::<Vec<_>>(), "each one declared");
    for name in funcs {
        let got = reads_of(&h.funcs, name);
        let d = decl[name.as_str()];
        let allowed: BTreeSet<String> = d.args.iter().map(|x| ounce_lift::patch::var(x)).collect();
        let beyond: Vec<&String> = got.difference(&allowed).collect();
        assert!(beyond.is_empty(), "{name} reads {beyond:?}, beyond its operands ({}:{})", d.at.0, d.at.1);
    }
}

#[derive(Clone)]
enum Decl {
    Computed,
    Set(BTreeSet<u64>),
}

/// Every guest function a replaced function's or an At row's host function enters, on any path through the host's
/// calls, is declared where the summaries read it: a constant target in its guest(...), a computed one as
/// guest(computed). A Value probe's host function enters none.
#[test]
fn guest_calls_declared() {
    let (h, s) = (host(), scan());
    let mut roots: BTreeMap<String, (String, Decl)> = BTreeMap::new();
    for f in &s.ps.funcs {
        // (its adapter, which calls it)
        let what = format!("the guest(...) of the PATCH_FUNC of {} at {}:{}", f.fname, f.at.0, f.at.1);
        roots.insert(f.name.clone(), (what, Decl::Set(f.guest.iter().copied().collect())));
    }
    for p in &s.at {
        // (a row's host function: what its rows declare, all of them)
        let (what, decl) =
            roots.get(&p.fname).cloned().unwrap_or((format!("the guest(...) of the PATCH_AT of {}", p.fname), Decl::Set(BTreeSet::new())));
        let decl = match (decl, &p.guest) {
            (Decl::Computed, _) | (_, Some(Guest::Computed)) => Decl::Computed,
            (Decl::Set(mut d), g) => {
                if let Some(Guest::List(l)) = g {
                    d.extend(l.iter().copied());
                }
                Decl::Set(d)
            }
        };
        roots.insert(p.fname.clone(), (what, decl));
    }
    for p in &s.probes {
        roots.entry(p.fname.clone()).or_insert((format!("Value probes cannot call guest code ({})", p.fname), Decl::Set(BTreeSet::new())));
    }
    assert!(roots.len() > 60, "{} roots", roots.len());
    // (the walk recurses through the host's calls: a thread with room for it)
    let bad = std::thread::Builder::new()
        .stack_size(256 << 20)
        .spawn(move || {
            let mut bad = vec![];
            for (name, (what, decl)) in &roots {
                let key = Key::Global(name.clone());
                assert!(h.graph.contains_key(&key), "{name} is not defined in runtime/*.c");
                for (how, t, path) in guest_entries(&h.graph, &key) {
                    let ok = match (decl, t) {
                        (Decl::Computed, _) => true,
                        (Decl::Set(d), Some(t)) => d.contains(&t),
                        _ => false,
                    };
                    if !ok {
                        bad.push(format!(
                            "{name} enters guest {} through {how} ({}): declare {} in {what}",
                            t.map_or("code at a computed address".to_string(), |t| format!("{t:#x}")),
                            path.join(" > "),
                            if t.is_some() { "it" } else { "\"computed\"" }
                        ));
                    }
                }
            }
            bad
        })
        .unwrap()
        .join()
        .unwrap();
    assert!(bad.is_empty(), "{}", bad.join("\n"));
}

/// No host function keeps a guest function's host pointer (f_<addr>) in a static, and a64_lookup / a64_fn_of are called
/// only by the boot and the thread start: so an indirect call elsewhere is of host code (or of a replaced function's
/// orig, the body its record counts), and guest_calls_declared sees every way into guest code.
#[test]
fn guest_pointers_not_kept() {
    let h = host();
    let stored = re(r"store ptr @(?:f|body)_([0-9a-f]{8}), ptr @");
    for (key, f) in &h.graph {
        let name = match key {
            Key::Global(n) => n.clone(),
            Key::Local(file, n) => format!("{file} {n}"),
        };
        let looks = f.calls.iter().any(|(c, _)| c == "a64_lookup" || c == "a64_fn_of");
        assert!(!looks || f.file == "hos_main.ll" || f.file == "hos_thread.ll", "{name} resolves a guest code address");
        let kept: Vec<&str> = f.lines.iter().filter(|l| stored.is_match(l)).map(|l| l.trim()).collect();
        assert!(kept.is_empty(), "{name} keeps a guest function in a static: {:?}", &kept[..1]);
    }
}

/// An HLE'd function (HLE: imp_*, and the NVN procs NV: n_nvn*) reads x19-x29 or v8-v15 of its caller only where its
/// declaration's reads(...) says so.
#[test]
fn hle() {
    let h = host();
    let cs: BTreeSet<String> = (19..30).map(|i| format!("x{i}")).chain((8..16).map(|i| format!("v{i}"))).collect();
    let hle = re(r"^(?:imp_|n_)(\w+)$");
    let mut n = 0;
    for name in h.funcs.keys() {
        let Some(m) = hle.captures(name) else { continue };
        n += 1;
        let got: BTreeSet<String> = reads_of(&h.funcs, name).intersection(&cs).cloned().collect();
        let declared: BTreeSet<String> = scan().effects(&m[1]).map(|d| d.reads.iter().cloned().collect()).unwrap_or_default();
        let beyond: Vec<&String> = got.difference(&declared).collect();
        assert!(beyond.is_empty(), "{name} reads its caller's {beyond:?}");
    }
    assert!(n > 300, "{n} HLE'd functions");
}

/// An SVC (runtime/patch.h) reads no register beyond its declared inputs: the lift passes poison there (a result it
/// leaves unwritten is the input it was, so such a register is an input too).
#[test]
fn svc_inputs() {
    let h = host();
    let src = read("runtime/hos_svc.c");
    let decls: Vec<(String, String, usize)> = re(r"(?m)^SVC\((0x[0-9A-F]{2}), (\w+), (\d+), (\d+)\)")
        .captures_iter(&src)
        .map(|c| (c[1].to_string(), c[2].to_string(), c[3].parse().unwrap()))
        .collect();
    assert!(decls.len() > 25, "{} SVCs", decls.len());
    let mut bad = vec![];
    for (num, name, ins) in decls {
        let (params, lines) = h.bodies.get(&format!("hos_svc_{num}")).unwrap_or_else(|| panic!("no hos_svc_{num}"));
        assert_eq!(params.len(), 10, "{name}");
        let used: Vec<usize> = (0..8)
            .filter(|&k| {
                let p = re(&format!(r"{}\b", regex::escape(params[k].as_deref().expect("a parameter without a name"))));
                lines.iter().any(|l| p.is_match(l))
            })
            .collect();
        let beyond: Vec<usize> = used.into_iter().filter(|&k| k >= ins).collect();
        if !beyond.is_empty() {
            bad.push(format!("SVC {num} {name} reads x{beyond:?} beyond its {ins} inputs"));
        }
    }
    assert!(bad.is_empty(), "{}", bad.join("\n"));
}

/// No host behaviour is keyed on a guest return address (the caller's patch is an At row at its call site): no
/// `x[30] ==` / `lr !=` and the like against a guest address, outside the listed diagnostics.
#[test]
fn no_return_address_keys() {
    let pat = re(r"(x\[30\]|\blr\b|->lr)\s*(==|!=)\s*0x8[0-9a-f]{7}|0x8[0-9a-f]{7}(ull|ULL)?\s*(==|!=)\s*(c->x\[30\]|\blr\b)");
    let mut files: Vec<PathBuf> = sources().into_iter().filter(|p| !p.starts_with(path("runtime/media"))).collect();
    files.sort();
    for p in files {
        let rel = p.strip_prefix(root()).unwrap().to_string_lossy().replace('\\', "/");
        if LR_LABELS.iter().any(|(f, _)| *f == rel) {
            continue;
        }
        let t = String::from_utf8_lossy(&std::fs::read(&p).unwrap()).into_owned();
        let hits: Vec<&str> = t.lines().filter(|l| pat.is_match(l)).collect();
        assert!(hits.is_empty(), "{rel}: {:?}", &hits[..hits.len().min(2)]);
    }
}
