//! Integer instruction semantics (nx2/lift2/sem_int.py): one template per family of encodings. Values are created in
//! the Python's order (its argument evaluation order), so the value numbers match.

use crate::ir::{x, BinOp, Builder, Call, Callee, Cmp, Op, ValueId, FC, FLAGS, FN, FV, FZ, SP};
use crate::ty::{Ty, I1, I128, I16, I32, I64, I8};
use ounce_decode::{Insn, Param, Fld};

pub fn ty(sf: u32) -> Ty {
    if sf != 0 {
        I64
    } else {
        I32
    }
}

/// Xn or Wn (the low 32 bits); register 31 is SP when sp, else the zero register
pub fn getx(b: &mut Builder, n: u32, sf: u32, sp: bool) -> ValueId {
    let v = if n == 31 {
        if !sp {
            return b.c(0, ty(sf));
        }
        b.get(SP)
    } else {
        b.get(x(n))
    };
    if sf != 0 {
        v
    } else {
        b.trunc(v, I32)
    }
}

/// Xn = v (an i32 is zero-extended); register 31 is SP when sp, else the write is dropped
pub fn setx(b: &mut Builder, n: u32, v: ValueId, sp: bool) {
    if n == 31 && !sp {
        return;
    }
    let v = b.zext(v, I64);
    b.set(if n == 31 { SP } else { x(n) }, v);
}

pub fn set_nzcv(b: &mut Builder, n: ValueId, z: ValueId, c: ValueId, v: ValueId) {
    b.set(FN, n);
    b.set(FZ, z);
    b.set(FC, c);
    b.set(FV, v);
}

fn nz(b: &mut Builder, r: ValueId) -> (ValueId, ValueId) {
    let t = b.ty(r);
    let zero = b.c(0, t);
    let n = b.icmp("slt", r, zero);
    let z = b.icmp("eq", r, zero);
    (n, z)
}

pub fn add_flags(b: &mut Builder, x: ValueId, y: ValueId) -> ValueId {
    let r = b.bin(BinOp::Add, x, y);
    let (n, z) = nz(b, r);
    let c = b.op(Op::Ovf("uadd"), I1, &[x, y]);
    let v = b.op(Op::Ovf("sadd"), I1, &[x, y]);
    set_nzcv(b, n, z, c, v);
    b.cmp = Some(Cmp { nzcv: [n, z, c, v], a: x, b: y, kind: "add" });
    r
}

pub fn sub_flags(b: &mut Builder, x: ValueId, y: ValueId) -> ValueId {
    let r = b.bin(BinOp::Sub, x, y);
    let (n, z) = nz(b, r);
    let c = b.icmp("uge", x, y);
    let v = b.op(Op::Ovf("ssub"), I1, &[x, y]);
    set_nzcv(b, n, z, c, v);
    b.cmp = Some(Cmp { nzcv: [n, z, c, v], a: x, b: y, kind: "sub" });
    r
}

fn logic_flags(b: &mut Builder, r: ValueId) {
    let (n, z) = nz(b, r);
    let f = b.c(0, I1);
    set_nzcv(b, n, z, f, f);
}

/// x + y + cin with NZCV (ADCS/SBCS; for SBCS y is already inverted)
fn carry_flags(b: &mut Builder, x: ValueId, y: ValueId, cin: ValueId) -> ValueId {
    let t = b.ty(x);
    let w = t.width();
    let wide = Ty::I(w as u16 + 1);
    let s = b.bin(BinOp::Add, x, y);
    let ci = b.zext(cin, t);
    let r = b.bin(BinOp::Add, s, ci);
    let xw = b.zext(x, wide);
    let yw = b.zext(y, wide);
    let s = b.bin(BinOp::Add, xw, yw);
    let ci = b.zext(cin, wide);
    let uw = b.bin(BinOp::Add, s, ci);
    let k = b.c(w as i128, wide);
    let sh = b.bin(BinOp::Lshr, uw, k);
    let c = b.trunc(sh, I1);
    let xs = b.sext(x, wide);
    let ys = b.sext(y, wide);
    let s = b.bin(BinOp::Add, xs, ys);
    let ci = b.zext(cin, wide);
    let sw = b.bin(BinOp::Add, s, ci);
    let rs = b.sext(r, wide);
    let v = b.icmp("ne", rs, sw);
    let (n, z) = nz(b, r);
    set_nzcv(b, n, z, c, v);
    r
}

fn ffold(code: u32) -> Option<&'static str> {
    Some(match code {
        0 => "oeq", 1 => "une", 2 => "uge", 3 => "olt", 4 => "olt", 5 => "uge", 6 => "uno", 7 => "ord", 8 => "ugt",
        9 => "ole", 10 => "oge", 11 => "ult", 12 => "ogt", 13 => "ule",
        _ => return None,
    })
}
fn ifold(code: u32) -> Option<&'static str> {
    Some(match code {
        0 => "eq", 1 => "ne", 2 => "uge", 3 => "ult", 8 => "ugt", 9 => "ule", 10 => "sge", 11 => "slt", 12 => "sgt",
        13 => "sle",
        _ => return None,
    })
}

/// the condition as an i1; folded into one compare of the last SUBS/CMP's operands when the flags are still its
pub fn cond(b: &mut Builder, code: u32) -> ValueId {
    if code >= 14 {
        return b.c(1, I1);
    }
    let n = b.get(FN);
    let z = b.get(FZ);
    let c = b.get(FC);
    let v = b.get(FV);
    if let Some(k) = b.cmp {
        if k.nzcv == [n, z, c, v] {
            if k.kind == "sub" {
                if let Some(p) = ifold(code) {
                    return b.icmp(p, k.a, k.b);
                }
            }
            if k.kind == "fcmp" {
                return b.op(Op::Fcmp(ffold(code).unwrap()), I1, &[k.a, k.b]);
            }
        }
    }
    let r = match code >> 1 {
        0 => z,
        1 => c,
        2 => n,
        3 => v,
        4 => {
            let nz_ = b.not(z);
            b.bin(BinOp::And, c, nz_)
        }
        5 => b.icmp("eq", n, v),
        _ => {
            let nz_ = b.not(z);
            let e = b.icmp("eq", n, v);
            b.bin(BinOp::And, nz_, e)
        }
    };
    if code & 1 != 0 {
        b.not(r)
    } else {
        r
    }
}

/// LSL/LSR/ASR/ROR by a constant below the width
fn shift(b: &mut Builder, v: ValueId, kind: u32, amt: u32) -> ValueId {
    if amt == 0 {
        return v;
    }
    let t = b.ty(v);
    let a = b.c(amt as i128, t);
    match kind {
        0 => b.bin(BinOp::Shl, v, a),
        1 => b.bin(BinOp::Lshr, v, a),
        2 => b.bin(BinOp::Ashr, v, a),
        _ => b.intr("fshr", t, &[v, v, a]),
    }
}

const SIZES: [Ty; 4] = [I8, I16, I32, I64];

/// ExtendReg: the low 8/16/32/64 bits of a register, zero- or sign-extended to t, shifted left by amt
pub fn extend(b: &mut Builder, v64: ValueId, option: u32, amt: u32, t: Ty) -> ValueId {
    let src = SIZES[(option & 3) as usize];
    let mut x = b.trunc(v64, src);
    if src.width() > t.width() {
        x = b.trunc(x, t);
    } else {
        x = if option & 4 != 0 { b.sext(x, t) } else { b.zext(x, t) };
    }
    if amt != 0 {
        let a = b.c(amt as i128, t);
        b.bin(BinOp::Shl, x, a)
    } else {
        x
    }
}

pub fn sx(v: u32, bits: u32) -> i128 {
    if v >> (bits - 1) & 1 != 0 {
        v as i128 - (1i128 << bits)
    } else {
        v as i128
    }
}

/// DecodeBitMasks (enc.bitmask): the logical immediate (None for a reserved encoding)
pub fn bitmask(n: u32, imms: u32, immr: u32, width: u32) -> Option<u128> {
    let combined = (n << 6) | (!imms & 0x3F);
    if combined == 0 {
        return None;
    }
    let length = 31 - combined.leading_zeros();
    if length < 1 {
        return None;
    }
    let esize = 1u32 << length;
    if esize > width {
        return None;
    }
    let levels = esize - 1;
    let (s, r) = (imms & levels, immr & levels);
    if s == levels {
        return None;
    }
    let em = if esize >= 128 { u128::MAX } else { (1u128 << esize) - 1 };
    let welem: u128 = (1u128 << (s + 1)) - 1;
    let elem = ((welem >> r) | if r == 0 { welem << esize } else { welem << (esize - r) }) & em;
    let mut v = 0u128;
    for i in 0..width / esize {
        v |= elem << (i * esize);
    }
    Some(v)
}

/// DecodeBitMasks(immediate = FALSE): (wmask, tmask)
fn bit_masks(n: u32, imms: u32, immr: u32, width: u32) -> (u128, u128) {
    let length = 31 - ((n << 6) | (!imms & 0x3F)).leading_zeros();
    let levels = (1u32 << length) - 1;
    let (s, r) = (imms & levels, immr & levels);
    let d = (s.wrapping_sub(r)) & levels;
    let esize = 1u32 << length;
    let mut welem: u128 = (1u128 << (s + 1)) - 1;
    let telem: u128 = (1u128 << (d + 1)) - 1;
    if r != 0 {
        welem = ((welem >> r) | (welem << (esize - r))) & ((1u128 << esize) - 1);
    }
    let (mut w, mut t) = (0u128, 0u128);
    for i in 0..width / esize {
        w |= welem << (i * esize);
        t |= telem << (i * esize);
    }
    (w, t)
}

fn pbool(i: &Insn, name: &str) -> bool {
    matches!(i.p(name), Some(Param::Bool(true)))
}
fn pstr(i: &Insn, name: &str) -> &'static str {
    match i.p(name) {
        Some(Param::Str(s)) => s,
        p => panic!("{}: param {name} {p:?}", i.name()),
    }
}

// ---- data processing, immediate
fn t_adr(b: &mut Builder, i: &Insn, pc: u64) {
    let mut v = (i.f(Fld::immhi) << 2 | i.f(Fld::immlo)) as i128;
    if v >> 20 != 0 {
        v -= 1 << 21;
    }
    let a = if i.f(Fld::op) != 0 { (pc as i128 & !0xFFF) + (v << 12) } else { pc as i128 + v };
    let c = b.c64(a);
    setx(b, i.f(Fld::Rd), c, false);
}

fn t_addsub_imm(b: &mut Builder, i: &Insn, _pc: u64) {
    let sf = i.f(Fld::sf);
    let x = getx(b, i.f(Fld::Rn), sf, true);
    let y = b.c((i.f(Fld::imm12) as i128) << if i.f(Fld::sh) != 0 { 12 } else { 0 }, ty(sf));
    let s = i.f(Fld::S) != 0;
    let r = if s {
        if i.f(Fld::op) != 0 { sub_flags(b, x, y) } else { add_flags(b, x, y) }
    } else {
        b.bin(if i.f(Fld::op) != 0 { BinOp::Sub } else { BinOp::Add }, x, y)
    };
    setx(b, i.f(Fld::Rd), r, !s);
}

const LOGIC: [BinOp; 4] = [BinOp::And, BinOp::Or, BinOp::Xor, BinOp::And];

fn t_logic_imm(b: &mut Builder, i: &Insn, _pc: u64) {
    let sf = i.f(Fld::sf);
    let x = getx(b, i.f(Fld::Rn), sf, false);
    let m = bitmask(i.f(Fld::N), i.f(Fld::imms), i.f(Fld::immr), if sf != 0 { 64 } else { 32 }).expect("bitmask");
    let y = b.c(m as i128, ty(sf));
    let opc = i.f(Fld::opc);
    let r = b.bin(LOGIC[opc as usize], x, y);
    if opc == 3 {
        logic_flags(b, r);
    }
    setx(b, i.f(Fld::Rd), r, opc != 3);
}

fn t_movewide(b: &mut Builder, i: &Insn, _pc: u64) {
    let (sf, opc, hw) = (i.f(Fld::sf), i.f(Fld::opc), i.f(Fld::hw));
    let t = ty(sf);
    let v = (i.f(Fld::imm16) as i128) << (16 * hw);
    let r = if opc == 2 {
        b.c(v, t)
    } else if opc == 0 {
        b.c(!v, t)
    } else {
        let old = getx(b, i.f(Fld::Rd), sf, false);
        let m = b.c(!(0xFFFFi128 << (16 * hw)), t);
        let a = b.bin(BinOp::And, old, m);
        let c = b.c(v, t);
        b.bin(BinOp::Or, a, c)
    };
    setx(b, i.f(Fld::Rd), r, false);
}

fn t_bitfield(b: &mut Builder, i: &Insn, _pc: u64) {
    let (sf, opc, immr, imms) = (i.f(Fld::sf), i.f(Fld::opc), i.f(Fld::immr), i.f(Fld::imms));
    let t = ty(sf);
    let width = if sf != 0 { 64 } else { 32 };
    let (wmask, tmask) = bit_masks(i.f(Fld::N), imms, immr, width);
    let src = getx(b, i.f(Fld::Rn), sf, false);
    let dst = if opc == 1 { getx(b, i.f(Fld::Rd), sf, false) } else { b.c(0, t) };
    let rot = if immr != 0 {
        let k = b.c(immr as i128, t);
        b.intr("fshr", t, &[src, src, k])
    } else {
        src
    };
    let k = b.c(!(wmask as i128), t);
    let l = b.bin(BinOp::And, dst, k);
    let k = b.c(wmask as i128, t);
    let r_ = b.bin(BinOp::And, rot, k);
    let bot = b.bin(BinOp::Or, l, r_);
    let top = if opc == 0 {
        let k = b.c((width - 1 - imms) as i128, t);
        let s = b.bin(BinOp::Shl, src, k);
        let k = b.c((width - 1) as i128, t);
        b.bin(BinOp::Ashr, s, k)
    } else {
        dst
    };
    let k = b.c(!(tmask as i128), t);
    let l = b.bin(BinOp::And, top, k);
    let k = b.c(tmask as i128, t);
    let r_ = b.bin(BinOp::And, bot, k);
    let r = b.bin(BinOp::Or, l, r_);
    setx(b, i.f(Fld::Rd), r, false);
}

fn t_extr(b: &mut Builder, i: &Insn, _pc: u64) {
    let sf = i.f(Fld::sf);
    let hi = getx(b, i.f(Fld::Rn), sf, false);
    let lo = getx(b, i.f(Fld::Rm), sf, false);
    let imms = i.f(Fld::imms);
    let r = if imms != 0 {
        let k = b.c(imms as i128, ty(sf));
        b.intr("fshr", ty(sf), &[hi, lo, k])
    } else {
        lo
    };
    setx(b, i.f(Fld::Rd), r, false);
}

// ---- data processing, register
fn t_logic_shift(b: &mut Builder, i: &Insn, _pc: u64) {
    let (sf, opc) = (i.f(Fld::sf), i.f(Fld::opc));
    let x = getx(b, i.f(Fld::Rn), sf, false);
    let m = getx(b, i.f(Fld::Rm), sf, false);
    let mut y = shift(b, m, i.f(Fld::shift), i.f(Fld::imm6));
    if i.f(Fld::N) != 0 {
        y = b.not(y);
    }
    let r = b.bin(LOGIC[opc as usize], x, y);
    if opc == 3 {
        logic_flags(b, r);
    }
    setx(b, i.f(Fld::Rd), r, false);
}

fn addsub(b: &mut Builder, i: &Insn, x: ValueId, y: ValueId) -> ValueId {
    if i.f(Fld::S) != 0 {
        if i.f(Fld::op) != 0 {
            sub_flags(b, x, y)
        } else {
            add_flags(b, x, y)
        }
    } else {
        b.bin(if i.f(Fld::op) != 0 { BinOp::Sub } else { BinOp::Add }, x, y)
    }
}

fn t_addsub_shift(b: &mut Builder, i: &Insn, _pc: u64) {
    let sf = i.f(Fld::sf);
    let x = getx(b, i.f(Fld::Rn), sf, false);
    let m = getx(b, i.f(Fld::Rm), sf, false);
    let y = shift(b, m, i.f(Fld::shift), i.f(Fld::imm6));
    let r = addsub(b, i, x, y);
    setx(b, i.f(Fld::Rd), r, false);
}

fn t_addsub_ext(b: &mut Builder, i: &Insn, _pc: u64) {
    let sf = i.f(Fld::sf);
    let t = ty(sf);
    let x = getx(b, i.f(Fld::Rn), sf, true);
    let m = getx(b, i.f(Fld::Rm), 1, false);
    let y = extend(b, m, i.f(Fld::option), i.f(Fld::imm3), t);
    let r = addsub(b, i, x, y);
    setx(b, i.f(Fld::Rd), r, i.f(Fld::S) == 0);
}

fn t_addsub_carry(b: &mut Builder, i: &Insn, _pc: u64) {
    let sf = i.f(Fld::sf);
    let x = getx(b, i.f(Fld::Rn), sf, false);
    let mut y = getx(b, i.f(Fld::Rm), sf, false);
    if i.f(Fld::op) != 0 {
        y = b.not(y);
    }
    let cin = b.get(FC);
    let r = if i.f(Fld::S) != 0 {
        carry_flags(b, x, y, cin)
    } else {
        let s = b.bin(BinOp::Add, x, y);
        let t = b.ty(x);
        let c = b.zext(cin, t);
        b.bin(BinOp::Add, s, c)
    };
    setx(b, i.f(Fld::Rd), r, false);
}

fn t_ccmp(b: &mut Builder, i: &Insn, _pc: u64) {
    let sf = i.f(Fld::sf);
    let x = getx(b, i.f(Fld::Rn), sf, false);
    let y = if pbool(i, "imm") { b.c(i.f(Fld::imm5) as i128, ty(sf)) } else { getx(b, i.f(Fld::Rm), sf, false) };
    let ok = cond(b, i.f(Fld::cond));
    if i.f(Fld::op) != 0 {
        sub_flags(b, x, y);
    } else {
        add_flags(b, x, y);
    }
    let nzcv = i.f(Fld::nzcv);
    let mut fl = [ok; 4];
    for (k, (&var, bit)) in FLAGS.iter().zip([3, 2, 1, 0]).enumerate() {
        let g = b.get(var);
        let c = b.c((nzcv >> bit & 1) as i128, I1);
        fl[k] = b.select(ok, g, c);
    }
    set_nzcv(b, fl[0], fl[1], fl[2], fl[3]);
    b.cmp = None;
}

fn t_csel(b: &mut Builder, i: &Insn, _pc: u64) {
    let sf = i.f(Fld::sf);
    let x = getx(b, i.f(Fld::Rn), sf, false);
    let mut y = getx(b, i.f(Fld::Rm), sf, false);
    let t = b.ty(y);
    match i.f(Fld::op) * 2 + i.f(Fld::o2) {
        1 => {
            let one = b.c(1, t);
            y = b.bin(BinOp::Add, y, one);
        }
        2 => y = b.not(y),
        3 => {
            let z = b.c(0, t);
            y = b.bin(BinOp::Sub, z, y);
        }
        _ => {}
    }
    let c = cond(b, i.f(Fld::cond));
    let r = b.select(c, x, y);
    setx(b, i.f(Fld::Rd), r, false);
}

fn t_dp2(b: &mut Builder, i: &Insn, _pc: u64) {
    let (sf, o) = (i.f(Fld::sf), i.f(Fld::opcode));
    let t = ty(sf);
    let x = getx(b, i.f(Fld::Rn), sf, false);
    let y = getx(b, i.f(Fld::Rm), sf, false);
    let zero = b.c(0, t);
    let r = if o == 2 || o == 3 {
        // ARM: division by zero gives 0, INT_MIN / -1 gives INT_MIN; LLVM leaves both undefined
        let is0 = b.icmp("eq", y, zero);
        let q = if o == 2 {
            let one = b.c(1, t);
            let d = b.select(is0, one, y);
            b.bin(BinOp::Udiv, x, d)
        } else {
            let minv = b.c(1i128 << (t.width() - 1), t);
            let e1 = b.icmp("eq", x, minv);
            let m1 = b.c(-1, t);
            let e2 = b.icmp("eq", y, m1);
            let ovf = b.bin(BinOp::And, e1, e2);
            let o_ = b.bin(BinOp::Or, is0, ovf);
            let one = b.c(1, t);
            let d = b.select(o_, one, y);
            let q = b.bin(BinOp::Sdiv, x, d);
            b.select(ovf, minv, q)
        };
        b.select(is0, zero, q)
    } else {
        let k = b.c((t.width() - 1) as i128, t);
        let amt = b.bin(BinOp::And, y, k);
        if o == 11 {
            b.intr("fshr", t, &[x, x, amt])
        } else {
            b.bin(
                match o {
                    8 => BinOp::Shl,
                    9 => BinOp::Lshr,
                    _ => BinOp::Ashr,
                },
                x,
                amt,
            )
        }
    };
    setx(b, i.f(Fld::Rd), r, false);
}

fn t_dp1(b: &mut Builder, i: &Insn, _pc: u64) {
    let (sf, o) = (i.f(Fld::sf), i.f(Fld::opcode));
    let t = ty(sf);
    let x = getx(b, i.f(Fld::Rn), sf, false);
    let r = match o {
        0 => b.intr("bitreverse", t, &[x]),
        1 => {
            let m = b.c(0x00FF00FF00FF00FF, t);
            let k = b.c(8, t);
            let s = b.bin(BinOp::Lshr, x, k);
            let l = b.bin(BinOp::And, s, m);
            let a = b.bin(BinOp::And, x, m);
            let k = b.c(8, t);
            let h = b.bin(BinOp::Shl, a, k);
            b.bin(BinOp::Or, l, h)
        }
        2 if sf != 0 => {
            let s = b.intr("bswap", t, &[x]);
            let k = b.c(32, t);
            b.intr("fshr", t, &[s, s, k])
        }
        2 | 3 => b.intr("bswap", t, &[x]),
        4 => b.intr("ctlz", t, &[x]),
        _ => {
            let k = b.c((t.width() - 1) as i128, t);
            let s = b.bin(BinOp::Ashr, x, k);
            let y = b.bin(BinOp::Xor, x, s);
            let c = b.intr("ctlz", t, &[y]);
            let one = b.c(1, t);
            b.bin(BinOp::Sub, c, one)
        }
    };
    setx(b, i.f(Fld::Rd), r, false);
}

fn t_dp3(b: &mut Builder, i: &Insn, _pc: u64) {
    let (sf, op31, o0) = (i.f(Fld::sf), i.f(Fld::op31), i.f(Fld::o0));
    let addsub = if o0 != 0 { BinOp::Sub } else { BinOp::Add };
    let r = if op31 == 0 {
        let x = getx(b, i.f(Fld::Rn), sf, false);
        let y = getx(b, i.f(Fld::Rm), sf, false);
        let a = getx(b, i.f(Fld::Ra), sf, false);
        let m = b.bin(BinOp::Mul, x, y);
        b.bin(addsub, a, m)
    } else if op31 == 2 || op31 == 6 {
        let signed = op31 == 2;
        let xn = getx(b, i.f(Fld::Rn), 1, false);
        let x = if signed { b.sext(xn, I128) } else { b.zext(xn, I128) };
        let ym = getx(b, i.f(Fld::Rm), 1, false);
        let y = if signed { b.sext(ym, I128) } else { b.zext(ym, I128) };
        let m = b.bin(BinOp::Mul, x, y);
        let k = b.c(64, I128);
        let s = b.bin(BinOp::Lshr, m, k);
        b.trunc(s, I64)
    } else {
        let signed = op31 == 1;
        let xn = getx(b, i.f(Fld::Rn), 0, false);
        let x = if signed { b.sext(xn, I64) } else { b.zext(xn, I64) };
        let ym = getx(b, i.f(Fld::Rm), 0, false);
        let y = if signed { b.sext(ym, I64) } else { b.zext(ym, I64) };
        let m = b.bin(BinOp::Mul, x, y);
        let a = getx(b, i.f(Fld::Ra), 1, false);
        b.bin(addsub, a, m)
    };
    setx(b, i.f(Fld::Rd), r, false);
}

// ---- loads and stores
fn load(b: &mut Builder, addr: ValueId, t: Ty) -> ValueId {
    b.op(Op::Load, t, &[addr])
}
pub fn store(b: &mut Builder, addr: ValueId, v: ValueId) {
    b.op(Op::Store, Ty::Void, &[addr, v]);
}

fn ldst_value(b: &mut Builder, addr: ValueId, size: u32, opc: u32, rt: u32) {
    if opc == 0 {
        let v = getx(b, rt, 1, false);
        let v = b.trunc(v, SIZES[size as usize]);
        store(b, addr, v);
        return;
    }
    let v = load(b, addr, SIZES[size as usize]);
    let r = match opc {
        1 => b.zext(v, I64),
        2 => b.sext(v, I64),
        _ => {
            let s = b.sext(v, I32);
            b.zext(s, I64)
        }
    };
    setx(b, rt, r, false);
}

fn t_ldst(b: &mut Builder, i: &Insn, _pc: u64) {
    let (size, opc, mode) = (i.f(Fld::size), i.f(Fld::opc), pstr(i, "mode"));
    if size == 3 && opc == 2 {
        return; // PRFM, PRFUM
    }
    let base = getx(b, i.f(Fld::Rn), 1, true);
    let mut wb = None;
    let addr = match mode {
        "uimm" => {
            let k = b.c64((i.f(Fld::imm12) as i128) << size);
            b.bin(BinOp::Add, base, k)
        }
        "unscaled" => {
            let k = b.c64(sx(i.f(Fld::imm9), 9));
            b.bin(BinOp::Add, base, k)
        }
        "post" => {
            let k = b.c64(sx(i.f(Fld::imm9), 9));
            wb = Some(b.bin(BinOp::Add, base, k));
            base
        }
        "pre" => {
            let k = b.c64(sx(i.f(Fld::imm9), 9));
            let a = b.bin(BinOp::Add, base, k);
            wb = Some(a);
            a
        }
        _ => {
            let m = getx(b, i.f(Fld::Rm), 1, false);
            let e = extend(b, m, i.f(Fld::option), if i.f(Fld::S) != 0 { size } else { 0 }, I64);
            b.bin(BinOp::Add, base, e)
        }
    };
    ldst_value(b, addr, size, opc, i.f(Fld::Rt));
    if let Some(w) = wb {
        setx(b, i.f(Fld::Rn), w, true);
    }
}

fn t_ldst_pair(b: &mut Builder, i: &Insn, _pc: u64) {
    let (opc, mode, l) = (i.f(Fld::opc), i.f(Fld::mode), i.f(Fld::L));
    let size = if opc == 2 { 3 } else { 2 };
    let step = 1i128 << size;
    let base = getx(b, i.f(Fld::Rn), 1, true);
    let off = b.c64(sx(i.f(Fld::imm7), 7) << size);
    let mut wb = None;
    let addr = match mode {
        1 => {
            wb = Some(b.bin(BinOp::Add, base, off));
            base
        }
        3 => {
            let a = b.bin(BinOp::Add, base, off);
            wb = Some(a);
            a
        }
        _ => b.bin(BinOp::Add, base, off),
    };
    let k = b.c64(step);
    let addr2 = b.bin(BinOp::Add, addr, k);
    let st = SIZES[size as usize];
    if l != 0 {
        let lo = load(b, addr, st);
        let hi = load(b, addr2, st);
        let lo = if opc == 1 { b.sext(lo, I64) } else { b.zext(lo, I64) };
        setx(b, i.f(Fld::Rt), lo, false);
        let hi = if opc == 1 { b.sext(hi, I64) } else { b.zext(hi, I64) };
        setx(b, i.f(Fld::Rt2), hi, false);
    } else {
        let v1 = getx(b, i.f(Fld::Rt), 1, false);
        let v1 = b.trunc(v1, st);
        let v2 = getx(b, i.f(Fld::Rt2), 1, false);
        let v2 = b.trunc(v2, st);
        store(b, addr, v1);
        store(b, addr2, v2);
    }
    if let Some(w) = wb {
        setx(b, i.f(Fld::Rn), w, true);
    }
}

fn t_ldr_lit(b: &mut Builder, i: &Insn, pc: u64) {
    let opc = i.f(Fld::opc);
    if opc == 3 {
        return;
    }
    let addr = b.c64(pc as i128 + (sx(i.f(Fld::imm19), 19) << 2));
    let v = load(b, addr, if opc != 1 { I32 } else { I64 });
    let r = if opc == 2 { b.sext(v, I64) } else { b.zext(v, I64) };
    setx(b, i.f(Fld::Rt), r, false);
}

fn t_hint(b: &mut Builder, i: &Insn, _pc: u64) {
    if i.f(Fld::CRm) == 0 && (i.f(Fld::op2) == 1 || i.f(Fld::op2) == 2) {
        b.call(Call { effects: true, ..Call::new(Callee::llvm("x86.sse2.pause")) }, Ty::Void, &[]);
    }
}

// ---- CRC32, pointer authentication, MTE, system registers and operations
fn t_crc32(b: &mut Builder, i: &Insn, _pc: u64) {
    let sz = i.f(Fld::sz);
    let m = getx(b, i.f(Fld::Rm), if sz == 3 { 1 } else { 0 }, false);
    let data = b.zext(m, I64);
    let acc = getx(b, i.f(Fld::Rn), 0, false);
    let poly = b.c(if i.f(Fld::C) != 0 { 0x82F63B78 } else { 0xEDB88320 }, I32);
    let n = b.c(8 << sz, I32);
    let r = b.call(Call::new(Callee::Helper(crate::emit::H_CRC32)), I32, &[acc, data, n, poly]);
    setx(b, i.f(Fld::Rd), r, false);
}

fn t_pac(_: &mut Builder, _: &Insn, _: u64) {}

fn t_pacga(b: &mut Builder, i: &Insn, _pc: u64) {
    let z = b.c64(0);
    setx(b, i.f(Fld::Rd), z, false);
}

fn t_stg(b: &mut Builder, i: &Insn, _pc: u64) {
    let op2 = i.f(Fld::op2);
    if op2 == 1 || op2 == 3 {
        let n = getx(b, i.f(Fld::Rn), 1, true);
        let k = b.c64(sx(i.f(Fld::imm9), 9) << 4);
        let a = b.bin(BinOp::Add, n, k);
        setx(b, i.f(Fld::Rn), a, true);
    }
}

const CTX_TPIDR: u32 = 0x310;
const CTX_TPIDRRO: u32 = 0x318;

/// enc.SYSREGS: (o0, op1, CRn, CRm, op2) -> name
fn sysreg_name(k: (u32, u32, u32, u32, u32)) -> &'static str {
    match k {
        (1, 3, 4, 2, 0) => "NZCV",
        (1, 3, 4, 4, 0) => "FPCR",
        (1, 3, 4, 4, 1) => "FPSR",
        (1, 3, 13, 0, 2) => "TPIDR_EL0",
        (1, 3, 13, 0, 3) => "TPIDRRO_EL0",
        (1, 3, 14, 0, 1) => "CNTPCT_EL0",
        (1, 3, 14, 0, 2) => "CNTVCT_EL0",
        (1, 3, 14, 0, 0) => "CNTFRQ_EL0",
        (1, 3, 0, 0, 1) => "CTR_EL0",
        (1, 3, 0, 0, 7) => "DCZID_EL0",
        _ => panic!("sysreg {k:?}"),
    }
}

fn ctx_call(func: &'static str, effects: bool) -> Call {
    Call::ctx(Callee::Rt(func), effects)
}

/// sem_fp.fp_sysreg_read
pub fn fp_sysreg_read(b: &mut Builder, name: &str) -> ValueId {
    let f = if name == "FPSR" { "a64_fpsr_read" } else { "a64_fpcr_read" };
    b.call(ctx_call(f, true), I64, &[])
}

/// sem_fp.fp_sysreg_write: a function writing FPCR is strictfp
pub fn fp_sysreg_write(b: &mut Builder, name: &str, x: ValueId) {
    if name == "FPCR" {
        b.f.strict_fp = true;
    }
    let f = if name == "FPSR" { "a64_fpsr_write" } else { "a64_fpcr_write" };
    b.call(ctx_call(f, true), Ty::Void, &[x]);
}

fn t_sysreg(b: &mut Builder, i: &Insn, _pc: u64) {
    let name = sysreg_name((i.f(Fld::o0), i.f(Fld::op1), i.f(Fld::CRn), i.f(Fld::CRm), i.f(Fld::op2)));
    let rt = i.f(Fld::Rt);
    if i.f(Fld::L) != 0 {
        let v = match name {
            "NZCV" => {
                let mut v = b.c64(0);
                for (k, &flag) in FLAGS.iter().enumerate() {
                    let g = b.get(flag);
                    let z = b.zext(g, I64);
                    let s = b.c64(31 - k as i128);
                    let sh = b.bin(BinOp::Shl, z, s);
                    v = b.bin(BinOp::Or, v, sh);
                }
                v
            }
            "FPCR" | "FPSR" => fp_sysreg_read(b, name),
            "TPIDR_EL0" => b.op(Op::CtxLoad(CTX_TPIDR), I64, &[]),
            "TPIDRRO_EL0" => b.op(Op::CtxLoad(CTX_TPIDRRO), I64, &[]),
            "CNTPCT_EL0" | "CNTVCT_EL0" => b.call(ctx_call("recomp_cntpct", false), I64, &[]),
            "CNTFRQ_EL0" => b.c64(31250000),
            "CTR_EL0" => b.c64(0x8444C004),
            _ => b.c64(4),
        };
        setx(b, rt, v, false);
        return;
    }
    let x = getx(b, rt, 1, false);
    match name {
        "NZCV" => {
            for (k, &flag) in FLAGS.iter().enumerate() {
                let s = b.c64(31 - k as i128);
                let sh = b.bin(BinOp::Lshr, x, s);
                let t = b.trunc(sh, I1);
                b.set(flag, t);
            }
        }
        "FPCR" | "FPSR" => fp_sysreg_write(b, name, x),
        "TPIDR_EL0" => {
            b.op(Op::CtxStore(CTX_TPIDR), Ty::Void, &[x]);
        }
        _ => {}
    }
}

fn t_sys(b: &mut Builder, i: &Insn, _pc: u64) {
    let k = (i.f(Fld::CRm), i.f(Fld::op2));
    if k == (4, 1) || k == (4, 4) {
        let t = getx(b, i.f(Fld::Rt), 1, false);
        let m = b.c64(!63);
        let base = b.bin(BinOp::And, t, m);
        for k in 0..8 {
            let o = b.c64(8 * k);
            let a = b.bin(BinOp::Add, base, o);
            let z = b.c64(0);
            store(b, a, z);
        }
    }
}

fn t_barrier(_: &mut Builder, _: &Insn, _: u64) {}

// ---- atomics and ordering
const RMW: [&str; 8] = ["add", "clr", "xor", "or", "max", "min", "umax", "umin"];

fn t_lse(b: &mut Builder, i: &Insn, _pc: u64) {
    let (size, rt, rn) = (i.f(Fld::size), i.f(Fld::Rt), i.f(Fld::Rn));
    let t = SIZES[size as usize];
    let addr = getx(b, rn, 1, true);
    if i.f(Fld::o3) != 0 && i.f(Fld::opc) == 4 {
        let l = b.op(Op::ALoad("acquire"), t, &[addr]);
        let z = b.zext(l, I64);
        setx(b, rt, z, false);
        return;
    }
    let s = getx(b, i.f(Fld::Rs), 1, false);
    let mut v = b.trunc(s, t);
    let op = if i.f(Fld::o3) != 0 {
        "xchg"
    } else {
        let op = RMW[i.f(Fld::opc) as usize];
        if op == "clr" {
            v = b.not(v);
            "and"
        } else {
            op
        }
    };
    let old = b.op(Op::Rmw(op), t, &[addr, v]);
    let z = b.zext(old, I64);
    setx(b, rt, z, false);
}

fn t_cas(b: &mut Builder, i: &Insn, _pc: u64) {
    let (size, rs, rn) = (i.f(Fld::size), i.f(Fld::Rs), i.f(Fld::Rn));
    let t = SIZES[size as usize];
    let addr = getx(b, rn, 1, true);
    let s = getx(b, rs, 1, false);
    let e = b.trunc(s, t);
    let n = getx(b, i.f(Fld::Rt), 1, false);
    let n = b.trunc(n, t);
    let old = b.op(Op::Cas, t, &[addr, e, n]);
    let z = b.zext(old, I64);
    setx(b, rs, z, false);
}

fn t_casp(b: &mut Builder, i: &Insn, _pc: u64) {
    let (sz, rs, rt, rn) = (i.f(Fld::sz), i.f(Fld::Rs), i.f(Fld::Rt), i.f(Fld::Rn));
    let w = if sz != 0 { 64 } else { 32 };
    let (t, half) = if sz != 0 { (I128, I64) } else { (I64, I32) };
    let addr = getx(b, rn, 1, true);
    let pair = |b: &mut Builder, r: u32| {
        let g = getx(b, r, 1, false);
        let l = b.trunc(g, half);
        let lo = b.zext(l, t);
        let g = getx(b, r + 1, 1, false);
        let h = b.trunc(g, half);
        let hi = b.zext(h, t);
        let k = b.c(w, t);
        let s = b.bin(BinOp::Shl, hi, k);
        b.bin(BinOp::Or, lo, s)
    };
    let e = pair(b, rs);
    let n = pair(b, rt);
    let old = b.op(Op::Cas, t, &[addr, e, n]);
    let l = b.trunc(old, half);
    let l = b.zext(l, I64);
    setx(b, rs, l, false);
    let k = b.c(w, t);
    let s = b.bin(BinOp::Lshr, old, k);
    let h = b.trunc(s, half);
    let h = b.zext(h, I64);
    setx(b, rs + 1, h, false);
}

fn t_ldstord(b: &mut Builder, i: &Insn, _pc: u64) {
    let (size, rt, rn) = (i.f(Fld::size), i.f(Fld::Rt), i.f(Fld::Rn));
    let t = SIZES[size as usize];
    let addr = getx(b, rn, 1, true);
    if i.f(Fld::L) != 0 {
        let l = b.op(Op::ALoad("acquire"), t, &[addr]);
        let z = b.zext(l, I64);
        setx(b, rt, z, false);
    } else {
        let v = getx(b, rt, 1, false);
        let v = b.trunc(v, t);
        b.op(Op::AStore("seq_cst"), Ty::Void, &[addr, v]);
    }
}

fn t_ldstex(b: &mut Builder, i: &Insn, _pc: u64) {
    let (size, rt, rn) = (i.f(Fld::size), i.f(Fld::Rt), i.f(Fld::Rn));
    let addr = getx(b, rn, 1, true);
    let n = b.c(1 << size, I32);
    if i.f(Fld::L) != 0 {
        let v = b.call(ctx_call("recomp_ldxr", false), I64, &[addr, n]);
        setx(b, rt, v, false);
    } else {
        let v = getx(b, rt, 1, false);
        let st = b.call(ctx_call("recomp_stxr", false), I32, &[addr, n, v]);
        setx(b, i.f(Fld::Rs), st, false);
    }
}

fn t_ldstexp(b: &mut Builder, i: &Insn, _pc: u64) {
    let (sz, rt, rn) = (i.f(Fld::sz), i.f(Fld::Rt), i.f(Fld::Rn));
    let addr = getx(b, rn, 1, true);
    let n = b.c(if sz != 0 { 8 } else { 4 }, I32);
    if i.f(Fld::L) != 0 {
        let c = Call::ctx(Callee::Helper(crate::emit::H_LDXP_LO), false);
        let lo = b.call(c, I64, &[addr, n]);
        let hi = b.op(Op::CtxLoad(0x340), I64, &[]);
        setx(b, rt, lo, false);
        setx(b, i.f(Fld::Rt2), hi, false);
    } else {
        let v1 = getx(b, rt, 1, false);
        let v2 = getx(b, i.f(Fld::Rt2), 1, false);
        let st = b.call(ctx_call("recomp_stxp", false), I32, &[addr, n, v1, v2]);
        setx(b, i.f(Fld::Rs), st, false);
    }
}

fn t_fence(b: &mut Builder, i: &Insn, _pc: u64) {
    if i.f(Fld::op2) == 2 {
        let z = b.c(0, I32);
        b.op(Op::CtxStore32(0x34C), Ty::Void, &[z]);
        return;
    }
    let full = i.f(Fld::CRm) & 3 == 3;
    b.op(Op::Fence(if full { "seq_cst" } else { "acq_rel" }), Ty::Void, &[]);
}

pub const TEMPLATES: &[(&str, crate::sem::Template)] = &[
    ("adr", t_adr),
    ("addsub_imm", t_addsub_imm),
    ("logic_imm", t_logic_imm),
    ("movewide", t_movewide),
    ("bitfield", t_bitfield),
    ("extr", t_extr),
    ("logic_shift", t_logic_shift),
    ("addsub_shift", t_addsub_shift),
    ("addsub_ext", t_addsub_ext),
    ("addsub_carry", t_addsub_carry),
    ("ccmp", t_ccmp),
    ("csel", t_csel),
    ("dp2", t_dp2),
    ("dp1", t_dp1),
    ("dp3", t_dp3),
    ("ldst", t_ldst),
    ("ldst_pair", t_ldst_pair),
    ("ldr_lit", t_ldr_lit),
    ("hint", t_hint),
    ("crc32", t_crc32),
    ("pac", t_pac),
    ("pacga", t_pacga),
    ("stg", t_stg),
    ("sysreg", t_sysreg),
    ("sys", t_sys),
    ("barrier", t_barrier),
    ("lse", t_lse),
    ("cas", t_cas),
    ("casp", t_casp),
    ("ldstord", t_ldstord),
    ("ldstex", t_ldstex),
    ("ldstexp", t_ldstexp),
    ("fence", t_fence),
];

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn masks() {
        assert_eq!(bitmask(1, 0, 0, 64), Some(1));
        assert_eq!(bitmask(0, 0b111100, 0, 32), Some(0x55555555));
        assert_eq!(bitmask(0, 0b111111, 0, 32), None);
    }
}
