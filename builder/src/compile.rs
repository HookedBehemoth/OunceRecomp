//! The lifted code's compile: nx2/build.py's flags, pass pipelines and code generation levels, in this process.
//! Each partition's object equals the one build.py makes (`opt | clang -x ir -c`), byte for byte.

use crate::llvm;
use std::sync::{Condvar, Mutex};

/// every partition's (and the generated code's) CPU: the game is built for x86-64-v3 alone
pub const CPU: &str = "x86-64-v3";

/// clang's and opt's -mllvm options of a partition (build.py DSE_FLAGS), global to the process
pub const LLVM_OPTIONS: &[&str] = &[
    "-capture-tracking-max-uses-to-explore=100000",
    "-dse-memoryssa-defs-per-block-limit=100000",
    "-dse-memoryssa-scanlimit=1000",
    "-dse-memoryssa-walklimit=1000",
    "-dse-memoryssa-otherbb-cost=1",
];

/// clang 23's -O1 pipeline for x86-64-v3 with `dse` after `memcpyopt`: build.py's `opt` for the cold partitions
/// (opt_argv: the pipeline text clang prints, which `opt` parses, so the text is exactly what build.py runs)
pub const PIPELINE_COLD: &str = include_str!("../data/pipeline_cold_O1.txt");

/// LoopRotation's -rotation-max-header-size: build.py gives `-rotation-max-header-size=0` to clang, which runs the
/// hot partitions' pipeline, but not to the cold ones' `opt` (the default, 16). The option is a global LLVM reads
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

pub fn init() -> Result<(), String> {
    llvm::init(LLVM_OPTIONS)
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

/// a partition's object. `name` is the module's source name, which the object's file symbol holds (build.py pipes
/// the IR to clang: "<stdin>", for objects equal to its)
pub fn partition(ir: &[u8], name: &str, level: Level) -> Result<Vec<u8>, String> {
    let r = level.recipe();
    gated(r.rotation, || llvm::compile(ir, name, &r))
}
