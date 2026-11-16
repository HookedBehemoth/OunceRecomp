//! ounce-lift: the lifter (nx2/lift2) in Rust. AArch64 functions from the lift export (EXPORT.md) -> typed SSA IR
//! (ir) through the semantic templates (sem) -> textual LLVM IR (emit), byte for byte the Python lifter's text.

pub mod bounds;
pub mod build;
pub mod check;
pub mod discover;
pub mod eden;
pub mod emit;
pub mod export;
pub mod fp_estimate;
pub mod game;
pub mod gen;
pub mod image;
pub mod ir;
pub mod nso;
pub mod part;
pub mod patch;
pub mod plan;
pub mod sem;
pub mod summary;
pub mod ty;

/// nanoseconds the parallel stages' tasks ran (their busy time: the process CPU less the pool's idle spinning)
pub static BUSY_NS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

/// runs a parallel stage's task, counting its time into BUSY_NS
#[inline]
pub fn busy<R>(f: impl FnOnce() -> R) -> R {
    let t = std::time::Instant::now();
    let r = f();
    BUSY_NS.fetch_add(t.elapsed().as_nanos() as u64, std::sync::atomic::Ordering::Relaxed);
    r
}
