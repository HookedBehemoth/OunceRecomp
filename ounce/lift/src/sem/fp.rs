//! FP scalar semantics and vector/FP register loads and stores (nx2/lift2/sem_fp.py): templates for enc_fp's table,
//! and the FP operations sem/neon.rs shares. Values are created in the Python's order (value numbers match).

use crate::ir::{vreg, BinOp, Builder, Call, CastOp, Cmp, Op, ValueId};
use crate::sem::int::{cond, getx, set_nzcv, setx, store, sx};
use crate::ty::{Ty, I1, I16, I32, I64, I8};
use ounce_decode::{info, Insn, Param};
use std::sync::OnceLock;

pub fn fty(ftype: u32) -> Ty {
    match ftype {
        0 => Ty::F32,
        1 => Ty::F64,
        3 => Ty::F16,
        _ => panic!("ftype {ftype}"),
    }
}

/// the integer type of an FP scalar (ITY)
pub fn ity_of(t: Ty) -> Ty {
    Ty::I(t.width() as u16)
}

fn default_nan() -> bool {
    static D: OnceLock<bool> = OnceLock::new();
    *D.get_or_init(|| std::env::var("LIFT2_DEFAULT_NAN").map_or(false, |v| v == "1"))
}

// ---- V register access
/// V register n as ty: a 128-bit type the whole register, a 64-bit vector type the low half, a scalar lane 0
pub fn vread(b: &mut Builder, n: u32, ty: Ty) -> ValueId {
    let v = b.get(vreg(n));
    let w = ty.width();
    if w == 128 {
        return b.bitcast(v, ty);
    }
    if ty.is_vec() {
        let c = b.bitcast(v, Ty::vec(2, I64));
        let e = b.extract(c, 0);
        return b.bitcast(e, ty);
    }
    let c = b.bitcast(v, Ty::vec(128 / w, ty));
    b.extract(c, 0)
}

/// V register n = val zero-extended to 128 bits
pub fn vwrite(b: &mut Builder, n: u32, val: ValueId) {
    let t = b.ty(val);
    let w = t.width();
    if w == 128 {
        let c = b.bitcast(val, Ty::V128);
        b.set(vreg(n), c);
        return;
    }
    let x = if !matches!(t, Ty::I(_)) { b.bitcast(val, Ty::I(w as u16)) } else { val };
    let x = b.zext(x, I64);
    let z = b.zero(Ty::V128);
    let r = b.insert(z, x, 0);
    b.set(vreg(n), r);
}

/// the bits of an FP constant (f32 or f64; f16 from an exactly representable value)
pub fn fbits(value: f64, ty: Ty) -> u64 {
    match ty {
        Ty::F32 => (value as f32).to_bits() as u64,
        Ty::F64 => value.to_bits(),
        Ty::F16 => f16_bits(value),
        _ => panic!("fconst {ty}"),
    }
}

fn f16_bits(v: f64) -> u64 {
    // (only exactly representable normal values, zeros and infinities: what the templates use)
    let s = if v.is_sign_negative() { 0x8000 } else { 0 };
    let a = v.abs();
    if a == 0.0 {
        return s;
    }
    if a.is_infinite() {
        return s | 0x7c00;
    }
    let e = a.log2().floor() as i32;
    let m = a / 2f64.powi(e) - 1.0;
    assert!((-14..=15).contains(&e), "f16 constant {v}");
    s | (((e + 15) as u64) << 10) | ((m * 1024.0) as u64)
}

/// an FP constant from a float
pub fn fconst(b: &mut Builder, value: f64, ty: Ty) -> ValueId {
    let bits = fbits(value, ty);
    b.c(bits as i128, ty)
}

/// an LLVM intrinsic's type suffix: f32, v4f32 (ir.py's name)
pub fn suffix(t: Ty) -> String {
    t.to_string()
}

pub fn llcall(b: &mut Builder, name: String, ty: Ty, args: &[ValueId]) -> ValueId {
    b.call(Call { func: name, ..Default::default() }, ty, args)
}

/// llvm.<name>.<suffix(ty)>
pub fn intr(b: &mut Builder, name: &str, ty: Ty, args: &[ValueId]) -> ValueId {
    llcall(b, format!("llvm.{name}.{}", suffix(ty)), ty, args)
}

/// the i1 shape of a type (i1, or a vector of i1)
pub fn i1_shape(t: Ty) -> Ty {
    if t.is_vec() {
        Ty::vec(t.lanes().0, I1)
    } else {
        I1
    }
}

pub fn fcmp(b: &mut Builder, pred: &'static str, ty: Ty, x: ValueId, y: ValueId) -> ValueId {
    b.op(Op::Fcmp(pred), ty, &[x, y])
}

pub fn fop(b: &mut Builder, op: BinOp, x: ValueId, y: ValueId) -> ValueId {
    let t = b.ty(x);
    b.op(Op::Bin(op), t, &[x, y])
}

pub fn fneg(b: &mut Builder, x: ValueId) -> ValueId {
    let t = b.ty(x);
    b.op(Op::FNeg, t, &[x])
}

const QNAN: [(Ty, u64); 3] = [(Ty::F16, 0x7E00), (Ty::F32, 0x7FC00000), (Ty::F64, 0x7FF8000000000000)];

/// LIFT2_DEFAULT_NAN: a NaN result from non-NaN operands becomes ARM's default NaN
pub fn nan_fix(b: &mut Builder, r: ValueId, ins: &[ValueId]) -> ValueId {
    if !default_nan() {
        return r;
    }
    let rt = b.ty(r);
    let mut gen = fcmp(b, "uno", i1_shape(rt), r, r);
    for &x in ins {
        let gt = b.ty(gen);
        let o = fcmp(b, "ord", gt, x, x);
        gen = b.bin(BinOp::And, gen, o);
    }
    let lane = if rt.is_vec() { rt.lanes().1.ty() } else { rt };
    let qn = QNAN.iter().find(|q| q.0 == lane).unwrap().1;
    let q = if rt.is_vec() {
        b.vconst_splat(qn, Ty::vec(rt.lanes().0, ity_of(lane)))
    } else {
        b.c(qn as i128, ity_of(lane))
    };
    let qc = b.bitcast(q, rt);
    b.select(gen, qc, r)
}

// ---- arithmetic shared with the vector forms
/// FMUL FDIV FADD FSUB FMAX FMIN FMAXNM FMINNM FNMUL (enc_fp.DP2's order)
pub fn fbin(b: &mut Builder, k: u32, x: ValueId, y: ValueId) -> ValueId {
    let ty = b.ty(x);
    if k <= 3 {
        let op = [BinOp::Fmul, BinOp::Fdiv, BinOp::Fadd, BinOp::Fsub][k as usize];
        let r = b.op(Op::Bin(op), ty, &[x, y]);
        return nan_fix(b, r, &[x, y]);
    }
    if k == 8 {
        let m = b.op(Op::Bin(BinOp::Fmul), ty, &[x, y]);
        let m = nan_fix(b, m, &[x, y]);
        return b.op(Op::FNeg, ty, &[m]);
    }
    intr(b, ["maximum", "minimum", "maximumnum", "minimumnum"][k as usize - 4], ty, &[x, y])
}

/// a + x * y, fused
pub fn fma(b: &mut Builder, a: ValueId, x: ValueId, y: ValueId) -> ValueId {
    let t = b.ty(a);
    let r = intr(b, "fma", t, &[x, y, a]);
    nan_fix(b, r, &[a, x, y])
}

pub fn fsqrt(b: &mut Builder, x: ValueId) -> ValueId {
    let t = b.ty(x);
    let r = intr(b, "sqrt", t, &[x]);
    nan_fix(b, r, &[x])
}

pub fn rint(o: u32) -> &'static str {
    match o {
        8 => "roundeven",
        9 => "ceil",
        10 => "floor",
        11 => "trunc",
        12 => "round",
        14 => "rint",
        15 => "nearbyint",
        _ => panic!("rint {o}"),
    }
}

/// FCVT[NPMZA][SU]: rounded by rnd (None: toward zero), then the saturating conversion
pub fn to_int(b: &mut Builder, x: ValueId, ity: Ty, signed: bool, rnd: Option<&str>) -> ValueId {
    let mut x = x;
    if let Some(r) = rnd {
        let t = b.ty(x);
        x = intr(b, r, t, &[x]);
    }
    let xt = b.ty(x);
    llcall(b, format!("llvm.fpto{}i.sat.{}.{}", if signed { "s" } else { "u" }, suffix(ity), suffix(xt)), ity, &[x])
}

/// FCMP's NZCV
fn flags_cmp(b: &mut Builder, x: ValueId, y: ValueId) -> [ValueId; 4] {
    let n = fcmp(b, "olt", I1, x, y);
    let z = fcmp(b, "oeq", I1, x, y);
    let c = fcmp(b, "uge", I1, x, y);
    let v = fcmp(b, "uno", I1, x, y);
    [n, z, c, v]
}

// ---- templates
fn t_fp_dp1(b: &mut Builder, i: &Insn, _pc: u64) {
    let ty = fty(i.f("ftype"));
    let o = i.f("opcode");
    let r = if matches!(o, 4 | 5 | 7) {
        let to = match o {
            4 => Ty::F32,
            5 => Ty::F64,
            _ => Ty::F16,
        };
        let x = vread(b, i.f("Rn"), ty);
        b.cast(if to.width() > ty.width() { CastOp::Fpext } else { CastOp::Fptrunc }, x, to)
    } else if o == 0 {
        vread(b, i.f("Rn"), ity_of(ty))
    } else {
        let x = vread(b, i.f("Rn"), ty);
        match o {
            1 => intr(b, "fabs", ty, &[x]),
            2 => b.op(Op::FNeg, ty, &[x]),
            3 => fsqrt(b, x),
            _ => intr(b, rint(o), ty, &[x]),
        }
    };
    vwrite(b, i.f("Rd"), r);
}

fn t_fp_dp2(b: &mut Builder, i: &Insn, _pc: u64) {
    let ty = fty(i.f("ftype"));
    let n = vread(b, i.f("Rn"), ty);
    let m = vread(b, i.f("Rm"), ty);
    let r = fbin(b, i.f("opcode"), n, m);
    vwrite(b, i.f("Rd"), r);
}

fn t_fp_dp3(b: &mut Builder, i: &Insn, _pc: u64) {
    let ty = fty(i.f("ftype"));
    let mut n = vread(b, i.f("Rn"), ty);
    let m = vread(b, i.f("Rm"), ty);
    let mut a = vread(b, i.f("Ra"), ty);
    let k = i.f("o1") * 2 + i.f("o0");
    if k == 1 || k == 2 {
        n = b.op(Op::FNeg, ty, &[n]);
    }
    if k == 2 || k == 3 {
        a = b.op(Op::FNeg, ty, &[a]);
    }
    let r = fma(b, a, n, m);
    vwrite(b, i.f("Rd"), r);
}

fn t_fp_cmp(b: &mut Builder, i: &Insn, _pc: u64) {
    let ty = fty(i.f("ftype"));
    let x = vread(b, i.f("Rn"), ty);
    let y = if i.f("opc") & 1 != 0 { b.c(0, ty) } else { vread(b, i.f("Rm"), ty) };
    let fl = flags_cmp(b, x, y);
    set_nzcv(b, fl[0], fl[1], fl[2], fl[3]);
    b.cmp = Some(Cmp { nzcv: fl, a: x, b: y, kind: "fcmp" });
}

fn t_fp_ccmp(b: &mut Builder, i: &Insn, _pc: u64) {
    let ty = fty(i.f("ftype"));
    let ok = cond(b, i.f("cond"));
    let x = vread(b, i.f("Rn"), ty);
    let y = vread(b, i.f("Rm"), ty);
    let fl = flags_cmp(b, x, y);
    let nzcv = i.f("nzcv");
    let mut s = [fl[0]; 4];
    for k in 0..4 {
        let c = b.c((nzcv >> (3 - k) & 1) as i128, I1);
        s[k] = b.select(ok, fl[k], c);
    }
    set_nzcv(b, s[0], s[1], s[2], s[3]);
    b.cmp = None;
}

fn t_fp_csel(b: &mut Builder, i: &Insn, _pc: u64) {
    let ty = ity_of(fty(i.f("ftype")));
    let c = cond(b, i.f("cond"));
    let n = vread(b, i.f("Rn"), ty);
    let m = vread(b, i.f("Rm"), ty);
    let r = b.select(c, n, m);
    vwrite(b, i.f("Rd"), r);
}

fn t_fp_imm(b: &mut Builder, i: &Insn, _pc: u64) {
    let ty = fty(i.f("ftype"));
    let c = b.c(info::fp_imm_bits(i.f("imm8"), i.f("ftype")) as i128, ity_of(ty));
    vwrite(b, i.f("Rd"), c);
}

fn cvt_rnd(rm: u32) -> Option<&'static str> {
    match rm {
        0 => Some("roundeven"),
        1 => Some("ceil"),
        2 => Some("floor"),
        _ => None,
    }
}

fn t_fp_int(b: &mut Builder, i: &Insn, _pc: u64) {
    let (sf, ft, rm, o) = (i.f("sf"), i.f("ftype"), i.f("rmode"), i.f("opcode"));
    let ity = if sf != 0 { I64 } else { I32 };
    if ft == 2 {
        let r = if o == 6 { i.f("Rn") } else { i.f("Rd") };
        let g = b.get(vreg(r));
        let v = b.bitcast(g, Ty::vec(2, I64));
        if o == 6 {
            let e = b.extract(v, 1);
            setx(b, i.f("Rd"), e, false);
        } else {
            let x = getx(b, i.f("Rn"), 1, false);
            let ins = b.insert(v, x, 1);
            let c = b.bitcast(ins, Ty::V128);
            b.set(vreg(i.f("Rd")), c);
        }
        return;
    }
    let ty = fty(ft);
    if o == 6 {
        let v = vread(b, i.f("Rn"), ity);
        setx(b, i.f("Rd"), v, false);
    } else if o == 7 {
        let x = getx(b, i.f("Rn"), sf, false);
        vwrite(b, i.f("Rd"), x);
    } else if o == 2 || o == 3 {
        let x = getx(b, i.f("Rn"), sf, false);
        let r = b.cast(if o == 2 { CastOp::Sitofp } else { CastOp::Uitofp }, x, ty);
        vwrite(b, i.f("Rd"), r);
    } else {
        let rnd = if o >= 4 { Some("round") } else { cvt_rnd(rm) };
        let x = vread(b, i.f("Rn"), ty);
        let r = to_int(b, x, ity, o & 1 == 0, rnd);
        setx(b, i.f("Rd"), r, false);
    }
}

fn t_fp_fixed(b: &mut Builder, i: &Insn, _pc: u64) {
    let (sf, o) = (i.f("sf"), i.f("opcode"));
    let ty = fty(i.f("ftype"));
    let ity = if sf != 0 { I64 } else { I32 };
    let fb = 64 - i.f("scale") as i32;
    if o == 2 || o == 3 {
        let g = getx(b, i.f("Rn"), sf, false);
        let x = b.cast(if o == 2 { CastOp::Sitofp } else { CastOp::Uitofp }, g, ty);
        let k = fconst(b, 2f64.powi(-fb), ty);
        let r = b.op(Op::Bin(BinOp::Fmul), ty, &[x, k]);
        vwrite(b, i.f("Rd"), r);
    } else {
        let v = vread(b, i.f("Rn"), ty);
        let k = fconst(b, 2f64.powi(fb), ty);
        let x = b.op(Op::Bin(BinOp::Fmul), ty, &[v, k]);
        let r = to_int(b, x, ity, o == 0, None);
        setx(b, i.f("Rd"), r, false);
    }
}

// ---- loads and stores
const VTY: [Ty; 5] = [I8, I16, I32, I64, Ty::V128];

fn load(b: &mut Builder, addr: ValueId, t: Ty) -> ValueId {
    b.op(Op::Load, t, &[addr])
}

/// sem_int.extend (ExtendReg), as the Python shares it
fn extend(b: &mut Builder, v64: ValueId, option: u32, amt: u32, t: Ty) -> ValueId {
    let src = [I8, I16, I32, I64][(option & 3) as usize];
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

fn ldst_reg(b: &mut Builder, addr: ValueId, l: u32, t: u32, sc: u32) {
    if l != 0 {
        let v = load(b, addr, VTY[sc as usize]);
        vwrite(b, t, v);
    } else {
        let v = vread(b, t, VTY[sc as usize]);
        store(b, addr, v);
    }
}

fn mode(i: &Insn) -> &'static str {
    match i.p("mode") {
        Some(Param::Str(s)) => s,
        _ => panic!("{}: no mode", i.name()),
    }
}

fn t_ldst_v(b: &mut Builder, i: &Insn, _pc: u64) {
    let sc = info::v_scale(i.f("size"), i.f("opc"));
    let base = getx(b, i.f("Rn"), 1, true);
    let mut wb = None;
    let addr = match mode(i) {
        "uimm" => {
            let c = b.c64((i.f("imm12") as i128) << sc);
            b.bin(BinOp::Add, base, c)
        }
        "unscaled" => {
            let c = b.c64(sx(i.f("imm9"), 9));
            b.bin(BinOp::Add, base, c)
        }
        "post" => {
            let c = b.c64(sx(i.f("imm9"), 9));
            wb = Some(b.bin(BinOp::Add, base, c));
            base
        }
        "pre" => {
            let c = b.c64(sx(i.f("imm9"), 9));
            let a = b.bin(BinOp::Add, base, c);
            wb = Some(a);
            a
        }
        _ => {
            let m = getx(b, i.f("Rm"), 1, false);
            let e = extend(b, m, i.f("option"), if i.f("S") != 0 { sc } else { 0 }, I64);
            b.bin(BinOp::Add, base, e)
        }
    };
    ldst_reg(b, addr, i.f("opc") & 1, i.f("Rt"), sc);
    if let Some(w) = wb {
        setx(b, i.f("Rn"), w, true);
    }
}

fn t_ldst_v_pair(b: &mut Builder, i: &Insn, _pc: u64) {
    let (opc, mode, l) = (i.f("opc"), i.f("mode"), i.f("L"));
    let sc = 2 + opc;
    let base = getx(b, i.f("Rn"), 1, true);
    let off = b.c64(sx(i.f("imm7"), 7) << sc);
    let mut wb = None;
    let addr = if mode == 1 {
        wb = Some(b.bin(BinOp::Add, base, off));
        base
    } else if mode == 3 {
        let a = b.bin(BinOp::Add, base, off);
        wb = Some(a);
        a
    } else {
        b.bin(BinOp::Add, base, off)
    };
    let c = b.c64(1i128 << sc);
    let addr2 = b.bin(BinOp::Add, addr, c);
    let t = VTY[sc as usize];
    if l != 0 {
        let lo = load(b, addr, t);
        let hi = load(b, addr2, t);
        vwrite(b, i.f("Rt"), lo);
        vwrite(b, i.f("Rt2"), hi);
    } else {
        let v1 = vread(b, i.f("Rt"), t);
        let v2 = vread(b, i.f("Rt2"), t);
        store(b, addr, v1);
        store(b, addr2, v2);
    }
    if let Some(w) = wb {
        setx(b, i.f("Rn"), w, true);
    }
}

fn t_ldr_v_lit(b: &mut Builder, i: &Insn, pc: u64) {
    let sc = 2 + i.f("opc");
    let a = b.c64(pc as i128 + (sx(i.f("imm19"), 19) << 2));
    let v = load(b, a, VTY[sc as usize]);
    vwrite(b, i.f("Rt"), v);
}

fn post_index(b: &mut Builder, i: &Insn, base: ValueId, nbytes: u32) {
    let inc = if i.f("Rm") == 31 { b.c64(nbytes as i128) } else { getx(b, i.f("Rm"), 1, false) };
    let s = b.bin(BinOp::Add, base, inc);
    setx(b, i.f("Rn"), s, true);
}

fn post(i: &Insn) -> bool {
    matches!(i.p("post"), Some(Param::Bool(true)))
}

/// LD1-LD4/ST1-ST4 (multiple structures)
fn t_ldst_mult(b: &mut Builder, i: &Insn, _pc: u64) {
    let (nreg, selem) = info::mult(i.f("opcode")).unwrap();
    let (q, size, t) = (i.f("Q"), i.f("size"), i.f("Rt"));
    let rbytes = if q != 0 { 16 } else { 8 };
    let lane = Ty::I(8 << size);
    let per = rbytes >> size;
    let rty = Ty::vec(per, lane);
    let base = getx(b, i.f("Rn"), 1, true);
    let regs: Vec<u32> = (0..nreg).map(|k| (t + k) % 32).collect();
    if selem == 1 {
        for (k, &rn) in regs.iter().enumerate() {
            let c = b.c64(k as i128 * rbytes as i128);
            let addr = b.bin(BinOp::Add, base, c);
            if i.f("L") != 0 {
                let v = load(b, addr, rty);
                vwrite(b, rn, v);
            } else {
                let v = vread(b, rn, rty);
                store(b, addr, v);
            }
        }
    } else {
        let allty = Ty::vec(per * selem, lane);
        if i.f("L") != 0 {
            let whole = load(b, base, allty);
            for (k, &rn) in regs.iter().enumerate() {
                let m: Vec<i32> = (0..per).map(|e| (k as u32 + selem * e) as i32).collect();
                let s = b.shuffle(whole, whole, &m);
                vwrite(b, rn, s);
            }
        } else {
            let vals: Vec<ValueId> = regs.iter().map(|&rn| vread(b, rn, rty)).collect();
            let mut cat = vals[0];
            for &v in &vals[1..] {
                let n = b.ty(cat).lanes().0;
                let wide = if n != per {
                    let m: Vec<i32> = (0..per as i32).chain(std::iter::repeat(-1).take((n - per) as usize)).collect();
                    b.shuffle(v, v, &m)
                } else {
                    v
                };
                let m: Vec<i32> = (0..n as i32).chain((0..per as i32).map(|k| n as i32 + k)).collect();
                cat = b.shuffle(cat, wide, &m);
            }
            let m: Vec<i32> = (0..per).flat_map(|e| (0..selem).map(move |k| (k * per + e) as i32)).collect();
            let s = b.shuffle(cat, cat, &m);
            store(b, base, s);
        }
    }
    if post(i) {
        post_index(b, i, base, nreg * rbytes);
    }
}

/// LD1-LD4/ST1-ST4 (single structure) of one lane, and LD1R-LD4R
fn t_ldst_one(b: &mut Builder, i: &Insn, _pc: u64) {
    let s = info::one_shape(i.f("opcode"), i.f("S"), i.f("size"), i.f("Q"), i.f("R"));
    let sc = s.scale;
    let lane = Ty::I(8 << sc);
    let base = getx(b, i.f("Rn"), 1, true);
    for k in 0..s.selem {
        let rn = (i.f("Rt") + k) % 32;
        let c = b.c64((k as i128) << sc);
        let addr = b.bin(BinOp::Add, base, c);
        if s.replicate {
            let per = (if i.f("Q") != 0 { 16 } else { 8 }) >> sc;
            let x = load(b, addr, lane);
            let z = b.zero(Ty::vec(per, lane));
            let v = b.insert(z, x, 0);
            let r = b.shuffle(v, v, &vec![0; per as usize]);
            vwrite(b, rn, r);
        } else if i.f("L") != 0 {
            let vt = Ty::vec(16 >> sc, lane);
            let old = vread(b, rn, vt);
            let x = load(b, addr, lane);
            let r = b.insert(old, x, s.index.unwrap());
            vwrite(b, rn, r);
        } else {
            let vt = Ty::vec(16 >> sc, lane);
            let old = vread(b, rn, vt);
            let x = b.extract(old, s.index.unwrap());
            store(b, addr, x);
        }
    }
    if post(i) {
        post_index(b, i, base, s.selem << sc);
    }
}

pub const TEMPLATES: &[(&str, crate::sem::Template)] = &[
    ("fp_dp1", t_fp_dp1),
    ("fp_dp2", t_fp_dp2),
    ("fp_dp3", t_fp_dp3),
    ("fp_cmp", t_fp_cmp),
    ("fp_ccmp", t_fp_ccmp),
    ("fp_csel", t_fp_csel),
    ("fp_imm", t_fp_imm),
    ("fp_int", t_fp_int),
    ("fp_fixed", t_fp_fixed),
    ("ldst_v", t_ldst_v),
    ("ldst_v_pair", t_ldst_v_pair),
    ("ldr_v_lit", t_ldr_v_lit),
    ("ldst_mult", t_ldst_mult),
    ("ldst_one", t_ldst_one),
];

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn consts() {
        assert_eq!(fbits(2.0, Ty::F32), 0x40000000);
        assert_eq!(fbits(0.5, Ty::F16), 0x3800);
        assert_eq!(fbits(f64::INFINITY, Ty::F16), 0x7c00);
        assert_eq!(fbits(2f64.powi(-64), Ty::F32), 0x1f800000);
    }
}
