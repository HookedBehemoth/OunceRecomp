//! The prepared texture store: every BNTX texture of a romfs deswizzled to linear blocks, ASTC re-encoded to BCn (or
//! kept exact where BCn falls below the quality floor), zstd-compressed into packs with an index, for the renderer to
//! copy straight into Vulkan images. `run::run` makes a store (and, when asked, the romfs copy's texture files with
//! their texels zeroed: `hollow`); `compare::compare` checks two against each other.

pub mod bntx;
pub mod compare;
pub mod convert;
pub mod hollow;
pub mod native;
pub mod plan;
pub mod romfs;
pub mod run;
pub mod store;

#[cfg(test)]
mod tests;
