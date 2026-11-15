//! IR -> textual LLVM IR (nx2/lift2/emit_llvm.py), byte for byte.
//!
//! The generic entry `void @f_<addr>(ptr %ctx)` loads the entry block's parameters from the guest context and stores
//! each register write where it happens (writeback). Context layout (rt/a64.h GuestContext): x0-x30 and SP at 8 * n,
//! N Z C V as bytes at 0x108-0x10B, V0-V31 at 0x110 + 16 * n. Guest memory: a getelementptr off A64_HOST_BASE.
//!
//! Declarations are collected as their text in a sorted set (the Python's order); `Decls` is the one place they are
//! made, so an in-process LLVM back end replaces it with a registry of typed declarations.

use crate::ir::*;
use crate::ty::{Ty, I1};
use std::collections::{BTreeSet, HashMap};
use std::fmt::Write;

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

/// emit_llvm.int_text: the signed spelling of x at width w
pub fn int_text(x: u128, w: u32) -> String {
    if w < 128 && w > 0 && x >> (w - 1) & 1 == 1 {
        let v = x as i128 - (1i128 << w);
        v.to_string()
    } else if w == 128 && x >> 127 == 1 {
        (x as i128).to_string()
    } else {
        x.to_string()
    }
}

/// a constant of any IR type; FP and FP vector constants as the bit pattern, bitcast
pub fn const_text(ty: Ty, bits: &[u128]) -> String {
    if ty.is_vec() {
        let (n, lane) = ty.lanes();
        let lw = lane.width();
        if bits.iter().all(|&b| b == 0) {
            return "zeroinitializer".into();
        }
        let items: Vec<String> = bits.iter().map(|&v| format!("i{lw} {}", int_text(v, lw))).collect();
        let body = format!("<{}>", items.join(", "));
        return if matches!(lane, crate::ty::Lane::I(_)) {
            body
        } else {
            format!("bitcast (<{n} x i{lw}> {body} to {})", ty.ll())
        };
    }
    if ty.is_fp() {
        return format!("bitcast (i{} {} to {})", ty.width(), int_text(bits[0], ty.width()), ty.ll());
    }
    if ty == I1 {
        return if bits[0] != 0 { "true" } else { "false" }.into();
    }
    int_text(bits[0], ty.width())
}

/// the declarations a module needs, as their text (and helper:<name> for an internal helper)
pub type Decls = BTreeSet<String>;

/// an internal function the IR's calls use, defined in each module that needs it, and the declarations it needs
pub struct Helper {
    pub text: String,
    pub decls: Vec<String>,
}

pub fn helpers() -> &'static HashMap<&'static str, Helper> {
    use std::sync::OnceLock;
    static H: OnceLock<HashMap<&'static str, Helper>> = OnceLock::new();
    H.get_or_init(|| {
        let mut h = HashMap::new();
        h.insert("lift2_crc32", Helper { text: CRC32.into(), decls: vec![] });
        h.insert("lift2_ldxp_lo", Helper { text: LDXP_LO.into(), decls: vec![] });
        h.insert(
            "lift2_icall",
            Helper {
                text: ICALL.into(),
                decls: vec![
                    "declare void @a64_icall_guest(ptr, i64, i64)".into(),
                    "declare i1 @llvm.expect.i1(i1, i1)".into(),
                ],
            },
        );
        for (name, text, decls) in crate::fp_estimate::helpers() {
            h.insert(name, Helper { text, decls });
        }
        h
    })
}

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

/// one module of functions
pub struct Module {
    pub funcs: Vec<String>,
    pub decls: Decls,
    pub defined: BTreeSet<String>,
    /// the generic entry also stores N, Z, C, V (tests against Unicorn)
    pub flags_out: bool,
}

impl Module {
    pub fn new(flags_out: bool) -> Module {
        Module { funcs: Vec::new(), decls: Decls::new(), defined: BTreeSet::new(), flags_out }
    }
    pub fn add(&mut self, f: &Function) {
        let text = emit_function(f, &mut self.decls, self.flags_out);
        self.funcs.push(text);
        self.defined.insert(f.name.clone());
    }
    pub fn text(&self) -> String {
        let mut parts: Vec<String> = vec![format!("target triple = \"{TRIPLE}\"")];
        let mut helper_texts = Vec::new();
        for d in &self.decls {
            if let Some(name) = d.strip_prefix("declare void @") {
                let name = name.split('(').next().unwrap();
                if self.defined.contains(name) {
                    continue;
                }
            }
            if let Some(h) = d.strip_prefix("helper:") {
                helper_texts.push(no_capture(&helpers()[h].text));
            } else {
                parts.push(no_capture(d));
            }
        }
        parts.extend(helper_texts);
        parts.extend(self.funcs.iter().cloned());
        let mut s = parts.join("\n");
        s.push('\n');
        s
    }
}

struct Em<'a> {
    f: &'a Function,
    out: String,
    decls: &'a mut Decls,
}

impl<'a> Em<'a> {
    fn r(&mut self, v: ValueId) -> String {
        let x = self.f.v(v);
        match &x.op {
            Op::Const(c) => const_text(x.ty, &[*c]),
            Op::VConst(l) => {
                let b: Vec<u128> = l.iter().map(|&q| q as u128).collect();
                const_text(x.ty, &b)
            }
            Op::CodeAddr(e) => {
                self.decls.insert(format!("declare void @f_{e:08x}(ptr)"));
                format!("ptrtoint (ptr @f_{e:08x} to i64)")
            }
            _ => format!("%v{}", v.n()),
        }
    }
    fn t(&mut self, v: ValueId) -> String {
        let ty = self.f.ty(v);
        let r = self.r(v);
        format!("{} {r}", ty.ll())
    }
    fn line(&mut self, s: std::fmt::Arguments) {
        self.out.push_str("  ");
        self.out.write_fmt(s).unwrap();
        self.out.push('\n');
    }
    fn raw(&mut self, s: std::fmt::Arguments) {
        self.out.write_fmt(s).unwrap();
        self.out.push('\n');
    }
}

pub fn emit_function(f: &Function, decls: &mut Decls, flags_out: bool) -> String {
    let mut e = Em { f, out: String::with_capacity(f.nvalues() * 48), decls };
    let strict = f.strict_fp;
    let mut feats: Vec<&str> = f.target_features.clone();
    feats.sort();
    feats.dedup();
    let tf = if feats.is_empty() { String::new() } else { format!(" \"target-features\"=\"{}\"", feats.join(",")) };
    e.raw(format_args!(
        "define void @{}(ptr noalias nocapture %ctx){}{} {{",
        f.name,
        if strict { " strictfp" } else { "" },
        tf
    ));
    e.raw(format_args!("entry:"));
    let eb = f.entry_block();
    let ebk = f.b(eb);
    let mut used: u128 = 0;
    for &(var, _) in &ebk.params {
        used |= bit(var);
    }
    for &b in &f.order {
        for &v in &f.b(b).ops {
            match &f.v(v).op {
                Op::Writeback(var) if flags_out || !is_flag(*var) => used |= bit(*var),
                Op::RegLoad(var) => used |= bit(*var),
                Op::FlagsOut { .. } => used |= 0xFu128 << FN,
                Op::SitePoison { mask, .. } => used |= mask & REGS,
                _ => {}
            }
        }
    }
    for var in 0..NVARS as Var {
        if used & bit(var) != 0 {
            e.line(format_args!("%cx.{} = getelementptr i8, ptr %ctx, i64 {}", var_name(var), ctx_off(var)));
        }
    }
    for &(var, p) in &ebk.params {
        let n = var_name(var);
        let ty = f.ty(p);
        if ty == I1 {
            e.line(format_args!("%in.{n}.b = load i8, ptr %cx.{n}, align 1"));
            e.line(format_args!("%in.{n} = trunc i8 %in.{n}.b to i1"));
        } else {
            e.line(format_args!("%in.{n} = load {}, ptr %cx.{n}, align 8", ty.ll()));
        }
    }
    for &b in &f.order {
        for &v in &f.b(b).ops {
            if let Op::RowIo(_) = f.v(v).op {
                let size = io_size(f, v);
                e.line(format_args!("%io{} = alloca [{} x i8], align 16", v.n(), size));
            }
        }
    }
    e.line(format_args!("br label %{}", ebk.name()));
    for &b in &f.order {
        let blk = f.b(b);
        e.raw(format_args!("{}:", blk.name()));
        for &(var, p) in &blk.params {
            let mut inc: Vec<String> = Vec::new();
            if b == eb {
                inc.push(format!("[ %in.{}, %entry ]", var_name(var)));
            }
            for &pr in &blk.preds {
                let pb = f.b(pr);
                let n = match pb.term.as_ref().unwrap() {
                    Term::Switch(_, cases, def) => {
                        cases.iter().filter(|(_, x)| *x == b).count() + (*def == b) as usize
                    }
                    _ => 1,
                };
                let val = e.r(pb.defs[var as usize].expect("pred def"));
                let s = format!("[ {val}, %{} ]", pb.name());
                for _ in 0..n {
                    inc.push(s.clone());
                }
            }
            let pr = e.r(p);
            e.line(format_args!("{pr} = phi {} {}", f.ty(p).ll(), inc.join(", ")));
        }
        for &v in &blk.ops {
            if let Op::Writeback(var) = f.v(v).op {
                if is_flag(var) && !flags_out {
                    continue;
                }
                let a = f.arg(v, 0);
                let n = var_name(var);
                if f.ty(a) == I1 {
                    let ra = e.r(a);
                    e.line(format_args!("%wb{} = zext i1 {ra} to i8", v.n()));
                    e.line(format_args!("store i8 %wb{}, ptr %cx.{n}, align 1", v.n()));
                } else {
                    let ta = e.t(a);
                    e.line(format_args!("store {ta}, ptr %cx.{n}, align 8"));
                }
                continue;
            }
            if strict && emit_strict(&mut e, v) {
                continue;
            }
            emit_op(&mut e, v);
        }
        match blk.term.as_ref().unwrap() {
            Term::Br(t) => e.line(format_args!("br label %{}", f.b(*t).name())),
            Term::CondBr(c, a, bb) => {
                if a == bb {
                    e.line(format_args!("br label %{}", f.b(*a).name()))
                } else {
                    let rc = e.r(*c);
                    e.line(format_args!("br i1 {rc}, label %{}, label %{}", f.b(*a).name(), f.b(*bb).name()))
                }
            }
            Term::Ret => e.line(format_args!("ret void")),
            Term::Switch(idx, cases, def) => {
                let cs: Vec<String> = cases.iter().map(|(k, x)| format!("i64 {k}, label %{}", f.b(*x).name())).collect();
                let ri = e.r(*idx);
                e.line(format_args!("switch i64 {ri}, label %{} [ {} ]", f.b(*def).name(), cs.join(" ")));
            }
            Term::Tail(sym) => {
                e.decls.insert(format!("declare void @{sym}(ptr)"));
                e.line(format_args!("musttail call void @{sym}(ptr %ctx)"));
                e.line(format_args!("ret void"));
            }
            Term::IJump(t, pc) => {
                let n = blk.name().to_string();
                let t = t.expect("ijump target");
                e.decls.insert("declare void @a64_ijump_guest(ptr, i64, i64)".into());
                e.decls.insert("declare i1 @llvm.expect.i1(i1, i1)".into());
                let rt = e.r(t);
                e.line(format_args!("%{n}.hi = lshr i64 {rt}, 32"));
                e.line(format_args!("%{n}.h = icmp ne i64 %{n}.hi, 0"));
                e.line(format_args!("%{n}.e = call i1 @llvm.expect.i1(i1 %{n}.h, i1 true)"));
                e.line(format_args!("br i1 %{n}.e, label %{n}.host, label %{n}.guest"));
                e.raw(format_args!("{n}.host:"));
                e.line(format_args!("%{n}.f = inttoptr i64 {rt} to ptr"));
                e.line(format_args!("musttail call void %{n}.f(ptr %ctx)"));
                e.line(format_args!("ret void"));
                e.raw(format_args!("{n}.guest:"));
                let tt = e.t(t);
                e.line(format_args!("call void @a64_ijump_guest(ptr %ctx, {tt}, i64 {pc})"));
                e.line(format_args!("ret void"));
            }
            Term::NoReturn(pc, w) => {
                e.decls.insert("declare void @a64_trap(ptr, i64, i32)".into());
                e.line(format_args!("call void @a64_trap(ptr %ctx, i64 {pc}, i32 {w})"));
                e.line(format_args!("unreachable"));
            }
            Term::Trap(pc, w) => {
                e.decls.insert("declare void @a64_trap(ptr, i64, i32)".into());
                e.line(format_args!("call void @a64_trap(ptr %ctx, i64 {pc}, i32 {w})"));
                e.line(format_args!("ret void"));
            }
        }
    }
    e.out.push('}');
    if strict {
        // every call of a strictfp function is strictfp: it may change the FP environment
        let mut out = String::with_capacity(e.out.len() + 1024);
        for (k, l) in e.out.split('\n').enumerate() {
            if k > 0 {
                out.push('\n');
            }
            out.push_str(l);
            if is_call_line(l) && !l.ends_with("strictfp") {
                out.push_str(" strictfp");
            }
        }
        return out;
    }
    e.out
}

/// the regex `\s+(%\S+ = )?call ` at a line's start
fn is_call_line(l: &str) -> bool {
    let t = l.trim_start();
    if t.len() == l.len() {
        return false;
    }
    if t.starts_with("call ") {
        return true;
    }
    if let Some(r) = t.strip_prefix('%') {
        if let Some(k) = r.find(|c: char| c.is_whitespace()) {
            return k > 0 && r[k..].starts_with(" = call ");
        }
    }
    false
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
    let (name, args, tys): (String, Vec<String>, Vec<String>);
    match &x.op {
        Op::Bin(op @ (BinOp::Fadd | BinOp::Fsub | BinOp::Fmul | BinOp::Fdiv)) => {
            name = format!("llvm.experimental.constrained.{}.{}", op.name(), x.ty);
            let mut ar: Vec<String> = a.iter().map(|&q| e.t(q)).collect();
            ar.extend([RND.into(), EXC.into()]);
            args = ar;
            let mut ts: Vec<String> = a.iter().map(|&q| f.ty(q).ll().to_string()).collect();
            ts.extend(["metadata".into(), "metadata".into()]);
            tys = ts;
        }
        Op::Fcmp(pred) => {
            name = format!("llvm.experimental.constrained.fcmp.{}", f.ty(a[0]));
            let mut ar: Vec<String> = a.iter().map(|&q| e.t(q)).collect();
            ar.extend([format!("metadata !\"{pred}\""), EXC.into()]);
            args = ar;
            let mut ts: Vec<String> = a.iter().map(|&q| f.ty(q).ll().to_string()).collect();
            ts.extend(["metadata".into(), "metadata".into()]);
            tys = ts;
        }
        Op::Cast(op @ (CastOp::Sitofp | CastOp::Uitofp | CastOp::Fptrunc | CastOp::Fpext)) => {
            name = format!("llvm.experimental.constrained.{}.{}.{}", op.name(), x.ty, f.ty(a[0]));
            let mut ar = vec![e.t(a[0])];
            if *op != CastOp::Fpext {
                ar.push(RND.into());
            }
            ar.push(EXC.into());
            let n = ar.len();
            args = ar;
            let mut ts = vec![f.ty(a[0]).ll().to_string()];
            ts.extend(std::iter::repeat("metadata".to_string()).take(n - 1));
            tys = ts;
        }
        Op::Call(c) if c.func.starts_with("llvm.") && {
            let k = c.func.split('.').nth(1).unwrap_or("");
            STRICT_ROUNDED.contains(&k) || STRICT_EXACT.contains(&k)
        } =>
        {
            let (base, sfx) = c.func[5..].split_once('.').unwrap_or((&c.func[5..], ""));
            name = format!("llvm.experimental.constrained.{base}.{sfx}");
            let mut ar: Vec<String> = a.iter().map(|&q| e.t(q)).collect();
            if STRICT_ROUNDED.contains(&base) {
                ar.push(RND.into());
            }
            ar.push(EXC.into());
            let extra = ar.len() - a.len();
            args = ar;
            let mut ts: Vec<String> = a.iter().map(|&q| f.ty(q).ll().to_string()).collect();
            ts.extend(std::iter::repeat("metadata".to_string()).take(extra));
            tys = ts;
        }
        _ => return false,
    }
    e.decls.insert(format!("declare {} @{name}({})", x.ty.ll(), tys.join(", ")));
    let r = e.r(v);
    e.line(format_args!("{r} = call {} @{name}({}) strictfp", x.ty.ll(), args.join(", ")));
    true
}

/// [(offset, size)] of a rowio's slots: 8 bytes for an X register, 16 for a vector, at their alignment
pub fn io_slots(f: &Function, v: ValueId) -> Vec<(u32, u32)> {
    let nargs = match &f.v(v).op {
        Op::RowIo(r) => r.nargs as usize,
        _ => unreachable!(),
    };
    let mut out = Vec::new();
    let mut off = 0;
    for &x in &f.args(v)[nargs..] {
        let size = if f.ty(x).is_vec() { 16 } else { 8 };
        off = (off + size - 1) / size * size;
        out.push((off, size));
        off += size;
    }
    out
}

fn io_size(f: &Function, v: ValueId) -> u32 {
    let s = io_slots(f, v);
    let (off, size) = s.last().unwrap();
    off + size
}

fn emit_op(e: &mut Em, v: ValueId) {
    let f = e.f;
    let x = f.v(v);
    let a = f.args(v);
    let r = e.r(v);
    let id = v.n();
    match &x.op {
        Op::Bin(op) => {
            let t0 = e.t(a[0]);
            let r1 = e.r(a[1]);
            e.line(format_args!("{r} = {} {t0}, {r1}", op.name()));
        }
        Op::Icmp(p) | Op::Fcmp(p) => {
            let kind = if matches!(x.op, Op::Icmp(_)) { "icmp" } else { "fcmp" };
            let t0 = e.t(a[0]);
            let r1 = e.r(a[1]);
            e.line(format_args!("{r} = {kind} {p} {t0}, {r1}"));
        }
        Op::Select => {
            let (t0, t1, t2) = (e.t(a[0]), e.t(a[1]), e.t(a[2]));
            e.line(format_args!("{r} = select {t0}, {t1}, {t2}"));
        }
        Op::Cast(op) => {
            let t0 = e.t(a[0]);
            e.line(format_args!("{r} = {} {t0} to {}", op.name(), x.ty.ll()));
        }
        Op::FNeg => {
            let t0 = e.t(a[0]);
            e.line(format_args!("{r} = fneg {t0}"));
        }
        Op::Extract(i) => {
            let t0 = e.t(a[0]);
            e.line(format_args!("{r} = extractelement {t0}, i32 {i}"));
        }
        Op::Insert(i) => {
            let (t0, t1) = (e.t(a[0]), e.t(a[1]));
            e.line(format_args!("{r} = insertelement {t0}, {t1}, i32 {i}"));
        }
        Op::Shuffle(mask) => {
            let m: Vec<String> =
                mask.iter().map(|&k| if k >= 0 { format!("i32 {k}") } else { "i32 poison".into() }).collect();
            let (t0, t1) = (e.t(a[0]), e.t(a[1]));
            e.line(format_args!("{r} = shufflevector {t0}, {t1}, <{} x i32> <{}>", mask.len(), m.join(", ")));
        }
        Op::Ovf(k) => {
            let t = f.ty(a[0]);
            let name = format!("llvm.{k}.with.overflow.{t}");
            e.decls.insert(format!("declare {{{t}, i1}} @{name}({t}, {t})"));
            let (t0, t1) = (e.t(a[0]), e.t(a[1]));
            e.line(format_args!("{r}.s = call {{{t}, i1}} @{name}({t0}, {t1})"));
            e.line(format_args!("{r} = extractvalue {{{t}, i1}} {r}.s, 1"));
        }
        Op::Intr(name) => {
            let t = x.ty;
            if *name == "ctlz" {
                e.decls.insert(format!("declare {t} @llvm.ctlz.{t}({t}, i1)"));
                let t0 = e.t(a[0]);
                e.line(format_args!("{r} = call {t} @llvm.ctlz.{t}({t0}, i1 false)"));
            } else {
                let ps = vec![t.to_string(); a.len()].join(", ");
                e.decls.insert(format!("declare {t} @llvm.{name}.{t}({ps})"));
                let ts: Vec<String> = a.iter().map(|&q| e.t(q)).collect();
                e.line(format_args!("{r} = call {t} @llvm.{name}.{t}({})", ts.join(", ")));
            }
        }
        Op::Load => {
            let t0 = e.t(a[0]);
            e.line(format_args!("{r}.p = getelementptr i8, ptr inttoptr (i64 {HOST_BASE} to ptr), {t0}"));
            e.line(format_args!("{r} = load {}, ptr {r}.p, align 1", x.ty.ll()));
        }
        Op::Store => {
            let (t0, t1) = (e.t(a[0]), e.t(a[1]));
            e.line(format_args!("%st{id}.p = getelementptr i8, ptr inttoptr (i64 {HOST_BASE} to ptr), {t0}"));
            e.line(format_args!("store {t1}, ptr %st{id}.p, align 1"));
        }
        Op::ALoad(order) => {
            let al = x.ty.width() / 8;
            let t0 = e.t(a[0]);
            e.line(format_args!("{r}.p = getelementptr i8, ptr inttoptr (i64 {HOST_BASE} to ptr), {t0}"));
            e.line(format_args!("{r} = load atomic {}, ptr {r}.p {order}, align {al}", x.ty));
        }
        Op::AStore(order) => {
            let al = f.ty(a[1]).width() / 8;
            let (t0, t1) = (e.t(a[0]), e.t(a[1]));
            e.line(format_args!("%as{id}.p = getelementptr i8, ptr inttoptr (i64 {HOST_BASE} to ptr), {t0}"));
            e.line(format_args!("store atomic {t1}, ptr %as{id}.p {order}, align {al}"));
        }
        Op::Rmw(kind) => {
            let al = x.ty.width() / 8;
            let (t0, t1) = (e.t(a[0]), e.t(a[1]));
            e.line(format_args!("{r}.p = getelementptr i8, ptr inttoptr (i64 {HOST_BASE} to ptr), {t0}"));
            e.line(format_args!("{r} = atomicrmw {kind} ptr {r}.p, {t1} seq_cst, align {al}"));
        }
        Op::Cas => {
            let al = x.ty.width() / 8;
            let (t0, t1, t2) = (e.t(a[0]), e.t(a[1]), e.t(a[2]));
            e.line(format_args!("{r}.p = getelementptr i8, ptr inttoptr (i64 {HOST_BASE} to ptr), {t0}"));
            e.line(format_args!("{r}.s = cmpxchg ptr {r}.p, {t1}, {t2} seq_cst seq_cst, align {al}"));
            e.line(format_args!("{r} = extractvalue {{{}, i1}} {r}.s, 0", x.ty));
        }
        Op::Fence(kind) => e.line(format_args!("fence {kind}")),
        Op::CtxStore32(off) => {
            let t0 = e.t(a[0]);
            e.line(format_args!("%cs{id} = getelementptr i8, ptr %ctx, i64 {off}"));
            e.line(format_args!("store {t0}, ptr %cs{id}, align 4"));
        }
        Op::CtxLoad(off) => {
            e.line(format_args!("{r}.p = getelementptr i8, ptr %ctx, i64 {off}"));
            e.line(format_args!("{r} = load i64, ptr {r}.p, align 8"));
        }
        Op::CtxStore(off) => {
            let t0 = e.t(a[0]);
            e.line(format_args!("%cs{id} = getelementptr i8, ptr %ctx, i64 {off}"));
            e.line(format_args!("store {t0}, ptr %cs{id}, align 8"));
        }
        Op::RegLoad(var) => {
            let n = var_name(*var);
            if x.ty == I1 {
                e.line(format_args!("{r}.b = load i8, ptr %cx.{n}, align 1"));
                e.line(format_args!("{r} = trunc i8 {r}.b to i1"));
            } else {
                e.line(format_args!("{r} = load {}, ptr %cx.{n}, align 8", x.ty.ll()));
            }
        }
        Op::Call(c) => {
            let mut args: Vec<String> = if c.ctx { vec!["ptr %ctx".into()] } else { vec![] };
            args.extend(a.iter().map(|&q| e.t(q)));
            let rt = if x.ty == Ty::Void { "void".to_string() } else { x.ty.ll().to_string() };
            if c.helper {
                e.decls.insert(format!("helper:{}", c.func));
                if let Some(h) = helpers().get(c.func.as_str()) {
                    e.decls.extend(h.decls.iter().cloned());
                }
            } else {
                let mut tys: Vec<String> = if c.ctx { vec!["ptr".into()] } else { vec![] };
                tys.extend(a.iter().map(|&q| f.ty(q).ll().to_string()));
                let pure = c.func.starts_with("a64_") && !c.effects && !c.ctx;
                e.decls.insert(format!(
                    "declare {rt} @{}({}){}",
                    c.func,
                    tys.join(", "),
                    if pure { " memory(none) nounwind willreturn" } else { "" }
                ));
            }
            let lhs = if x.ty == Ty::Void { String::new() } else { format!("{r} = ") };
            e.line(format_args!("{lhs}call {rt} @{}({})", c.func, args.join(", ")));
        }
        Op::RowIo(row) => {
            let n = row.nargs as usize;
            for (k, (off, _)) in io_slots(f, v).into_iter().enumerate() {
                let q = a[n + k];
                let tq = e.t(q);
                e.line(format_args!("%io{id}.{k} = getelementptr i8, ptr %io{id}, i64 {off}"));
                e.line(format_args!("store {tq}, ptr %io{id}.{k}, align {}", if f.ty(q).is_vec() { 16 } else { 8 }));
            }
            let mut args: Vec<String> = if row.ctx { vec!["ptr %ctx".into()] } else { vec![] };
            args.extend(a[..n].iter().map(|&q| e.t(q)));
            args.push(format!("ptr %io{id}"));
            let rt = if x.ty == Ty::Void { "void".to_string() } else { x.ty.ll().to_string() };
            let mut tys: Vec<String> = if row.ctx { vec!["ptr".into()] } else { vec![] };
            tys.extend(a[..n].iter().map(|&q| f.ty(q).ll().to_string()));
            tys.push("ptr".into());
            e.decls.insert(format!("declare {rt} @{}({})", row.func, tys.join(", ")));
            let lhs = if x.ty == Ty::Void { String::new() } else { format!("{r} = ") };
            e.line(format_args!("{lhs}call {rt} @{}({})", row.func, args.join(", ")));
        }
        Op::RowOut(k) => {
            let src = a[0].n();
            e.line(format_args!(
                "{r} = load {}, ptr %io{src}.{k}, align {}",
                x.ty.ll(),
                if x.ty.is_vec() { 16 } else { 8 }
            ));
        }
        Op::Gcall { sym, stop, .. } => {
            e.decls.insert(format!("declare void @{sym}(ptr)"));
            e.line(format_args!("call void @{sym}(ptr %ctx){}", if *stop { " noreturn" } else { "" }));
        }
        Op::Icall(pc) => {
            e.decls.insert("helper:lift2_icall".into());
            e.decls.extend(helpers()["lift2_icall"].decls.iter().cloned());
            let t0 = e.t(a[0]);
            e.line(format_args!("call void @lift2_icall(ptr %ctx, {t0}, i64 {pc})"));
        }
        Op::Svc(imm) => {
            e.decls.insert("declare void @a64_svc(ptr, i32)".into());
            e.line(format_args!("call void @a64_svc(ptr %ctx, i32 {imm})"));
        }
        Op::SitePoison { mask, pc } => {
            for var in 0..NVARS as Var {
                if is_flag(var) || mask & bit(var) == 0 {
                    continue;
                }
                let pv = ((SITE_POISON << 48 | (var as u64) << 40 | pc & 0xFF_FFFF_FFFF) as i64).to_string();
                let n = var_name(var);
                if var >= V0 {
                    e.line(format_args!("store <2 x i64> <i64 {pv}, i64 {pv}>, ptr %cx.{n}, align 8"));
                } else {
                    e.line(format_args!("store i64 {pv}, ptr %cx.{n}, align 8"));
                }
            }
        }
        Op::Barrier => e.line(format_args!("call void asm sideeffect \"\", \"~{{memory}}\"()")),
        Op::FlagsOut { .. } => {
            // (emit_op is called without flags_out: every flagsout stores)
            for (k, nm) in ["n", "z", "c", "v"].iter().enumerate() {
                let rk = e.r(a[k]);
                e.line(format_args!("%fo{id}.{nm} = zext i1 {rk} to i8"));
                e.line(format_args!("store i8 %fo{id}.{nm}, ptr %cx.{nm}, align 1"));
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
        assert_eq!(int_text(u64::MAX as u128, 64), "-1");
        assert_eq!(const_text(Ty::F32, &[0x3f800000]), "bitcast (i32 1065353216 to float)");
        assert_eq!(const_text(Ty::parse("v4i32"), &[1, 0, 0, 0]), "<i32 1, i32 0, i32 0, i32 0>");
        assert_eq!(const_text(Ty::parse("v2f32"), &[0, 0]), "zeroinitializer");
        assert!(is_call_line("  %v1 = call i64 @x()"));
        assert!(is_call_line("  call void @x()"));
        assert!(!is_call_line("call void @x()"));
    }
}
