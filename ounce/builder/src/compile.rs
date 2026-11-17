//! The lifted code's compile in this process: each partition's object is the one `opt | clang -x ir -c` makes with
//! the same flags, pass pipeline and code generation level, byte for byte.

use crate::llvm;
use std::sync::{Condvar, Mutex};

/// every partition's (and the generated code's) CPU: the game is built for x86-64-v3 alone
pub const CPU: &str = "x86-64-v3";

/// clang's and opt's -mllvm options of a partition (the dead-store limits). LLVM's options are globals of the process:
/// `use_options` sets them for a phase of compiles that all want the same values.
pub const LLVM_OPTIONS: &[(&str, u32)] = &[
    ("capture-tracking-max-uses-to-explore", 100000),
    ("dse-memoryssa-defs-per-block-limit", 100000),
    ("dse-memoryssa-scanlimit", 1000),
    ("dse-memoryssa-walklimit", 1000),
    ("dse-memoryssa-otherbb-cost", 1),
];

/// the options any phase sets: LLVM_OPTIONS' and the rotation threshold
const TOUCHED: &[&str] = &[
    "capture-tracking-max-uses-to-explore",
    "dse-memoryssa-defs-per-block-limit",
    "dse-memoryssa-scanlimit",
    "dse-memoryssa-walklimit",
    "dse-memoryssa-otherbb-cost",
    "rotation-max-header-size",
];

/// their values before the builder set any (LLVM's defaults: what clang uses without -mllvm)
static DEFAULTS: std::sync::OnceLock<Vec<(&'static str, u32)>> = std::sync::OnceLock::new();

/// Sets every option a phase may touch: LLVM's defaults, then `over`. No compile may run meanwhile.
pub fn use_options(over: &[(&str, u32)]) {
    for (name, v) in DEFAULTS.get().expect("compile::init first") {
        llvm::uint_option(name, Some(*v));
    }
    for (name, v) in over {
        llvm::uint_option(name, Some(*v)).unwrap_or_else(|| panic!("LLVM has no option {name}"));
    }
}

/// clang 23's -O1 pipeline for x86-64-v3 with `dse` after `memcpyopt`: the cold partitions' pipeline
/// (the pipeline text clang prints, which `opt` parses)
pub const PIPELINE_COLD: &str = include_str!("../data/pipeline_cold_O1.txt");

/// LoopRotation's -rotation-max-header-size: 0 for the hot partitions' pipeline, the default (16) for the cold
/// ones'. The option is a global LLVM reads
/// while the pass runs, so the gate below keeps hot and cold compiles from running at the same time.
const ROTATION_HOT: u32 = 0;
const ROTATION_COLD: u32 = 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Level {
    /// the O1 pipeline with DSE, -O1 code generation
    Cold,
    /// clang -O2 (RX_HOT_OPT's default): its own pipeline (default<O2> with the SLP vectorizer; the pipeline text
    /// clang prints loses VectorCombine's early-folds-only setting), -O2 code generation
    Hot,
}

impl Level {
    fn recipe(self) -> llvm::Recipe<'static> {
        match self {
            Level::Cold => llvm::Recipe { cpu: CPU, pipeline: PIPELINE_COLD.trim(), level: 1, slp: false, rotation: Some(ROTATION_COLD) },
            Level::Hot => llvm::Recipe { cpu: CPU, pipeline: "default<O2>", level: 2, slp: true, rotation: Some(ROTATION_HOT) },
        }
    }
}

/// the targets, and LLVM's defaults of the options the phases set; then the partitions' options are in force
pub fn init() -> Result<(), String> {
    llvm::init(&[])?;
    DEFAULTS.get_or_init(|| TOUCHED.iter().map(|n| (*n, llvm::uint_option(n, None).unwrap_or_else(|| panic!("LLVM has no option {n}")))).collect());
    use_options(LLVM_OPTIONS);
    Ok(())
}

/// Compiles at one rotation threshold at a time: (the value in force, compiles running under it)
static GATE: Mutex<(Option<u32>, usize)> = Mutex::new((None, 0));
static GATE_CV: Condvar = Condvar::new();

fn gated<R>(rotation: Option<u32>, f: impl FnOnce() -> R) -> R {
    if let Some(v) = rotation {
        let mut g = GATE.lock().unwrap();
        while g.1 > 0 && g.0 != Some(v) {
            g = GATE_CV.wait(g).unwrap();
        }
        *g = (Some(v), g.1 + 1);
    }
    let r = f();
    if rotation.is_some() {
        let mut g = GATE.lock().unwrap();
        g.1 -= 1;
        if g.1 == 0 {
            GATE_CV.notify_all();
        }
    }
    r
}

/// a partition's object. `name` is the module's source name, which the object's file symbol holds (clang reading
/// the IR to clang: "<stdin>", for objects equal to its)
pub fn partition(ir: &[u8], name: &str, level: Level) -> Result<Vec<u8>, String> {
    let r = level.recipe();
    gated(r.rotation, || llvm::compile(ir, name, &r))
}
