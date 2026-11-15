//! The IR (nx2/lift2/ir.py): typed SSA values in blocks, and the builder that keeps guest registers as SSA values.
//!
//! Values and blocks live in per-function arenas and are named by index. A value's index is its number in the
//! emitted text (`%v<n>`), so values are created in exactly the order ir.py creates them (constants and block
//! parameters included). A block reads a guest register with `get`; the first read of a register the block has not
//! written becomes a block parameter, and `finish` gives every predecessor the value it passes (Braun et al. without
//! the trivial-phi removal).

use crate::ty::{Ty, I1, I64};
use std::collections::HashMap;
use std::hash::{BuildHasherDefault, Hasher};
use std::num::NonZeroU32;

/// a guest register variable: x0-x30 (0-30), sp (31), the flags n z c v (32-35), v0-v31 (36-67); its index is its
/// summary bit
pub type Var = u8;
pub const NVARS: usize = 68;
pub const SP: Var = 31;
pub const FN: Var = 32;
pub const FZ: Var = 33;
pub const FC: Var = 34;
pub const FV: Var = 35;
pub const V0: Var = 36;
pub const FLAGS: [Var; 4] = [FN, FZ, FC, FV];

pub fn x(n: u32) -> Var {
    debug_assert!(n < 31);
    n as Var
}
pub fn vreg(n: u32) -> Var {
    V0 + n as Var
}
pub fn var_name(v: Var) -> String {
    match v {
        0..=30 => format!("x{v}"),
        SP => "sp".into(),
        FN => "n".into(),
        FZ => "z".into(),
        FC => "c".into(),
        FV => "v".into(),
        _ => format!("v{}", v - V0),
    }
}
pub fn var_parse(s: &str) -> Option<Var> {
    Some(match s {
        "sp" => SP,
        "n" => FN,
        "z" => FZ,
        "c" => FC,
        "v" => FV,
        _ if s.starts_with('x') => s[1..].parse::<u8>().ok().filter(|&n| n < 31)?,
        _ if s.starts_with('v') => V0 + s[1..].parse::<u8>().ok().filter(|&n| n < 32)?,
        _ => return None,
    })
}
pub fn var_type(v: Var) -> Ty {
    if is_flag(v) {
        I1
    } else if v >= V0 {
        Ty::V128
    } else {
        I64
    }
}
pub fn is_flag(v: Var) -> bool {
    (FN..=FV).contains(&v)
}
pub const fn bit(v: Var) -> u128 {
    1u128 << v
}
/// every variable's bit (summary.ALL)
pub const ALL: u128 = (1u128 << NVARS) - 1;
/// the registers without the flags (build_ir._REGS)
pub const REGS: u128 = ALL & !(0xFu128 << FN);

#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug, PartialOrd, Ord)]
pub struct ValueId(NonZeroU32);
impl ValueId {
    /// the value's number (`%v<n>`)
    pub fn n(self) -> u32 {
        self.0.get()
    }
}

#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub struct BlockId(pub u32);

#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub enum BinOp {
    Add, Sub, Mul, And, Or, Xor, Shl, Lshr, Ashr, Udiv, Sdiv, Fadd, Fsub, Fmul, Fdiv,
}
impl BinOp {
    pub fn name(self) -> &'static str {
        use BinOp::*;
        match self {
            Add => "add", Sub => "sub", Mul => "mul", And => "and", Or => "or", Xor => "xor", Shl => "shl",
            Lshr => "lshr", Ashr => "ashr", Udiv => "udiv", Sdiv => "sdiv", Fadd => "fadd", Fsub => "fsub",
            Fmul => "fmul", Fdiv => "fdiv",
        }
    }
}

#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub enum CastOp {
    Zext, Sext, Trunc, Bitcast, Sitofp, Uitofp, Fptosi, Fptoui, Fpext, Fptrunc,
}
impl CastOp {
    pub fn name(self) -> &'static str {
        use CastOp::*;
        match self {
            Zext => "zext", Sext => "sext", Trunc => "trunc", Bitcast => "bitcast", Sitofp => "sitofp",
            Uitofp => "uitofp", Fptosi => "fptosi", Fptoui => "fptoui", Fpext => "fpext", Fptrunc => "fptrunc",
        }
    }
}

/// a symbol the IR names: a function's generic entry `f_<addr>`, a setjmp owner's `sjcall_<addr>` stub, or any other
/// name of the export (HLE and patch functions), interned in the partition's `Names`
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug, PartialOrd, Ord)]
pub enum Sym {
    Entry(u64),
    SjCall(u64),
    Name(u32),
}

/// what a call calls
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub enum Callee {
    /// `llvm.<name>[.<ty>[.<ty>]]`: an LLVM intrinsic and its overload types (Void: none)
    Llvm(&'static str, [Ty; 2]),
    /// a runtime function of the host (a64_*, recomp_*)
    Rt(&'static str),
    /// an internal function of emit::helpers(), by index
    Helper(u8),
    /// a patch row's host function
    Host(Sym),
}

impl Callee {
    pub fn llvm(name: &'static str) -> Callee {
        Callee::Llvm(name, [Ty::Void; 2])
    }
    pub fn llvm1(name: &'static str, t: Ty) -> Callee {
        Callee::Llvm(name, [t, Ty::Void])
    }
    pub fn llvm2(name: &'static str, t: Ty, u: Ty) -> Callee {
        Callee::Llvm(name, [t, u])
    }
}

/// a call of an LLVM intrinsic, a runtime function, an internal helper or a host function (kept in the function's
/// call table: Op::Call and Op::RowIo name it by index)
#[derive(Copy, Clone, Debug)]
pub struct Call {
    pub callee: Callee,
    /// the context pointer passed first
    pub ctx: bool,
    /// reads or writes state: never removed or moved
    pub effects: bool,
    /// with ctx: the registers it reads of the context
    pub regs: u128,
}

impl Call {
    pub fn new(callee: Callee) -> Call {
        Call { callee, ctx: false, effects: false, regs: REGS }
    }
    /// a call with the context that reads no register of it
    pub fn ctx(callee: Callee, effects: bool) -> Call {
        Call { callee, ctx: true, effects, regs: 0 }
    }
}

#[derive(Clone, Debug)]
pub enum Op {
    Const(u128),
    /// a vector constant: its lanes packed, lane 0 in the low bits
    VConst(u128),
    /// the host function pointer of the function at a guest address (code_ptrs)
    CodeAddr(u64),
    Param(Var),
    Bin(BinOp),
    Icmp(&'static str),
    Fcmp(&'static str),
    Select,
    Cast(CastOp),
    FNeg,
    /// uadd sadd usub ssub: the overflow bit
    Ovf(&'static str),
    /// ctlz bitreverse bswap fshr
    Intr(&'static str),
    Extract(u32),
    Insert(u32),
    Shuffle(Box<[i32]>),
    /// a call of the function's call table
    Call(u32),
    Load,
    Store,
    ALoad(&'static str),
    AStore(&'static str),
    Rmw(&'static str),
    Cas,
    Fence(&'static str),
    CtxStore32(u32),
    CtxLoad(u32),
    CtxStore(u32),
    RegLoad(Var),
    Writeback(Var),
    /// stop: the callee stops the game (an import the host neither implements nor answers): a noreturn call
    Gcall { sym: Sym, callee: u64, pc: u64, stop: bool },
    Icall(u64),
    Svc(u32),
    Barrier,
    FlagsOut { always: bool },
    SitePoison { mask: u128, pc: u64 },
    /// an At row's call of the call table that also takes a struct of the registers it rewrites: its first nargs
    /// arguments are the row's, the others the registers
    RowIo { call: u32, nargs: u32 },
    RowOut(u32),
}

#[derive(Clone, Debug)]
pub struct Value {
    pub ty: Ty,
    pub op: Op,
    a0: u32,
    an: u32,
}

#[derive(Clone, Debug)]
pub enum Term {
    Br(BlockId),
    CondBr(ValueId, BlockId, BlockId),
    Switch(ValueId, Vec<(u64, BlockId)>, BlockId),
    Ret,
    /// a tail call of another function's generic entry
    Tail(Sym),
    /// a BR the lift could not resolve: (target, pc); None until the exit's block is filled
    IJump(Option<ValueId>, u64),
    /// a call of a callee proven never to return came back: (pc, word)
    NoReturn(u64, u32),
    Trap(u64, u32),
}

#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub enum BKey {
    Pc(u64),
    /// an exit the lifter adds: (kind letter, pc): b't' a tail call's, b'j' an indirect jump's
    Exit(u8, u64),
}

#[derive(Clone, Debug)]
pub struct Block {
    pub key: BKey,
    pub ops: Vec<ValueId>,
    /// the block parameters in creation order
    pub params: Vec<(Var, ValueId)>,
    pub defs: [Option<ValueId>; NVARS],
    pub term: Option<Term>,
    pub preds: Vec<BlockId>,
    /// a call in the block made every guest register the context's again (reads after it load)
    pub called: bool,
}

impl Block {
    fn new(key: BKey) -> Block {
        Block {
            key,
            ops: Vec::new(),
            params: Vec::new(),
            defs: [None; NVARS],
            term: None,
            preds: Vec::new(),
            called: false,
        }
    }
    pub fn pc(&self) -> u64 {
        match self.key {
            BKey::Pc(p) | BKey::Exit(_, p) => p,
        }
    }
    fn add_param(&mut self, var: Var, v: ValueId) {
        self.params.push((var, v));
    }
    /// the block's label
    pub fn name(&self) -> BlockName {
        BlockName(self.key)
    }
}

pub struct BlockName(BKey);
impl std::fmt::Display for BlockName {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self.0 {
            BKey::Pc(p) => write!(f, "b{p:08x}"),
            BKey::Exit(k, p) => write!(f, "{}{p:08x}", k as char),
        }
    }
}

#[derive(Clone, PartialEq, Eq, Hash)]
enum CKey {
    Int(u128, Ty),
    Vec(u128, Ty),
    Code(u64),
}

pub struct Function {
    pub entry: u64,
    values: Vec<Value>,
    args: Vec<ValueId>,
    pub blocks: Vec<Block>,
    /// the live blocks in creation order
    pub order: Vec<BlockId>,
    map: FxMap<BKey, BlockId>,
    /// guest variables some block writes (a bit per Var)
    pub written: u128,
    consts: FxMap<CKey, ValueId>,
    /// the calls of Op::Call and Op::RowIo
    pub calls: Vec<Call>,
    /// every block's successors (link): succ[succ_at[b]..succ_at[b + 1]]
    succ_at: Vec<u32>,
    succ: Vec<BlockId>,
    /// writes FPCR: emitted strictfp (sem_fp)
    pub strict_fp: bool,
    /// target features (AES-NI, PCLMUL)
    pub target_features: Vec<&'static str>,
    pub code_sites: u32,
}

impl Function {
    pub fn new(entry: u64) -> Function {
        Function {
            entry,
            values: Vec::new(),
            args: Vec::new(),
            blocks: Vec::new(),
            order: Vec::new(),
            map: FxMap::default(),
            succ_at: Vec::new(),
            succ: Vec::new(),
            written: 0,
            consts: FxMap::default(),
            calls: Vec::new(),
            strict_fp: false,
            target_features: Vec::new(),
            code_sites: 0,
        }
    }

    pub fn new_value(&mut self, ty: Ty, op: Op, args: &[ValueId]) -> ValueId {
        let a0 = self.args.len() as u32;
        self.args.extend_from_slice(args);
        self.values.push(Value { ty, op, a0, an: args.len() as u32 });
        ValueId(NonZeroU32::new(self.values.len() as u32).unwrap())
    }
    pub fn v(&self, v: ValueId) -> &Value {
        &self.values[v.n() as usize - 1]
    }
    pub fn ty(&self, v: ValueId) -> Ty {
        self.v(v).ty
    }
    pub fn args(&self, v: ValueId) -> &[ValueId] {
        let x = self.v(v);
        &self.args[x.a0 as usize..(x.a0 + x.an) as usize]
    }
    pub fn arg(&self, v: ValueId, k: usize) -> ValueId {
        self.args(v)[k]
    }
    /// the constant's bits, if v is an integer constant ("const")
    pub fn call(&self, k: u32) -> &Call {
        &self.calls[k as usize]
    }
    pub fn konst(&self, v: ValueId) -> Option<u128> {
        match self.v(v).op {
            Op::Const(c) => Some(c),
            _ => None,
        }
    }
    pub fn nvalues(&self) -> usize {
        self.values.len()
    }

    pub fn block(&mut self, key: BKey) -> BlockId {
        if let Some(&b) = self.map.get(&key) {
            return b;
        }
        let id = BlockId(self.blocks.len() as u32);
        self.blocks.push(Block::new(key));
        self.order.push(id);
        self.map.insert(key, id);
        id
    }
    /// the successors of every block from their terminators, in the order the passes visit them: a branch's target, a
    /// conditional branch's two (one if equal), a switch's default then its cases' blocks, each once
    pub fn link(&mut self) {
        let n = self.blocks.len();
        self.succ_at.clear();
        self.succ.clear();
        let mut seen = vec![u32::MAX; n];
        for k in 0..n {
            self.succ_at.push(self.succ.len() as u32);
            let mut add = |s: BlockId, succ: &mut Vec<BlockId>| {
                if seen[s.0 as usize] != k as u32 {
                    seen[s.0 as usize] = k as u32;
                    succ.push(s);
                }
            };
            match &self.blocks[k].term {
                Some(Term::Br(b)) => add(*b, &mut self.succ),
                Some(Term::CondBr(_, a, b)) => {
                    add(*a, &mut self.succ);
                    add(*b, &mut self.succ);
                }
                Some(Term::Switch(_, cases, def)) => {
                    add(*def, &mut self.succ);
                    for &(_, b) in cases {
                        add(b, &mut self.succ);
                    }
                }
                _ => {}
            }
        }
        self.succ_at.push(self.succ.len() as u32);
    }
    /// a block's successors (after link)
    pub fn succs(&self, b: BlockId) -> &[BlockId] {
        &self.succ[self.succ_at[b.0 as usize] as usize..self.succ_at[b.0 as usize + 1] as usize]
    }
    pub fn find(&self, key: BKey) -> Option<BlockId> {
        self.map.get(&key).copied()
    }
    pub fn b(&self, b: BlockId) -> &Block {
        &self.blocks[b.0 as usize]
    }
    pub fn bm(&mut self, b: BlockId) -> &mut Block {
        &mut self.blocks[b.0 as usize]
    }
    pub fn entry_block(&self) -> BlockId {
        self.map[&BKey::Pc(self.entry)]
    }
    /// drops the blocks not in keep (ids stay valid; the block leaves the order)
    pub fn retain_blocks(&mut self, keep: impl Fn(BlockId) -> bool) {
        let gone: Vec<BlockId> = self.order.iter().copied().filter(|&b| !keep(b)).collect();
        for b in gone {
            let k = self.b(b).key;
            self.map.remove(&k);
        }
        self.order.retain(|&b| keep(b));
    }
}

/// the last compare's flags and operands: cond() folds a condition on them into one compare
#[derive(Copy, Clone)]
pub struct Cmp {
    pub nzcv: [ValueId; 4],
    pub a: ValueId,
    pub b: ValueId,
    pub kind: &'static str,
}

/// appends operations to one block at a time; guest registers through get()/set()
pub struct Builder<'f> {
    pub f: &'f mut Function,
    pub blk: BlockId,
    pub cmp: Option<Cmp>,
}

impl<'f> Builder<'f> {
    pub fn new(f: &'f mut Function) -> Builder<'f> {
        Builder { f, blk: BlockId(u32::MAX), cmp: None }
    }
    pub fn at(&mut self, b: BlockId) {
        self.blk = b;
        self.cmp = None;
    }
    pub fn ty(&self, v: ValueId) -> Ty {
        self.f.ty(v)
    }
    pub fn konst(&self, v: ValueId) -> Option<u128> {
        self.f.konst(v)
    }

    // ---- values
    /// an integer constant (v masked to the width); a vector type is a splat (vconst)
    pub fn c(&mut self, v: i128, ty: Ty) -> ValueId {
        if ty.is_vec() {
            return self.vconst_splat(v as u64, ty);
        }
        let v = (v as u128) & ty.mask();
        let key = CKey::Int(v, ty);
        if let Some(&c) = self.f.consts.get(&key) {
            return c;
        }
        let c = self.f.new_value(ty, Op::Const(v), &[]);
        self.f.consts.insert(key, c);
        c
    }
    /// an i64 constant
    pub fn c64(&mut self, v: i128) -> ValueId {
        self.c(v, I64)
    }
    /// the address of the function at guest address e as the game sees it (code_ptrs)
    pub fn codeaddr(&mut self, e: u64) -> ValueId {
        let key = CKey::Code(e);
        if let Some(&c) = self.f.consts.get(&key) {
            return c;
        }
        let c = self.f.new_value(I64, Op::CodeAddr(e), &[]);
        self.f.consts.insert(key, c);
        c
    }
    pub fn op(&mut self, op: Op, ty: Ty, args: &[ValueId]) -> ValueId {
        let v = self.f.new_value(ty, op, args);
        self.f.bm(self.blk).ops.push(v);
        v
    }

    pub fn get(&mut self, var: Var) -> ValueId {
        let b = self.blk;
        if let Some(v) = self.f.b(b).defs[var as usize] {
            return v;
        }
        let v = if self.f.b(b).called {
            self.op(Op::RegLoad(var), var_type(var), &[])
        } else {
            // (a block's first read of a register it has not written: a parameter; defs keeps it, and only a call,
            // after which reads load, clears defs)
            let p = self.f.new_value(var_type(var), Op::Param(var), &[]);
            self.f.bm(b).add_param(var, p);
            p
        };
        self.f.bm(b).defs[var as usize] = Some(v);
        v
    }
    /// the callee may have changed any guest register: the block's later reads load them from the context
    pub fn after_call(&mut self) {
        let b = self.f.bm(self.blk);
        b.defs = [None; NVARS];
        b.called = true;
        self.cmp = None;
    }
    pub fn set(&mut self, var: Var, value: ValueId) {
        assert_eq!(self.ty(value), var_type(var), "set {}", var_name(var));
        self.f.bm(self.blk).defs[var as usize] = Some(value);
        self.f.written |= bit(var);
        self.op(Op::Writeback(var), Ty::Void, &[value]);
    }

    // ---- integer helpers
    pub fn bin(&mut self, op: BinOp, a: ValueId, b: ValueId) -> ValueId {
        let ta = self.ty(a);
        assert_eq!(ta, self.ty(b), "{}", op.name());
        if let (Some(x), Some(y)) = (self.konst(a), self.konst(b)) {
            if let Some(r) = fold(op, x, y, ta.width()) {
                return self.c(r as i128, ta);
            }
        }
        if op == BinOp::Add {
            if let Some(y) = self.konst(b) {
                if let Op::Bin(BinOp::Add) = self.f.v(a).op {
                    let (a0, a1) = (self.f.arg(a, 0), self.f.arg(a, 1));
                    if let Some(z) = self.konst(a1) {
                        // (x + c1) + c2 -> x + (c1 + c2): an address chain
                        let c = self.c(z.wrapping_add(y) as i128, ta);
                        return self.bin(BinOp::Add, a0, c);
                    }
                }
                if y == 0 {
                    return a;
                }
            }
        }
        self.op(Op::Bin(op), ta, &[a, b])
    }
    pub fn icmp(&mut self, pred: &'static str, a: ValueId, b: ValueId) -> ValueId {
        self.op(Op::Icmp(pred), I1, &[a, b])
    }
    pub fn select(&mut self, c: ValueId, a: ValueId, b: ValueId) -> ValueId {
        if let Some(k) = self.konst(c) {
            return if k != 0 { a } else { b };
        }
        let t = self.ty(a);
        self.op(Op::Select, t, &[c, a, b])
    }
    pub fn zext(&mut self, a: ValueId, ty: Ty) -> ValueId {
        if self.ty(a) == ty {
            return a;
        }
        if let Some(k) = self.konst(a) {
            return self.c(k as i128, ty);
        }
        self.op(Op::Cast(CastOp::Zext), ty, &[a])
    }
    pub fn sext(&mut self, a: ValueId, ty: Ty) -> ValueId {
        let ta = self.ty(a);
        if ta == ty {
            return a;
        }
        if let Some(k) = self.konst(a) {
            let w = ta.width();
            let v = if w < 128 && k >> (w - 1) & 1 == 1 { (k as i128) - (1i128 << w) } else { k as i128 };
            return self.c(v, ty);
        }
        self.op(Op::Cast(CastOp::Sext), ty, &[a])
    }
    pub fn trunc(&mut self, a: ValueId, ty: Ty) -> ValueId {
        if self.ty(a) == ty {
            return a;
        }
        if let Some(k) = self.konst(a) {
            return self.c(k as i128, ty);
        }
        if let Op::Cast(CastOp::Zext) = self.f.v(a).op {
            let s = self.f.arg(a, 0);
            if self.ty(s) == ty {
                return s;
            }
        }
        self.op(Op::Cast(CastOp::Trunc), ty, &[a])
    }
    pub fn intr(&mut self, name: &'static str, ty: Ty, args: &[ValueId]) -> ValueId {
        self.op(Op::Intr(name), ty, args)
    }
    pub fn not(&mut self, a: ValueId) -> ValueId {
        let t = self.ty(a);
        let m = self.c(-1, t);
        self.bin(BinOp::Xor, a, m)
    }

    // ---- vectors and FP: constants of FP types are kept as their bits
    /// a vector constant, one value per lane
    pub fn vconst(&mut self, bits: &[u64], ty: Ty) -> ValueId {
        let (n, lane) = ty.lanes();
        assert_eq!(bits.len(), n as usize);
        let w = lane.width();
        let m = if w >= 64 { u64::MAX } else { (1u64 << w) - 1 };
        let packed = bits.iter().enumerate().fold(0u128, |p, (k, &x)| p | ((x & m) as u128) << (k as u32 * w));
        let key = CKey::Vec(packed, ty);
        if let Some(&c) = self.f.consts.get(&key) {
            return c;
        }
        let c = self.f.new_value(ty, Op::VConst(packed), &[]);
        self.f.consts.insert(key, c);
        c
    }
    pub fn vconst_splat(&mut self, bits: u64, ty: Ty) -> ValueId {
        let n = ty.lanes().0 as usize;
        self.vconst(&vec![bits; n], ty)
    }
    pub fn zero(&mut self, ty: Ty) -> ValueId {
        if ty.is_vec() {
            self.vconst_splat(0, ty)
        } else {
            self.c(0, ty)
        }
    }
    pub fn bitcast(&mut self, a: ValueId, ty: Ty) -> ValueId {
        if self.ty(a) == ty {
            return a;
        }
        if let Op::Cast(CastOp::Bitcast) = self.f.v(a).op {
            let s = self.f.arg(a, 0);
            return self.bitcast(s, ty);
        }
        if let Some(k) = self.konst(a) {
            if !ty.is_vec() {
                return self.c(k as i128, ty);
            }
        }
        self.op(Op::Cast(CastOp::Bitcast), ty, &[a])
    }
    /// sitofp uitofp fptosi fptoui fpext fptrunc (and the integer casts on vectors)
    pub fn cast(&mut self, op: CastOp, a: ValueId, ty: Ty) -> ValueId {
        self.op(Op::Cast(op), ty, &[a])
    }
    /// a call of an LLVM intrinsic or a runtime function returning ty (Void for none)
    pub fn call(&mut self, call: Call, ty: Ty, args: &[ValueId]) -> ValueId {
        let k = self.f.calls.len() as u32;
        self.f.calls.push(call);
        self.op(Op::Call(k), ty, args)
    }
    pub fn extract(&mut self, vec: ValueId, i: u32) -> ValueId {
        let t = self.ty(vec).lanes().1.ty();
        self.op(Op::Extract(i), t, &[vec])
    }
    pub fn insert(&mut self, vec: ValueId, val: ValueId, i: u32) -> ValueId {
        let t = self.ty(vec);
        assert_eq!(t.lanes().1.ty(), self.ty(val));
        self.op(Op::Insert(i), t, &[vec, val])
    }
    /// shufflevector: lanes of a then b by index (-1: poison), the result has mask.len() lanes
    pub fn shuffle(&mut self, a: ValueId, b: ValueId, mask: &[i32]) -> ValueId {
        let lane = self.ty(a).lanes().1;
        let t = Ty::Vec(mask.len() as u8, lane);
        self.op(Op::Shuffle(mask.into()), t, &[a, b])
    }
}

/// the lane type of a vector's lanes as a Ty (convenience for templates)
pub fn lane_ty(t: Ty) -> Ty {
    t.lanes().1.ty()
}

pub fn fold(op: BinOp, a: u128, b: u128, w: u32) -> Option<u128> {
    let m = if w >= 128 { u128::MAX } else { (1u128 << w) - 1 };
    Some(match op {
        BinOp::Add => a.wrapping_add(b) & m,
        BinOp::Sub => a.wrapping_sub(b) & m,
        BinOp::And => a & b,
        BinOp::Or => a | b,
        BinOp::Xor => a ^ b,
        BinOp::Mul => a.wrapping_mul(b) & m,
        BinOp::Shl if b < w as u128 => (a << b) & m,
        BinOp::Lshr if b < w as u128 => a >> b,
        _ => return None,
    })
}

/// predecessor lists, and the values each predecessor passes for its successors' parameters (after link)
pub fn finish(f: &mut Function) {
    for k in 0..f.order.len() {
        let b = f.order[k];
        f.bm(b).preds.clear();
    }
    for k in 0..f.order.len() {
        let b = f.order[k];
        for j in 0..f.succs(b).len() {
            let s = f.succs(b)[j];
            f.bm(s).preds.push(b);
        }
    }
    let mut work: Vec<(BlockId, Var)> = Vec::new();
    for &b in &f.order {
        for &(var, _) in &f.b(b).params {
            work.push((b, var));
        }
    }
    while let Some((b, var)) = work.pop() {
        for k in 0..f.b(b).preds.len() {
            let p = f.b(b).preds[k];
            if f.b(p).defs[var as usize].is_some() {
                continue;
            }
            if f.b(p).called {
                let v = f.new_value(var_type(var), Op::RegLoad(var), &[]);
                let pb = f.bm(p);
                pb.defs[var as usize] = Some(v);
                pb.ops.push(v);
            } else {
                let v = f.new_value(var_type(var), Op::Param(var), &[]);
                let pb = f.bm(p);
                pb.add_param(var, v);
                pb.defs[var as usize] = Some(v);
                work.push((p, var));
            }
        }
    }
}

/// FxHash (rustc's): the maps keyed by small integers in one function's lift need no DoS-resistant hash
#[derive(Default, Clone, Copy)]
pub struct Fx(u64);
impl Hasher for Fx {
    fn finish(&self) -> u64 {
        self.0
    }
    fn write(&mut self, bytes: &[u8]) {
        for c in bytes.chunks(8) {
            let mut w = [0u8; 8];
            w[..c.len()].copy_from_slice(c);
            self.write_u64(u64::from_le_bytes(w));
        }
    }
    fn write_u8(&mut self, i: u8) {
        self.write_u64(i as u64)
    }
    fn write_u16(&mut self, i: u16) {
        self.write_u64(i as u64)
    }
    fn write_u32(&mut self, i: u32) {
        self.write_u64(i as u64)
    }
    fn write_u64(&mut self, i: u64) {
        self.0 = (self.0.rotate_left(5) ^ i).wrapping_mul(0x51_7c_c1_b7_27_22_0a_95);
    }
    fn write_u128(&mut self, i: u128) {
        self.write_u64(i as u64);
        self.write_u64((i >> 64) as u64)
    }
    fn write_usize(&mut self, i: usize) {
        self.write_u64(i as u64)
    }
}
pub type FxMap<K, V> = HashMap<K, V, BuildHasherDefault<Fx>>;

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn folds_and_numbers() {
        let mut f = Function::new(0x1000);
        let e = f.block(BKey::Pc(0x1000));
        let mut b = Builder::new(&mut f);
        b.at(e);
        let x = b.get(x(0));
        let c1 = b.c64(8);
        let a = b.bin(BinOp::Add, x, c1);
        let c2 = b.c64(8);
        assert_eq!(c1, c2);
        let a2 = b.bin(BinOp::Add, a, c2);
        // x + 16, through a new constant
        assert_eq!(b.f.args(a2)[0], x);
        assert_eq!(b.konst(b.f.args(a2)[1]), Some(16));
        let m = b.c(-1, I1);
        assert_eq!(b.konst(m), Some(1));
        assert_eq!(var_parse("v31"), Some(67));
        assert_eq!(var_name(67), "v31");
    }
}
