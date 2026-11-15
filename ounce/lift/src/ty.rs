//! IR types: integers of any width up to 128 ("i1" .. "i128", "i33" for carries), FP scalars, the guest V register
//! (v128, LLVM <2 x i64>) and lane-typed vectors reached from it by bitcast (nx2/lift2/ir.py).

use std::fmt;

#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub enum Lane {
    I(u8),
    F16,
    F32,
    F64,
}

#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub enum Ty {
    Void,
    I(u16),
    F16,
    F32,
    F64,
    /// a guest V register: LLVM <2 x i64>, a type of its own (ir.py's "v128", not "v2i64")
    V128,
    Vec(u8, Lane),
}

pub const I1: Ty = Ty::I(1);
pub const I8: Ty = Ty::I(8);
pub const I16: Ty = Ty::I(16);
pub const I32: Ty = Ty::I(32);
pub const I64: Ty = Ty::I(64);
pub const I128: Ty = Ty::I(128);
/// <2 x i64> as a lane type (not the V register's own v128)
pub const V2I64: Ty = Ty::Vec(2, Lane::I(64));

impl Lane {
    pub fn width(self) -> u32 {
        match self {
            Lane::I(w) => w as u32,
            Lane::F16 => 16,
            Lane::F32 => 32,
            Lane::F64 => 64,
        }
    }
    pub fn ty(self) -> Ty {
        match self {
            Lane::I(w) => Ty::I(w as u16),
            Lane::F16 => Ty::F16,
            Lane::F32 => Ty::F32,
            Lane::F64 => Ty::F64,
        }
    }
    pub fn of(t: Ty) -> Lane {
        match t {
            Ty::I(w) => Lane::I(w as u8),
            Ty::F16 => Lane::F16,
            Ty::F32 => Lane::F32,
            Ty::F64 => Lane::F64,
            _ => panic!("no lane type {t}"),
        }
    }
    fn ll(self) -> &'static str {
        match self {
            Lane::I(8) => "i8",
            Lane::I(16) => "i16",
            Lane::I(32) => "i32",
            Lane::I(64) => "i64",
            Lane::I(1) => "i1",
            Lane::F16 => "half",
            Lane::F32 => "float",
            Lane::F64 => "double",
            Lane::I(_) => unreachable!(),
        }
    }
}

impl Ty {
    /// the bit width (ir.WIDTH)
    pub fn width(self) -> u32 {
        match self {
            Ty::Void => 0,
            Ty::I(w) => w as u32,
            Ty::F16 => 16,
            Ty::F32 => 32,
            Ty::F64 => 64,
            Ty::V128 => 128,
            Ty::Vec(n, l) => n as u32 * l.width(),
        }
    }
    pub fn is_vec(self) -> bool {
        matches!(self, Ty::V128 | Ty::Vec(..))
    }
    pub fn is_fp(self) -> bool {
        matches!(self, Ty::F16 | Ty::F32 | Ty::F64)
    }
    /// (lane count, lane type) of a vector type; v128 is (2, i64)
    pub fn lanes(self) -> (u32, Lane) {
        match self {
            Ty::V128 => (2, Lane::I(64)),
            Ty::Vec(n, l) => (n as u32, l),
            _ => panic!("lanes of {self}"),
        }
    }
    pub fn vec(n: u32, lane: Ty) -> Ty {
        Ty::Vec(n as u8, Lane::of(lane))
    }
    /// all-ones mask of the width (u128::MAX for 128)
    pub fn mask(self) -> u128 {
        let w = self.width();
        if w >= 128 {
            u128::MAX
        } else {
            (1u128 << w) - 1
        }
    }
    /// ir.py's spelling ("i64", "f32", "v128", "v4f32"): it names LLVM intrinsics' overloads
    pub fn parse(s: &str) -> Ty {
        match s {
            "void" => Ty::Void,
            "f16" => Ty::F16,
            "f32" => Ty::F32,
            "f64" => Ty::F64,
            "v128" => Ty::V128,
            _ if s.starts_with('i') => Ty::I(s[1..].parse().expect(s)),
            _ if s.starts_with('v') => {
                let k = s.find(['i', 'f']).expect(s);
                let n: u8 = s[1..k].parse().expect(s);
                Ty::Vec(n, Lane::of(Ty::parse(&s[k..])))
            }
            _ => panic!("type {s}"),
        }
    }
    /// LLVM's spelling (emit_llvm.lltype)
    pub fn ll(self) -> LL {
        LL(self)
    }
}

/// ir.py's name of the type
impl fmt::Display for Ty {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match *self {
            Ty::Void => f.write_str("void"),
            Ty::I(w) => write!(f, "i{w}"),
            Ty::F16 => f.write_str("f16"),
            Ty::F32 => f.write_str("f32"),
            Ty::F64 => f.write_str("f64"),
            Ty::V128 => f.write_str("v128"),
            Ty::Vec(n, l) => write!(f, "v{n}{}", l.ty()),
        }
    }
}

/// LLVM's spelling of a type
pub struct LL(pub Ty);

impl fmt::Display for LL {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self.0 {
            Ty::Void => f.write_str("void"),
            Ty::I(w) => write!(f, "i{w}"),
            Ty::F16 => f.write_str("half"),
            Ty::F32 => f.write_str("float"),
            Ty::F64 => f.write_str("double"),
            Ty::V128 => f.write_str("<2 x i64>"),
            Ty::Vec(n, l) => write!(f, "<{n} x {}>", l.ll()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn names() {
        for s in ["i1", "i33", "i128", "f16", "v128", "v4f32", "v16i8", "v2f32", "v2i64"] {
            assert_eq!(Ty::parse(s).to_string(), s);
        }
        assert_eq!(Ty::parse("v4f32").ll().to_string(), "<4 x float>");
        assert_eq!(Ty::V128.ll().to_string(), "<2 x i64>");
        assert_eq!(Ty::parse("v8i16").width(), 128);
    }
}
