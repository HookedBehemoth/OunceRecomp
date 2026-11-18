//! The direct back end (renderer/) without a game or a GPU: layer 0's window scheme, strict mode, the register-pressure
//! pass on SPIR-V (vk_spv_late.cpp), the Sirit parts the translator relies on (DrawParameters, struct identity), and IR
//! programs through the translator itself (its subgroup operations).

use ounce_tests::*;
use regex::Regex;
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::OnceLock;

// ---- layer 0's windows (renderer/layer0_windows.h, vk_pools.cpp)

/// The windows are a granule short of 4 GB: the longest whole range, the last granule of a stretch in one window only,
/// pieces cut at a window's end (fixtures/direct_layer0_windows_test.cpp).
#[test]
fn layer0_windows() {
    let t = Tmp::new("direct-layer0");
    let src = fixture_path("direct_layer0_windows_test.cpp");
    let exe = compile(
        &cxx(),
        &["-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread", &inc("renderer"), src.to_str().unwrap()],
        &t.join("layer0"),
    );
    run(clean(&exe).current_dir(root()));
}

/// vk_pools.cpp binds through the pieces (cut at the window's end), imports outside its lock, publishes a chunk's bits
/// only after its binds were waited for, and reads no environment.
#[test]
fn layer0_pools() {
    let pools = read("renderer/vk_pools.cpp");
    assert!(!pools.contains("getenv"), "vk_pools.cpp reads the environment (no switch but HOS_DBG, read in vk_main.cpp)");
    // every bind goes through range_places (pieces cut at the window's end); chunk_places only names the windows to make
    assert!(block(&pools, "void add_binds(").contains("l0::range_places(lo, hi, ps)"));
    let claimed = block(&pools, "void bind_claimed(");
    let cp = &claimed[find(claimed, "l0::chunk_places(")..];
    let lp = find(cp, "for (int i = 0; i < n; i++)");
    let line_end = lp + find(&cp[lp..], "\n");
    assert!(cp[..line_end].contains("window_make(pl[i].window)"), "chunk_places makes the windows");
    // imports outside mu; binds submitted and waited for before the bits; the bits before the chunk's state
    assert!(find(claimed, "dma_buf_memory(") < find(claimed, "std::lock_guard lk(mu)"), "imports outside the lock");
    assert!(find(claimed, "submit_binds(binds)") < find(claimed, "bits->set(c)"), "the binds before the bits");
    assert!(find(claimed, "bits->set(c)") < find(claimed, "cstate[c].store(CH_BOUND"), "the bits before the state");
    assert!(block(&pools, "VkResult submit_binds(").contains("vkWaitForFences"));
    // the importer claims only what is still queued; the host's discard check counts chunks being imported
    assert!(block(&pools, "void worker_main(").contains("compare_exchange_strong(st, CH_IMPORTING"));
    assert!(block(&pools, "bool layer0_bound(").contains("CH_IMPORTING"));
    // a span: whole in the window of its start, or not given
    let span = block(&pools, "bool layer0_span(");
    assert!(find(span, "l0::fits_window(gpu, size)") < find(span, "bind_claimed(claimed)"));
}

// ---- strict mode (renderer/vk_check.{h,cpp}), read from the source

struct Strict {
    check: String,
    header: String,
    sources: BTreeMap<String, String>,
}

fn strict() -> &'static Strict {
    static S: OnceLock<Strict> = OnceLock::new();
    S.get_or_init(|| {
        let mut sources = BTreeMap::new();
        for e in std::fs::read_dir(path("renderer")).unwrap() {
            let p = e.unwrap().path();
            if p.extension().is_some_and(|x| x == "cpp") {
                sources.insert(p.file_name().unwrap().to_string_lossy().into_owned(), std::fs::read_to_string(&p).unwrap());
            }
        }
        Strict { check: read("renderer/vk_check.cpp"), header: read("renderer/vk_check.h"), sources }
    })
}

fn c_strings(text: &str) -> Vec<String> {
    static R: OnceLock<Regex> = OnceLock::new();
    R.get_or_init(|| Regex::new(r#""((?:[^"\\]|\\.)*)""#).unwrap()).captures_iter(text).map(|c| c[1].to_string()).collect()
}

/// The known list is one table, ended by its sentinel; each entry is an event reason the back end really raises (or a
/// validation message id, or an object an ObjectsFor scope names), once, with a reason why it is known.
#[test]
fn strict_known_list() {
    let s = strict();
    let table = &s.check[find(&s.check, "const StrictKnown kStrictKnown[] = {")..];
    let table = &table[..find(table, "};")];
    let entry = Regex::new(r#"\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\}"#).unwrap();
    let entries: Vec<(String, String)> =
        entry.captures_iter(table).map(|c| (c[1].to_string(), c_strings(&c[2]).concat())).collect();
    assert!(table.trim_end().ends_with("{nullptr, nullptr},"), "the table ends with its sentinel");
    assert!(s.check.contains("sizeof kStrictKnown / sizeof kStrictKnown[0] - 1"));
    let raised: BTreeSet<String> = s.sources.values().flat_map(|t| c_strings(t)).collect();
    let validation = Regex::new(r"^validation:\S+$").unwrap();
    let objects_for = Regex::new(r"ObjectsFor objects_for\(([^;]*)\);").unwrap();
    const PREFIX: &str = "Vulkan object made in steady play: ";
    for (reason, why) in &entries {
        assert!(why.len() > 20, "a known entry says why: {reason}");
        if reason.starts_with("validation:") {
            assert!(validation.is_match(reason), "{reason}");
            continue;
        }
        if let Some(cause) = reason.strip_prefix(PREFIX) {
            let fors: BTreeSet<String> =
                s.sources.values().flat_map(|t| objects_for.captures_iter(t).flat_map(|m| c_strings(&m[1])).collect::<Vec<_>>()).collect();
            assert!(fors.contains(cause), "a known object cause no ObjectsFor names: {reason}");
            continue;
        }
        assert!(raised.contains(reason), "a known reason the back end never raises: {reason}");
    }
    let unique: BTreeSet<&String> = entries.iter().map(|(r, _)| r).collect();
    assert_eq!(unique.len(), entries.len(), "each reason once");
}

/// Strict ends the run with a nonzero code on any event not known once armed, and on any validation warning or error,
/// after the reason reaches the log.
#[test]
fn strict_stops_and_logs() {
    let s = strict();
    let ev = block(&s.check, "void strict_event(const char* reason, const char* where, ...) {");
    assert!(ev.contains("if (!strict_enabled) return;"));
    assert!(find(ev, "if (k) {") < find(ev, "stop(86)"), "a known reason never stops");
    assert!(ev.contains("strict: STOP at present"));
    let msg = block(&s.check, "VKAPI_ATTR VkBool32 VKAPI_CALL on_message(");
    assert!(msg.contains("stop(87)"));
    assert!(msg.contains("s >= 2"), "warnings and errors stop");
    let stop = block(&s.check, "[[noreturn]] void stop(int code) {");
    assert!(find(stop, "log_flush()") < find(stop, "_exit(code)"), "the reason reaches the log");
    let made = block(&s.check, "void validation_instance_made(VkInstance instance) {");
    assert!(made.contains("layer_enabled()"), "asked here or by the loader's variables");
    assert!(made.contains("strict: known: %s: %s"), "the known list in the log");
    assert!(s.header.contains("kStrictQuiet"));
    assert!(block(&s.check, "void strict_present(uint64_t present) {").contains("m + kStrictQuiet"));
    assert!(s.sources["vk_present.cpp"].contains("strict_present(n)"));
}

/// The back end's switches are HOS_DBG only, read in vk_main.cpp.
#[test]
fn strict_switches() {
    let s = strict();
    let getenv = Regex::new(r#"getenv\("([^"]+)"\)"#).unwrap();
    for (name, text) in &s.sources {
        if text.contains("getenv") {
            assert_eq!(name, "vk_main.cpp", "{name} reads the environment");
            for m in getenv.captures_iter(text) {
                assert_eq!(&m[1], "HOS_DBG");
            }
        }
    }
    assert!(s.check.contains("dbg(\"strict\")"));
}

/// Every place that counts a skipped or dropped draw, dispatch, clear, copy, report or job also raises the event, so a
/// skip cannot be counted without strict seeing it.
#[test]
fn strict_every_skip_is_an_event() {
    let s = strict();
    let draw = &s.sources["vk_draw.cpp"];
    assert!(block(draw, "void count(List* l, Reason r) {").contains("if (r != R_DRAWN && strict_on()) strict_draw(l, r);"));
    let sd = block(draw, "void strict_draw(List* l, Reason r) {");
    assert!(sd.contains("strict_program_miss(") && sd.contains("strict_event(kReasons[r]"));
    // (every draw skip returns through count(): no other early return in draw_emit records a skip)
    let emit = block(draw, "void draw_emit(List* l, uint32_t op, const void* data, uint32_t size, bool recording) {");
    for m in Regex::new(r"return ([^;]*);").unwrap().captures_iter(emit) {
        let body = &m[1];
        assert!(body.is_empty() || body.starts_with("count(l, "), "draw_emit returns without counting: {body}");
    }
    assert!(block(&s.sources["vk_compute.cpp"], "void count(List* l, Reason r) {").contains("strict_dispatch(l, r)"));
    let passes = &s.sources["vk_pass.cpp"];
    let pc = block(passes, "void count(int k) {");
    for k in ["S_NO_IMAGE", "S_MASKED", "S_CLEAR_NO_TARGET", "S_CLEAR_DISABLED", "S_COPY_SKIPPED", "S_CLEAR_TEX_SKIPPED", "S_UNRECORDED", "S_JOB_OUTSIDE"] {
        assert!(pc.contains(k), "{k}");
    }
    let jc = block(passes, "void jcount(int k) {");
    for k in ["J_NO_FORMATS", "J_MISMATCH", "J_UNENDED", "J_NOT_SECONDARY", "J_NO_PASS"] {
        assert!(jc.contains(k), "{k}");
    }
    assert!(block(passes, "extern \"C\" __attribute__((visibility(\"default\"))) int gpu_pass_known(").contains("strict_event(kJobNames[J_MISSED]"));
    let qc = block(&s.sources["vk_query.cpp"], "void count(int k, int64_t n = 1) {");
    for k in ["Q_NO_ROOM", "Q_NOT_MEMORY", "Q_UNRECORDED", "Q_TOO_MANY"] {
        assert!(qc.contains(k), "{k}");
    }
    // (Vulkan objects: one counter, made only through object_made)
    for (name, text) in &s.sources {
        if name != "vk_check.cpp" {
            assert!(!text.contains("st_objects.fetch_add"), "{name}");
        }
    }
    assert!(s.sources["vk_dump.cpp"].contains("const DebugObjects debug_aid;"));
    assert!(s.sources["vk_task.cpp"].contains("strict_event(kNames[T_REFUSED]"));
}

// ---- the register-pressure pass (renderer/vk_spv_late.cpp through tools/shaders/spvlate.cpp)

fn spv_text(s: &str) -> Vec<u32> {
    let mut b = s.as_bytes().to_vec();
    b.push(0);
    while b.len() % 4 != 0 {
        b.push(0);
    }
    b.chunks(4).map(|c| u32::from_le_bytes(c.try_into().unwrap())).collect()
}

/// A compute module: a uniform block of 8 words, a private word; ids given out as they come.
struct Module {
    out: Vec<Vec<u32>>,
    ids: u32,
    u32_: u32,
    boolean: u32,
    p_u32: u32,
    var: u32,
    priv_: u32,
    consts: Vec<u32>,
    p_phys: u32,
    addr: u32,
}

impl Module {
    fn new(physical: bool, subgroup: bool) -> Module {
        let mut m = Module { out: vec![], ids: 0, u32_: 0, boolean: 0, p_u32: 0, var: 0, priv_: 0, consts: vec![], p_phys: 0, addr: 0 };
        let void = m.id();
        let fnt = m.id();
        m.u32_ = m.id();
        m.boolean = m.id();
        let block = m.id();
        let arr = m.id();
        let p_block = m.id();
        m.p_u32 = m.id();
        m.var = m.id();
        let p_priv = m.id();
        m.priv_ = m.id();
        let main = m.id();
        let eight = m.id();
        m.consts = (0..8).map(|_| m.id()).collect();
        let u64_ = m.id();
        m.p_phys = m.id();
        m.addr = m.id();
        let (u, b, var, pv) = (m.u32_, m.boolean, m.var, m.priv_);
        m.op(17, &[1]); // OpCapability Shader
        if physical {
            m.op(17, &[5347]); // PhysicalStorageBufferAddresses
            m.op(17, &[11]); // Int64
        }
        if subgroup {
            m.op(17, &[61]); // GroupNonUniform
            m.op(17, &[64]); // GroupNonUniformBallot
        }
        m.op(14, &[if physical { 5348 } else { 0 }, 1]); // OpMemoryModel
        let mut ep = vec![5, main];
        ep.extend(spv_text("main"));
        ep.extend([var, pv]);
        m.op(15, &ep); // OpEntryPoint GLCompute
        m.op(16, &[main, 17, 1, 1, 1]); // OpExecutionMode LocalSize
        m.op(71, &[arr, 6, 16]); // ArrayStride
        m.op(71, &[block, 2]); // Block
        m.op(72, &[block, 0, 35, 0]); // Offset
        m.op(71, &[var, 34, 0]); // DescriptorSet
        m.op(71, &[var, 33, 0]); // Binding
        m.op(19, &[void]);
        m.op(33, &[fnt, void]);
        m.op(21, &[u, 32, 0]);
        m.op(20, &[b]);
        m.op(43, &[u, eight, 8]);
        for (i, c) in m.consts.clone().into_iter().enumerate() {
            m.op(43, &[u, c, i as u32]);
        }
        m.op(28, &[arr, u, eight]);
        m.op(30, &[block, arr]);
        m.op(32, &[p_block, 2, block]);
        m.op(32, &[m.p_u32, 2, u]);
        m.op(59, &[p_block, var, 2]);
        m.op(32, &[p_priv, 6, u]);
        m.op(59, &[p_priv, pv, 6]);
        if physical {
            m.op(21, &[u64_, 64, 0]);
            m.op(32, &[m.p_phys, 5349, u]);
            m.op(43, &[u64_, m.addr, 4096, 0]);
        }
        m.op(54, &[void, main, 0, fnt]);
        m
    }

    fn id(&mut self) -> u32 {
        self.ids += 1;
        self.ids
    }

    fn op(&mut self, code: u32, operands: &[u32]) {
        let mut i = vec![(operands.len() as u32 + 1) << 16 | code];
        i.extend_from_slice(operands);
        self.out.push(i);
    }

    /// a word of the uniform block: its access chain and its load
    fn load(&mut self, index: usize) -> u32 {
        let (chain, value) = (self.id(), self.id());
        let (p, v, c0, ci, u) = (self.p_u32, self.var, self.consts[0], self.consts[index], self.u32_);
        self.op(65, &[p, chain, v, c0, ci]);
        self.op(61, &[u, value, chain]);
        value
    }

    fn add(&mut self, a: u32, b: u32) -> u32 {
        let r = self.id();
        let u = self.u32_;
        self.op(128, &[u, r, a, b]); // OpIAdd
        r
    }

    fn words(mut self) -> Vec<u32> {
        self.op(56, &[]); // OpFunctionEnd
        let mut w = vec![0x07230203, 0x00010600, 0, self.ids + 1, 0];
        w.extend(self.out.into_iter().flatten());
        w
    }
}

/// main: 8 loads of the uniform block's words, a store to the private word, a chain of adds over the loads; a branch;
/// a block with a phi and a second store
fn spv_module(physical_store: bool) -> Vec<u32> {
    let mut m = Module::new(physical_store, false);
    let (entry, then, merge) = (m.id(), m.id(), m.id());
    m.op(248, &[entry]);
    let mut loads: Vec<u32> = (0..8).map(|i| m.load(i)).collect();
    let (u, pv) = (m.u32_, m.priv_);
    if physical_store {
        let ptr = m.id();
        m.op(120, &[m.p_phys, ptr, m.addr]); // OpConvertUToPtr
        m.op(62, &[ptr, m.consts[1], 2, 4]); // OpStore Aligned 4
        let (ptr2, through) = (m.id(), m.id());
        m.op(120, &[m.p_phys, ptr2, m.addr]);
        m.op(61, &[u, through, ptr2, 2, 4]); // a load by address: it must stay between the stores around it
        loads[0] = through;
        let ptr3 = m.id();
        m.op(120, &[m.p_phys, ptr3, m.addr]);
        m.op(62, &[ptr3, m.consts[2], 2, 4]); // the second store by address
    }
    m.op(62, &[pv, m.consts[0]]); // a store: nothing of the chain is needed yet
    let mut acc = loads[0];
    for &v in &loads[1..] {
        acc = m.add(acc, v);
    }
    m.op(62, &[pv, acc]); // the chain's store
    m.op(249, &[then]); // OpBranch
    m.op(248, &[then]);
    let phi = m.id();
    m.op(245, &[u, phi, acc, entry]); // OpPhi
    let twice = m.add(phi, phi);
    m.op(62, &[pv, twice]);
    m.op(249, &[merge]);
    m.op(248, &[merge]);
    m.op(253, &[]); // OpReturn
    m.words()
}

struct BranchIds {
    then: u32,
    loads: Vec<u32>,
    for_loop: u32,
    elected: u32,
}

/// main: 8 loads that only a branch uses, a value that only a loop's body uses, a subgroup operation that only the
/// branch uses; the branch; the loop
fn spv_branching() -> (Vec<u32>, BranchIds) {
    let mut m = Module::new(false, true);
    let (entry, then, merge, header, body, done) = (m.id(), m.id(), m.id(), m.id(), m.id(), m.id());
    let (u, b, pv) = (m.u32_, m.boolean, m.priv_);
    m.op(248, &[entry]);
    let loads: Vec<u32> = (0..8).map(|i| m.load(i)).collect();
    let (l1, l2) = (m.load(1), m.load(2));
    let for_loop = m.add(l1, l2); // used by the loop's body alone
    let elected = m.id();
    m.op(333, &[b, elected, m.consts[3]]); // OpGroupNonUniformElect (scope Subgroup = 3): used by the branch alone
    let cond = m.id();
    let l0 = m.load(0);
    m.op(171, &[b, cond, l0, m.consts[0]]); // OpINotEqual
    m.op(247, &[merge, 0]); // OpSelectionMerge
    m.op(250, &[cond, then, merge]); // OpBranchConditional
    m.op(248, &[then]);
    let mut acc = loads[0];
    for &v in &loads[1..] {
        acc = m.add(acc, v);
    }
    let chosen = m.id();
    m.op(169, &[u, chosen, elected, acc, m.consts[0]]); // OpSelect
    m.op(62, &[pv, chosen]);
    m.op(249, &[merge]);
    m.op(248, &[merge]);
    m.op(249, &[header]);
    m.op(248, &[header]);
    let (i, i2) = (m.id(), m.id());
    m.op(245, &[u, i, m.consts[0], merge, i2, body]);
    let less = m.id();
    m.op(176, &[b, less, i, m.consts[4]]); // OpULessThan
    m.op(246, &[done, body, 0]); // OpLoopMerge
    m.op(250, &[less, body, done]);
    m.op(248, &[body]);
    let s = m.add(for_loop, i);
    m.op(62, &[pv, s]);
    m.op(128, &[u, i2, i, m.consts[1]]);
    m.op(249, &[header]);
    m.op(248, &[done]);
    m.op(253, &[]);
    (m.words(), BranchIds { then, loads, for_loop, elected })
}

fn instructions(words: &[u32]) -> Vec<Vec<u32>> {
    let (mut out, mut i) = (vec![], 5);
    while i < words.len() {
        let wc = (words[i] >> 16) as usize;
        out.push(words[i..i + wc].to_vec());
        i += wc;
    }
    out
}

fn sorted(words: &[u32]) -> Vec<Vec<u32>> {
    let mut v = instructions(words);
    v.sort();
    v
}

/// why after is not before rearranged: the same instructions, effects in order, every value made before it is used, a
/// phi its block's first instruction
fn spv_check(before: &[u32], after: &[u32], what: &str) -> Option<String> {
    if sorted(before) != sorted(after) {
        return Some(format!("{what}: the instructions are not the same set"));
    }
    let effects = |seq: Vec<Vec<u32>>| -> Vec<Vec<u32>> {
        seq.into_iter().filter(|x| [62, 245, 246, 247, 248, 249, 250, 253, 54, 56].contains(&(x[0] & 0xffff))).collect()
    };
    if effects(instructions(before)) != effects(instructions(after)) {
        return Some(format!("{what}: the instructions with effects changed order"));
    }
    let (mut made, mut body, mut prev): (BTreeSet<u32>, bool, Option<Vec<u32>>) = (BTreeSet::new(), false, None);
    for x in instructions(after) {
        let code = x[0] & 0xffff;
        if code == 54 {
            body = true;
        }
        if !body {
            made.extend(x[1..x.len().min(3)].iter().copied());
            continue;
        }
        if [65, 61, 128, 120, 169, 171, 176].contains(&code) {
            // (operands after the result; a load's and a convert's first one alone)
            let ops = if code == 61 || code == 120 { &x[3..x.len().min(4)] } else { &x[3..] };
            for o in ops {
                if !made.contains(o) {
                    return Some(format!("{what}: {o} used before it is made"));
                }
            }
            made.insert(x[2]);
        } else if code == 333 {
            made.insert(x[2]);
        } else if code == 245 {
            made.insert(x[2]);
            if prev.as_ref().is_none_or(|p| p[0] & 0xffff != 248) {
                return Some(format!("{what}: a phi is not its block's first instruction"));
            }
        }
        prev = Some(x);
    }
    None
}

fn most_loads_held(words: &[u32]) -> usize {
    let seq = instructions(words);
    let mut last_use = HashMap::new();
    for (n, x) in seq.iter().enumerate() {
        for &o in x.iter().skip(3) {
            last_use.insert(o, n as i64);
        }
    }
    let (mut held, mut most) = (BTreeSet::new(), 0);
    for (n, x) in seq.iter().enumerate() {
        if x[0] & 0xffff == 61 {
            held.insert(x[2]);
        }
        most = most.max(held.len());
        held.retain(|v| last_use.get(v).copied().unwrap_or(-1) > n as i64);
    }
    most
}

/// {result id: the label of the block that makes it}
fn block_of(words: &[u32]) -> BTreeMap<u32, u32> {
    let (mut out, mut label) = (BTreeMap::new(), None);
    for x in instructions(words) {
        let code = x[0] & 0xffff;
        if code == 248 {
            label = Some(x[1]);
        } else if let Some(l) = label {
            if [61, 65, 128, 169, 171, 176, 333].contains(&code) {
                out.insert(x[2], l);
            }
        }
    }
    out
}

fn to_bytes(words: &[u32]) -> Vec<u8> {
    words.iter().flat_map(|w| w.to_le_bytes()).collect()
}

fn from_bytes(b: &[u8]) -> Vec<u32> {
    b.chunks(4).map(|c| u32::from_le_bytes(c.try_into().unwrap())).collect()
}

struct SpvLate {
    tmp: Tmp,
    exe: PathBuf,
    val: PathBuf,
}

impl SpvLate {
    /// the pass on words (name: the files' name, args: spvlate's): its output (checked by spirv-val) and its line
    fn run(&self, words: &[u32], name: &str, args: &[&str]) -> (Vec<u32>, String) {
        let (src, dst) = (self.tmp.join(&format!("{name}.spv")), self.tmp.join(&format!("{name}.late.spv")));
        std::fs::write(&src, to_bytes(words)).unwrap();
        let line = text(&run(Command::new(&self.exe).arg(&src).arg(&dst).args(args)).stdout);
        let ok = output(Command::new(&self.val).args(["--target-env", "vulkan1.3", "--scalar-block-layout"]).arg(&dst)).status.success();
        assert!(ok, "{name} after the pass is not valid SPIR-V");
        (from_bytes(&std::fs::read(&dst).unwrap()), line)
    }
}

/// Modules shaped as the translator emits them (everything loaded first, used later): within the budget a module is
/// left word for word; past it (a budget of 2) the loads follow their uses with every instruction there once, effects
/// in order, values made before use, a phi first in its block, fewer loads held (8 before, 4 or fewer after); the same
/// input gives the same output; a load by address stays between the two stores by address around it; across blocks
/// the loads only a branch uses go into it, a value a loop uses from outside stays out, a subgroup operation stays in
/// its block, and nothing leaves its block unless the function is past sink_peak. With glslangValidator and spirv-opt
/// the synthetic spilling stage (tools/shaders/probes/spill.mesh) comes out holding about the budget.
#[test]
fn spv_late() {
    let tmp = Tmp::new("spv-late");
    let exe = compile(&cxx(), &["-std=c++20", "-O1", path("tools/shaders/spvlate.cpp").to_str().unwrap()], &tmp.join("spvlate"));
    let p = SpvLate { tmp, exe, val: tool("spirv-val") };

    let plain = spv_module(false);
    let own = p.tmp.join("check.spv");
    std::fs::write(&own, to_bytes(&plain)).unwrap();
    assert!(output(Command::new(&p.val).args(["--target-env", "vulkan1.3"]).arg(&own)).status.success(), "the test's own module is not valid SPIR-V");
    let (same, _) = p.run(&plain, "within", &[]);
    assert!(same == plain, "a module within the budget was changed");

    let (late, _) = p.run(&plain, "plain", &["--budget", "2"]);
    let (again, _) = p.run(&plain, "plain2", &["--budget", "2"]);
    if let Some(why) = spv_check(&plain, &late, "the module") {
        panic!("{why}");
    }
    let (before, after) = (most_loads_held(&plain), most_loads_held(&late));
    assert!(before == 8 && after <= 4, "loaded values held at most: {before} before (8 expected), {after} after (4 or fewer expected)");
    assert!(again == late, "the same module came out differently twice");

    let writer = spv_module(true);
    let (late_w, _) = p.run(&writer, "writer", &["--budget", "2"]);
    if let Some(why) = spv_check(&writer, &late_w, "the module that writes by address") {
        panic!("{why}");
    }
    let seq = instructions(&late_w);
    let through: Vec<usize> = seq.iter().enumerate().filter(|(_, x)| x[0] & 0xffff == 61 && x.len() == 6).map(|(n, _)| n).collect();
    let stores: Vec<usize> = seq.iter().enumerate().filter(|(_, x)| x[0] & 0xffff == 62 && x.len() == 5).map(|(n, _)| n).collect();
    assert!(
        late_w != writer && through.len() == 1 && stores.len() == 2 && stores[0] < through[0] && through[0] < stores[1],
        "the load by address left the two stores by address around it (or nothing moved)"
    );

    let (words, ids) = spv_branching();
    let (local, _) = p.run(&words, "branch_local", &["--budget", "2", "--sink-peak", "0"]);
    if let Some(why) = spv_check(&words, &local, "the branching module, within blocks") {
        panic!("{why}");
    }
    assert!(block_of(&local) == block_of(&words), "an instruction left its block with sink_peak 0");
    let (sunk, line) = p.run(&words, "branch", &["--budget", "2", "--sink-peak", "4"]);
    if let Some(why) = spv_check(&words, &sunk, "the branching module") {
        panic!("{why}");
    }
    let (was, now) = (block_of(&words), block_of(&sunk));
    let gone = ids.loads[1..].iter().filter(|v| now[v] == ids.then).count();
    assert!(gone == 7, "{gone} of the 7 loads that only the branch uses went into it ({})", line.trim());
    assert!(now[&ids.for_loop] == was[&ids.for_loop], "a value the loop's body uses from outside went into the loop");
    assert!(now[&ids.elected] == was[&ids.elected], "a subgroup operation left its block");

    let Some(glslang) = which("glslangValidator") else {
        println!("glslangValidator not found: the synthetic spilling stage not tried");
        return;
    };
    let Some(opt) = which("spirv-opt") else {
        println!("spirv-opt not found: the synthetic spilling stage not tried");
        return;
    };
    // (the stage's locals as values, as the translator's stages have them: glslang keeps them in variables)
    let (spv, ssa) = (p.tmp.join("spill.in.spv"), p.tmp.join("spill.ssa.spv"));
    run(Command::new(glslang).args(["-V", "--target-env", "vulkan1.3", "-o"]).arg(&spv).arg(path("tools/shaders/probes/spill.mesh")));
    run(Command::new(opt)
        .args(["--eliminate-local-single-block", "--eliminate-local-single-store", "--ssa-rewrite", "--eliminate-dead-code-aggressive", "-o"])
        .arg(&ssa)
        .arg(&spv));
    let src_words = from_bytes(&std::fs::read(&ssa).unwrap());
    let (out, line) = p.run(&src_words, "spill", &[]);
    let m = Regex::new(r"values held at most (\d+) -> (\d+)").unwrap().captures(&line).map(|c| (c[1].parse::<u32>().unwrap(), c[2].parse::<u32>().unwrap()));
    assert!(
        m.is_some_and(|(b, a)| b >= 320 && a <= 200) && sorted(&src_words) == sorted(&out),
        "the synthetic spilling stage: {} (320 or more before and 200 or fewer after expected)",
        line.trim()
    );
}

// ---- Sirit (third_party/sirit), as the shader translator uses it

fn spirv_headers() -> PathBuf {
    match std::env::var_os("OUNCE_SPIRV_HEADERS").filter(|v| !v.is_empty()) {
        Some(v) => PathBuf::from(v),
        None => path("third_party/SPIRV-Headers/include"),
    }
}

const SIRIT_FLAGS: &[&str] = &["-std=c++20", "-O1"];

fn sirit_includes() -> Vec<String> {
    vec![inc("third_party/sirit/include"), inc("third_party/sirit/src"), format!("-I{}", spirv_headers().display())]
}

fn cpp_files(dir: &Path, out: &mut Vec<PathBuf>) {
    for e in std::fs::read_dir(dir).unwrap() {
        let p = e.unwrap().path();
        if p.is_dir() {
            cpp_files(&p, out);
        } else if p.extension().is_some_and(|x| x == "cpp") {
            out.push(p);
        }
    }
}

/// FNV-1a 64
fn fnv(h: &mut u64, b: &[u8]) {
    for &x in b {
        *h ^= x as u64;
        *h = h.wrapping_mul(0x100000001b3);
    }
}

fn all_files(dir: &Path, out: &mut Vec<PathBuf>) {
    for e in std::fs::read_dir(dir).unwrap() {
        let p = e.unwrap().path();
        if p.is_dir() {
            all_files(&p, out);
        } else {
            out.push(p);
        }
    }
}

/// Objects of `files` (under `base`), compiled once in parallel and kept in the target folder under a hash of every file
/// in `hashed`, the SPIR-V headers' path, the compiler and the flags
fn cached_objects(tag: &str, base: &Path, mut files: Vec<PathBuf>, hashed: &[PathBuf], flags: &[String]) -> Vec<PathBuf> {
    let mut all = vec![];
    for d in hashed {
        all_files(d, &mut all);
    }
    all.sort();
    files.sort();
    let mut h = 0xcbf29ce484222325u64;
    for p in &all {
        fnv(&mut h, p.to_string_lossy().as_bytes());
        fnv(&mut h, &std::fs::read(p).unwrap());
    }
    fnv(&mut h, spirv_headers().to_string_lossy().as_bytes());
    fnv(&mut h, cxx().as_bytes());
    for f in flags {
        fnv(&mut h, f.as_bytes());
    }
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR")).join(format!("{tag}-{h:016x}"));
    std::fs::create_dir_all(&dir).unwrap();
    let objs: Vec<PathBuf> = files
        .iter()
        .map(|f| dir.join(f.strip_prefix(base).unwrap().to_string_lossy().replace(['/', '\\'], "_").replace(".cpp", ".o")))
        .collect();
    // (as many compilers at once as there are cores: the translator's sources are many and large)
    let todo: Vec<(&PathBuf, &PathBuf)> = files.iter().zip(&objs).filter(|(_, o)| !o.is_file()).collect();
    let next = std::sync::atomic::AtomicUsize::new(0);
    let workers = std::thread::available_parallelism().map_or(4, |n| n.get()).min(todo.len());
    std::thread::scope(|s| {
        for _ in 0..workers {
            s.spawn(|| {
                while let Some(&(src, obj)) = todo.get(next.fetch_add(1, std::sync::atomic::Ordering::Relaxed)) {
                    // (written beside, then renamed: a run stopped halfway leaves no half object)
                    let part = obj.with_extension(format!("o.{}", std::process::id()));
                    run(Command::new(cxx()).args(flags).arg("-c").arg(src).arg("-o").arg(&part));
                    std::fs::rename(&part, obj).unwrap();
                }
            });
        }
    });
    objs
}

/// Sirit's objects (cached_objects)
fn sirit_objects() -> &'static [PathBuf] {
    static OBJS: OnceLock<Vec<PathBuf>> = OnceLock::new();
    OBJS.get_or_init(|| {
        let sirit = path("third_party/sirit");
        let mut files = vec![];
        cpp_files(&sirit.join("src"), &mut files);
        let flags: Vec<String> = SIRIT_FLAGS.iter().map(|f| f.to_string()).chain(sirit_includes()).collect();
        cached_objects("sirit", &sirit, files, &[sirit.join("include"), sirit.join("src")], &flags)
    })
}

/// The flags tools/shadercensus/build.sh builds the translator's callers with
fn eden_flags() -> Vec<String> {
    let mut f: Vec<String> = ["-std=c++20", "-O1", "-g0", "-D_GNU_SOURCE", "-Wno-everything", "-DMKWVK_BUILDING", "-DMKWVK_INCLUDES=2"]
        .iter()
        .map(|f| f.to_string())
        .collect();
    for h in ["algorithm", "cstdio", "cstdlib", "cstring", "functional", "string", "vector"] {
        f.extend(["-include".to_string(), h.to_string()]);
    }
    f.extend([inc("third_party/eden/compat"), inc("third_party/eden"), inc("runtime/os")]);
    f.extend(sirit_includes());
    f.push(format!("-I{}", spirv_headers().join("spirv/unified1").display()));
    f
}

/// The translator's objects (cached_objects): the sources its meson.build files list
fn eden_objects() -> &'static [PathBuf] {
    static OBJS: OnceLock<Vec<PathBuf>> = OnceLock::new();
    OBJS.get_or_init(|| {
        let eden = path("third_party/eden");
        let listed = Regex::new(r"'([\w/]+\.cpp)'").unwrap();
        let mut all = vec![];
        all_files(&eden.join("shader_recompiler"), &mut all);
        let mut files = vec![];
        for m in all.iter().filter(|p| p.file_name().is_some_and(|n| n == "meson.build")) {
            let text = std::fs::read_to_string(m).unwrap();
            files.extend(listed.captures_iter(&text).map(|c| m.parent().unwrap().join(&c[1])));
        }
        assert!(files.len() > 100, "the translator's meson.build files list {} sources", files.len());
        cached_objects("eden", &eden, files, &[eden.clone(), path("third_party/sirit"), path("runtime/os")], &eden_flags())
    })
}

/// compiles a fixture against Sirit, runs it with a folder for its modules and checks each of them with spirv-val
fn sirit_fixture(name: &str, cpp: &str, modules: usize) {
    let val = tool("spirv-val");
    let objs = sirit_objects();
    let t = Tmp::new(name);
    let src = t.write("fixture.cpp", cpp);
    let exe = t.join("fixture");
    run(Command::new(cxx()).args(SIRIT_FLAGS).args(sirit_includes()).arg(&src).args(objs).arg("-o").arg(&exe));
    run(Command::new(&exe).arg(t.path()));
    for case in 0..modules {
        run(Command::new(&val).args(["--target-env", "vulkan1.3"]).arg(t.join(&format!("{case}.spv"))));
    }
}

/// The DrawParameters capability block of the translator (eden's emit_spirv.cpp) is valid SPIR-V over all 64
/// combinations of the inputs a program loads and the profile's support for the Vulkan instance and vertex ids.
#[test]
fn draw_parameters() {
    let source = read("third_party/eden/shader_recompiler/backend/spirv/emit_spirv.cpp");
    let at = find(&source, "ctx.AddCapability(spv::Capability::DrawParameters);");
    let start = source[..at].rfind("\n    if (").expect("no if before the DrawParameters capability") + 1;
    let end = at + find(&source[at..], "\n    }") + 6;
    sirit_fixture("draw-parameters", &fixture("draw_parameters.cpp").replace("@CAPABILITY@", &source[start..end]), 64);
}

/// Sirit keeps independently decorated structures separate (a first-task block, Private local memory and a
/// global-memory wrapper), while scalar and pointer declarations keep their reuse.
#[test]
fn spirv_struct_identity() {
    sirit_fixture("struct-identity", &fixture("spirv_struct_identity.cpp"), 2);
}

/// IR programs as an Ampere front writes them, through the translator's passes and SPIR-V back end
/// (fixtures/subgroup_ir_ops.cpp): the uniform registers and predicates through the SSA rewrite, SubgroupReadFirstLane and
/// the eight SubgroupReduce ops, valid SPIR-V on a 32-lane subgroup and on a wider one (clusters of 32),
/// MaximallyReconvergesKHR only where the profile supports it, and a translation error (not the lane's own value) in a
/// stage without subgroup operations.
#[test]
fn subgroup_ir_ops() {
    let val = tool("spirv-val");
    let dis = tool("spirv-dis");
    let t = Tmp::new("subgroup-ir-ops");
    let exe = t.join("fixture");
    run(Command::new(cxx())
        .args(eden_flags())
        .arg(fixture_path("subgroup_ir_ops.cpp"))
        .args(eden_objects())
        .args(sirit_objects())
        .arg("-lpthread")
        .arg("-o")
        .arg(&exe));
    run(Command::new(&exe).arg(t.path()));
    let reduces = ["IAdd", "UMin", "UMax", "SMin", "SMax", "BitwiseAnd", "BitwiseOr", "BitwiseXor"];
    for (case, wider, reconverges) in [(0, false, true), (1, true, true), (2, false, false)] {
        let spv = t.join(&format!("{case}.spv"));
        run(Command::new(&val).args(["--target-env", "vulkan1.3"]).arg(&spv));
        let asm = text(&run(Command::new(&dis).arg(&spv)).stdout);
        let has = |s: &str| asm.contains(s);
        assert!(has("OpCapability GroupNonUniformArithmetic"), "case {case}: no GroupNonUniformArithmetic");
        assert!(has("OpGroupNonUniformBroadcastFirst") != wider, "case {case}: the first lane's read");
        let group = if wider { "ClusteredReduce" } else { "Reduce" };
        for r in reduces {
            let want = format!("= OpGroupNonUniform{r} %u32 %u32_3 {group} ");
            assert!(has(&want), "case {case}: no {want}");
        }
        assert!(has("MaximallyReconvergesKHR") == reconverges, "case {case}: MaximallyReconvergesKHR");
        assert!(has("\"SPV_KHR_maximal_reconvergence\"") == reconverges, "case {case}: the extension");
    }
    let err = std::fs::read_to_string(t.join("3.err")).expect("case 3 (no subgroup operations) gave no error");
    // (the program's first subgroup operation fails it: LaneId, as every warp emitter requires subgroups)
    assert!(err.contains("without subgroup operations"), "case 3's error: {err}");
}

/// The post-pass IR interpreter (tools/shaders/irexec) on small programs with known results
/// (fixtures/irexec_micro.cpp): the subgroup reductions, ballots and shuffles in lockstep, a divergent if and a loop of
/// phis reconverging, the loop safety counter and the step cap, float edge cases (NaN through NMin/NMax and saturate,
/// denormals flushed, conversions saturating), NIR's bit field semantics, a fragment quad's derivatives with demoted
/// lanes, and shared atomics in lane order.
#[test]
fn irexec_micro() {
    let t = Tmp::new("irexec-micro");
    let exe = t.join("fixture");
    run(Command::new(cxx())
        .args(eden_flags())
        .arg(fixture_path("irexec_micro.cpp"))
        .arg(path("tools/shaders/irexec/irexec.cpp"))
        .args(eden_objects())
        .args(sirit_objects())
        .arg("-lpthread")
        .arg("-o")
        .arg(&exe));
    let out = output(&mut Command::new(&exe));
    let stdout = text(&out.stdout);
    assert!(out.status.success(), "irexec micro programs failed:\n{stdout}{}", text(&out.stderr));
    assert!(stdout.lines().filter(|l| l.starts_with("ok ")).count() >= 37, "cases missing:\n{stdout}");
}

/// The differential harness (tools/shaders/difftest) on a handful of instruction encodings (D1 of work/shader-plan):
/// each a micro program loading its registers from c[1], run through the old front twice on edge-value inputs; both runs
/// translate and agree, and a move's result is its source's value in every lane.
#[test]
fn difftest_micro() {
    let t = Tmp::new("difftest-micro");
    let exe = t.join("difftest");
    run(Command::new(cxx())
        .args(eden_flags())
        .args(["-DVK_NO_PROTOTYPES", &inc("third_party/SPIRV-Headers/include")])
        .arg(path("tools/shaders/difftest/difftest.cpp"))
        .arg(path("tools/shaders/irexec/irexec.cpp"))
        .args(eden_objects())
        .args(sirit_objects())
        .arg("-lpthread")
        .arg("-o")
        .arg(&exe));
    let insts = [
        "000fe40000000f000000001600197202", // MOV R25, R22
        "081fe2000041000000000012140e7220", // FMUL
        "000fe200000100190000001010147223", // FFMA
        "000e2200000010000000001b001b7308", // MUFU
        "100fe2000000410020000011ff157230", // HADD2
        "000e22000021f1000000000b000b7305", // F2I
        "000fe400078ec0ff000000ff08087812", // LOP3
        "000fe40007ffe0ff0000000d060b7210", // IADD3
        "040fe200078e020500000020080d7824", // IMAD imm
        "000fc80003f24070000000030800720c", // ISETP
        "000fe4000400000000000001ff0e7807", // SEL
        "000fe4000bf252700000003f0500728c", // UISETP
        "000fe40008000f000000000500137c02", // MOV R, UR
    ];
    let list = t.write("insts.txt", &(insts.join("\n") + "\n"));
    let seed = t.join("micro.seed");
    run(Command::new(&exe).arg("micro").arg("--insts").arg(&list).arg("--out").arg(&seed));
    let out = output(Command::new(&exe).arg("d1").arg(&seed).args(["--a", "old", "--b", "old"]));
    let stdout = text(&out.stdout);
    assert!(out.status.success(), "difftest d1 failed:\n{stdout}{}", text(&out.stderr));
    let lines: Vec<&str> = stdout.lines().filter(|l| l.starts_with('{')).collect();
    assert_eq!(lines.len(), insts.len(), "one line an encoding:\n{stdout}");
    for l in &lines {
        assert!(l.contains("\"a\": \"ok\"") && l.contains("\"equal\": true") && l.contains("\"compared\": 8"), "not translated, or old against old differs: {l}");
    }
    let shown = text(&output(Command::new(&exe).arg("d1").arg(&seed).args(["--first", "0", "--count", "1", "--show"])).stdout);
    let reg = |r: u32, lane: u32| {
        let want = format!("# a R{r} lane {lane} = ");
        shown.lines().find_map(|l| l.strip_prefix(want.as_str())).unwrap_or_else(|| panic!("no {want} in\n{shown}")).to_string()
    };
    for lane in 0..32 {
        assert_eq!(reg(25, lane), reg(22, lane), "MOV R25, R22 in lane {lane}");
    }
}
