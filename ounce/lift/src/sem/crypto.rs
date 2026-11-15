//! The cryptographic extension's semantics (nx2/lift2/sem_crypto.py): AES through AES-NI, SHA1/SHA256 lane by lane on
//! 32-bit values. Values are created in the Python's order.

use crate::ir::{BinOp, Builder, Callee, ValueId};
use crate::sem::fp::{llcall, vread, vwrite};
use crate::ty::{Ty, I32, I64};
use ounce_decode::Insn;

fn v2i64() -> Ty {
    Ty::vec(2, I64)
}

fn x86(b: &mut Builder, name: &'static str, args: &[ValueId]) -> ValueId {
    llcall(b, Callee::llvm(name), v2i64(), args)
}

fn t_crypto_aes(b: &mut Builder, i: &Insn, _pc: u64) {
    b.f.target_features.push("+aes");
    let op = i.f("op");
    let mut x = vread(b, if op < 2 { i.f("Rd") } else { i.f("Rn") }, v2i64());
    let zero = b.zero(v2i64());
    let r = if op < 2 {
        let n = vread(b, i.f("Rn"), v2i64());
        x = b.bin(BinOp::Xor, x, n);
        x86(b, if op == 0 { "x86.aesni.aesenclast" } else { "x86.aesni.aesdeclast" }, &[x, zero])
    } else if op == 2 {
        let d = x86(b, "x86.aesni.aesdeclast", &[x, zero]);
        x86(b, "x86.aesni.aesenc", &[d, zero])
    } else {
        x86(b, "x86.aesni.aesimc", &[x])
    };
    vwrite(b, i.f("Rd"), r);
}

// ---- SHA, on lists of i32 values (lane 0 first)
fn lanes32(b: &mut Builder, n: u32) -> Vec<ValueId> {
    let v = vread(b, n, Ty::vec(4, I32));
    (0..4).map(|k| b.extract(v, k)).collect()
}

fn pack32(b: &mut Builder, xs: &[ValueId]) -> ValueId {
    let mut v = b.zero(Ty::vec(4, I32));
    for (k, &x) in xs.iter().enumerate() {
        v = b.insert(v, x, k as u32);
    }
    v
}

fn rol(b: &mut Builder, x: ValueId, n: i128) -> ValueId {
    let c = b.c(n, I32);
    b.intr("fshl", I32, &[x, x, c])
}

fn ror(b: &mut Builder, x: ValueId, n: i128) -> ValueId {
    let c = b.c(n, I32);
    b.intr("fshr", I32, &[x, x, c])
}

fn add(b: &mut Builder, xs: &[ValueId]) -> ValueId {
    let mut r = xs[0];
    for &x in &xs[1..] {
        r = b.bin(BinOp::Add, r, x);
    }
    r
}

fn xor(b: &mut Builder, xs: &[ValueId]) -> ValueId {
    let mut r = xs[0];
    for &x in &xs[1..] {
        r = b.bin(BinOp::Xor, r, x);
    }
    r
}

fn choose(b: &mut Builder, x: ValueId, y: ValueId, z: ValueId) -> ValueId {
    let a = b.bin(BinOp::Xor, y, z);
    let c = b.bin(BinOp::And, a, x);
    b.bin(BinOp::Xor, c, z)
}

fn parity(b: &mut Builder, x: ValueId, y: ValueId, z: ValueId) -> ValueId {
    xor(b, &[x, y, z])
}

fn majority(b: &mut Builder, x: ValueId, y: ValueId, z: ValueId) -> ValueId {
    let a = b.bin(BinOp::And, x, y);
    let o = b.bin(BinOp::Or, x, y);
    let c = b.bin(BinOp::And, o, z);
    b.bin(BinOp::Or, a, c)
}

type F3 = fn(&mut Builder, ValueId, ValueId, ValueId) -> ValueId;

fn sha1_rounds(b: &mut Builder, i: &Insn, f: F3) {
    let mut xs = lanes32(b, i.f("Rd"));
    let mut y = vread(b, i.f("Rn"), I32);
    let w = lanes32(b, i.f("Rm"));
    for e in 0..4 {
        let t = f(b, xs[1], xs[2], xs[3]);
        let r5 = rol(b, xs[0], 5);
        y = add(b, &[y, r5, t, w[e]]);
        xs[1] = rol(b, xs[1], 30);
        // <Y, X> = ROL(Y : X, 32)
        let top = xs[3];
        xs = vec![y, xs[0], xs[1], xs[2]];
        y = top;
    }
    let r = pack32(b, &xs);
    vwrite(b, i.f("Rd"), r);
}

fn sigma0(b: &mut Builder, x: ValueId) -> ValueId {
    let a = ror(b, x, 2);
    let c = ror(b, x, 13);
    let d = ror(b, x, 22);
    xor(b, &[a, c, d])
}

fn sigma1(b: &mut Builder, x: ValueId) -> ValueId {
    let a = ror(b, x, 6);
    let c = ror(b, x, 11);
    let d = ror(b, x, 25);
    xor(b, &[a, c, d])
}

fn sha256_hash(b: &mut Builder, x: &[ValueId], y: &[ValueId], w: &[ValueId], part1: bool) -> Vec<ValueId> {
    let (mut x, mut y) = (x.to_vec(), y.to_vec());
    for e in 0..4 {
        let chs = choose(b, y[0], y[1], y[2]);
        let maj = majority(b, x[0], x[1], x[2]);
        let s1 = sigma1(b, y[0]);
        let t = add(b, &[y[3], s1, chs, w[e]]);
        x[3] = add(b, &[t, x[3]]);
        let s0 = sigma0(b, x[0]);
        y[3] = add(b, &[t, s0, maj]);
        let nx = vec![y[3], x[0], x[1], x[2]];
        let ny = vec![x[3], y[0], y[1], y[2]];
        x = nx;
        y = ny;
    }
    if part1 { x } else { y }
}

fn t_crypto_sha3(b: &mut Builder, i: &Insn, _pc: u64) {
    let o = i.f("opcode");
    if o < 3 {
        let f: F3 = [choose as F3, parity, majority][o as usize];
        sha1_rounds(b, i, f);
    } else if o == 3 {
        let d = vread(b, i.f("Rd"), v2i64());
        let n = vread(b, i.f("Rn"), v2i64());
        let m = vread(b, i.f("Rm"), v2i64());
        let t = b.shuffle(d, n, &[1, 2]);
        let a = b.bin(BinOp::Xor, t, d);
        let r = b.bin(BinOp::Xor, a, m);
        vwrite(b, i.f("Rd"), r);
    } else if o == 4 || o == 5 {
        let d = lanes32(b, i.f("Rd"));
        let n = lanes32(b, i.f("Rn"));
        let w = lanes32(b, i.f("Rm"));
        let r = if o == 4 { sha256_hash(b, &d, &n, &w, true) } else { sha256_hash(b, &n, &d, &w, false) };
        let p = pack32(b, &r);
        vwrite(b, i.f("Rd"), p);
    } else {
        // SHA256SU1
        let d = lanes32(b, i.f("Rd"));
        let n = lanes32(b, i.f("Rn"));
        let m = lanes32(b, i.f("Rm"));
        let t0 = [n[1], n[2], n[3], m[0]];
        let t1 = [m[2], m[3]];
        let mut r: Vec<ValueId> = Vec::new();
        for e in 0..4 {
            let elt = if e < 2 { t1[e] } else { r[e - 2] };
            let a = ror(b, elt, 17);
            let c = ror(b, elt, 19);
            let k = b.c(10, I32);
            let s = b.bin(BinOp::Lshr, elt, k);
            let elt = xor(b, &[a, c, s]);
            let v = add(b, &[elt, d[e], t0[e]]);
            r.push(v);
        }
        let p = pack32(b, &r);
        vwrite(b, i.f("Rd"), p);
    }
}

fn t_crypto_sha2(b: &mut Builder, i: &Insn, _pc: u64) {
    let o = i.f("opcode");
    if o == 0 {
        let x = vread(b, i.f("Rn"), I32);
        let r = rol(b, x, 30);
        vwrite(b, i.f("Rd"), r);
    } else if o == 1 {
        // SHA1SU1
        let d = lanes32(b, i.f("Rd"));
        let n = lanes32(b, i.f("Rn"));
        let mut t = Vec::new();
        for k in 0..4 {
            t.push(if k < 3 { b.bin(BinOp::Xor, d[k], n[k + 1]) } else { d[3] });
        }
        let mut r: Vec<ValueId> = (0..4).map(|k| rol(b, t[k], 1)).collect();
        let x = rol(b, t[0], 2);
        r[3] = b.bin(BinOp::Xor, r[3], x);
        let p = pack32(b, &r);
        vwrite(b, i.f("Rd"), p);
    } else {
        // SHA256SU0
        let d = lanes32(b, i.f("Rd"));
        let n = lanes32(b, i.f("Rn"));
        let tt = [d[1], d[2], d[3], n[0]];
        let mut r = Vec::new();
        for e in 0..4 {
            let a = ror(b, tt[e], 7);
            let c = ror(b, tt[e], 18);
            let k = b.c(3, I32);
            let s = b.bin(BinOp::Lshr, tt[e], k);
            let x = xor(b, &[a, c, s]);
            r.push(add(b, &[x, d[e]]));
        }
        let p = pack32(b, &r);
        vwrite(b, i.f("Rd"), p);
    }
}

pub const TEMPLATES: &[(&str, crate::sem::Template)] = &[
    ("crypto_aes", t_crypto_aes),
    ("crypto_sha3", t_crypto_sha3),
    ("crypto_sha2", t_crypto_sha2),
];
