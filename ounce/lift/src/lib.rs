//! ounce-lift: the lifter (nx2/lift2) in Rust. AArch64 functions from the lift export (EXPORT.md) -> typed SSA IR
//! (ir) through the semantic templates (sem) -> textual LLVM IR (emit), byte for byte the Python lifter's text.

pub mod build;
pub mod emit;
pub mod export;
pub mod fp_estimate;
pub mod game;
pub mod ir;
pub mod part;
pub mod sem;
pub mod ty;
