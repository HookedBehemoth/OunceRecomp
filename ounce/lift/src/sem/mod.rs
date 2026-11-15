//! The semantic templates by name (nx2/lift2/sem.py): each class's module registers its templates with their class
//! (int, fp, neon, crypto). A template lowers one decoded instruction at a pc into IR through the builder.

pub mod crypto;
pub mod fp;
pub mod int;
pub mod neon;

use crate::ir::Builder;
use ounce_decode::Insn;
use std::collections::HashMap;
use std::sync::OnceLock;

pub type Template = fn(&mut Builder, &Insn, u64);

pub struct Entry {
    pub f: Template,
    pub class: &'static str,
}

/// template name -> its lowering and class; a template the decoder names but no module registers is not lifted yet
pub fn templates() -> &'static HashMap<&'static str, Entry> {
    static T: OnceLock<HashMap<&'static str, Entry>> = OnceLock::new();
    T.get_or_init(|| {
        let mut t = HashMap::new();
        for &(name, f) in int::TEMPLATES {
            t.insert(name, Entry { f, class: "int" });
        }
        for (class, ts) in [("fp", fp::TEMPLATES), ("neon", neon::TEMPLATES), ("crypto", crypto::TEMPLATES)] {
            for &(name, f) in ts {
                t.insert(name, Entry { f, class });
            }
        }
        t
    })
}

pub fn get(name: &str) -> Option<&'static Entry> {
    templates().get(name)
}
