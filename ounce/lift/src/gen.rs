//! The generated sources of a standalone lift, as IR (gen/*.ll), so the build needs no C compiler for them: the
//! runtime's tables (a64_table: every function; a64_hooks: the hooked addresses; a64_code_slots: the relocated slots
//! holding a function), the image's description (hos_image), the import stops (a64_stubs) and the NVN functions
//! routed to runtime/nvn2.c (nvn2_exports); functions.txt; and the guest model rt64/a64.h, a64_rt.c (a64-kit's runtime
//! with the 64-bit guest window), which reads no dump.

use crate::eden::{nvn_proc_name, Facts};
use crate::image::{Hook, Image};
use std::collections::{BTreeSet, HashMap};
use std::fmt::Write;
use std::path::Path;

fn fname(a: u64) -> String {
    format!("f_{a:08x}")
}

/// a64_hooks.ll: f_<address> tail-calls the hook's host function; an answered hook (its f_<address> is lifted IR)
/// is a comment naming it
pub fn hooks_ll(hooks: &[Hook], answered: &BTreeSet<u64>) -> String {
    let mut m = Ll::new();
    for x in hooks {
        if answered.contains(&x.addr) {
            writeln!(m.s, "; f_{:08x} answers {}: lifted IR", x.addr, x.c).unwrap();
        } else {
            writeln!(m.s, "define dso_local void @f_{:08x}({CTX} %0) local_unnamed_addr #0 {{\n  musttail call void @{}({CTX} %0) #2\n  ret void\n}}", x.addr, x.c).unwrap();
        }
    }
    let live: Vec<&Hook> = hooks.iter().filter(|x| !answered.contains(&x.addr)).collect();
    for n in live.iter().map(|x| x.c.as_str()).collect::<BTreeSet<&str>>() {
        writeln!(m.s, "declare void @{n}({CTX}) local_unnamed_addr #1").unwrap();
    }
    m.s.push_str(&fn_attrs(false));
    m.s
}

/// a64_table.ll: the function count, their addresses (sorted: a64_lookup's binary search) and their pointers
pub fn table_ll(functions: &[u64], hooks: &[Hook]) -> String {
    let mut fns: Vec<u64> = functions.iter().copied().chain(hooks.iter().map(|h| h.addr)).collect();
    fns.sort_unstable();
    fns.dedup();
    let mut m = Ll::new();
    for &a in &fns {
        writeln!(m.s, "declare void @{}(ptr)", fname(a)).unwrap();
    }
    m.int("a64_fn_count", 32, fns.len() as u64);
    m.ints("a64_fn_addr", 32, fns.iter().copied());
    let mut init = String::with_capacity(fns.len() * 18 + 2);
    init.push('[');
    for (i, &a) in fns.iter().enumerate() {
        write!(init, "{}ptr @{}", if i > 0 { ", " } else { "" }, fname(a)).unwrap();
    }
    init.push(']');
    m.global("a64_fn_ptr", &format!("[{} x ptr]", fns.len()), &init, Ll::array_align(fns.len() as u64 * 8, 8));
    m.s
}

/// The relocated slots holding a function of the table, by address: (slot, the function's index in a64_table)
fn slot_list(img: &Image, facts: &Facts, functions: &[u64]) -> Vec<(u64, usize)> {
    let mut fns: Vec<u64> = functions.iter().copied().chain(img.hooks.iter().map(|h| h.addr)).collect();
    fns.sort_unstable();
    fns.dedup();
    let headers: Vec<u64> = img.modules.iter().flat_map(|m| m.header()).collect();
    let mut slots: Vec<(u64, usize)> = facts
        .reloc_slots
        .iter()
        .filter_map(|&at| {
            let v = u64::from_le_bytes(img.read(at, 8).unwrap().try_into().unwrap());
            let k = fns.binary_search(&v).ok()?;
            (!headers.contains(&v)).then_some((at, k))
        })
        .collect();
    slots.sort_unstable();
    slots
}

/// a64_code_slots.ll: the relocated slots holding a function of the table, and that function's index; the host
/// writes the function's host pointer there
pub fn code_slots_ll(img: &Image, facts: &Facts, functions: &[u64]) -> String {
    let slots = slot_list(img, facts, functions);
    let mut m = Ll::new();
    m.int("a64_code_slot_count", 32, slots.len() as u64);
    m.ints("a64_code_slot", 32, slots.iter().map(|x| x.0));
    m.ints("a64_code_slot_fn", 32, slots.iter().map(|x| x.1 as u64));
    m.s
}

/// The attribute groups of a module of one-line functions, as clang gives C functions here (the target CPU is the
/// compile's, as for the partitions): #0 the definitions, #1 the declarations, #2 the calls; `noreturn` on all
/// three when the callee does not return
fn fn_attrs(noreturn: bool) -> String {
    let nr = if noreturn { "noreturn " } else { "" };
    format!(
        "attributes #0 = {{ {nr}nounwind sspstrong uwtable \"min-legal-vector-width\"=\"0\" \"no-trapping-math\"=\"true\" \"stack-protector-buffer-size\"=\"8\" }}\n\
         attributes #1 = {{ {nr}\"no-trapping-math\"=\"true\" \"stack-protector-buffer-size\"=\"8\" }}\n\
         attributes #2 = {{ {nr}nounwind }}\n"
    )
}

/// The context parameter (`Ctx *A64_NOESC c`)
const CTX: &str = "ptr noundef captures(address)";

/// An IR module of constant globals with the linkage and alignment clang gives a C file's `const` definitions in a
/// PIE (dso_local; an array of 16 bytes or more aligned to 16), so the objects equal those of the C they replace
struct Ll {
    s: String,
    strs: usize,
}

impl Ll {
    fn new() -> Ll {
        Ll { s: format!("target triple = \"{}\"\n", crate::emit::TRIPLE), strs: 0 }
    }

    fn global(&mut self, name: &str, ty: &str, init: &str, align: u64) {
        writeln!(self.s, "@{name} = dso_local local_unnamed_addr constant {ty} {init}, align {align}").unwrap();
    }

    fn array_align(bytes: u64, natural: u64) -> u64 {
        if bytes >= 16 { natural.max(16) } else { natural }
    }

    fn int(&mut self, name: &str, bits: u32, v: u64) {
        let v = if bits == 32 { v as u32 as i32 as i64 } else { v as i64 };
        self.global(name, &format!("i{bits}"), &v.to_string(), bits as u64 / 8);
    }

    fn ints(&mut self, name: &str, bits: u32, vs: impl ExactSizeIterator<Item = u64>) {
        let n = vs.len();
        let mut init = String::with_capacity(n * 16 + 2);
        if n == 0 {
            init.push_str("zeroinitializer");
        } else {
            init.push('[');
            for (i, v) in vs.enumerate() {
                let v = if bits == 32 { v as u32 as i32 as i64 } else { v as i64 };
                write!(init, "{}i{bits} {v}", if i > 0 { ", " } else { "" }).unwrap();
            }
            init.push(']');
        }
        let w = bits as u64 / 8;
        self.global(name, &format!("[{n} x i{bits}]"), &init, Self::array_align(n as u64 * w, w));
    }

    /// a C string's bytes and its terminator: (type, initializer)
    fn c_bytes(text: &str) -> (String, String) {
        let mut init = String::from("c\"");
        for &b in text.as_bytes().iter().chain([&0u8]) {
            if b.is_ascii_graphic() && b != b'"' && b != b'\\' || b == b' ' {
                init.push(b as char);
            } else {
                write!(init, "\\{b:02X}").unwrap();
            }
        }
        init.push('"');
        (format!("[{} x i8]", text.len() + 1), init)
    }

    /// `const char name[] = "text";`
    fn chars(&mut self, name: &str, text: &str) {
        let (ty, init) = Self::c_bytes(text);
        self.global(name, &ty, &init, Self::array_align(text.len() as u64 + 1, 1));
    }

    /// a string literal: clang's private .str constant; its name
    fn literal(&mut self, text: &str) -> String {
        let id = if self.strs == 0 { ".str".to_string() } else { format!(".str.{}", self.strs) };
        self.strs += 1;
        let (ty, init) = Self::c_bytes(text);
        writeln!(self.s, "@{id} = private unnamed_addr constant {ty} {init}, align 1").unwrap();
        id
    }

    /// `const char *const name[] = {"a", ...};`
    fn strs(&mut self, name: &str, texts: &[&str]) {
        let mut refs = Vec::new();
        for t in texts {
            let id = self.literal(t);
            refs.push(format!("ptr @{id}"));
        }
        let n = texts.len() as u64;
        let init = if n == 0 { "zeroinitializer".to_string() } else { format!("[{}]", refs.join(", ")) };
        self.global(name, &format!("[{n} x ptr]"), &init, Self::array_align(n * 8, 8));
    }
}

/// functions.txt: every lifted function and its lifted body's size
pub fn functions_txt(sizes: &[(u64, usize)]) -> String {
    let mut s = String::new();
    for (e, n) in sizes {
        writeln!(s, "{e:08x} {n}").unwrap();
    }
    s
}

/// nvn2_exports.ll: imp_<symbol> of each NVN function the SDK has, routed to runtime/nvn2.c's nvn2_dispatch
pub fn nvn_exports_ll(names: &[String]) -> String {
    let mut m = Ll::new();
    let mut calls = Vec::new();
    for n in names {
        writeln!(m.s, "@imp_{n}.k = internal global [2 x ptr] zeroinitializer, align 16").unwrap();
        calls.push((n, m.literal(nvn_proc_name(n).unwrap())));
    }
    for (n, lit) in calls {
        writeln!(m.s, "define dso_local void @imp_{n}({CTX} %0) local_unnamed_addr #0 {{\n  tail call void @nvn2_dispatch(ptr noundef %0, ptr noundef nonnull @{lit}, ptr noundef nonnull @imp_{n}.k) #2\n  ret void\n}}").unwrap();
    }
    m.s.push_str("declare void @nvn2_dispatch(ptr noundef, ptr noundef, ptr noundef) local_unnamed_addr #1\n");
    m.s.push_str(&fn_attrs(false));
    m.s
}

/// a symbol's demangled form (cpp_demangle, a fixed version: the generated file does not depend on the toolchain),
/// the symbol itself when it is no C++ name
fn demangle(sym: &str) -> String {
    let opts = cpp_demangle::DemangleOptions::default();
    cpp_demangle::Symbol::new(sym).ok().and_then(|d| d.demangle_with_options(&opts).ok()).unwrap_or_else(|| sym.to_string())
}

/// The hooks a64_stubs.ll stops in: those the host does not define, by symbol (each HLE / PATCH_ANSWER, and each NVN
/// function hooked (nvn2_exports.ll); a hook's C name is its symbol made an identifier)
fn stub_hooks<'a>(hooks: &'a [Hook], implemented: &BTreeSet<String>) -> Vec<&'a Hook> {
    hooks.iter().filter(|h| !implemented.contains(&h.sym) && nvn_proc_name(&h.sym).is_none()).collect()
}

/// a64_stubs.ll: each hook the host does not define stops the game, naming the import; and the number of stubs
pub fn stubs_ll(hooks: &[Hook], implemented: &BTreeSet<String>) -> (String, usize) {
    let mut m = Ll::new();
    let mut calls = Vec::new();
    let hs = stub_hooks(hooks, implemented);
    let n = hs.len();
    for h in hs {
        let sym = m.literal(&h.sym);
        calls.push((h, sym, m.literal(&demangle(&h.sym))));
    }
    for (h, sym, name) in calls {
        writeln!(m.s, "define dso_local void @{}({CTX} %0) local_unnamed_addr #0 {{\n  tail call void @hos_import_stop({CTX} %0, ptr noundef nonnull @{sym}, ptr noundef nonnull @{name}) #2\n  unreachable\n}}", h.c).unwrap();
    }
    writeln!(m.s, "declare void @hos_import_stop({CTX}, ptr noundef, ptr noundef) local_unnamed_addr #1").unwrap();
    m.s.push_str(&fn_attrs(true));
    (m.s, n)
}

/// the SDK thread entry's start and exit hooks (standalone.thread_hooks)
fn thread_hooks(img: &Image) -> (u64, u64) {
    for m in &img.modules {
        let mut pc = m.base;
        while pc < m.base + m.size - 12 {
            if img.in_code(pc) && img.word(pc) == 0xD53BD061 && img.word(pc + 4) == 0x9107E021 && img.word(pc + 8) == 0xF9000020 {
                let mut hooks = Vec::new();
                let mut page: HashMap<u32, u64> = HashMap::new();
                let mut q = pc + 12;
                while q < pc + 12 + 4 * 40 {
                    let w = img.word(q);
                    if w & 0x9F000000 == 0x90000000 {
                        let imm = (((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)) as i64;
                        let imm = if imm & (1 << 20) != 0 { imm - (1 << 21) } else { imm };
                        page.insert(w & 31, ((q & !0xFFF) as i64 + (imm << 12)) as u64);
                    } else if w & 0xFFC00000 == 0xF9400000 && page.contains_key(&((w >> 5) & 31)) {
                        let got = page.remove(&((w >> 5) & 31)).unwrap() + ((w >> 10) & 0xFFF) as u64 * 8;
                        hooks.push(u64::from_le_bytes(img.read(got, 8).unwrap().try_into().unwrap()));
                    }
                    if hooks.len() == 2 {
                        return (hooks[0], hooks[1]);
                    }
                    q += 4;
                }
            }
            pc += 4;
        }
    }
    (0, 0)
}

/// What hos_image.ll defines: the main thread's start from main.npdm, the image's bounds and modules, the entry
/// points the host calls, the main module's build id and the thread hooks
struct ImageDefs {
    stack: u64,
    prio: u8,
    core: u8,
    base: u64,
    end: u64,
    /// (init, name) of each module with an init, rtld excepted
    mods: Vec<(u64, String)>,
    start: u64,
    main: u64,
    program_id: u64,
    build_id: String,
    /// hos_entry_<name>: the libc and SDK entries after nnMain
    entries: Vec<(&'static str, u64)>,
    thread_start: u64,
    thread_exit: u64,
}

fn image_defs(img: &Image, facts: &Facts, npdm: &[u8]) -> Result<ImageDefs, String> {
    let ex = |n: &str| facts.exports.iter().find(|e| e.0 == n).map_or(0, |e| e.1);
    if npdm.len() < 0x80 || &npdm[..4] != b"META" {
        return Err("main.npdm: no META header".into());
    }
    let le = |b: &[u8]| b.iter().rev().fold(0u64, |v, &x| v << 8 | x as u64);
    let (aoff, asz) = (le(&npdm[0x70..0x74]) as usize, le(&npdm[0x74..0x78]) as usize);
    let aci = &npdm[aoff.min(npdm.len())..(aoff + asz).min(npdm.len())];
    let (thread_start, thread_exit) = thread_hooks(img);
    Ok(ImageDefs {
        stack: le(&npdm[0x1C..0x20]),
        prio: npdm[0xE],
        core: npdm[0xF],
        base: img.base,
        end: img.end,
        mods: img.modules.iter().filter(|m| m.init != 0 && m.name != "rtld").map(|m| (m.init, m.name.clone())).collect(),
        start: ex("nninitStartup"),
        main: ex("nnMain"),
        program_id: if aci.len() >= 4 && &aci[..4] == b"ACI0" { le(&aci[0x10..0x18]) } else { 0 },
        build_id: img.modules.iter().find(|m| m.name == "main").map_or("", |m| m.build_id.as_str()).to_string(),
        entries: ["_init_libc0", "_init_libc1", "_init_libc2", "nnosInitialize", "nninitInitializeSdkModule", "nvnBootstrapLoaderInternal"]
            .into_iter()
            .map(|sym| (sym.trim_start_matches('_'), ex(sym)))
            .collect(),
        thread_start,
        thread_exit,
    })
}

/// hos_image.ll
pub fn image_ll(img: &Image, facts: &Facts, npdm: &[u8]) -> Result<String, String> {
    let d = image_defs(img, facts, npdm)?;
    let mut m = Ll::new();
    m.int("hos_main_stack_size", 64, d.stack);
    m.int("hos_main_priority", 32, d.prio as u64);
    m.int("hos_main_core", 32, d.core as u64);
    m.int("hos_image_base", 64, d.base);
    m.int("hos_image_end", 64, d.end);
    m.int("hos_module_count", 32, d.mods.len() as u64);
    m.ints("hos_module_init", 64, d.mods.iter().map(|x| x.0));
    m.strs("hos_module_name", &d.mods.iter().map(|x| x.1.as_str()).collect::<Vec<_>>());
    m.int("hos_entry_nninitStartup", 64, d.start);
    m.int("hos_entry_nnMain", 64, d.main);
    m.int("hos_program_id", 64, d.program_id);
    m.chars("hos_main_build_id", &d.build_id);
    for (name, a) in &d.entries {
        m.int(&format!("hos_entry_{name}"), 64, *a);
    }
    m.int("hos_thread_start_hook", 64, d.thread_start);
    m.int("hos_thread_exit_hook", 64, d.thread_exit);
    Ok(m.s)
}

const ADDR64: &str = "#ifndef A64_ADDR64\n#define A64_ADDR64 1   /* (nx2/rtgen.py: 64-bit guest window) */\n#endif\n#if A64_ADDR64\n#define A64_PTR(a) (A64_MEM + (uint64_t)(a))\n#else\n#define A64_PTR(a) (A64_MEM + (uint32_t)(a))\n#endif";
const LOOKUP: &str = "    uint32_t a = (uint32_t)addr, lo = 0, hi = a64_fn_count;";

/// rt64/a64.h and a64_rt.c (rtgen.write): a64-kit's rt/ sources with the 64-bit guest window, each substitution
/// required to match exactly once
pub fn rt64(root: &Path) -> Result<Vec<(&'static str, String)>, String> {
    let lookup_new = format!("#if A64_ADDR64\n    if (addr >> 32) return NULL;   /* (code lies below 4 GB: never a truncated match) */\n#endif\n{LOOKUP}");
    let subst: [(&str, Vec<(&str, &str)>); 2] = [
        ("a64.h", vec![("#define A64_PTR(a) (A64_MEM + (uint32_t)(a))", ADDR64)]),
        ("a64_rt.c", vec![(LOOKUP, lookup_new.as_str())]),
    ];
    let mut out = Vec::new();
    for (name, subs) in subst {
        let p = root.join("third_party/a64-kit/rt").join(name);
        let mut text = std::fs::read_to_string(&p).map_err(|e| format!("{}: {e}", p.display()))?;
        for (old, new) in subs {
            let n = text.matches(old).count();
            if n != 1 {
                return Err(format!("rtgen: {:?} found {n} times in a64-kit rt/{name} (expected once)", &old[..old.len().min(60)]));
            }
            text = text.replacen(old, new, 1);
        }
        out.push((name, format!("/* Generated by mkw-recomp nx2/rtgen.py from a64-kit rt/{name} (64-bit guest window). Do not edit. */\n{text}")));
    }
    Ok(out)
}
