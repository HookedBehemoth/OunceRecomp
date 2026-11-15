//! IR -> textual LLVM IR (nx2/lift2/emit_llvm.py), byte for byte.
//!
//! The generic entry `void @f_<addr>(ptr %ctx)` loads the entry block's parameters from the guest context and stores
//! each register write where it happens (writeback). Context layout (rt/a64.h GuestContext): x0-x30 and SP at 8 * n,
//! N Z C V as bytes at 0x108-0x10B, V0-V31 at 0x110 + 16 * n. Guest memory: a getelementptr off A64_HOST_BASE.
//!
//! A module's functions are written straight into one byte buffer. The declarations they need are collected as typed
//! `Decl`s and written, in the Python's order (sorted by their text), when the module is finished; an in-process LLVM
//! back end would turn the same `Decl`s into function declarations.

use crate::export::Names;
use crate::ir::*;
use crate::ty::{Lane, Ty, I1, I64};
use std::collections::HashSet;
use std::sync::OnceLock;

pub const HOST_BASE: u64 = 0x100000000000;
pub const TRIPLE: &str = "x86_64-pc-linux-gnu";
/// the verification build's poison: 0xDEAF, the register, the call's guest address (build_ir.SITE_POISON)
pub const SITE_POISON: u64 = 0xDEAF;

pub fn ctx_off(v: Var) -> u32 {
    match v {
        0..=30 => 8 * v as u32,
        SP => 248,
        FN..=FV => 0x108 + (v - FN) as u32,
        _ => 0x110 + 16 * (v - V0) as u32,
    }
}

// ---- writing numbers, types and names into a byte buffer

fn dec(out: &mut Vec<u8>, v: u128) {
    let mut buf = [0u8; 40];
    let mut i = buf.len();
    if v <= u64::MAX as u128 {
        let mut v = v as u64;
        loop {
            i -= 1;
            buf[i] = b'0' + (v % 10) as u8;
            v /= 10;
            if v == 0 {
                break;
            }
        }
    } else {
        let mut v = v;
        loop {
            i -= 1;
            buf[i] = b'0' + (v % 10) as u8;
            v /= 10;
            if v == 0 {
                break;
            }
        }
    }
    out.extend_from_slice(&buf[i..]);
}

fn sdec(out: &mut Vec<u8>, v: i128) {
    if v < 0 {
        out.push(b'-');
    }
    dec(out, v.unsigned_abs());
}

/// `%08x`
fn hex8(out: &mut Vec<u8>, v: u64) {
    let n = ((64 - v.leading_zeros()).div_ceil(4)).max(8);
    for k in (0..n).rev() {
        out.push(b"0123456789abcdef"[(v >> (4 * k) & 15) as usize]);
    }
}

/// emit_llvm.int_text: the signed spelling of x at width w
fn int_text(out: &mut Vec<u8>, x: u128, w: u32) {
    if w < 128 && w > 0 && x >> (w - 1) & 1 == 1 {
        sdec(out, x as i128 - (1i128 << w));
    } else if w == 128 && x >> 127 == 1 {
        sdec(out, x as i128);
    } else {
        dec(out, x);
    }
}

fn lane_ll(l: Lane) -> &'static str {
    match l {
        Lane::I(1) => "i1",
        Lane::I(8) => "i8",
        Lane::I(16) => "i16",
        Lane::I(32) => "i32",
        Lane::I(64) => "i64",
        Lane::F16 => "half",
        Lane::F32 => "float",
        Lane::F64 => "double",
        Lane::I(w) => panic!("lane i{w}"),
    }
}

/// LLVM's spelling of a type (emit_llvm.lltype)
fn ll(out: &mut Vec<u8>, t: Ty) {
    match t {
        Ty::Void => out.extend_from_slice(b"void"),
        Ty::I(w) => {
            out.push(b'i');
            dec(out, w as u128);
        }
        Ty::F16 => out.extend_from_slice(b"half"),
        Ty::F32 => out.extend_from_slice(b"float"),
        Ty::F64 => out.extend_from_slice(b"double"),
        Ty::V128 => out.extend_from_slice(b"<2 x i64>"),
        Ty::Vec(n, l) => {
            out.push(b'<');
            dec(out, n as u128);
            out.extend_from_slice(b" x ");
            out.extend_from_slice(lane_ll(l).as_bytes());
            out.push(b'>');
        }
    }
}

/// ir.py's name of a type ("i64", "v4f32"): it names LLVM intrinsics' overloads
fn irname(out: &mut Vec<u8>, t: Ty) {
    match t {
        Ty::Void => out.extend_from_slice(b"void"),
        Ty::I(w) => {
            out.push(b'i');
            dec(out, w as u128);
        }
        Ty::F16 => out.extend_from_slice(b"f16"),
        Ty::F32 => out.extend_from_slice(b"f32"),
        Ty::F64 => out.extend_from_slice(b"f64"),
        Ty::V128 => out.extend_from_slice(b"v128"),
        Ty::Vec(n, l) => {
            out.push(b'v');
            dec(out, n as u128);
            irname(out, l.ty());
        }
    }
}

fn var_name(out: &mut Vec<u8>, v: Var) {
    match v {
        0..=30 => {
            out.push(b'x');
            dec(out, v as u128);
        }
        SP => out.extend_from_slice(b"sp"),
        FN => out.push(b'n'),
        FZ => out.push(b'z'),
        FC => out.push(b'c'),
        FV => out.push(b'v'),
        _ => {
            out.push(b'v');
            dec(out, (v - V0) as u128);
        }
    }
}

/// a symbol's name
pub fn sym(out: &mut Vec<u8>, names: &Names, s: Sym) {
    match s {
        Sym::Entry(a) => {
            out.extend_from_slice(b"f_");
            hex8(out, a);
        }
        Sym::SjCall(a) => {
            out.extend_from_slice(b"sjcall_");
            hex8(out, a);
        }
        Sym::Name(k) => out.extend_from_slice(names.name(k).as_bytes()),
    }
}

fn callee(out: &mut Vec<u8>, names: &Names, c: Callee) {
    match c {
        Callee::Llvm(name, tys) => {
            out.extend_from_slice(b"llvm.");
            out.extend_from_slice(name.as_bytes());
            for t in tys.into_iter().filter(|&t| t != Ty::Void) {
                out.push(b'.');
                irname(out, t);
            }
        }
        Callee::Rt(name) => out.extend_from_slice(name.as_bytes()),
        Callee::Helper(k) => out.extend_from_slice(helpers()[k as usize].name.as_bytes()),
        Callee::Host(s) => sym(out, names, s),
    }
}

fn block_name(out: &mut Vec<u8>, k: BKey) {
    match k {
        BKey::Pc(p) => {
            out.push(b'b');
            hex8(out, p);
        }
        BKey::Exit(c, p) => {
            out.push(c);
            hex8(out, p);
        }
    }
}

/// a constant of any IR type: FP and FP vector constants as the bit pattern, bitcast; a vector's lanes packed in bits
fn const_text(out: &mut Vec<u8>, ty: Ty, bits: u128) {
    if ty.is_vec() {
        let (n, lane) = ty.lanes();
        let lw = lane.width();
        if bits == 0 {
            out.extend_from_slice(b"zeroinitializer");
            return;
        }
        let fp = !matches!(lane, Lane::I(_));
        if fp {
            out.extend_from_slice(b"bitcast (<");
            dec(out, n as u128);
            out.extend_from_slice(b" x i");
            dec(out, lw as u128);
            out.extend_from_slice(b"> ");
        }
        out.push(b'<');
        let m = if lw >= 128 { u128::MAX } else { (1u128 << lw) - 1 };
        for k in 0..n {
            if k > 0 {
                out.extend_from_slice(b", ");
            }
            out.push(b'i');
            dec(out, lw as u128);
            out.push(b' ');
            int_text(out, bits >> (k * lw) & m, lw);
        }
        out.push(b'>');
        if fp {
            out.extend_from_slice(b" to ");
            ll(out, ty);
            out.push(b')');
        }
        return;
    }
    if ty.is_fp() {
        out.extend_from_slice(b"bitcast (i");
        dec(out, ty.width() as u128);
        out.push(b' ');
        int_text(out, bits, ty.width());
        out.extend_from_slice(b" to ");
        ll(out, ty);
        out.push(b')');
        return;
    }
    if ty == I1 {
        out.extend_from_slice(if bits != 0 { b"true" } else { b"false" });
        return;
    }
    int_text(out, bits, ty.width());
}

// ---- declarations

/// a parameter type of a declaration
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub enum P {
    T(Ty),
    Ptr,
    Meta,
}

const MAXP: usize = 16;

/// a declaration's parameter types
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub struct Params {
    n: u8,
    p: [P; MAXP],
}

impl Params {
    fn new() -> Params {
        Params { n: 0, p: [P::Ptr; MAXP] }
    }
    fn push(&mut self, p: P) {
        assert!((self.n as usize) < MAXP, "emit: a declaration of more than {MAXP} parameters");
        self.p[self.n as usize] = p;
        self.n += 1;
    }
    fn of(f: &Function, ctx: bool, args: &[ValueId]) -> Params {
        let mut p = Params::new();
        if ctx {
            p.push(P::Ptr);
        }
        for &a in args {
            p.push(P::T(f.ty(a)));
        }
        p
    }
    fn write(&self, out: &mut Vec<u8>) {
        for (k, &p) in self.p[..self.n as usize].iter().enumerate() {
            if k > 0 {
                out.extend_from_slice(b", ");
            }
            match p {
                P::T(t) => ll(out, t),
                P::Ptr => out.extend_from_slice(b"ptr"),
                P::Meta => out.extend_from_slice(b"metadata"),
            }
        }
    }
}

/// the name of a constrained FP intrinsic: `llvm.experimental.constrained.<...>`
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub enum Strict {
    /// fadd fsub fmul fdiv of a type
    Bin(BinOp, Ty),
    Fcmp(Ty),
    /// (op, to, from)
    Cast(CastOp, Ty, Ty),
    /// an intrinsic's name and its overload types
    Llvm(&'static str, [Ty; 2]),
}

/// a declaration a module needs
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub enum Decl {
    /// `declare void @<sym>(ptr)`: a generic entry, a stub or another function taking the context
    Fn(Sym),
    /// a callee of a call, with its signature; pure: `memory(none) nounwind willreturn`
    Call { callee: Callee, ret: Ty, params: Params, pure: bool },
    /// `llvm.<k>.with.overflow.<t>`
    Ovf(&'static str, Ty),
    /// `llvm.<name>.<t>` of n operands of type t; ctlz: (t, i1)
    Intr(&'static str, Ty, u8),
    Strict { name: Strict, ret: Ty, params: Params },
    /// a runtime function's declaration as its text
    Fixed(&'static str),
    /// an internal function of helpers(): its definition
    Helper(u8),
}

const IJUMP_GUEST: &str = "declare void @a64_ijump_guest(ptr, i64, i64)";
const EXPECT: &str = "declare i1 @llvm.expect.i1(i1, i1)";
const TRAP: &str = "declare void @a64_trap(ptr, i64, i32)";
const SVC: &str = "declare void @a64_svc(ptr, i32)";

impl Decl {
    /// its text (a helper: `helper:<name>`, which sorts it after every declaration as the Python's set does)
    pub fn write(&self, out: &mut Vec<u8>, names: &Names) {
        match *self {
            Decl::Fn(s) => {
                out.extend_from_slice(b"declare void @");
                sym(out, names, s);
                out.extend_from_slice(b"(ptr)");
            }
            Decl::Call { callee: c, ret, params, pure } => {
                out.extend_from_slice(b"declare ");
                ll(out, ret);
                out.extend_from_slice(b" @");
                callee(out, names, c);
                out.push(b'(');
                params.write(out);
                out.push(b')');
                if pure {
                    out.extend_from_slice(b" memory(none) nounwind willreturn");
                }
            }
            Decl::Ovf(k, t) => {
                out.extend_from_slice(b"declare {");
                irname(out, t);
                out.extend_from_slice(b", i1} @llvm.");
                out.extend_from_slice(k.as_bytes());
                out.extend_from_slice(b".with.overflow.");
                irname(out, t);
                out.push(b'(');
                irname(out, t);
                out.extend_from_slice(b", ");
                irname(out, t);
                out.push(b')');
            }
            Decl::Intr(name, t, n) => {
                out.extend_from_slice(b"declare ");
                irname(out, t);
                out.extend_from_slice(b" @llvm.");
                out.extend_from_slice(name.as_bytes());
                out.push(b'.');
                irname(out, t);
                out.push(b'(');
                if name == "ctlz" {
                    irname(out, t);
                    out.extend_from_slice(b", i1");
                } else {
                    for k in 0..n {
                        if k > 0 {
                            out.extend_from_slice(b", ");
                        }
                        irname(out, t);
                    }
                }
                out.push(b')');
            }
            Decl::Strict { name, ret, params } => {
                out.extend_from_slice(b"declare ");
                ll(out, ret);
                out.extend_from_slice(b" @");
                strict_name(out, name);
                out.push(b'(');
                params.write(out);
                out.push(b')');
            }
            Decl::Fixed(s) => out.extend_from_slice(s.as_bytes()),
            Decl::Helper(k) => {
                out.extend_from_slice(b"helper:");
                out.extend_from_slice(helpers()[k as usize].name.as_bytes());
            }
        }
    }
    /// the symbol `declare void @<name>(...)` declares, if the module may define it
    fn void_sym(&self) -> Option<Sym> {
        match *self {
            Decl::Fn(s) => Some(s),
            Decl::Call { callee: Callee::Host(s), ret: Ty::Void, .. } => Some(s),
            _ => None,
        }
    }
}

fn strict_name(out: &mut Vec<u8>, s: Strict) {
    out.extend_from_slice(b"llvm.experimental.constrained.");
    match s {
        Strict::Bin(op, t) => {
            out.extend_from_slice(op.name().as_bytes());
            out.push(b'.');
            irname(out, t);
        }
        Strict::Fcmp(t) => {
            out.extend_from_slice(b"fcmp.");
            irname(out, t);
        }
        Strict::Cast(op, to, from) => {
            out.extend_from_slice(op.name().as_bytes());
            out.push(b'.');
            irname(out, to);
            out.push(b'.');
            irname(out, from);
        }
        Strict::Llvm(name, tys) => {
            out.extend_from_slice(name.as_bytes());
            for t in tys.into_iter().filter(|&t| t != Ty::Void) {
                out.push(b'.');
                irname(out, t);
            }
        }
    }
}

/// an internal function the IR's calls use, defined in each module that needs it, and the declarations it needs
pub struct Helper {
    pub name: &'static str,
    /// the definition, `captures(none)` already applied
    pub text: String,
    pub decls: Vec<Decl>,
}

pub const H_CRC32: u8 = 0;
pub const H_LDXP_LO: u8 = 1;
pub const H_ICALL: u8 = 2;

/// FRECPE (f "frecpe") / FRSQRTE ("frsqrte") of an f32 or f64 lane: the helper's index
pub fn estimate_helper(f: &str, lane: Ty) -> u8 {
    let k = match f {
        "frecpe" => 0,
        "frsqrte" => 2,
        _ => panic!("estimate {f}"),
    };
    3 + k
        + match lane {
            Ty::F32 => 0,
            Ty::F64 => 1,
            _ => panic!("estimate of {lane}"),
        }
}

pub fn helpers() -> &'static [Helper] {
    static H: OnceLock<Vec<Helper>> = OnceLock::new();
    H.get_or_init(|| {
        let mut h = vec![
            Helper { name: "lift2_crc32", text: CRC32.into(), decls: vec![] },
            Helper { name: "lift2_ldxp_lo", text: no_capture(LDXP_LO), decls: vec![] },
            Helper { name: "lift2_icall", text: ICALL.into(), decls: vec![Decl::Fixed(ICALL_GUEST), Decl::Fixed(EXPECT)] },
        ];
        // (in estimate_helper's order: frecpe f32 f64, frsqrte f32 f64)
        for (name, text, decls) in crate::fp_estimate::helpers() {
            let decls = decls
                .iter()
                .map(|d| {
                    assert_eq!(d, "declare i64 @llvm.ctlz.i64(i64, i1)");
                    Decl::Intr("ctlz", I64, 1)
                })
                .collect();
            h.push(Helper { name, text: no_capture(&text), decls });
        }
        assert_eq!(h[estimate_helper("frsqrte", Ty::F64) as usize].name, "lift2_frsqrte_f64");
        h
    })
}

const ICALL_GUEST: &str = "declare void @a64_icall_guest(ptr, i64, i64)";

const CRC32: &str = "define internal i32 @lift2_crc32(i32 %crc, i64 %data, i32 %nbits, i32 %poly) alwaysinline {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %loop ]
  %c = phi i32 [ %crc, %entry ], [ %c1, %loop ]
  %d = phi i64 [ %data, %entry ], [ %d1, %loop ]
  %dl = trunc i64 %d to i32
  %x = xor i32 %c, %dl
  %bit = and i32 %x, 1
  %m = sub i32 0, %bit
  %p = and i32 %m, %poly
  %cs = lshr i32 %c, 1
  %c1 = xor i32 %cs, %p
  %d1 = lshr i64 %d, 1
  %i1 = add i32 %i, 1
  %more = icmp ult i32 %i1, %nbits
  br i1 %more, label %loop, label %done
done:
  ret i32 %c1
}";
const LDXP_LO: &str = "declare void @recomp_ldxp(ptr, i64, i32, ptr, ptr)
define internal i64 @lift2_ldxp_lo(ptr %ctx, i64 %a, i32 %n) alwaysinline {
  %lo = alloca i64
  %hi = alloca i64
  call void @recomp_ldxp(ptr %ctx, i64 %a, i32 %n, ptr %lo, ptr %hi)
  %v = load i64, ptr %lo
  ret i64 %v
}";
const ICALL: &str = "define internal void @lift2_icall(ptr %ctx, i64 %t, i64 %site) alwaysinline {
  %hi = lshr i64 %t, 32
  %h = icmp ne i64 %hi, 0
  %e = call i1 @llvm.expect.i1(i1 %h, i1 true)
  br i1 %e, label %host, label %guest
host:
  %f = inttoptr i64 %t to ptr
  call void %f(ptr %ctx)
  ret void
guest:
  call void @a64_icall_guest(ptr %ctx, i64 %t, i64 %site)
  ret void
}";

/// `captures(none)` on the first parameter of every declaration (not llvm.*) whose first parameter is the context:
/// emit_llvm.CTX_PARAM's regex `^declare (\S+) @(?!llvm\.)([\w.$]+)\(ptr(?=[,)])` over each line
pub fn no_capture(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 64);
    for (k, line) in text.split('\n').enumerate() {
        if k > 0 {
            out.push('\n');
        }
        match no_capture_at(line) {
            Some(at) => {
                out.push_str(&line[..at]);
                out.push_str(" captures(none)");
                out.push_str(&line[at..]);
            }
            None => out.push_str(line),
        }
    }
    out
}

/// where " captures(none)" goes in a line the regex matches (after its "(ptr")
fn no_capture_at(line: &str) -> Option<usize> {
    let rest = line.strip_prefix("declare ")?;
    let sp = rest.find(' ')?;
    if sp == 0 {
        return None;
    }
    let name_at = rest[sp..].strip_prefix(" @")?;
    if name_at.starts_with("llvm.") {
        return None;
    }
    let n = name_at.bytes().take_while(|&c| c.is_ascii_alphanumeric() || c == b'_' || c == b'.' || c == b'$').count();
    if n == 0 {
        return None;
    }
    let after = name_at[n..].strip_prefix("(ptr")?;
    if !(after.starts_with(',') || after.starts_with(')')) {
        return None;
    }
    Some(line.len() - after.len())
}

// ---- the module

/// one module of functions: their text, and the declarations and definitions they make
pub struct Module<'n> {
    names: &'n Names,
    body: Vec<u8>,
    decls: HashSet<Decl>,
    /// the symbols the module defines (a declaration of one is left out)
    pub defined: HashSet<Sym>,
    /// the generic entry also stores N, Z, C, V (tests against Unicorn)
    pub flags_out: bool,
}

impl<'n> Module<'n> {
    /// body: the buffer the functions' text goes to (emptied; a worker passes the one of its last module);
    /// insns: the module's instructions, to size it (about 256 bytes of text per instruction)
    pub fn new(names: &'n Names, flags_out: bool, mut body: Vec<u8>, insns: usize) -> Module<'n> {
        body.clear();
        body.reserve(insns * 256);
        Module { names, body, decls: HashSet::new(), defined: HashSet::new(), flags_out }
    }
    pub fn add(&mut self, f: &Function, name: Sym) {
        emit_function(f, name, self);
        self.body.push(b'\n');
        self.defined.insert(name);
    }
    pub fn declare(&mut self, d: Decl) {
        self.decls.insert(d);
    }
    pub fn decls(&self) -> impl Iterator<Item = &Decl> {
        self.decls.iter()
    }
    /// (header, body): the triple, the declarations (sorted by their text, each once) and the helpers' definitions;
    /// then the functions, each followed by a newline
    pub fn finish(self) -> (Vec<u8>, Vec<u8>) {
        let mut texts: Vec<(Vec<u8>, &Decl)> = self
            .decls
            .iter()
            .filter(|d| d.void_sym().is_none_or(|s| !self.defined.contains(&s)))
            .map(|d| {
                let mut t = Vec::with_capacity(48);
                d.write(&mut t, self.names);
                (t, d)
            })
            .collect();
        texts.sort_unstable_by(|a, b| a.0.cmp(&b.0));
        texts.dedup_by(|a, b| a.0 == b.0);
        let mut head = Vec::with_capacity(texts.len() * 48 + 4096);
        head.extend_from_slice(b"target triple = \"");
        head.extend_from_slice(TRIPLE.as_bytes());
        head.extend_from_slice(b"\"\n");
        let mut helpers_used = Vec::new();
        for (t, d) in &texts {
            if let Decl::Helper(k) = d {
                helpers_used.push(*k);
                continue;
            }
            let line = std::str::from_utf8(t).expect("utf-8");
            match no_capture_at(line) {
                Some(at) => {
                    head.extend_from_slice(&t[..at]);
                    head.extend_from_slice(b" captures(none)");
                    head.extend_from_slice(&t[at..]);
                }
                None => head.extend_from_slice(t),
            }
            head.push(b'\n');
        }
        for k in helpers_used {
            head.extend_from_slice(helpers()[k as usize].text.as_bytes());
            head.push(b'\n');
        }
        (head, self.body)
    }
}

// ---- a function

struct Em<'a, 'n> {
    f: &'a Function,
    m: &'a mut Module<'n>,
    strict: bool,
}

impl Em<'_, '_> {
    fn s(&mut self, s: &str) -> &mut Self {
        self.m.body.extend_from_slice(s.as_bytes());
        self
    }
    /// a line's indent
    fn l(&mut self) -> &mut Self {
        self.s("  ")
    }
    fn nl(&mut self) -> &mut Self {
        self.m.body.push(b'\n');
        self
    }
    /// the end of a call's line: a strictfp function's calls are strictfp (they may change the FP environment)
    fn call_nl(&mut self) -> &mut Self {
        if self.strict {
            self.s(" strictfp");
        }
        self.nl()
    }
    fn n(&mut self, v: u64) -> &mut Self {
        dec(&mut self.m.body, v as u128);
        self
    }
    fn i(&mut self, v: i64) -> &mut Self {
        sdec(&mut self.m.body, v as i128);
        self
    }
    /// the value's name `%v<n>`
    fn v(&mut self, v: ValueId) -> &mut Self {
        self.s("%v").n(v.n() as u64)
    }
    /// an operand: a constant's text or the value's name
    fn r(&mut self, v: ValueId) -> &mut Self {
        let x = self.f.v(v);
        match x.op {
            Op::Const(c) => const_text(&mut self.m.body, x.ty, c),
            Op::VConst(c) => const_text(&mut self.m.body, x.ty, c),
            Op::CodeAddr(e) => {
                self.m.declare(Decl::Fn(Sym::Entry(e)));
                self.s("ptrtoint (ptr @f_");
                hex8(&mut self.m.body, e);
                self.s(" to i64)");
            }
            _ => {
                self.v(v);
            }
        }
        self
    }
    /// an operand with its type
    fn t(&mut self, v: ValueId) -> &mut Self {
        let t = self.f.ty(v);
        self.ll(t).s(" ").r(v)
    }
    /// operands with their types, joined by ", "
    fn ts(&mut self, vs: &[ValueId]) -> &mut Self {
        for (k, &v) in vs.iter().enumerate() {
            if k > 0 {
                self.s(", ");
            }
            self.t(v);
        }
        self
    }
    fn ll(&mut self, t: Ty) -> &mut Self {
        ll(&mut self.m.body, t);
        self
    }
    fn irn(&mut self, t: Ty) -> &mut Self {
        irname(&mut self.m.body, t);
        self
    }
    fn var(&mut self, v: Var) -> &mut Self {
        var_name(&mut self.m.body, v);
        self
    }
    fn blk(&mut self, b: BlockId) -> &mut Self {
        block_name(&mut self.m.body, self.f.b(b).key);
        self
    }
    fn sym(&mut self, s: Sym) -> &mut Self {
        sym(&mut self.m.body, self.m.names, s);
        self
    }
    fn callee(&mut self, c: Callee) -> &mut Self {
        callee(&mut self.m.body, self.m.names, c);
        self
    }
    /// `getelementptr i8, ptr inttoptr (i64 HOST_BASE to ptr), <t a>`: guest memory at a
    fn gep(&mut self, a: ValueId) -> &mut Self {
        self.s(" = getelementptr i8, ptr inttoptr (i64 ").n(HOST_BASE).s(" to ptr), ").t(a).nl()
    }
}

fn emit_function(f: &Function, name: Sym, m: &mut Module) {
    let flags_out = m.flags_out;
    let strict = f.strict_fp;
    let mut e = Em { f, m, strict };
    e.s("define void @").sym(name).s("(ptr noalias nocapture %ctx)");
    if strict {
        e.s(" strictfp");
    }
    if !f.target_features.is_empty() {
        let mut feats: Vec<&str> = f.target_features.clone();
        feats.sort_unstable();
        feats.dedup();
        e.s(" \"target-features\"=\"");
        for (k, t) in feats.iter().enumerate() {
            if k > 0 {
                e.s(",");
            }
            e.s(t);
        }
        e.s("\"");
    }
    e.s(" {\nentry:\n");
    let eb = f.entry_block();
    let ebk = f.b(eb);
    let mut used: u128 = 0;
    for &(var, _) in &ebk.params {
        used |= bit(var);
    }
    for &b in &f.order {
        for &v in &f.b(b).ops {
            match f.v(v).op {
                Op::Writeback(var) if flags_out || !is_flag(var) => used |= bit(var),
                Op::RegLoad(var) => used |= bit(var),
                Op::FlagsOut { .. } => used |= 0xFu128 << FN,
                Op::SitePoison { mask, .. } => used |= mask & REGS,
                _ => {}
            }
        }
    }
    for var in 0..NVARS as Var {
        if used & bit(var) != 0 {
            e.l().s("%cx.").var(var).s(" = getelementptr i8, ptr %ctx, i64 ").n(ctx_off(var) as u64).nl();
        }
    }
    for &(var, p) in &ebk.params {
        let ty = f.ty(p);
        if ty == I1 {
            e.l().s("%in.").var(var).s(".b = load i8, ptr %cx.").var(var).s(", align 1").nl();
            e.l().s("%in.").var(var).s(" = trunc i8 %in.").var(var).s(".b to i1").nl();
        } else {
            e.l().s("%in.").var(var).s(" = load ").ll(ty).s(", ptr %cx.").var(var).s(", align 8").nl();
        }
    }
    for &b in &f.order {
        for &v in &f.b(b).ops {
            if let Op::RowIo { .. } = f.v(v).op {
                let size = io_slots(f, v).last().map_or(0, |(off, size)| off + size);
                e.l().s("%io").n(v.n() as u64).s(" = alloca [").n(size as u64).s(" x i8], align 16").nl();
            }
        }
    }
    e.l().s("br label %").blk(eb).nl();
    for &b in &f.order {
        let blk = f.b(b);
        e.blk(b).s(":\n");
        for &(var, p) in &blk.params {
            e.l().v(p).s(" = phi ").ll(f.ty(p)).s(" ");
            let mut first = true;
            if b == eb {
                e.s("[ %in.").var(var).s(", %entry ]");
                first = false;
            }
            for &pr in &blk.preds {
                let pb = f.b(pr);
                let n = match pb.term.as_ref().unwrap() {
                    Term::Switch(_, cases, def) => cases.iter().filter(|(_, x)| *x == b).count() + (*def == b) as usize,
                    _ => 1,
                };
                let val = pb.defs[var as usize].expect("pred def");
                for _ in 0..n {
                    if !first {
                        e.s(", ");
                    }
                    first = false;
                    e.s("[ ").r(val).s(", %").blk(pr).s(" ]");
                }
            }
            e.nl();
        }
        for &v in &blk.ops {
            if let Op::Writeback(var) = f.v(v).op {
                if is_flag(var) && !flags_out {
                    continue;
                }
                let a = f.arg(v, 0);
                if f.ty(a) == I1 {
                    e.l().s("%wb").n(v.n() as u64).s(" = zext i1 ").r(a).s(" to i8").nl();
                    e.l().s("store i8 %wb").n(v.n() as u64).s(", ptr %cx.").var(var).s(", align 1").nl();
                } else {
                    e.l().s("store ").t(a).s(", ptr %cx.").var(var).s(", align 8").nl();
                }
                continue;
            }
            if strict && emit_strict(&mut e, v) {
                continue;
            }
            emit_op(&mut e, v);
        }
        match blk.term.as_ref().unwrap() {
            Term::Br(t) => {
                e.l().s("br label %").blk(*t).nl();
            }
            Term::CondBr(c, a, bb) => {
                if a == bb {
                    e.l().s("br label %").blk(*a).nl();
                } else {
                    e.l().s("br i1 ").r(*c).s(", label %").blk(*a).s(", label %").blk(*bb).nl();
                }
            }
            Term::Ret => {
                e.l().s("ret void").nl();
            }
            Term::Switch(idx, cases, def) => {
                e.l().s("switch i64 ").r(*idx).s(", label %").blk(*def).s(" [ ");
                for (k, (c, x)) in cases.iter().enumerate() {
                    if k > 0 {
                        e.s(" ");
                    }
                    e.s("i64 ").n(*c).s(", label %").blk(*x);
                }
                e.s(" ]").nl();
            }
            Term::Tail(s) => {
                e.m.declare(Decl::Fn(*s));
                e.l().s("musttail call void @").sym(*s).s("(ptr %ctx)").nl();
                e.l().s("ret void").nl();
            }
            Term::IJump(t, pc) => {
                let t = t.expect("ijump target");
                e.m.declare(Decl::Fixed(IJUMP_GUEST));
                e.m.declare(Decl::Fixed(EXPECT));
                let k = blk.key;
                e.l().s("%").blk(b).s(".hi = lshr i64 ").r(t).s(", 32").nl();
                e.l().s("%").blk(b).s(".h = icmp ne i64 %").blk(b).s(".hi, 0").nl();
                e.l().s("%").blk(b).s(".e = call i1 @llvm.expect.i1(i1 %").blk(b).s(".h, i1 true)").call_nl();
                e.l().s("br i1 %").blk(b).s(".e, label %").blk(b).s(".host, label %").blk(b).s(".guest").nl();
                block_name(&mut e.m.body, k);
                e.s(".host:\n");
                e.l().s("%").blk(b).s(".f = inttoptr i64 ").r(t).s(" to ptr").nl();
                e.l().s("musttail call void %").blk(b).s(".f(ptr %ctx)").nl();
                e.l().s("ret void").nl();
                e.blk(b).s(".guest:\n");
                e.l().s("call void @a64_ijump_guest(ptr %ctx, ").t(t).s(", i64 ").n(*pc).s(")").call_nl();
                e.l().s("ret void").nl();
            }
            Term::NoReturn(pc, w) => {
                e.m.declare(Decl::Fixed(TRAP));
                e.l().s("call void @a64_trap(ptr %ctx, i64 ").n(*pc).s(", i32 ").n(*w as u64).s(")").call_nl();
                e.l().s("unreachable").nl();
            }
            Term::Trap(pc, w) => {
                e.m.declare(Decl::Fixed(TRAP));
                e.l().s("call void @a64_trap(ptr %ctx, i64 ").n(*pc).s(", i32 ").n(*w as u64).s(")").call_nl();
                e.l().s("ret void").nl();
            }
        }
    }
    e.s("}");
}

// constrained FP intrinsics for a strictfp function's operations that depend on the FP environment
const STRICT_ROUNDED: [&str; 4] = ["fma", "sqrt", "rint", "nearbyint"];
const STRICT_EXACT: [&str; 7] = ["floor", "ceil", "trunc", "round", "roundeven", "maximum", "minimum"];
const EXC: &str = "metadata !\"fpexcept.ignore\"";
const RND: &str = "metadata !\"round.dynamic\"";

fn emit_strict(e: &mut Em, v: ValueId) -> bool {
    let f = e.f;
    let x = f.v(v);
    let a = f.args(v);
    // (the name, whether the rounding mode is passed, the comparison's predicate)
    let (name, rnd, pred) = match x.op {
        Op::Bin(op @ (BinOp::Fadd | BinOp::Fsub | BinOp::Fmul | BinOp::Fdiv)) => (Strict::Bin(op, x.ty), true, None),
        Op::Fcmp(p) => (Strict::Fcmp(f.ty(a[0])), false, Some(p)),
        Op::Cast(op @ (CastOp::Sitofp | CastOp::Uitofp | CastOp::Fptrunc | CastOp::Fpext)) => {
            (Strict::Cast(op, x.ty, f.ty(a[0])), op != CastOp::Fpext, None)
        }
        Op::Call(k) => match f.call(k).callee {
            Callee::Llvm(name, tys) => {
                let base = name.split('.').next().unwrap_or("");
                if STRICT_ROUNDED.contains(&base) {
                    (Strict::Llvm(name, tys), true, None)
                } else if STRICT_EXACT.contains(&base) {
                    (Strict::Llvm(name, tys), false, None)
                } else {
                    return false;
                }
            }
            _ => return false,
        },
        _ => return false,
    };
    let mut params = Params::of(f, false, a);
    if pred.is_some() || rnd {
        params.push(P::Meta);
    }
    params.push(P::Meta);
    e.m.declare(Decl::Strict { name, ret: x.ty, params });
    e.l().v(v).s(" = call ").ll(x.ty).s(" @");
    strict_name(&mut e.m.body, name);
    e.s("(").ts(a);
    if let Some(p) = pred {
        e.s(", metadata !\"").s(p).s("\"");
    } else if rnd {
        e.s(", ").s(RND);
    }
    e.s(", ").s(EXC).s(") strictfp").nl();
    true
}

/// [(offset, size)] of a rowio's slots: 8 bytes for an X register, 16 for a vector, at their alignment
pub fn io_slots(f: &Function, v: ValueId) -> impl Iterator<Item = (u32, u32)> + '_ {
    let nargs = match f.v(v).op {
        Op::RowIo { nargs, .. } => nargs as usize,
        _ => unreachable!(),
    };
    f.args(v)[nargs..].iter().scan(0u32, move |off, &x| {
        let size = if f.ty(x).is_vec() { 16 } else { 8 };
        let at = off.div_ceil(size) * size;
        *off = at + size;
        Some((at, size))
    })
}

fn emit_op(e: &mut Em, v: ValueId) {
    let f = e.f;
    let x = f.v(v);
    let a = f.args(v);
    let id = v.n() as u64;
    match x.op {
        Op::Bin(op) => {
            e.l().v(v).s(" = ").s(op.name()).s(" ").t(a[0]).s(", ").r(a[1]).nl();
        }
        Op::Icmp(p) | Op::Fcmp(p) => {
            let kind = if matches!(x.op, Op::Icmp(_)) { "icmp" } else { "fcmp" };
            e.l().v(v).s(" = ").s(kind).s(" ").s(p).s(" ").t(a[0]).s(", ").r(a[1]).nl();
        }
        Op::Select => {
            e.l().v(v).s(" = select ").ts(a).nl();
        }
        Op::Cast(op) => {
            e.l().v(v).s(" = ").s(op.name()).s(" ").t(a[0]).s(" to ").ll(x.ty).nl();
        }
        Op::FNeg => {
            e.l().v(v).s(" = fneg ").t(a[0]).nl();
        }
        Op::Extract(i) => {
            e.l().v(v).s(" = extractelement ").t(a[0]).s(", i32 ").n(i as u64).nl();
        }
        Op::Insert(i) => {
            e.l().v(v).s(" = insertelement ").ts(a).s(", i32 ").n(i as u64).nl();
        }
        Op::Shuffle(ref mask) => {
            e.l().v(v).s(" = shufflevector ").ts(a).s(", <").n(mask.len() as u64).s(" x i32> <");
            for (j, &k) in mask.iter().enumerate() {
                if j > 0 {
                    e.s(", ");
                }
                if k >= 0 {
                    e.s("i32 ").n(k as u64);
                } else {
                    e.s("i32 poison");
                }
            }
            e.s(">").nl();
        }
        Op::Ovf(k) => {
            let t = f.ty(a[0]);
            e.m.declare(Decl::Ovf(k, t));
            e.l().v(v).s(".s = call {").irn(t).s(", i1} @llvm.").s(k).s(".with.overflow.").irn(t).s("(").ts(a).s(")");
            e.call_nl();
            e.l().v(v).s(" = extractvalue {").irn(t).s(", i1} ").v(v).s(".s, 1").nl();
        }
        Op::Intr(name) => {
            let t = x.ty;
            e.m.declare(Decl::Intr(name, t, a.len() as u8));
            e.l().v(v).s(" = call ").irn(t).s(" @llvm.").s(name).s(".").irn(t).s("(").ts(a);
            if name == "ctlz" {
                e.s(", i1 false");
            }
            e.s(")").call_nl();
        }
        Op::Load => {
            e.l().v(v).s(".p").gep(a[0]);
            e.l().v(v).s(" = load ").ll(x.ty).s(", ptr ").v(v).s(".p, align 1").nl();
        }
        Op::Store => {
            e.l().s("%st").n(id).s(".p").gep(a[0]);
            e.l().s("store ").t(a[1]).s(", ptr %st").n(id).s(".p, align 1").nl();
        }
        Op::ALoad(order) => {
            let al = x.ty.width() / 8;
            e.l().v(v).s(".p").gep(a[0]);
            e.l().v(v).s(" = load atomic ").irn(x.ty).s(", ptr ").v(v).s(".p ").s(order).s(", align ").n(al as u64).nl();
        }
        Op::AStore(order) => {
            let al = f.ty(a[1]).width() / 8;
            e.l().s("%as").n(id).s(".p").gep(a[0]);
            e.l().s("store atomic ").t(a[1]).s(", ptr %as").n(id).s(".p ").s(order).s(", align ").n(al as u64).nl();
        }
        Op::Rmw(kind) => {
            let al = x.ty.width() / 8;
            e.l().v(v).s(".p").gep(a[0]);
            e.l().v(v).s(" = atomicrmw ").s(kind).s(" ptr ").v(v).s(".p, ").t(a[1]).s(" seq_cst, align ").n(al as u64).nl();
        }
        Op::Cas => {
            let al = x.ty.width() / 8;
            e.l().v(v).s(".p").gep(a[0]);
            e.l().v(v).s(".s = cmpxchg ptr ").v(v).s(".p, ").t(a[1]).s(", ").t(a[2]);
            e.s(" seq_cst seq_cst, align ").n(al as u64).nl();
            e.l().v(v).s(" = extractvalue {").irn(x.ty).s(", i1} ").v(v).s(".s, 0").nl();
        }
        Op::Fence(kind) => {
            e.l().s("fence ").s(kind).nl();
        }
        Op::CtxStore32(off) => {
            e.l().s("%cs").n(id).s(" = getelementptr i8, ptr %ctx, i64 ").n(off as u64).nl();
            e.l().s("store ").t(a[0]).s(", ptr %cs").n(id).s(", align 4").nl();
        }
        Op::CtxLoad(off) => {
            e.l().v(v).s(".p = getelementptr i8, ptr %ctx, i64 ").n(off as u64).nl();
            e.l().v(v).s(" = load i64, ptr ").v(v).s(".p, align 8").nl();
        }
        Op::CtxStore(off) => {
            e.l().s("%cs").n(id).s(" = getelementptr i8, ptr %ctx, i64 ").n(off as u64).nl();
            e.l().s("store ").t(a[0]).s(", ptr %cs").n(id).s(", align 8").nl();
        }
        Op::RegLoad(var) => {
            if x.ty == I1 {
                e.l().v(v).s(".b = load i8, ptr %cx.").var(var).s(", align 1").nl();
                e.l().v(v).s(" = trunc i8 ").v(v).s(".b to i1").nl();
            } else {
                e.l().v(v).s(" = load ").ll(x.ty).s(", ptr %cx.").var(var).s(", align 8").nl();
            }
        }
        Op::Call(k) => {
            let c = *f.call(k);
            if let Callee::Helper(h) = c.callee {
                e.m.declare(Decl::Helper(h));
                for &d in &helpers()[h as usize].decls {
                    e.m.declare(d);
                }
            } else {
                let pure = matches!(c.callee, Callee::Rt(n) if n.starts_with("a64_")) && !c.effects && !c.ctx;
                e.m.declare(Decl::Call { callee: c.callee, ret: x.ty, params: Params::of(f, c.ctx, a), pure });
            }
            e.l();
            if x.ty != Ty::Void {
                e.v(v).s(" = ");
            }
            e.s("call ").ll(x.ty).s(" @").callee(c.callee).s("(");
            if c.ctx {
                e.s("ptr %ctx");
                if !a.is_empty() {
                    e.s(", ");
                }
            }
            e.ts(a).s(")").call_nl();
        }
        Op::RowIo { call, nargs } => {
            let c = *f.call(call);
            let n = nargs as usize;
            for (k, (off, size)) in io_slots(f, v).enumerate() {
                let q = a[n + k];
                e.l().s("%io").n(id).s(".").n(k as u64).s(" = getelementptr i8, ptr %io").n(id).s(", i64 ").n(off as u64).nl();
                e.l().s("store ").t(q).s(", ptr %io").n(id).s(".").n(k as u64).s(", align ").n(size as u64).nl();
            }
            let mut params = Params::of(f, c.ctx, &a[..n]);
            params.push(P::Ptr);
            e.m.declare(Decl::Call { callee: c.callee, ret: x.ty, params, pure: false });
            e.l();
            if x.ty != Ty::Void {
                e.v(v).s(" = ");
            }
            e.s("call ").ll(x.ty).s(" @").callee(c.callee).s("(");
            if c.ctx {
                e.s("ptr %ctx, ");
            }
            for &q in &a[..n] {
                e.t(q).s(", ");
            }
            e.s("ptr %io").n(id).s(")").call_nl();
        }
        Op::RowOut(k) => {
            let src = a[0].n() as u64;
            let al = if x.ty.is_vec() { 16 } else { 8 };
            e.l().v(v).s(" = load ").ll(x.ty).s(", ptr %io").n(src).s(".").n(k as u64).s(", align ").n(al).nl();
        }
        Op::Gcall { sym, stop, .. } => {
            e.m.declare(Decl::Fn(sym));
            e.l().s("call void @").sym(sym).s("(ptr %ctx)");
            if stop {
                e.s(" noreturn");
            }
            e.call_nl();
        }
        Op::Icall(pc) => {
            e.m.declare(Decl::Helper(H_ICALL));
            for &d in &helpers()[H_ICALL as usize].decls {
                e.m.declare(d);
            }
            e.l().s("call void @lift2_icall(ptr %ctx, ").t(a[0]).s(", i64 ").n(pc).s(")").call_nl();
        }
        Op::Svc(imm) => {
            e.m.declare(Decl::Fixed(SVC));
            e.l().s("call void @a64_svc(ptr %ctx, i32 ").n(imm as u64).s(")").call_nl();
        }
        Op::SitePoison { mask, pc } => {
            for var in 0..NVARS as Var {
                if is_flag(var) || mask & bit(var) == 0 {
                    continue;
                }
                let pv = (SITE_POISON << 48 | (var as u64) << 40 | pc & 0xFF_FFFF_FFFF) as i64;
                if var >= V0 {
                    e.l().s("store <2 x i64> <i64 ").i(pv).s(", i64 ").i(pv).s(">, ptr %cx.").var(var).s(", align 8").nl();
                } else {
                    e.l().s("store i64 ").i(pv).s(", ptr %cx.").var(var).s(", align 8").nl();
                }
            }
        }
        Op::Barrier => {
            e.l().s("call void asm sideeffect \"\", \"~{memory}\"()").call_nl();
        }
        Op::FlagsOut { .. } => {
            // (emit_op is called without flags_out: every flagsout stores)
            for (k, nm) in ["n", "z", "c", "v"].iter().enumerate() {
                e.l().s("%fo").n(id).s(".").s(nm).s(" = zext i1 ").r(a[k]).s(" to i8").nl();
                e.l().s("store i8 %fo").n(id).s(".").s(nm).s(", ptr %cx.").s(nm).s(", align 1").nl();
            }
        }
        Op::Writeback(_) | Op::Param(_) | Op::Const(_) | Op::VConst(_) | Op::CodeAddr(_) => {
            panic!("emit: op {:?}", x.op)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn s(f: impl Fn(&mut Vec<u8>)) -> String {
        let mut o = Vec::new();
        f(&mut o);
        String::from_utf8(o).unwrap()
    }
    #[test]
    fn capture() {
        assert_eq!(no_capture("declare void @f_00001000(ptr)"), "declare void @f_00001000(ptr captures(none))");
        assert_eq!(
            no_capture("declare i64 @recomp_ldxr(ptr, i64, i32)"),
            "declare i64 @recomp_ldxr(ptr captures(none), i64, i32)"
        );
        assert_eq!(no_capture("declare i1 @llvm.expect.i1(i1, i1)"), "declare i1 @llvm.expect.i1(i1, i1)");
        assert_eq!(no_capture("declare {i64, i1} @x(ptr)"), "declare {i64, i1} @x(ptr)");
        assert_eq!(no_capture("declare void @x(ptrx)"), "declare void @x(ptrx)");
        assert_eq!(no_capture("a\ndeclare void @x(ptr)"), "a\ndeclare void @x(ptr captures(none))");
    }
    #[test]
    fn consts() {
        assert_eq!(s(|o| int_text(o, u64::MAX as u128, 64)), "-1");
        assert_eq!(s(|o| int_text(o, 1u128 << 127, 128)), (i128::MIN).to_string());
        assert_eq!(s(|o| const_text(o, Ty::F32, 0x3f800000)), "bitcast (i32 1065353216 to float)");
        assert_eq!(s(|o| const_text(o, Ty::parse("v4i32"), 1)), "<i32 1, i32 0, i32 0, i32 0>");
        assert_eq!(s(|o| const_text(o, Ty::parse("v4i32"), 0xffffffffu128 << 96)), "<i32 0, i32 0, i32 0, i32 -1>");
        assert_eq!(s(|o| const_text(o, Ty::parse("v2f32"), 0)), "zeroinitializer");
        assert_eq!(
            s(|o| const_text(o, Ty::parse("v2f32"), 0x3f800000)),
            "bitcast (<2 x i32> <i32 1065353216, i32 0> to <2 x float>)"
        );
        assert_eq!(s(|o| hex8(o, 0x1000)), "00001000");
        assert_eq!(s(|o| hex8(o, 0x1_0000_0000)), "100000000");
        assert_eq!(s(|o| dec(o, u128::MAX)), u128::MAX.to_string());
    }
}
