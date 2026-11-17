//! The AArch64 decoder of the ounce lifter: a port of generate/encoding/enc.py and its tables (enc_fp, enc_neon, enc_crypto).
//!
//! `decode` gives what `enc.decode` gives: the same entry (name, template, params) and the same fields, or None where
//! Python gives None. The tables (src/tables.rs) are generated from the Python ones by generate/gen_tables.py, the
//! validity of each entry as a truth table over the fields its expression reads. src/info.rs holds the hand-ported
//! helpers the semantic templates call (enc_neon's *_info, bitmask, SYSREGS, ...).

pub mod info;
mod tables;

pub use tables::Fld;

impl Fld {
    /// the field's name in the patterns ("Rn")
    pub fn name(self) -> &'static str {
        tables::FLD_NAMES[self as usize]
    }
}

/// A fixed parameter of an entry (the Python params dict's values).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Param {
    Bool(bool),
    Int(i64),
    Str(&'static str),
}

/// A field of an entry's pattern: `width` bits from bit `shift`.
#[derive(Debug, Clone, Copy)]
pub struct Field {
    pub name: &'static str,
    pub shift: u32,
    pub width: u32,
}

impl Field {
    #[inline]
    pub fn get(&self, word: u32) -> u32 {
        (word >> self.shift) & ((1u32 << self.width) - 1)
    }
}

/// One entry of the table: enc.py's Entry.
#[derive(Debug)]
pub struct Encoding {
    /// index in `all_encodings()` (and in Python's enc.ENTRIES)
    pub id: u16,
    pub name: &'static str,
    pub template: &'static str,
    pub params: &'static [(&'static str, Param)],
    pub mask: u32,
    pub value: u32,
    /// in pattern order (bit 31 first), as Python's Entry.fields
    pub fields: &'static [Field],
    /// (shift, width) of the fields the validity reads, the last one the low bits of the table index
    valid_fields: &'static [(u32, u32)],
    /// bit per combination of `valid_fields`; None: always valid
    valid_table: Option<&'static [u64]>,
}

impl Encoding {
    /// the field by name, or None
    #[inline]
    pub fn field(&self, name: &str) -> Option<&'static Field> {
        self.fields.iter().find(|f| f.name == name)
    }

    /// the param by name, or None
    pub fn param(&self, name: &str) -> Option<&'static Param> {
        self.params.iter().find(|(k, _)| *k == name).map(|(_, v)| v)
    }

    /// whether a word matching mask/value is allocated (Python's entry.valid(**fields))
    #[inline]
    pub fn valid(&self, word: u32) -> bool {
        match self.valid_table {
            None => true,
            Some(t) => {
                let mut idx = 0u32;
                for &(shift, width) in self.valid_fields {
                    idx = (idx << width) | ((word >> shift) & ((1u32 << width) - 1));
                }
                (t[(idx >> 6) as usize] >> (idx & 63)) & 1 != 0
            }
        }
    }
}

/// A decoded instruction: the entry and the word (fields are read from the word on demand).
#[derive(Debug, Clone, Copy)]
pub struct Insn {
    pub enc: &'static Encoding,
    pub word: u32,
}

impl Insn {
    /// a field's value (`i.f(Fld::Rd)`); panics if the entry has no such field
    #[inline]
    pub fn f(&self, field: Fld) -> u32 {
        let at = tables::FIELD_AT[self.enc.id as usize][field as usize] as u32;
        if at == 0 {
            panic!("{}: no field {}", self.enc.name, field.name());
        }
        (self.word >> (at & 0xFF)) & ((1u32 << (at >> 8)) - 1)
    }

    /// whether the entry has the field
    #[inline]
    pub fn has(&self, field: Fld) -> bool {
        tables::FIELD_AT[self.enc.id as usize][field as usize] != 0
    }

    #[inline]
    pub fn p(&self, param: &str) -> Option<&'static Param> {
        self.enc.param(param)
    }

    /// the entry's name ("ADDSUB_IMM")
    #[inline]
    pub fn name(&self) -> &'static str {
        self.enc.name
    }

    /// (name, value) of every field, in pattern order (Python's Insn.f)
    pub fn fields(&self) -> impl Iterator<Item = (&'static str, u32)> + '_ {
        self.enc.fields.iter().map(move |f| (f.name, f.get(self.word)))
    }
}

/// The Insn for a word, or None (not in the decoded subset, or unallocated): enc.decode.
#[inline]
pub fn decode(word: u32) -> Option<Insn> {
    decode_id(word).map(|i| Insn { enc: &tables::ENCODINGS[i as usize], word })
}

/// The index of the word's entry in `all_encodings()`, or None.
#[inline]
pub fn decode_id(word: u32) -> Option<u16> {
    let b = (word >> tables::DISPATCH_SHIFT) as usize;
    let (lo, hi) = (tables::BUCKET_START[b] as usize, tables::BUCKET_START[b + 1] as usize);
    for &i in &tables::BUCKET_ENTRY[lo..hi] {
        let e = &tables::ENCODINGS[i as usize];
        if word & e.mask == e.value && e.valid(word) {
            return Some(i as u16);
        }
    }
    None
}

/// Every entry, in Python's enc.ENTRIES order.
pub fn all_encodings() -> &'static [Encoding] {
    &tables::ENCODINGS
}

/// The entry by name ("LDST_UIMM").
pub fn encoding(name: &str) -> Option<&'static Encoding> {
    tables::ENCODINGS.iter().find(|e| e.name == name)
}

/// Every entry a word matches and is valid in (enc.matches: the overlap check).
pub fn matches(word: u32) -> Vec<&'static str> {
    tables::ENCODINGS.iter().filter(|e| word & e.mask == e.value && e.valid(word)).map(|e| e.name).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_words() {
        // add x0, x1, #0x10
        let i = decode(0x91004020).unwrap();
        assert_eq!(i.name(), "ADDSUB_IMM");
        assert_eq!(i.enc.template, "addsub_imm");
        assert_eq!((i.f(Fld::sf), i.f(Fld::op), i.f(Fld::S), i.f(Fld::sh)), (1, 0, 0, 0));
        assert_eq!((i.f(Fld::imm12), i.f(Fld::Rn), i.f(Fld::Rd)), (0x10, 1, 0));
        // ldr x0, [x1, #8]
        let i = decode(0xf9400420).unwrap();
        assert_eq!(i.name(), "LDST_UIMM");
        assert_eq!(i.p("mode"), Some(&Param::Str("uimm")));
        assert_eq!(i.f(Fld::imm12), 1);
        // ret
        assert_eq!(decode(0xd65f03c0).unwrap().name(), "RET");
        // retaa: params {"pac": True}
        let i = decode(0xd65f0bff).unwrap();
        assert_eq!(i.name(), "RETA");
        assert_eq!(i.p("pac"), Some(&Param::Bool(true)));
        assert!(!i.has(Fld::Rn));
        // nop
        assert_eq!(decode(0xd503201f).unwrap().name(), "HINT");
        // movz with opc 1: unallocated
        assert!(decode(0x92800000 & !(3 << 29) | (1 << 29)).is_none());
        // and w0, w0, #0 is reserved (imms all ones at the element size)
        assert!(decode(0x12000000 | (0x3f << 10)).is_none());
        // fadd s0, s1, s2
        assert_eq!(decode(0x1e222820).unwrap().name(), "FP_DP2");
        // add v0.4s, v1.4s, v2.4s
        assert_eq!(decode(0x4ea28420).unwrap().name(), "SIMD_3SAME");
        // aese v0.16b, v1.16b
        assert_eq!(decode(0x4e284820).unwrap().name(), "CRYPTO_AES");
        assert!(decode(0xffffffff).is_none());
    }

    #[test]
    fn ids_and_names() {
        for (k, e) in all_encodings().iter().enumerate() {
            assert_eq!(e.id as usize, k);
            assert_eq!(encoding(e.name).unwrap().id as usize, k);
            assert_eq!(e.mask & e.value, e.value);
            let lit = e.fields.iter().fold(0u32, |m, f| m | (((1u64 << f.width) - 1) << f.shift) as u32);
            assert_eq!(lit & e.mask, 0);
            assert_eq!(lit | e.mask, u32::MAX);
        }
    }

    /// The typed field table is the fields' by name: every name of every entry, and nothing else.
    #[test]
    fn field_table() {
        for e in all_encodings() {
            for (k, name) in tables::FLD_NAMES.iter().enumerate() {
                let at = tables::FIELD_AT[e.id as usize][k] as u32;
                match e.field(name) {
                    Some(f) => assert_eq!((at & 0xFF, at >> 8), (f.shift, f.width), "{} {name}", e.name),
                    None => assert_eq!(at, 0, "{} {name}", e.name),
                }
            }
        }
        assert_eq!(Fld::Rn.name(), "Rn");
        assert_eq!(tables::FLD_NAMES.len(), tables::NFLD);
    }

    /// The dispatch finds what a scan of every entry finds, and no word matches two entries (enc.py's rule).
    #[test]
    fn dispatch_equals_scan() {
        let mut x: u64 = 0x9e3779b97f4a7c15;
        for _ in 0..2_000_000 {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            let w = x as u32;
            let m = matches(w);
            assert!(m.len() <= 1, "{w:#010x}: {m:?}");
            assert_eq!(decode(w).map(|i| i.name()), m.first().copied(), "{w:#010x}");
        }
    }
}
