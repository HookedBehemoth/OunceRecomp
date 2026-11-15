//! Advanced SIMD (NEON) semantics (nx2/lift2/sem_neon.py): templates for enc_neon's table, lane-generic over the
//! arrangements; scalar forms use the same operations on scalar types. Values are created in the Python's order
//! (its argument evaluation order), so the value numbers match. Raw `b.op` where the Python calls b.op (no folding).

use crate::ir::{BinOp, Builder, Call, Callee, CastOp, Op, ValueId};
use crate::sem::fp::{fbin, fconst, fma, fsqrt, intr, llcall, to_int, vread, vwrite};
use crate::sem::int::{getx, setx};
use crate::ty::{Ty, I1, I128, I16, I32, I64, I8};
use ounce_decode::{info, Insn, Param, Fld};

// ---- types
fn lane_t(size: u32, fp: bool) -> Ty {
    if fp {
        if size == 2 { Ty::F32 } else { Ty::F64 }
    } else {
        Ty::I(8 << size)
    }
}

#[allow(non_snake_case)]
fn VT(size: u32, q: u32, fp: bool) -> Ty {
    Ty::vec((if q != 0 { 16 } else { 8 }) >> size, lane_t(size, fp))
}

fn shape(t: Ty, lane: Ty) -> Ty {
    if t.is_vec() { Ty::vec(t.lanes().0, lane) } else { lane }
}

fn lane_of(t: Ty) -> Ty {
    if t.is_vec() { t.lanes().1.ty() } else { t }
}

fn lw(t: Ty) -> u32 {
    lane_of(t).width()
}

fn ity(t: Ty) -> Ty {
    shape(t, Ty::I(lw(t) as u16))
}

fn widen(t: Ty) -> Ty {
    shape(t, Ty::I(2 * lw(t) as u16))
}

fn vty(n: u32, lane: Ty) -> Ty {
    Ty::vec(n, lane)
}

#[allow(non_snake_case)]
fn K(b: &mut Builder, x: i128, t: Ty) -> ValueId {
    if t.is_vec() {
        let (n, lane) = t.lanes();
        if lane.ty().is_fp() {
            let c = b.vconst_splat(x as u64, vty(n, Ty::I(lane.width() as u16)));
            return b.bitcast(c, t);
        }
        return b.vconst_splat(x as u64, t);
    }
    b.c(x, t)
}

/// an FP constant of a scalar or vector FP type
#[allow(non_snake_case)]
fn fK(b: &mut Builder, value: f64, t: Ty) -> ValueId {
    let c = fconst(b, value, lane_of(t));
    if t.is_vec() {
        let bits = b.konst(c).unwrap() as i128;
        K(b, bits, t)
    } else {
        c
    }
}

fn icmp(b: &mut Builder, pred: &'static str, x: ValueId, y: ValueId) -> ValueId {
    let t = shape(b.ty(x), I1);
    b.op(Op::Icmp(pred), t, &[x, y])
}

fn fcmp(b: &mut Builder, pred: &'static str, x: ValueId, y: ValueId) -> ValueId {
    let t = shape(b.ty(x), I1);
    b.op(Op::Fcmp(pred), t, &[x, y])
}

/// a compare as ARM's all-ones/zero lanes
fn mask(b: &mut Builder, pred: &'static str, x: ValueId, y: ValueId, fp: bool) -> ValueId {
    let m = if fp { fcmp(b, pred, x, y) } else { icmp(b, pred, x, y) };
    let t = ity(b.ty(x));
    b.op(Op::Cast(CastOp::Sext), t, &[m])
}

fn bin(b: &mut Builder, op: BinOp, x: ValueId, y: ValueId) -> ValueId {
    b.bin(op, x, y)
}

fn fop(b: &mut Builder, op: BinOp, t: Ty, x: ValueId, y: ValueId) -> ValueId {
    b.op(Op::Bin(op), t, &[x, y])
}

fn call(b: &mut Builder, callee: Callee, t: Ty, args: &[ValueId]) -> ValueId {
    llcall(b, callee, t, args)
}

/// scalar x in every lane of t
fn splat(b: &mut Builder, x: ValueId, t: Ty) -> ValueId {
    let n = t.lanes().0;
    let z = K(b, 0, t);
    let v = b.insert(z, x, 0);
    b.shuffle(v, v, &vec![0; n as usize])
}

fn lane_extract(b: &mut Builder, n: u32, size: u32, idx: u32, fp: bool) -> ValueId {
    let v = vread(b, n, vty(16 >> size, lane_t(size, fp)));
    b.extract(v, idx)
}

/// the low (Q=0) or high (Q=1) 64 bits of Vn as a 64-bit vector of size lanes
fn half(b: &mut Builder, n: u32, size: u32, q: u32) -> ValueId {
    if q == 0 {
        return vread(b, n, VT(size, 0, false));
    }
    let v = vread(b, n, vty(2, I64));
    let e = b.extract(v, 1);
    b.bitcast(e, VT(size, 0, false))
}

/// Vd = r (a 64-bit vector): the low half with the upper zeroed, or (Q) the upper half with the lower kept
fn put_half(b: &mut Builder, d: u32, r: ValueId, q: u32) {
    if q == 0 {
        vwrite(b, d, r);
        return;
    }
    let old = vread(b, d, vty(2, I64));
    let x = b.bitcast(r, I64);
    let v = b.insert(old, x, 1);
    vwrite(b, d, v);
}

fn ext(b: &mut Builder, x: ValueId, t: Ty, signed: bool) -> ValueId {
    b.op(Op::Cast(if signed { CastOp::Sext } else { CastOp::Zext }), t, &[x])
}

fn trunc(b: &mut Builder, x: ValueId, t: Ty) -> ValueId {
    b.op(Op::Cast(CastOp::Trunc), t, &[x])
}

fn smax_of(w: u32) -> i128 {
    (1i128 << (w - 1)) - 1
}

/// the saturating add/sub intrinsic of SQADD UQADD SQSUB UQSUB
fn sat_name(mn: &str) -> &'static str {
    match mn {
        "sqadd" => "sadd.sat",
        "uqadd" => "uadd.sat",
        "sqsub" => "ssub.sat",
        _ => "usub.sat",
    }
}

fn minmax_name(mn: &str) -> &'static str {
    match mn {
        "smax" => "smax",
        "umax" => "umax",
        "smin" => "smin",
        _ => "umin",
    }
}

// ---- integer operations, any integer type
fn abd(b: &mut Builder, x: ValueId, y: ValueId, signed: bool) -> ValueId {
    let gt = icmp(b, if signed { "sgt" } else { "ugt" }, x, y);
    let s1 = bin(b, BinOp::Sub, x, y);
    let s2 = bin(b, BinOp::Sub, y, x);
    b.select(gt, s1, s2)
}

fn halving(b: &mut Builder, x: ValueId, y: ValueId, signed: bool, sub: bool, rnd: bool) -> ValueId {
    let xt = b.ty(x);
    let wt = widen(xt);
    let ex = ext(b, x, wt, signed);
    let ey = ext(b, y, wt, signed);
    let mut s = bin(b, if sub { BinOp::Sub } else { BinOp::Add }, ex, ey);
    if rnd {
        let one = K(b, 1, wt);
        s = bin(b, BinOp::Add, s, one);
    }
    let one = K(b, 1, wt);
    let sh = bin(b, if signed { BinOp::Ashr } else { BinOp::Lshr }, s, one);
    trunc(b, sh, xt)
}

/// SSHL/USHL/SRSHL/URSHL: x shifted by the signed low byte of each lane of m (left if positive)
fn shl_reg(b: &mut Builder, x: ValueId, m: ValueId, signed: bool, rnd: bool) -> ValueId {
    let t = b.ty(x);
    let w = lw(t) as i128;
    let sh = if w > 8 {
        let tr = trunc(b, m, shape(t, I8));
        b.op(Op::Cast(CastOp::Sext), t, &[tr])
    } else {
        m
    };
    let k0 = K(b, 0, t);
    let pos = icmp(b, "sge", sh, k0);
    let kw = K(b, w, t);
    let inr = icmp(b, "slt", sh, kw);
    let l = bin(b, BinOp::Shl, x, sh);
    let k0 = K(b, 0, t);
    let left = b.select(inr, l, k0);
    let k0 = K(b, 0, t);
    let nsh = bin(b, BinOp::Sub, k0, sh);
    let mut right;
    if signed {
        let kw1 = K(b, w - 1, t);
        let cl = call(b, Callee::llvm1("umin", t), t, &[nsh, kw1]);
        right = bin(b, BinOp::Ashr, x, cl);
        if rnd {
            let k1 = K(b, 1, t);
            let n1 = bin(b, BinOp::Sub, nsh, k1);
            let kw1 = K(b, w - 1, t);
            let um = call(b, Callee::llvm1("umin", t), t, &[n1, kw1]);
            let ls = bin(b, BinOp::Lshr, x, um);
            let k1 = K(b, 1, t);
            let bit = bin(b, BinOp::And, ls, k1);
            let kw = K(b, w, t);
            let ok = icmp(b, "ult", n1, kw);
            let k0 = K(b, 0, t);
            let sel = b.select(ok, bit, k0);
            right = bin(b, BinOp::Add, right, sel);
            let kw = K(b, w, t);
            let big = icmp(b, "ugt", nsh, kw);
            let k0 = K(b, 0, t);
            right = b.select(big, k0, right);
        }
    } else {
        let kw = K(b, w, t);
        let ok = icmp(b, "ult", nsh, kw);
        let ls = bin(b, BinOp::Lshr, x, nsh);
        let k0 = K(b, 0, t);
        right = b.select(ok, ls, k0);
        if rnd {
            let k1 = K(b, 1, t);
            let n1 = bin(b, BinOp::Sub, nsh, k1);
            let kw = K(b, w, t);
            let ok1 = icmp(b, "ult", n1, kw);
            let ls = bin(b, BinOp::Lshr, x, n1);
            let k1 = K(b, 1, t);
            let a = bin(b, BinOp::And, ls, k1);
            let k0 = K(b, 0, t);
            let bit = b.select(ok1, a, k0);
            right = bin(b, BinOp::Add, right, bit);
        }
    }
    b.select(pos, left, right)
}

/// x (a wide integer type) clamped to the range of the narrow type nt and truncated
fn sat_narrow(b: &mut Builder, x: ValueId, nt: Ty, signed_in: bool, signed_out: bool) -> ValueId {
    let w = lw(nt);
    let t = b.ty(x);
    let (lo, hi) = if signed_out { (-(1i128 << (w - 1)), smax_of(w)) } else { (0, (1i128 << w) - 1) };
    let mut x = x;
    if signed_in {
        let k = if lo != 0 || signed_out { K(b, lo, t) } else { K(b, 0, t) };
        x = call(b, Callee::llvm1("smax", t), t, &[x, k]);
        let k = K(b, hi, t);
        x = call(b, Callee::llvm1("smin", t), t, &[x, k]);
    } else {
        let k = K(b, hi, t);
        x = call(b, Callee::llvm1("umin", t), t, &[x, k]);
    }
    trunc(b, x, nt)
}

/// SQDMULH/SQRDMULH
fn sqdmulh(b: &mut Builder, x: ValueId, y: ValueId, rnd: bool) -> ValueId {
    let t = b.ty(x);
    let w = lw(t);
    let wt = widen(t);
    let ex = ext(b, x, wt, true);
    let ey = ext(b, y, wt, true);
    let mut p = bin(b, BinOp::Mul, ex, ey);
    if rnd {
        let k = K(b, 1i128 << (w - 2), wt);
        p = bin(b, BinOp::Add, p, k);
    }
    let k = K(b, (w - 1) as i128, wt);
    let r = bin(b, BinOp::Ashr, p, k);
    sat_narrow(b, r, t, true, true)
}

/// the integer three-same operations; d (the destination's old value) for the accumulating ones
fn int3(b: &mut Builder, mn: &str, x: ValueId, y: ValueId, d: Option<ValueId>) -> ValueId {
    let t = b.ty(x);
    let signed = mn.starts_with('s');
    match mn {
        "add" => bin(b, BinOp::Add, x, y),
        "sub" => bin(b, BinOp::Sub, x, y),
        "mul" => bin(b, BinOp::Mul, x, y),
        "sqadd" | "uqadd" | "sqsub" | "uqsub" => {
            call(b, Callee::llvm1(sat_name(mn), t), t, &[x, y])
        }
        "cmgt" => mask(b, "sgt", x, y, false),
        "cmhi" => mask(b, "ugt", x, y, false),
        "cmge" => mask(b, "sge", x, y, false),
        "cmhs" => mask(b, "uge", x, y, false),
        "cmeq" => mask(b, "eq", x, y, false),
        "cmtst" => {
            let a = bin(b, BinOp::And, x, y);
            let k = K(b, 0, t);
            mask(b, "ne", a, k, false)
        }
        "smax" | "umax" | "smin" | "umin" => call(b, Callee::llvm1(minmax_name(mn), t), t, &[x, y]),
        "shadd" | "uhadd" | "srhadd" | "urhadd" | "shsub" | "uhsub" => {
            halving(b, x, y, signed, mn.contains("sub"), mn.contains("rh"))
        }
        "sabd" | "uabd" => abd(b, x, y, signed),
        "saba" | "uaba" => {
            let a = abd(b, x, y, signed);
            bin(b, BinOp::Add, d.unwrap(), a)
        }
        "mla" => {
            let m = bin(b, BinOp::Mul, x, y);
            bin(b, BinOp::Add, d.unwrap(), m)
        }
        "mls" => {
            let m = bin(b, BinOp::Mul, x, y);
            bin(b, BinOp::Sub, d.unwrap(), m)
        }
        "sshl" | "ushl" | "srshl" | "urshl" => shl_reg(b, x, y, signed, mn[1..3].contains('r')),
        "sqdmulh" | "sqrdmulh" => sqdmulh(b, x, y, mn == "sqrdmulh"),
        "sqshl" | "uqshl" | "sqrshl" | "uqrshl" => {
            let rnd = mn[2..].contains('r');
            lanewise(b, x, y, |b, u, w| qshl_lane(b, u, w, signed, rnd))
        }
        "pmul" => pmul(b, x, y),
        _ => panic!("int3 {mn}"),
    }
}

/// fn(lane of x, lane of y) for every lane
fn lanewise(b: &mut Builder, x: ValueId, y: ValueId, f: impl Fn(&mut Builder, ValueId, ValueId) -> ValueId) -> ValueId {
    let t = b.ty(x);
    if !t.is_vec() {
        return f(b, x, y);
    }
    let mut r = x;
    for k in 0..t.lanes().0 {
        let ex = b.extract(x, k);
        let ey = b.extract(y, k);
        let v = f(b, ex, ey);
        r = b.insert(r, v, k);
    }
    r
}

/// [SU]Q[R]SHL by register, one lane, computed in i128
fn qshl_lane(b: &mut Builder, x: ValueId, m: ValueId, signed: bool, rnd: bool) -> ValueId {
    let t = b.ty(x);
    let w = t.width();
    let wt = I128;
    let src = if w > 8 { trunc(b, m, I8) } else { m };
    let sh = b.op(Op::Cast(CastOp::Sext), wt, &[src]);
    let xx = ext(b, x, wt, signed);
    let c0 = b.c(0, wt);
    let pos = b.icmp("sge", sh, c0);
    let c64 = b.c(64, wt);
    let um = call(b, Callee::llvm1("umin", I128), wt, &[sh, c64]);
    let l = bin(b, BinOp::Shl, xx, um);
    let c0 = b.c(0, wt);
    let neg = bin(b, BinOp::Sub, c0, sh);
    let c127 = b.c(127, wt);
    let n = call(b, Callee::llvm1("umin", I128), wt, &[neg, c127]);
    let mut rr = xx;
    if rnd {
        let c1 = b.c(1, wt);
        let c1b = b.c(1, wt);
        let n1 = bin(b, BinOp::Sub, n, c1b);
        let s = bin(b, BinOp::Shl, c1, n1);
        rr = bin(b, BinOp::Add, xx, s);
    }
    let r = bin(b, if signed { BinOp::Ashr } else { BinOp::Lshr }, rr, n);
    let mut v = b.select(pos, l, r);
    if signed {
        let lo = b.c(-(1i128 << (w - 1)), wt);
        v = call(b, Callee::llvm1("smax", I128), wt, &[v, lo]);
        let hi = b.c(smax_of(w), wt);
        v = call(b, Callee::llvm1("smin", I128), wt, &[v, hi]);
    } else {
        let hi = b.c((1i128 << w) - 1, wt);
        v = call(b, Callee::llvm1("umin", I128), wt, &[v, hi]);
    }
    trunc(b, v, t)
}

/// PMUL/PMULL: carry-less products, per lane
fn pmul(b: &mut Builder, x: ValueId, y: ValueId) -> ValueId {
    let t = b.ty(x);
    let mut r = K(b, 0, t);
    for i in 0..8 {
        let ki = K(b, i, t);
        let s = bin(b, BinOp::Lshr, y, ki);
        let k1 = K(b, 1, t);
        let bit = bin(b, BinOp::And, s, k1);
        let ki = K(b, i, t);
        let sl = bin(b, BinOp::Shl, x, ki);
        let k0 = K(b, 0, t);
        let ng = bin(b, BinOp::Sub, k0, bit);
        let a = bin(b, BinOp::And, sl, ng);
        r = bin(b, BinOp::Xor, r, a);
    }
    r
}

/// ARM's pairwise operations: the lane pairs of x then y
fn pairwise(b: &mut Builder, x: ValueId, y: ValueId, f: impl Fn(&mut Builder, ValueId, ValueId) -> ValueId) -> ValueId {
    let n = b.ty(x).lanes().0 as i32;
    let m: Vec<i32> = (0..2 * n).step_by(2).collect();
    let ev = b.shuffle(x, y, &m);
    let m: Vec<i32> = (1..2 * n).step_by(2).collect();
    let od = b.shuffle(x, y, &m);
    f(b, ev, od)
}

// ---- FP operations, any FP type
fn fneg(b: &mut Builder, x: ValueId) -> ValueId {
    let t = b.ty(x);
    b.op(Op::FNeg, t, &[x])
}

fn fabs(b: &mut Builder, x: ValueId) -> ValueId {
    let t = b.ty(x);
    intr(b, "fabs", t, &[x])
}

/// lanes where one operand is a zero and the other an infinity
fn zero_inf(b: &mut Builder, x: ValueId, y: ValueId) -> ValueId {
    let t = b.ty(x);
    let inf = fK(b, f64::INFINITY, t);
    let z = fK(b, 0.0, t);
    let ax = fabs(b, x);
    let ay = fabs(b, y);
    let xz = fcmp(b, "oeq", x, z);
    let yz = fcmp(b, "oeq", y, z);
    let xi = fcmp(b, "oeq", ax, inf);
    let yi = fcmp(b, "oeq", ay, inf);
    let a = bin(b, BinOp::And, xz, yi);
    let c = bin(b, BinOp::And, xi, yz);
    bin(b, BinOp::Or, a, c)
}

const FBIN: [&str; 8] = ["fmul", "fdiv", "fadd", "fsub", "fmax", "fmin", "fmaxnm", "fminnm"];

fn fp3(b: &mut Builder, mn: &str, x: ValueId, y: ValueId, d: Option<ValueId>) -> ValueId {
    let t = b.ty(x);
    if let Some(k) = FBIN.iter().position(|&s| s == mn) {
        return fbin(b, k as u32, x, y);
    }
    match mn {
        "fmla" => fma(b, d.unwrap(), x, y),
        "fmls" => {
            let nx = fneg(b, x);
            fma(b, d.unwrap(), nx, y)
        }
        "fabd" => {
            let s = fbin(b, 3, x, y);
            fabs(b, s)
        }
        "fcmeq" => mask(b, "oeq", x, y, true),
        "fcmge" => mask(b, "oge", x, y, true),
        "fcmgt" => mask(b, "ogt", x, y, true),
        "facge" | "facgt" => {
            let ax = fabs(b, x);
            let ay = fabs(b, y);
            mask(b, if mn == "facge" { "oge" } else { "ogt" }, ax, ay, true)
        }
        "frecps" => {
            let two = fK(b, 2.0, t);
            let nx = fneg(b, x);
            let r = fma(b, two, nx, y);
            let zi = zero_inf(b, x, y);
            let two = fK(b, 2.0, t);
            b.select(zi, two, r)
        }
        "frsqrts" => {
            let three = fK(b, 3.0, t);
            let nx = fneg(b, x);
            let m = fma(b, three, nx, y);
            let h = fK(b, 0.5, t);
            let r = fop(b, BinOp::Fmul, t, m, h);
            let zi = zero_inf(b, x, y);
            let k = fK(b, 1.5, t);
            b.select(zi, k, r)
        }
        "fmulx" => {
            let r = fop(b, BinOp::Fmul, t, x, y);
            let it = ity(t);
            let bx = b.bitcast(x, it);
            let by = b.bitcast(y, it);
            let xo = bin(b, BinOp::Xor, bx, by);
            let k = K(b, 1i128 << (lw(t) - 1), it);
            let sgn = bin(b, BinOp::And, xo, k);
            let two = fK(b, 2.0, t);
            let tb = b.bitcast(two, it);
            let o = bin(b, BinOp::Or, tb, sgn);
            let two = b.bitcast(o, t);
            let zi = zero_inf(b, x, y);
            b.select(zi, two, r)
        }
        _ => panic!("fp3 {mn}"),
    }
}

fn fp_pair(b: &mut Builder, mn: &str, x: ValueId, y: ValueId) -> ValueId {
    let k: &'static str = match mn {
        "faddp" => "fadd",
        "fmaxp" => "fmax",
        "fminp" => "fmin",
        "fmaxnmp" => "fmaxnm",
        _ => "fminnm",
    };
    pairwise(b, x, y, |b, e, o| fp3(b, k, e, o, None))
}

fn int_pair(b: &mut Builder, mn: &str, x: ValueId, y: ValueId) -> ValueId {
    let k: &'static str = match mn {
        "addp" => "add",
        "smaxp" => "smax",
        "umaxp" => "umax",
        "sminp" => "smin",
        _ => "umin",
    };
    pairwise(b, x, y, |b, e, o| int3(b, k, e, o, None))
}

/// FRECPE/FRSQRTE: the inline estimate (fp_estimate) for each lane
fn estimate(b: &mut Builder, x: ValueId, f: &str) -> ValueId {
    let t = b.ty(x);
    let lt = lane_of(t);
    let c = || Call::ctx(Callee::Helper(crate::emit::estimate_helper(f, lt)), false);
    if !t.is_vec() {
        return b.call(c(), lt, &[x]);
    }
    let mut r = x;
    for k in 0..t.lanes().0 {
        let e = b.extract(x, k);
        let v = b.call(c(), lt, &[e]);
        r = b.insert(r, v, k);
    }
    r
}

fn cvt2(mn: &str) -> Option<(Option<&'static str>, bool)> {
    Some(match mn {
        "fcvtns" => (Some("roundeven"), true),
        "fcvtms" => (Some("floor"), true),
        "fcvtas" => (Some("round"), true),
        "fcvtps" => (Some("ceil"), true),
        "fcvtzs" => (None, true),
        "fcvtnu" => (Some("roundeven"), false),
        "fcvtmu" => (Some("floor"), false),
        "fcvtau" => (Some("round"), false),
        "fcvtpu" => (Some("ceil"), false),
        "fcvtzu" => (None, false),
        _ => return None,
    })
}

fn frint2(mn: &str) -> Option<&'static str> {
    Some(match mn {
        "frintn" => "roundeven",
        "frintm" => "floor",
        "frintp" => "ceil",
        "frintz" => "trunc",
        "frinta" => "round",
        "frintx" => "rint",
        "frinti" => "nearbyint",
        _ => return None,
    })
}

fn fp2(b: &mut Builder, mn: &str, x: ValueId) -> ValueId {
    let t = b.ty(x);
    if let Some(r) = frint2(mn) {
        return intr(b, r, t, &[x]);
    }
    if let Some((rnd, signed)) = cvt2(mn) {
        return to_int(b, x, ity(t), signed, rnd);
    }
    match mn {
        "fabs" => fabs(b, x),
        "fneg" => fneg(b, x),
        "fsqrt" => fsqrt(b, x),
        "frecpe" | "frsqrte" => estimate(b, x, mn),
        "frecpx" => frecpx(b, x),
        "fcmgt" | "fcmeq" | "fcmlt" | "fcmge" | "fcmle" => {
            let p = match mn {
                "fcmgt" => "ogt",
                "fcmeq" => "oeq",
                "fcmlt" => "olt",
                "fcmge" => "oge",
                _ => "ole",
            };
            let z = fK(b, 0.0, t);
            mask(b, p, x, z, true)
        }
        _ => panic!("fp2 {mn}"),
    }
}

fn scalar(i: &Insn) -> bool {
    matches!(i.p("scalar"), Some(Param::Bool(true)))
}

// ---- templates
fn t_simd_copy(b: &mut Builder, i: &Insn, _pc: u64) {
    let q = i.f(Fld::Q);
    let (kind, size) = info::copy_info(q, i.f(Fld::op), i.f(Fld::imm5), i.f(Fld::imm4)).unwrap();
    let idx = i.f(Fld::imm5) >> (size + 1);
    let lt = lane_t(size, false);
    match kind {
        "dup_elem" => {
            let e = lane_extract(b, i.f(Fld::Rn), size, idx, false);
            let r = splat(b, e, VT(size, q, false));
            vwrite(b, i.f(Fld::Rd), r);
        }
        "dup_gen" => {
            let x = getx(b, i.f(Fld::Rn), 1, false);
            let x = if size < 3 { trunc(b, x, lt) } else { x };
            let r = splat(b, x, VT(size, q, false));
            vwrite(b, i.f(Fld::Rd), r);
        }
        "smov" | "umov" => {
            let mut x = lane_extract(b, i.f(Fld::Rn), size, idx, false);
            if kind == "smov" {
                x = ext(b, x, if q != 0 { I64 } else { I32 }, true);
            }
            setx(b, i.f(Fld::Rd), x, false);
        }
        _ => {
            let vt = vty(16 >> size, lt);
            let x = if kind == "ins_gen" {
                let x = getx(b, i.f(Fld::Rn), 1, false);
                if size < 3 { trunc(b, x, lt) } else { x }
            } else {
                lane_extract(b, i.f(Fld::Rn), size, i.f(Fld::imm4) >> size, false)
            };
            let old = vread(b, i.f(Fld::Rd), vt);
            let r = b.insert(old, x, idx);
            vwrite(b, i.f(Fld::Rd), r);
        }
    }
}

fn t_simd_dups(b: &mut Builder, i: &Insn, _pc: u64) {
    let size = info::lowest(i.f(Fld::imm5)).unwrap();
    let e = lane_extract(b, i.f(Fld::Rn), size, i.f(Fld::imm5) >> (size + 1), false);
    vwrite(b, i.f(Fld::Rd), e);
}

fn operand_types(i: &Insn, size: u32, fp: bool, sc: bool) -> Ty {
    if sc { lane_t(size, fp) } else { VT(size, i.f(Fld::Q), fp) }
}

fn t_simd_3same(b: &mut Builder, i: &Insn, _pc: u64) {
    let sc = scalar(i);
    let (mn, kind, size) =
        info::same3_info(if sc { 1 } else { i.f(Fld::Q) }, i.f(Fld::U), i.f(Fld::size), i.f(Fld::opcode), sc).unwrap();
    if kind == "l" {
        let t = VT(0, i.f(Fld::Q), false);
        let n = vread(b, i.f(Fld::Rn), t);
        let m = vread(b, i.f(Fld::Rm), t);
        let d = vread(b, i.f(Fld::Rd), t);
        let r = match mn {
            "and" => bin(b, BinOp::And, n, m),
            "orr" => bin(b, BinOp::Or, n, m),
            "eor" => bin(b, BinOp::Xor, n, m),
            "bic" | "orn" => {
                let nm = b.not(m);
                bin(b, if mn == "bic" { BinOp::And } else { BinOp::Or }, n, nm)
            }
            "bsl" => {
                let a = bin(b, BinOp::And, d, n);
                let nd = b.not(d);
                let c = bin(b, BinOp::And, nd, m);
                bin(b, BinOp::Or, a, c)
            }
            "bit" => {
                let a = bin(b, BinOp::And, n, m);
                let nm = b.not(m);
                let c = bin(b, BinOp::And, d, nm);
                bin(b, BinOp::Or, a, c)
            }
            _ => {
                let a = bin(b, BinOp::And, d, m);
                let nm = b.not(m);
                let c = bin(b, BinOp::And, n, nm);
                bin(b, BinOp::Or, a, c)
            }
        };
        vwrite(b, i.f(Fld::Rd), r);
        return;
    }
    let fp = kind == "f";
    let t = operand_types(i, size, fp, sc);
    let n = vread(b, i.f(Fld::Rn), t);
    let m = vread(b, i.f(Fld::Rm), t);
    let d = if matches!(mn, "saba" | "uaba" | "mla" | "mls" | "fmla" | "fmls") {
        Some(vread(b, i.f(Fld::Rd), t))
    } else {
        None
    };
    let r = if matches!(mn, "faddp" | "fmaxp" | "fminp" | "fmaxnmp" | "fminnmp") {
        fp_pair(b, mn, n, m)
    } else if matches!(mn, "addp" | "smaxp" | "umaxp" | "sminp" | "uminp") {
        int_pair(b, mn, n, m)
    } else if fp {
        fp3(b, mn, n, m, d)
    } else {
        int3(b, mn, n, m, d)
    };
    vwrite(b, i.f(Fld::Rd), r);
}

fn t_simd_3diff(b: &mut Builder, i: &Insn, _pc: u64) {
    let (mn, kind, size) = info::diff3_info(i.f(Fld::U), i.f(Fld::size), i.f(Fld::opcode)).unwrap();
    let (q, u) = (i.f(Fld::Q), i.f(Fld::U));
    let signed = u == 0;
    let wt = VT(size + 1, 1, false);
    if kind == "n" {
        let x = vread(b, i.f(Fld::Rn), wt);
        let y = vread(b, i.f(Fld::Rm), wt);
        let mut s = bin(b, if mn.contains("sub") { BinOp::Sub } else { BinOp::Add }, x, y);
        let w = 8i128 << size;
        if mn.starts_with('r') {
            let k = K(b, 1i128 << (w - 1), wt);
            s = bin(b, BinOp::Add, s, k);
        }
        let k = K(b, w, wt);
        let sh = bin(b, BinOp::Lshr, s, k);
        let r = trunc(b, sh, VT(size, 0, false));
        put_half(b, i.f(Fld::Rd), r, q);
        return;
    }
    if size == 3 {
        b.f.target_features.push("+pclmul");
        let x = vread(b, i.f(Fld::Rn), vty(2, I64));
        let y = vread(b, i.f(Fld::Rm), vty(2, I64));
        let c = b.c(if q != 0 { 0x11 } else { 0 }, I8);
        let r = call(b, Callee::llvm("x86.pclmulqdq"), vty(2, I64), &[x, y, c]);
        vwrite(b, i.f(Fld::Rd), r);
        return;
    }
    let hm = half(b, i.f(Fld::Rm), size, q);
    let y = ext(b, hm, wt, signed);
    if kind == "w" {
        let x = vread(b, i.f(Fld::Rn), wt);
        let r = bin(b, if mn.contains("sub") { BinOp::Sub } else { BinOp::Add }, x, y);
        vwrite(b, i.f(Fld::Rd), r);
        return;
    }
    let xn = half(b, i.f(Fld::Rn), size, q);
    let yn = half(b, i.f(Fld::Rm), size, q);
    let x = ext(b, xn, wt, signed);
    let d = vread(b, i.f(Fld::Rd), wt);
    let r = match mn {
        "saddl" | "uaddl" => bin(b, BinOp::Add, x, y),
        "ssubl" | "usubl" => bin(b, BinOp::Sub, x, y),
        "sabdl" | "uabdl" => {
            let a = abd(b, xn, yn, signed);
            b.op(Op::Cast(CastOp::Zext), wt, &[a])
        }
        "sabal" | "uabal" => {
            let a = abd(b, xn, yn, signed);
            let z = b.op(Op::Cast(CastOp::Zext), wt, &[a]);
            bin(b, BinOp::Add, d, z)
        }
        "smull" | "umull" => bin(b, BinOp::Mul, x, y),
        "smlal" | "umlal" => {
            let m = bin(b, BinOp::Mul, x, y);
            bin(b, BinOp::Add, d, m)
        }
        "smlsl" | "umlsl" => {
            let m = bin(b, BinOp::Mul, x, y);
            bin(b, BinOp::Sub, d, m)
        }
        "pmull" => {
            let zx = b.op(Op::Cast(CastOp::Zext), wt, &[xn]);
            let zy = b.op(Op::Cast(CastOp::Zext), wt, &[yn]);
            pmul(b, zx, zy)
        }
        "sqdmull" | "sqdmlal" | "sqdmlsl" => {
            let pr = bin(b, BinOp::Mul, x, y);
            let mut r = call(b, Callee::llvm1("sadd.sat", wt), wt, &[pr, pr]);
            if mn != "sqdmull" {
                let op = if mn == "sqdmlal" { "add" } else { "sub" };
                r = call(b, Callee::llvm1(if op == "add" { "sadd.sat" } else { "ssub.sat" }, wt), wt, &[d, r]);
            }
            r
        }
        _ => panic!("3diff {mn}"),
    };
    vwrite(b, i.f(Fld::Rd), r);
}

/// REV16/32/64: the lanes of x reversed in groups of group lanes
fn rev(b: &mut Builder, x: ValueId, group: u32) -> ValueId {
    let n = b.ty(x).lanes().0;
    let m: Vec<i32> =
        (0..n).step_by(group as usize).flat_map(|g| (0..group).map(move |k| (g + group - 1 - k) as i32)).collect();
    b.shuffle(x, x, &m)
}

fn t_simd_2misc(b: &mut Builder, i: &Insn, _pc: u64) {
    let sc = scalar(i);
    let q = if sc { 1 } else { i.f(Fld::Q) };
    let (mn, kind, size) = info::misc2_info(q, i.f(Fld::U), i.f(Fld::size), i.f(Fld::opcode), sc).unwrap();
    let (rn, rd) = (i.f(Fld::Rn), i.f(Fld::Rd));
    let v4f16 = vty(4, Ty::F16);
    match kind {
        "f" | "fz" => {
            let t = operand_types(i, size, true, sc);
            let x = vread(b, rn, t);
            let r = if mn == "scvtf" || mn == "ucvtf" {
                let x = vread(b, rn, ity(t));
                b.cast(if mn == "scvtf" { CastOp::Sitofp } else { CastOp::Uitofp }, x, t)
            } else {
                fp2(b, mn, x)
            };
            vwrite(b, rd, r);
        }
        "fn" => {
            let x = vread(b, rn, if size == 3 { vty(2, Ty::F64) } else { vty(4, Ty::F32) });
            let r = b.cast(CastOp::Fptrunc, x, if size == 3 { vty(2, Ty::F32) } else { v4f16 });
            put_half(b, rd, r, q);
        }
        "fl" => {
            let h = half(b, rn, 0, q);
            let x = b.bitcast(h, if size == 3 { vty(2, Ty::F32) } else { v4f16 });
            let r = b.cast(CastOp::Fpext, x, if size == 3 { vty(2, Ty::F64) } else { vty(4, Ty::F32) });
            vwrite(b, rd, r);
        }
        "fx" => {
            if sc {
                let x = vread(b, rn, Ty::F64);
                let r = round_odd(b, x);
                vwrite(b, rd, r);
            } else {
                let x = vread(b, rn, vty(2, Ty::F64));
                let r = round_odd(b, x);
                put_half(b, rd, r, q);
            }
        }
        "n" => {
            let (x, nt) = if sc {
                (vread(b, rn, lane_t(size + 1, false)), lane_t(size, false))
            } else {
                (vread(b, rn, VT(size + 1, 1, false)), VT(size, 0, false))
            };
            let r = if mn == "xtn" { trunc(b, x, nt) } else { sat_narrow(b, x, nt, mn != "uqxtn", mn == "sqxtn") };
            if sc {
                vwrite(b, rd, r);
            } else {
                put_half(b, rd, r, q);
            }
        }
        "s" => {
            let wt = VT(size + 1, 1, false);
            let h = half(b, rn, size, q);
            let z = b.op(Op::Cast(CastOp::Zext), wt, &[h]);
            let k = K(b, 8i128 << size, wt);
            let r = bin(b, BinOp::Shl, z, k);
            vwrite(b, rd, r);
        }
        "p" => {
            let t = VT(size, q, false);
            let n = t.lanes().0;
            let wt = vty(n / 2, lane_t(size + 1, false));
            let x = vread(b, rn, t);
            let signed = mn.starts_with('s');
            let m: Vec<i32> = (0..n as i32).step_by(2).collect();
            let s = b.shuffle(x, x, &m);
            let ev = ext(b, s, wt, signed);
            let m: Vec<i32> = (1..n as i32).step_by(2).collect();
            let s = b.shuffle(x, x, &m);
            let od = ext(b, s, wt, signed);
            let mut r = bin(b, BinOp::Add, ev, od);
            if mn.contains("adal") {
                let d = vread(b, rd, wt);
                r = bin(b, BinOp::Add, d, r);
            }
            vwrite(b, rd, r);
        }
        _ => {
            let t = if mn == "rbit" { VT(0, q, false) } else if sc { lane_t(size, false) } else { VT(size, q, false) };
            let x = vread(b, rn, t);
            let w = lw(t);
            let r = match mn {
                "rev64" | "rev32" | "rev16" => {
                    let g: u32 = mn[3..].parse().unwrap();
                    rev(b, x, g / w)
                }
                "cnt" => intr(b, "ctpop", t, &[x]),
                "clz" => {
                    let c = b.c(0, I1);
                    call(b, Callee::llvm1("ctlz", t), t, &[x, c])
                }
                "cls" => {
                    let k = K(b, (w - 1) as i128, t);
                    let s = bin(b, BinOp::Ashr, x, k);
                    let y = bin(b, BinOp::Xor, x, s);
                    let c = b.c(0, I1);
                    let z = call(b, Callee::llvm1("ctlz", t), t, &[y, c]);
                    let k = K(b, 1, t);
                    bin(b, BinOp::Sub, z, k)
                }
                "not" => b.not(x),
                "rbit" => intr(b, "bitreverse", t, &[x]),
                "abs" => {
                    let c = b.c(0, I1);
                    call(b, Callee::llvm1("abs", t), t, &[x, c])
                }
                "neg" => {
                    let k = K(b, 0, t);
                    bin(b, BinOp::Sub, k, x)
                }
                "suqadd" | "usqadd" => {
                    let d = vread(b, rd, t);
                    let signed_d = mn == "suqadd";
                    lanewise(b, d, x, |b, u, v| {
                        let s = d_add(b, u, v, signed_d);
                        sat_lane(b, s, w, signed_d)
                    })
                }
                "sqabs" | "sqneg" => {
                    let mn_ = K(b, 1i128 << (w - 1), t);
                    let a = if mn == "sqabs" {
                        let c = b.c(0, I1);
                        call(b, Callee::llvm1("abs", t), t, &[x, c])
                    } else {
                        let k = K(b, 0, t);
                        bin(b, BinOp::Sub, k, x)
                    };
                    let e = icmp(b, "eq", x, mn_);
                    let k = K(b, smax_of(w), t);
                    b.select(e, k, a)
                }
                _ if kind == "z" => {
                    let p = match mn {
                        "cmgt" => "sgt",
                        "cmeq" => "eq",
                        "cmlt" => "slt",
                        "cmge" => "sge",
                        _ => "sle",
                    };
                    let k = K(b, 0, t);
                    mask(b, p, x, k, false)
                }
                _ => panic!("2misc {mn}"),
            };
            vwrite(b, rd, r);
        }
    }
}

/// SUQADD/USQADD's sum, exact in i128
fn d_add(b: &mut Builder, d: ValueId, n: ValueId, signed_d: bool) -> ValueId {
    let a = ext(b, d, I128, signed_d);
    let c = ext(b, n, I128, !signed_d);
    bin(b, BinOp::Add, a, c)
}

fn sat_lane(b: &mut Builder, v: ValueId, w: u32, signed: bool) -> ValueId {
    let (lo, hi) = if signed { (-(1i128 << (w - 1)), smax_of(w)) } else { (0, (1i128 << w) - 1) };
    let k = b.c(lo, I128);
    let v = call(b, Callee::llvm1("smax", I128), I128, &[v, k]);
    let k = b.c(hi, I128);
    let v = call(b, Callee::llvm1("smin", I128), I128, &[v, k]);
    trunc(b, v, Ty::I(w as u16))
}

/// FCVTXN: double to single with round-to-odd
fn round_odd(b: &mut Builder, x: ValueId) -> ValueId {
    let xt = b.ty(x);
    let t = shape(xt, Ty::F32);
    let it = ity(t);
    let r = b.cast(CastOp::Fptrunc, x, t);
    let back = b.cast(CastOp::Fpext, r, xt);
    let inexact = fcmp(b, "one", back, x);
    let ab = fabs(b, back);
    let ax = fabs(b, x);
    let past = fcmp(b, "ogt", ab, ax);
    let bits = b.bitcast(r, it);
    let c = bin(b, BinOp::And, inexact, past);
    let k = K(b, 1, it);
    let s = bin(b, BinOp::Sub, bits, k);
    let bits = b.select(c, s, bits);
    let k = K(b, 1, it);
    let o = bin(b, BinOp::Or, bits, k);
    let bits = b.select(inexact, o, bits);
    b.bitcast(bits, t)
}

/// FRECPX
fn frecpx(b: &mut Builder, x: ValueId) -> ValueId {
    let t = b.ty(x);
    let it = ity(t);
    let w = lw(t);
    let ff: u32 = if w == 32 { 23 } else { 52 };
    let em = (1i128 << (w - 1 - ff)) - 1;
    let u = b.bitcast(x, it);
    let k = K(b, ff as i128, it);
    let s = bin(b, BinOp::Lshr, u, k);
    let k = K(b, em, it);
    let e = bin(b, BinOp::And, s, k);
    let k = K(b, 1i128 << (w - 1), it);
    let sign = bin(b, BinOp::And, u, k);
    let k = K(b, em, it);
    let xe = bin(b, BinOp::Xor, e, k);
    let k = K(b, ff as i128, it);
    let ne = bin(b, BinOp::Shl, xe, k);
    let k = K(b, 0, it);
    let z = icmp(b, "eq", e, k);
    let k = K(b, (em - 1) << ff, it);
    let r = b.select(z, k, ne);
    let nan = fcmp(b, "uno", x, x);
    let k = K(b, 1i128 << (ff - 1), it);
    let q = bin(b, BinOp::Or, u, k);
    let sr = bin(b, BinOp::Or, sign, r);
    let r = b.select(nan, q, sr);
    b.bitcast(r, t)
}

fn tree(b: &mut Builder, mut xs: Vec<ValueId>, f: impl Fn(&mut Builder, ValueId, ValueId) -> ValueId) -> ValueId {
    while xs.len() > 1 {
        xs = xs.chunks(2).map(|p| f(&mut *b, p[0], p[1])).collect();
    }
    xs[0]
}

fn t_simd_across(b: &mut Builder, i: &Insn, _pc: u64) {
    let (mn, kind, size) = info::across_info(i.f(Fld::Q), i.f(Fld::U), i.f(Fld::size), i.f(Fld::opcode)).unwrap();
    if kind == "f" {
        let x = vread(b, i.f(Fld::Rn), vty(4, Ty::F32));
        let k: &'static str = &mn[..mn.len() - 1];
        let xs: Vec<ValueId> = (0..4).map(|j| b.extract(x, j)).collect();
        let r = tree(b, xs, |b, u, w| fp3(b, k, u, w, None));
        vwrite(b, i.f(Fld::Rd), r);
        return;
    }
    let t = VT(size, i.f(Fld::Q), false);
    let x = vread(b, i.f(Fld::Rn), t);
    let r = if mn == "saddlv" || mn == "uaddlv" {
        let wt = shape(t, lane_t(size + 1, false));
        let e = ext(b, x, wt, mn.starts_with('s'));
        call(b, Callee::llvm1("vector.reduce.add", wt), lane_t(size + 1, false), &[e])
    } else {
        let op = match mn {
            "addv" => "vector.reduce.add",
            "smaxv" => "vector.reduce.smax",
            "umaxv" => "vector.reduce.umax",
            "sminv" => "vector.reduce.smin",
            _ => "vector.reduce.umin",
        };
        call(b, Callee::llvm1(op, t), lane_t(size, false), &[x])
    };
    vwrite(b, i.f(Fld::Rd), r);
}

fn t_simd_spair(b: &mut Builder, i: &Insn, _pc: u64) {
    let (mn, kind, size) = info::pair_info(i.f(Fld::U), i.f(Fld::size), i.f(Fld::opcode)).unwrap();
    let t = if kind == "f" && size == 2 {
        vty(2, Ty::F32)
    } else if kind == "f" {
        vty(2, Ty::F64)
    } else {
        vty(2, I64)
    };
    let x = vread(b, i.f(Fld::Rn), t);
    let e0 = b.extract(x, 0);
    let e1 = b.extract(x, 1);
    let r = if kind == "f" {
        let k = match mn {
            "faddp" => "fadd",
            "fmaxp" => "fmax",
            "fminp" => "fmin",
            "fmaxnmp" => "fmaxnm",
            _ => "fminnm",
        };
        fp3(b, k, e0, e1, None)
    } else {
        bin(b, BinOp::Add, e0, e1)
    };
    vwrite(b, i.f(Fld::Rd), r);
}

fn t_simd_perm(b: &mut Builder, i: &Insn, _pc: u64) {
    let t = VT(i.f(Fld::size), i.f(Fld::Q), false);
    let x = vread(b, i.f(Fld::Rn), t);
    let y = vread(b, i.f(Fld::Rm), t);
    let n = t.lanes().0 as i32;
    let o = i.f(Fld::opcode);
    let part = (o >> 2) as i32;
    let m: Vec<i32> = if o & 3 == 1 {
        (0..n).map(|k| 2 * k + part).collect()
    } else if o & 3 == 2 {
        (0..n).step_by(2).flat_map(|k| [k + part, n + k + part]).collect()
    } else {
        let base = part * n / 2;
        (0..n / 2).flat_map(|k| [base + k, n + base + k]).collect()
    };
    let r = b.shuffle(x, y, &m);
    vwrite(b, i.f(Fld::Rd), r);
}

fn t_simd_ext(b: &mut Builder, i: &Insn, _pc: u64) {
    let t = VT(0, i.f(Fld::Q), false);
    let x = vread(b, i.f(Fld::Rn), t);
    let y = vread(b, i.f(Fld::Rm), t);
    let n = t.lanes().0 as i32;
    let m: Vec<i32> = (0..n).map(|k| i.f(Fld::imm4) as i32 + k).collect();
    let r = b.shuffle(x, y, &m);
    vwrite(b, i.f(Fld::Rd), r);
}

/// TBL/TBX
fn t_simd_tbl(b: &mut Builder, i: &Insn, _pc: u64) {
    let q = i.f(Fld::Q);
    let v16 = vty(16, I8);
    let idx = if q != 0 {
        vread(b, i.f(Fld::Rm), v16)
    } else {
        let a = vread(b, i.f(Fld::Rm), vty(8, I8));
        let c = vread(b, i.f(Fld::Rm), vty(8, I8));
        let m: Vec<i32> = (0..8).chain(std::iter::repeat_n(-1, 8)).collect();
        b.shuffle(a, c, &m)
    };
    let mut r: Option<ValueId> = None;
    for k in 0..=i.f(Fld::len) {
        let tab = vread(b, (i.f(Fld::Rn) + k) % 32, v16);
        let mut ix = if k != 0 {
            let c = K(b, 16 * k as i128, v16);
            bin(b, BinOp::Sub, idx, c)
        } else {
            idx
        };
        let c = K(b, 0x70, v16);
        ix = call(b, Callee::llvm1("uadd.sat", Ty::parse("v16i8")), v16, &[ix, c]);
        let s = call(b, Callee::llvm("x86.ssse3.pshuf.b.128"), v16, &[tab, ix]);
        r = Some(match r {
            None => s,
            Some(p) => bin(b, BinOp::Or, p, s),
        });
    }
    let mut r = r.unwrap();
    if i.f(Fld::op) != 0 {
        let d = vread(b, i.f(Fld::Rd), v16);
        let c = K(b, 16 * (i.f(Fld::len) as i128 + 1), v16);
        let out = b.op(Op::Icmp("uge"), vty(16, I1), &[idx, c]);
        r = b.select(out, d, r);
    }
    if q == 0 {
        let m: Vec<i32> = (0..8).collect();
        r = b.shuffle(r, r, &m);
    }
    vwrite(b, i.f(Fld::Rd), r);
}

fn t_simd_modimm(b: &mut Builder, i: &Insn, _pc: u64) {
    let q = i.f(Fld::Q);
    let (mn, _t, _sk) = info::modimm_info(q, i.f(Fld::op), i.f(Fld::cmode), i.f(Fld::o2)).unwrap();
    let imm8 = i.f(Fld::abc) << 5 | i.f(Fld::defgh);
    let mut val = info::modimm_value(i.f(Fld::op), i.f(Fld::cmode), imm8);
    if mn == "mvni" {
        val = !val;
    }
    let v2 = vty(2, I64);
    let c = b.vconst(&if q != 0 { [val, val] } else { [val, 0] }, v2);
    if mn == "orr" || mn == "bic" {
        let d = vread(b, i.f(Fld::Rd), v2);
        let mut r = if mn == "bic" {
            let c = b.vconst(&if q != 0 { [!val, !val] } else { [!val, 0] }, v2);
            bin(b, BinOp::And, d, c)
        } else {
            bin(b, BinOp::Or, d, c)
        };
        if q == 0 {
            let z = b.zero(v2);
            let e = b.extract(r, 0);
            r = b.insert(z, e, 0);
        }
        vwrite(b, i.f(Fld::Rd), r);
        return;
    }
    vwrite(b, i.f(Fld::Rd), c);
}

/// x >> sh for 1 <= sh <= the lane width, rounded when rnd
fn rshift(b: &mut Builder, x: ValueId, sh: i128, signed: bool, rnd: bool) -> ValueId {
    let t = b.ty(x);
    let w = lw(t) as i128;
    let mut r = if signed {
        let k = K(b, sh.min(w - 1), t);
        bin(b, BinOp::Ashr, x, k)
    } else if sh < w {
        let k = K(b, sh, t);
        bin(b, BinOp::Lshr, x, k)
    } else {
        K(b, 0, t)
    };
    if rnd {
        let k = K(b, sh - 1, t);
        let s = bin(b, BinOp::Lshr, x, k);
        let k = K(b, 1, t);
        let a = bin(b, BinOp::And, s, k);
        r = bin(b, BinOp::Add, r, a);
    }
    r
}

fn t_simd_shift(b: &mut Builder, i: &Insn, _pc: u64) {
    let sc = scalar(i);
    let q = if sc { 1 } else { i.f(Fld::Q) };
    let (mn, kind, esz) = info::shift_info(q, i.f(Fld::U), i.f(Fld::immh), i.f(Fld::opcode), sc).unwrap();
    let sh = info::shift_amount(kind, esz, i.f(Fld::immh), i.f(Fld::immb)) as i128;
    let u = i.f(Fld::U);
    let (rn, rd) = (i.f(Fld::Rn), i.f(Fld::Rd));
    if kind == "f" {
        let ft = if sc { lane_t(esz, true) } else { VT(esz, q, true) };
        let r = if mn == "scvtf" || mn == "ucvtf" {
            let v = vread(b, rn, ity(ft));
            let x = b.cast(if mn == "scvtf" { CastOp::Sitofp } else { CastOp::Uitofp }, v, ft);
            let k = fK(b, 2f64.powi(-(sh as i32)), ft);
            fop(b, BinOp::Fmul, ft, x, k)
        } else {
            let v = vread(b, rn, ft);
            let k = fK(b, 2f64.powi(sh as i32), ft);
            let m = fop(b, BinOp::Fmul, ft, v, k);
            to_int(b, m, ity(ft), mn == "fcvtzs", None)
        };
        vwrite(b, rd, r);
        return;
    }
    if kind == "w" {
        let wt = VT(esz + 1, 1, false);
        let h = half(b, rn, esz, q);
        let x = ext(b, h, wt, u == 0);
        let r = if sh != 0 {
            let k = K(b, sh, wt);
            bin(b, BinOp::Shl, x, k)
        } else {
            x
        };
        vwrite(b, rd, r);
        return;
    }
    if kind == "n" {
        let wt = if sc { lane_t(esz + 1, false) } else { VT(esz + 1, 1, false) };
        let nt = if sc { lane_t(esz, false) } else { VT(esz, 0, false) };
        let mut x = vread(b, rn, wt);
        let rnd = matches!(mn, "rshrn" | "sqrshrn" | "uqrshrn" | "sqrshrun");
        let r = if mn == "shrn" || mn == "rshrn" {
            if rnd {
                let k = K(b, 1i128 << (sh - 1), wt);
                x = bin(b, BinOp::Add, x, k);
            }
            let k = K(b, sh, wt);
            let s = bin(b, BinOp::Lshr, x, k);
            trunc(b, s, nt)
        } else {
            let signed_in = mn.starts_with("sq");
            let s = rshift(b, x, sh, signed_in, rnd);
            sat_narrow(b, s, nt, signed_in, mn == "sqshrn" || mn == "sqrshrn")
        };
        if sc {
            vwrite(b, rd, r);
        } else {
            put_half(b, rd, r, q);
        }
        return;
    }
    let t = if sc { lane_t(esz, false) } else { VT(esz, q, false) };
    let x = vread(b, rn, t);
    let w = 8u32 << esz;
    let all = (1i128 << w) - 1;
    let r = if kind == "r" {
        if mn == "sri" {
            let d = vread(b, rd, t);
            let keep = !(all >> sh) & all;
            let k = K(b, keep, t);
            let a = bin(b, BinOp::And, d, k);
            let s = rshift(b, x, sh, false, false);
            bin(b, BinOp::Or, a, s)
        } else {
            let mut r = rshift(b, x, sh, u == 0, mn.as_bytes()[1] == b'r');
            if mn.ends_with("sra") {
                let d = vread(b, rd, t);
                r = bin(b, BinOp::Add, d, r);
            }
            r
        }
    } else if mn == "shl" {
        let k = K(b, sh, t);
        bin(b, BinOp::Shl, x, k)
    } else if mn == "sli" {
        let d = vread(b, rd, t);
        let k = K(b, (1i128 << sh) - 1, t);
        let a = bin(b, BinOp::And, d, k);
        let k = K(b, sh, t);
        let s = bin(b, BinOp::Shl, x, k);
        bin(b, BinOp::Or, a, s)
    } else {
        let k = K(b, sh, t);
        let s = bin(b, BinOp::Shl, x, k);
        if mn == "uqshl" {
            let k = K(b, sh, t);
            let back = bin(b, BinOp::Lshr, s, k);
            let lost = icmp(b, "ne", back, x);
            let k = K(b, all, t);
            b.select(lost, k, s)
        } else if mn == "sqshl" {
            let k = K(b, sh, t);
            let back = bin(b, BinOp::Ashr, s, k);
            let lost = icmp(b, "ne", back, x);
            let k = K(b, 0, t);
            let neg = icmp(b, "slt", x, k);
            let lo = K(b, 1i128 << (w - 1), t);
            let hi = K(b, smax_of(w), t);
            let sat = b.select(neg, lo, hi);
            b.select(lost, sat, s)
        } else {
            let k = K(b, sh, t);
            let back = bin(b, BinOp::Lshr, s, k);
            let lost = icmp(b, "ne", back, x);
            let k = K(b, 0, t);
            let neg = icmp(b, "slt", x, k);
            let k0 = K(b, 0, t);
            let ka = K(b, all, t);
            let inner = b.select(lost, ka, s);
            b.select(neg, k0, inner)
        }
    };
    vwrite(b, rd, r);
}

fn t_simd_elem(b: &mut Builder, i: &Insn, _pc: u64) {
    let sc = scalar(i);
    let q = if sc { 1 } else { i.f(Fld::Q) };
    let (mn, kind, size) = info::elem_info(q, i.f(Fld::U), i.f(Fld::size), i.f(Fld::L), i.f(Fld::opcode), sc).unwrap();
    let (rm, idx) = info::elem_index(size, i.f(Fld::L), i.f(Fld::M), i.f(Fld::Rm), i.f(Fld::H));
    let fp = kind == "f";
    let e = lane_extract(b, rm, size, idx, fp);
    let (rn, rd) = (i.f(Fld::Rn), i.f(Fld::Rd));
    if kind == "l" {
        let wt = VT(size + 1, 1, false);
        let signed = i.f(Fld::U) == 0;
        let xn = half(b, rn, size, q);
        let x = ext(b, xn, wt, signed);
        let s = splat(b, e, VT(size, 0, false));
        let y = ext(b, s, wt, signed);
        let d = vread(b, rd, wt);
        let mut pr = bin(b, BinOp::Mul, x, y);
        let r = if mn.starts_with("sqd") {
            pr = call(b, Callee::llvm1("sadd.sat", wt), wt, &[pr, pr]);
            if mn != "sqdmull" {
                let op = if mn == "sqdmlal" { "add" } else { "sub" };
                pr = call(b, Callee::llvm1(if op == "add" { "sadd.sat" } else { "ssub.sat" }, wt), wt, &[d, pr]);
            }
            pr
        } else if mn.ends_with("mull") {
            pr
        } else if mn.ends_with("mlal") {
            bin(b, BinOp::Add, d, pr)
        } else {
            bin(b, BinOp::Sub, d, pr)
        };
        vwrite(b, rd, r);
        return;
    }
    let t = if sc { lane_t(size, fp) } else { VT(size, q, fp) };
    let x = vread(b, rn, t);
    let y = if sc { e } else { splat(b, e, t) };
    let d = if matches!(mn, "mla" | "mls" | "fmla" | "fmls") { Some(vread(b, rd, t)) } else { None };
    let r = if fp { fp3(b, mn, x, y, d) } else { int3(b, mn, x, y, d) };
    vwrite(b, rd, r);
}

/// SDOT/UDOT
fn t_simd_dot(b: &mut Builder, i: &Insn, _pc: u64) {
    let q = i.f(Fld::Q);
    let signed = i.f(Fld::U) == 0;
    let n = if q != 0 { 16 } else { 8 };
    let bt = VT(0, q, false);
    let wt = vty(n, I32);
    let v = vread(b, i.f(Fld::Rn), bt);
    let x = ext(b, v, wt, signed);
    let v = vread(b, i.f(Fld::Rm), bt);
    let y = ext(b, v, wt, signed);
    let pr = bin(b, BinOp::Mul, x, y);
    let mut s: Option<ValueId> = None;
    for k in 0..4 {
        let m: Vec<i32> = (k..n as i32).step_by(4).collect();
        let part = b.shuffle(pr, pr, &m);
        s = Some(match s {
            None => part,
            Some(p) => bin(b, BinOp::Add, p, part),
        });
    }
    let d = vread(b, i.f(Fld::Rd), VT(2, q, false));
    let r = bin(b, BinOp::Add, d, s.unwrap());
    vwrite(b, i.f(Fld::Rd), r);
}

/// FNEG/FABS of half-precision lanes: the sign bit
fn t_simd_2misc16(b: &mut Builder, i: &Insn, _pc: u64) {
    let t = VT(1, i.f(Fld::Q), false);
    let x = vread(b, i.f(Fld::Rn), t);
    let mn = info::fp16_misc_info(i.f(Fld::U), i.f(Fld::a), i.f(Fld::opcode)).unwrap();
    let r = if mn == "fneg" {
        let k = K(b, 0x8000, t);
        bin(b, BinOp::Xor, x, k)
    } else {
        let k = K(b, 0x7FFF, t);
        bin(b, BinOp::And, x, k)
    };
    vwrite(b, i.f(Fld::Rd), r);
}

#[allow(dead_code)]
const _TYPES: [Ty; 2] = [I16, I32];

pub const TEMPLATES: &[(&str, crate::sem::Template)] = &[
    ("simd_copy", t_simd_copy),
    ("simd_dups", t_simd_dups),
    ("simd_3same", t_simd_3same),
    ("simd_3diff", t_simd_3diff),
    ("simd_2misc", t_simd_2misc),
    ("simd_across", t_simd_across),
    ("simd_spair", t_simd_spair),
    ("simd_perm", t_simd_perm),
    ("simd_ext", t_simd_ext),
    ("simd_tbl", t_simd_tbl),
    ("simd_modimm", t_simd_modimm),
    ("simd_shift", t_simd_shift),
    ("simd_elem", t_simd_elem),
    ("simd_dot", t_simd_dot),
    ("simd_2misc16", t_simd_2misc16),
];
