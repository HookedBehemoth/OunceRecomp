//! ounce-core: what `ounce` and its window share, with no LLVM in it. The user's config file (`config`), the games
//! of a checkout (`games`), where a build and a run write (`layout`) and a game run (`run`).

pub mod config;
pub mod dirs;
pub mod games;
pub mod layout;
pub mod run;
pub mod sys;
