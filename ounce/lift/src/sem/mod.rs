//! The semantic templates by name (nx2/lift2/sem.py): each class's module registers its templates with their class
//! (int, fp, neon, crypto). A template lowers one decoded instruction at a pc into IR through the builder.

pub mod crypto;
pub mod fp;
pub mod int;
pub mod neon;

use crate::ir::Builder;
use ounce_decode::{all_encodings, Insn};
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

/// what the lift does with an instruction, by its encoding: the branches and exceptions build_ir lowers itself
/// (they end a block), or a semantic template
#[derive(Copy, Clone, Debug)]
pub enum Kind {
    /// B and BL (op)
    B,
    Bcond,
    /// CBZ, CBNZ
    Cbz,
    /// TBZ, TBNZ
    Tbz,
    Ret,
    Udf,
    Br,
    Blr,
    /// SVC, BRK, HLT, ... (opc)
    Exc,
    /// lowered by a template; code: an address-forming form (ADR/ADRP, ADD immediate) the code-pointer check reads
    Sem(Template, Code),
    /// a template the decoder names but no module registers (the lift stops; --skip counts it)
    Missing,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Code {
    No,
    Adr,
    AddSubImm,
}

impl Kind {
    /// ends a block
    pub fn is_term(self) -> bool {
        !matches!(self, Kind::Sem(..) | Kind::Missing)
    }
}

/// the kind of each encoding, by its id
pub fn kinds() -> &'static [Kind] {
    static K: OnceLock<Vec<Kind>> = OnceLock::new();
    K.get_or_init(|| {
        all_encodings()
            .iter()
            .map(|e| match e.template {
                "b" => Kind::B,
                "bcond" => Kind::Bcond,
                "cbz" => Kind::Cbz,
                "tbz" => Kind::Tbz,
                "ret" => Kind::Ret,
                "udf" => Kind::Udf,
                "br" => Kind::Br,
                "blr" => Kind::Blr,
                "exc" => Kind::Exc,
                t => match get(t) {
                    Some(en) => Kind::Sem(
                        en.f,
                        match t {
                            "adr" => Code::Adr,
                            "addsub_imm" => Code::AddSubImm,
                            _ => Code::No,
                        },
                    ),
                    None => Kind::Missing,
                },
            })
            .collect()
    })
}

/// the instruction's kind
#[inline]
pub fn kind(i: &Insn) -> Kind {
    kinds()[i.enc.id as usize]
}
