//! A guest function -> IR (nx2/lift2/build_ir.py: scope, build, omit_stores, cycle_edges), and the patch rows'
//! lowering (nx2/lift2/probes.py Value.lower, At.post).

use crate::bounds::Records;
use crate::export::{AtArg, AtRow, Bound, Func, Mode, Reg, ValueArg, ValueRow};
use crate::ir::*;
use crate::sem;
use crate::sem::{Code, Kind};
use crate::sem::int::{cond, getx, setx, sx};
use crate::ty::{Ty, I128, I32, I64, V2I64};
use ounce_decode::{decode, Insn, Fld};
use std::collections::{HashMap, HashSet};


/// what lifting one function needs to know (build_ir.Program, for one function: the export's facts)
pub struct Prog<'a> {
    pub entry: u64,
    /// the body's pcs, ascending
    pub body: &'a [u64],
    /// the body's pcs that are function entries, ascending
    pub entries: &'a [u64],
    /// by br
    pub jt: &'a [(u64, Vec<u64>)],
    pub jt_index: &'a [(u64, u64, u32, u32, Vec<u64>)],
    pub code_targets: &'a HashSet<u64>,
    /// by pc
    pub code_sites: &'a [(u64, u64, Option<u64>)],
    pub probes: HashMap<u64, &'a ValueRow>,
    pub before: HashMap<u64, Vec<&'a AtRow>>,
    pub after: HashMap<u64, Vec<&'a AtRow>>,
    pub replace: HashMap<u64, Vec<&'a AtRow>>,
    pub rec: &'a Records<'a>,
    pub own: Bound,
    pub omit: bool,
}

impl<'a> Prog<'a> {
    pub fn new(f: &'a Func, code_targets: &'a HashSet<u64>, rec: &'a Records<'a>, omit: bool) -> Prog<'a> {
        let mut p = Prog {
            entry: f.entry,
            body: &f.pcs,
            entries: &f.body_entries,
            jt: &f.jt,
            jt_index: &f.jt_index,
            code_targets,
            code_sites: &f.code_sites,
            probes: f.values.iter().map(|v| (v.pc, v)).collect(),
            before: HashMap::new(),
            after: HashMap::new(),
            replace: HashMap::new(),
            rec,
            own: rec.own(f.entry),
            omit,
        };
        for r in &f.rows {
            let m = match r.mode {
                Mode::Before => &mut p.before,
                Mode::After => &mut p.after,
                Mode::Instead => &mut p.replace,
            };
            m.entry(r.pc).or_default().push(r);
        }
        p
    }
    fn callee(&self, t: u64) -> u64 {
        self.rec.callee(t)
    }
    fn bound(&self, c: u64) -> Bound {
        self.rec.bound(c)
    }
    /// a branch to t stays in the function (a goto), or leaves it (a tail call)
    fn inside(&self, t: u64) -> bool {
        self.in_body(t) && (t == self.entry || self.entries.binary_search(&t).is_err())
    }
    fn in_body(&self, pc: u64) -> bool {
        self.body.binary_search(&pc).is_ok()
    }
    /// the jump table's targets of the BR at pc
    fn jt(&self, pc: u64) -> Option<&'a [u64]> {
        self.jt.binary_search_by_key(&pc, |r| r.0).ok().map(|k| &self.jt[k].1[..])
    }
    /// (entry, page) of the code pointer site at pc
    fn code_site(&self, pc: u64) -> Option<(u64, Option<u64>)> {
        self.code_sites.binary_search_by_key(&pc, |r| r.0).ok().map(|k| (self.code_sites[k].1, self.code_sites[k].2))
    }
}

pub fn target(i: &Insn, pc: u64) -> Option<u64> {
    let off = match sem::kind(i) {
        Kind::B => sx(i.f(Fld::imm26), 26),
        Kind::Bcond | Kind::Cbz => sx(i.f(Fld::imm19), 19),
        Kind::Tbz => sx(i.f(Fld::imm14), 14),
        _ => return None,
    };
    Some((pc as i128 + (off << 2)) as u64)
}

/// None if every instruction decodes and has a template (the bin's "skipped: template X" otherwise)
pub fn scope(pcs: &[u64], words: &[u32]) -> Option<String> {
    for (&pc, &w) in pcs.iter().zip(words) {
        let i = match decode(w) {
            Some(i) => i,
            None => return Some(format!("word {w:08x} at {pc:#x} not decoded")),
        };
        if let Kind::Missing = sem::kind(&i) {
            return Some(format!("template {}", i.enc.template));
        }
    }
    None
}

/// (summary bits: x19-x29, SP) a BL keeps in SSA
const KEEP: [Var; 12] = [19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, SP];

/// a worker's buffers for build's per-function work, kept from one function to the next
#[derive(Default)]
pub struct Scratch {
    insns: Vec<Insn>,
    /// a bit per instruction: a block starts there
    leader: Vec<bool>,
    /// the blocks leaving the function (tail calls, indirect jumps, answers)
    exits: Vec<BlockId>,
    /// a byte per block: reached, or the depth-first search's state
    mark: Vec<u8>,
    /// blocks to visit, or the blocks closing a cycle
    blocks: Vec<BlockId>,
    stack: Vec<(BlockId, usize)>,
    /// per block: (gen, kill), live-in
    flow: Vec<(u128, u128)>,
    live: Vec<u128>,
    keep: Vec<ValueId>,
}

/// the function's IR into fun (emptied first: a worker lifts every function into the same one)
pub fn build(f: &Func, prog: &Prog, fun: &mut Function, sc: &mut Scratch) {
    let (entry, pcs, words) = (f.entry, &f.pcs[..], &f.words[..]);
    fun.reset(entry);
    if let Some(plt) = &f.plt {
        let e = fun.block(BKey::Pc(entry));
        let mut b = Builder::new(fun);
        b.at(e);
        let s = b.c64(plt.slot as i128);
        b.set(x(16), s);
        let t = if plt.target_is_code { b.codeaddr(plt.target) } else { b.c64(plt.target as i128) };
        b.set(x(17), t);
        let tail = prog.bound(prog.callee(plt.target)).tail;
        fun.bm(e).term = Some(Term::Tail(tail));
        fun.link();
        finish(fun);
        return;
    }
    let Scratch { insns, leader, exits, mark, blocks, stack, flow, live, keep } = sc;
    exits.clear();

    // the BR's table index read at its load: BR -> (block, value)
    let mut index_load: HashMap<u64, (u64, u32, u32)> = HashMap::new();
    // (plan::parts keeps the tables whose load is in the body with no probe or row up to the BR: the summaries read
    // their targets as the BR's)
    for (br, load, reg, opt, _) in prog.jt_index {
        index_load.insert(*load, (*br, *reg, *opt));
    }
    let mut index_at: HashMap<u64, (BlockId, ValueId)> = HashMap::new();
    insns.clear();
    insns.extend(words.iter().map(|&w| decode(w).expect("decode")));
    // (a block starts at each instruction marked: a branch target in the body, the one after a branch)
    leader.clear();
    leader.resize(pcs.len(), false);
    let mut lead = |pc: u64| {
        if let Ok(k) = pcs.binary_search(&pc) {
            leader[k] = true;
        }
    };
    lead(entry);
    for (k, &pc) in pcs.iter().enumerate() {
        let i = &insns[k];
        let kd = sem::kind(i);
        if kd.is_term() {
            if let Some(tg) = target(i, pc) {
                lead(tg);
            }
            if let Kind::Br = kd {
                for &x in prog.jt(pc).unwrap_or(&[]) {
                    lead(x);
                }
            }
            lead(pc + 4);
        }
    }
    fun.block(BKey::Pc(entry));

    // the block a branch to t goes to: one of the function's, or a tail-call exit
    // (an answered callee, PATCH_ANSWER: a return with x0 the answer)
    fn goto(b: &mut Builder, prog: &Prog, exits: &mut Vec<BlockId>, t: u64) -> BlockId {
        if prog.inside(t) {
            return b.f.block(BKey::Pc(t));
        }
        let bd = prog.bound(prog.callee(t));
        let key = BKey::Exit(if bd.answer.is_some() { b'a' } else { b't' }, t);
        if let Some(blk) = b.f.find(key) {
            return blk;
        }
        let blk = b.f.block(key);
        exits.push(blk);
        match bd.answer {
            None => b.f.bm(blk).term = Some(Term::Tail(bd.tail)),
            Some(answer) => {
                b.f.bm(blk).term = Some(Term::Ret);
                let (here, cmp) = (b.blk, b.cmp);
                b.at(blk);
                let v = b.c64(answer as i128);
                b.set(x(0), v);
                b.blk = here;
                b.cmp = cmp;
            }
        }
        blk
    }

    let mut cur: Option<BlockId> = None;
    let mut b = Builder::new(fun);
    for (k, &pc) in pcs.iter().enumerate() {
        if leader[k] || cur.is_none() {
            let nxt = b.f.block(BKey::Pc(pc));
            if let Some(c) = cur {
                if b.f.b(c).term.is_none() {
                    b.f.bm(c).term = Some(Term::Br(nxt));
                }
            }
            cur = Some(nxt);
            b.at(nxt);
        }
        let c = cur.unwrap();
        for row in prog.before.get(&pc).map(|v| v.as_slice()).unwrap_or(&[]) {
            at_post(&mut b, row);
        }
        if let Some(&(br, reg, opt)) = index_load.get(&pc) {
            let mut v = b.get(x(reg));
            if opt == 2 || opt == 6 {
                let t = b.trunc(v, I32);
                v = if opt == 2 { b.zext(t, I64) } else { b.sext(t, I64) };
            }
            index_at.insert(br, (c, v));
        }
        let i = &insns[k];
        let kd = sem::kind(i);
        let falls_out = |b: &mut Builder, exits: &mut Vec<BlockId>| {
            let g = goto(b, prog, exits, pc + 4);
            b.f.bm(c).term = Some(Term::Br(g));
        };
        if !kd.is_term() {
            let (lower, code) = match kd {
                Kind::Sem(f, c) => (Some(f), c),
                _ => (None, Code::No),
            };
            let site = prog.code_site(pc);
            let probe = prog.probes.get(&pc).copied();
            if let Some((_, Some(page))) = site {
                if probe.is_none() {
                    let v = b.get(x(i.f(Fld::Rn)));
                    if b.konst(v) != Some(page as u128) {
                        panic!("code pointer at {pc:#x}: x{} does not hold the page {page:#x} here", i.f(Fld::Rn));
                    }
                }
            }
            match probe {
                Some(p) => value_lower(&mut b, p),
                None => (lower.unwrap_or_else(|| panic!("{}", i.enc.template)))(&mut b, i, pc),
            }
            if let (Some((e, _)), None) = (site, probe) {
                let ca = b.codeaddr(e);
                setx(&mut b, i.f(Fld::Rd), ca, false);
                b.f.code_sites += 1;
            } else if code != Code::No
                && !prog.code_targets.is_empty()
                && (if i.has(Fld::Rd) { i.f(Fld::Rd) } else { 31 }) != 31
                && !(code == Code::Adr && i.f(Fld::op) != 0)
                && (!i.has(Fld::sf) || i.f(Fld::sf) != 0)
            {
                if let Some(v) = b.f.b(c).defs[x(i.f(Fld::Rd)) as usize] {
                    if let Some(k) = b.konst(v) {
                        if prog.code_targets.contains(&(k as u64)) {
                            panic!("f_{k:08x}'s address formed at {pc:#x}, not a code_ptrs.sites shape");
                        }
                    }
                }
            }
            for row in prog.after.get(&pc).map(|v| v.as_slice()).unwrap_or(&[]) {
                at_post(&mut b, row);
            }
            if !prog.in_body(pc + 4) {
                falls_out(&mut b, exits);
                cur = None;
            }
            continue;
        }
        let is_bl = matches!(kd, Kind::B) && i.f(Fld::op) != 0;
        if prog.replace.contains_key(&pc) && (matches!(kd, Kind::Blr) || is_bl) {
            let ra = b.c64(pc as i128 + 4);
            b.set(x(30), ra);
            for row in &prog.replace[&pc] {
                at_post(&mut b, row);
            }
            for row in prog.after.get(&pc).map(|v| v.as_slice()).unwrap_or(&[]) {
                at_post(&mut b, row);
            }
            if !prog.in_body(pc + 4) {
                falls_out(&mut b, exits);
                cur = None;
            }
            continue;
        }
        if is_bl {
            let callee = prog.callee(target(i, pc).unwrap());
            let bd = prog.bound(callee);
            if let Some(answer) = bd.answer {
                // (PATCH_ANSWER: no call; x0 the answer, x30 the BL's, everything else as it was)
                let ra = b.c64(pc as i128 + 4);
                b.set(x(30), ra);
                let v = b.c64(answer as i128);
                b.set(x(0), v);
                for row in prog.after.get(&pc).map(|v| v.as_slice()).unwrap_or(&[]) {
                    at_post(&mut b, row);
                }
                if !prog.in_body(pc + 4) {
                    falls_out(&mut b, exits);
                    cur = None;
                }
                continue;
            }
            let mut held: Vec<(Var, ValueId)> = Vec::new();
            for &var in KEEP.iter().filter(|&&v| bd.keeps >> v & 1 != 0) {
                let mut v = b.f.b(c).defs[var as usize];
                if v.is_none() && !b.f.b(c).called {
                    v = Some(b.get(var));
                }
                if let Some(v) = v {
                    held.push((var, v));
                }
            }
            let ra = b.c64(pc as i128 + 4);
            b.set(x(30), ra);
            b.op(Op::Gcall { sym: bd.symbol, callee, pc, stop: bd.stop }, Ty::Void, &[]);
            b.after_call();
            for (var, v) in held {
                b.f.bm(c).defs[var as usize] = Some(v);
            }
            for row in prog.after.get(&pc).map(|v| v.as_slice()).unwrap_or(&[]) {
                at_post(&mut b, row);
            }
            if !prog.after.contains_key(&pc) && bd.noreturn {
                b.f.bm(c).term = Some(Term::NoReturn(pc, i.word));
                cur = None;
                continue;
            }
            if !prog.in_body(pc + 4) {
                falls_out(&mut b, exits);
                cur = None;
            }
            continue;
        }
        if let Kind::Blr = kd {
            let rn = i.f(Fld::Rn);
            let tv = if rn != 31 { b.get(x(rn)) } else { b.c64(0) };
            let ra = b.c64(pc as i128 + 4);
            b.set(x(30), ra);
            b.op(Op::Icall(pc), Ty::Void, &[tv]);
            b.after_call();
            for row in prog.after.get(&pc).map(|v| v.as_slice()).unwrap_or(&[]) {
                at_post(&mut b, row);
            }
            if !prog.in_body(pc + 4) {
                falls_out(&mut b, exits);
                cur = None;
            }
            continue;
        }
        if matches!(kd, Kind::Exc) && i.f(Fld::opc) == 0 {
            let imm = i.f(Fld::imm16);
            let Some((ins, outs)) = prog.rec.prog.svc(imm) else {
                // (an SVC the host does not declare: a stop, as the summaries take it)
                b.f.bm(c).term = Some(Term::SvcStop(pc, imm));
                cur = None;
                continue;
            };
            // (hos_svc_<number>(x0..x7, pc, more) -> x0 | x1 << 64, no context: its inputs as values, poison beyond;
            // its results x0 and x1 from the value, x2 up from more, a local array holding the registers' values before;
            // every other register keeps its value)
            let mut vals = Vec::with_capacity(9 + outs.saturating_sub(2) as usize);
            for k in 0..8 {
                vals.push(if k < ins { b.get(x(k)) } else { b.poison(I64) });
            }
            vals.push(b.c64(pc as i128));
            for k in 2..outs {
                vals.push(b.get(x(k)));
            }
            let kc = b.f.calls.len() as u32;
            b.f.calls.push(Call { effects: true, ctx: false, ..Call::new(Callee::Host(Sym::Svc(imm))) });
            let r = b.op(Op::RowIo { call: kc, nargs: 9 }, I128, &vals);
            if outs > 0 {
                let lo = b.trunc(r, I64);
                b.set(x(0), lo);
            }
            if outs > 1 {
                let s = b.c(64, I128);
                let hi = b.bin(BinOp::Lshr, r, s);
                let hi = b.trunc(hi, I64);
                b.set(x(1), hi);
            }
            for k in 2..outs {
                let v = b.op(Op::RowOut(k - 2), I64, &[r]);
                b.set(x(k), v);
            }
            if !prog.in_body(pc + 4) {
                falls_out(&mut b, exits);
                cur = None;
            }
            continue;
        }
        if prog.after.contains_key(&pc) {
            panic!("code after the branch at {pc:#x}");
        }
        if matches!(kd, Kind::Br) && prog.replace.contains_key(&pc) {
            // (a row in place of a tail call: its result the callee's, then return)
            for row in &prog.replace[&pc] {
                at_post(&mut b, row);
            }
            b.f.bm(c).term = Some(Term::Ret);
            cur = None;
            continue;
        }
        let term = match kd {
            Kind::B => Term::Br(goto(&mut b, prog, exits, target(i, pc).unwrap())),
            Kind::Bcond => {
                let cv = cond(&mut b, i.f(Fld::cond));
                let tb = goto(&mut b, prog, exits, target(i, pc).unwrap());
                let fb = goto(&mut b, prog, exits, pc + 4);
                Term::CondBr(cv, tb, fb)
            }
            Kind::Cbz => {
                let v = getx(&mut b, i.f(Fld::Rt), i.f(Fld::sf), false);
                let t0 = b.ty(v);
                let z = b.c(0, t0);
                let cv = b.icmp(if i.f(Fld::op) != 0 { "ne" } else { "eq" }, v, z);
                let tb = goto(&mut b, prog, exits, target(i, pc).unwrap());
                let fb = goto(&mut b, prog, exits, pc + 4);
                Term::CondBr(cv, tb, fb)
            }
            Kind::Tbz => {
                let bit = i.f(Fld::b5) << 5 | i.f(Fld::b40);
                let rt = i.f(Fld::Rt);
                let r = if rt != 31 { b.get(x(rt)) } else { b.c64(0) };
                let m = b.c64(1i128 << bit);
                let v = b.bin(BinOp::And, r, m);
                let z = b.c64(0);
                let cv = b.icmp(if i.f(Fld::op) != 0 { "ne" } else { "eq" }, v, z);
                let tb = goto(&mut b, prog, exits, target(i, pc).unwrap());
                let fb = goto(&mut b, prog, exits, pc + 4);
                Term::CondBr(cv, tb, fb)
            }
            Kind::Ret => Term::Ret,
            Kind::Br => {
                let rn = i.f(Fld::Rn);
                let xv = if rn != 31 { b.get(x(rn)) } else { b.c64(0) };
                let ij = {
                    let key = BKey::Exit(b'j', pc);
                    let blk = b.f.block(key);
                    b.f.bm(blk).term = Some(Term::IJump(None, pc));
                    exits.push(blk);
                    blk
                };
                let mut targets: Vec<u64> = prog.jt(pc).unwrap_or(&[]).to_vec();
                targets.sort();
                targets.dedup();
                let at = index_at.get(&pc).copied();
                if at.is_some_and(|a| a.0 != c) {
                    panic!("f_{entry:08x}: the table of the BR at {pc:#x} is loaded in another block");
                }
                let term = if let Some((_, at)) = at {
                    let tab = &prog.jt_index.iter().find(|r| r.0 == pc).unwrap().4;
                    let cases: Vec<(u64, BlockId)> =
                        tab.iter().enumerate().map(|(n, &t2)| (n as u64, goto(&mut b, prog, exits, t2))).collect();
                    Term::Switch(at, cases, ij)
                } else if !targets.is_empty() {
                    let lo = targets[0];
                    let l = b.c64(lo as i128);
                    let d = b.bin(BinOp::Sub, xv, l);
                    let two = b.c64(2);
                    let idx = b.intr("fshr", I64, &[d, d, two]);
                    let cases: Vec<(u64, BlockId)> =
                        targets.iter().map(|&t2| ((t2 - lo) >> 2, goto(&mut b, prog, exits, t2))).collect();
                    Term::Switch(idx, cases, ij)
                } else {
                    Term::Br(ij)
                };
                let save = b.blk;
                b.at(ij);
                let tv = if rn != 31 { b.get(x(rn)) } else { b.c64(0) };
                b.f.bm(ij).term = Some(Term::IJump(Some(tv), pc));
                b.blk = save;
                term
            }
            _ => Term::Trap(pc, i.word),
        };
        b.f.bm(c).term = Some(term);
        cur = None;
    }
    fun.link();
    if fun.order.iter().any(|&bl| matches!(fun.b(bl).term, Some(Term::NoReturn(..) | Term::SvcStop(..)))) {
        // blocks only a noreturn call's return reached: gone
        let e = fun.entry_block();
        mark.clear();
        mark.resize(fun.blocks.len(), 0);
        mark[e.0 as usize] = 1;
        blocks.clear();
        blocks.push(e);
        while let Some(bl) = blocks.pop() {
            for &s in fun.succs(bl) {
                if mark[s.0 as usize] == 0 {
                    mark[s.0 as usize] = 1;
                    blocks.push(s);
                }
            }
        }
        fun.retain_blocks(|bl| mark[bl.0 as usize] != 0);
        exits.retain(|bl| mark[bl.0 as usize] != 0);
    }
    if fun.written & (0xFu128 << FN) != 0 {
        let mut b = Builder::new(fun);
        for &bl in exits.iter() {
            b.at(bl);
            let fl: Vec<ValueId> = FLAGS.iter().map(|&v| b.get(v)).collect();
            b.op(Op::FlagsOut { always: false }, Ty::Void, &fl);
        }
    }
    for &bl in &fun.order {
        if fun.b(bl).term.is_none() {
            panic!("block {} has no terminator", fun.b(bl).name());
        }
    }
    cycle_edges(fun, mark, stack, blocks);
    for &bl in blocks.iter() {
        let v = fun.new_value(Ty::Void, Op::Barrier, &[]);
        fun.bm(bl).ops.push(v);
    }
    finish(fun);
    if prog.omit {
        omit_stores(fun, prog, flow, live, keep);
    }
}

/// summary bits of the callee-saved registers and SP (build_ir._CALLEE_SAVED)
const CALLEE_SAVED: u128 = (0x7FFu128 << 19) | (1u128 << SP) | (0xFFu128 << (V0 + 8));

/// (read, killed) by an operation as register masks (build_ir._reads)
fn reads(f: &Function, v: ValueId, prog: &Prog) -> (u128, u128) {
    match &f.v(v).op {
        Op::RegLoad(var) => (bit(*var), 0),
        Op::Gcall { callee, .. } => {
            // (a register the callee preserves, its record's keeps, is written back as the callee found it in the
            // context: the call does not kill a store before it, which a later reader still needs)
            let bd = prog.bound(*callee);
            (bd.reads & REGS, bd.must & !bd.keeps & REGS)
        }
        Op::Icall(_) => (REGS, 0),
        Op::Call(k) | Op::RowIo { call: k, .. } if f.call(*k).ctx => (f.call(*k).regs & REGS, 0),
        _ => (0, 0),
    }
}

/// deletes each register's write-through store no reader can read before the register's next store (build_ir.omit_stores;
/// the verification build's poison is not ported: the export is of the build without it)
fn omit_stores(f: &mut Function, prog: &Prog, info: &mut Vec<(u128, u128)>, live_in: &mut Vec<u128>, keep: &mut Vec<ValueId>) {
    let at_ret = (prog.own.results | CALLEE_SAVED) & REGS;
    let blocks = std::mem::take(&mut f.order);
    info.clear();
    info.resize(f.blocks.len(), (0, 0));
    for &bl in &blocks {
        let (mut gen, mut kill) = (0u128, 0u128);
        for &v in f.b(bl).ops.iter().rev() {
            if let Op::Writeback(var) = f.v(v).op {
                let m = bit(var) & REGS;
                gen &= !m;
                kill |= m;
                continue;
            }
            let (rd, k) = reads(f, v, prog);
            if rd == REGS {
                gen = REGS;
                kill = 0;
            } else if rd != 0 || k != 0 {
                gen = rd | (gen & !k);
                kill = (kill | k) & !rd;
            }
        }
        info[bl.0 as usize] = (gen, kill);
    }
    live_in.clear();
    live_in.resize(f.blocks.len(), 0);
    let live_out = |f: &Function, live_in: &[u128], bl: BlockId| -> u128 {
        match f.b(bl).term.as_ref().unwrap() {
            Term::Ret => at_ret,
            Term::Tail(_) | Term::IJump(..) | Term::NoReturn(..) | Term::Trap(..) => REGS,
            // (a stop that reads no register)
            Term::SvcStop(..) => 0,
            _ => f.succs(bl).iter().fold(0, |o, s| o | live_in[s.0 as usize]),
        }
    };
    let mut changed = true;
    while changed {
        changed = false;
        for &bl in blocks.iter().rev() {
            let (gen, kill) = info[bl.0 as usize];
            let li = gen | (live_out(f, live_in, bl) & !kill);
            if li != live_in[bl.0 as usize] {
                live_in[bl.0 as usize] = li;
                changed = true;
            }
        }
    }
    for &bl in &blocks {
        let mut live = live_out(f, live_in, bl);
        keep.clear();
        for &v in f.b(bl).ops.iter().rev() {
            if let Op::Writeback(var) = f.v(v).op {
                let m = bit(var);
                if m & REGS != 0 && live & m == 0 {
                    continue;
                }
                live &= !m;
                keep.push(v);
                continue;
            }
            let (rd, k) = reads(f, v, prog);
            if rd == REGS {
                live = REGS;
            } else if rd != 0 || k != 0 {
                live = rd | (live & !k);
            }
            keep.push(v);
        }
        let ops = &mut f.bm(bl).ops;
        ops.clear();
        ops.extend(keep.iter().rev());
    }
    f.order = blocks;
}

/// the blocks whose terminator has an edge closing a cycle (back edges of a depth-first search from the entry, then
/// from any block it did not reach), into out; state and stack: buffers
pub fn cycle_edges(f: &Function, state: &mut Vec<u8>, stack: &mut Vec<(BlockId, usize)>, out: &mut Vec<BlockId>) {
    let entry = f.entry_block();
    // (0 unvisited, 1 on the stack, 2 done)
    state.clear();
    state.resize(f.blocks.len(), 0);
    out.clear();
    stack.clear();
    for root in std::iter::once(entry).chain(f.order.iter().copied().filter(|&b| b != entry)) {
        if state[root.0 as usize] != 0 {
            continue;
        }
        state[root.0 as usize] = 1;
        stack.push((root, 0));
        while let Some(top) = stack.last_mut() {
            let blk = top.0;
            let Some(&nxt) = f.succs(blk).get(top.1) else {
                state[blk.0 as usize] = 2;
                stack.pop();
                continue;
            };
            top.1 += 1;
            match state[nxt.0 as usize] {
                1 => {
                    if out.last() != Some(&blk) {
                        out.push(blk);
                    }
                }
                0 => {
                    state[nxt.0 as usize] = 1;
                    stack.push((nxt, 0));
                }
                _ => {}
            }
        }
    }
    // (each once: state 3 marks one kept)
    out.retain(|b| std::mem::replace(&mut state[b.0 as usize], 3) != 3);
}

// ---- patch rows (nx2/lift2/probes.py)

/// sem_fp.vread: V register n as ty
pub fn vread(b: &mut Builder, n: u32, ty: Ty) -> ValueId {
    let v = b.get(vreg(n));
    let w = ty.width();
    if w == 128 {
        return b.bitcast(v, ty);
    }
    if ty.is_vec() {
        let c = b.bitcast(v, V2I64);
        let e = b.extract(c, 0);
        return b.bitcast(e, ty);
    }
    let c = b.bitcast(v, Ty::vec(128 / w, ty));
    b.extract(c, 0)
}

/// sem_fp.vwrite: V register n = val zero-extended to 128 bits
pub fn vwrite(b: &mut Builder, n: u32, val: ValueId) {
    let t = b.ty(val);
    let w = t.width();
    if w == 128 {
        let c = b.bitcast(val, Ty::V128);
        b.set(vreg(n), c);
        return;
    }
    let xv = if !matches!(t, Ty::I(_)) { b.bitcast(val, Ty::I(w as u16)) } else { val };
    let xv = b.zext(xv, I64);
    let z = b.zero(Ty::V128);
    let ins = b.insert(z, xv, 0);
    b.set(vreg(n), ins);
}

/// probes.Value.lower: the instruction's result register gets fn(args)
fn value_lower(b: &mut Builder, p: &ValueRow) {
    let mut vals = Vec::with_capacity(p.args.len());
    for &a in &p.args {
        vals.push(match a {
            ValueArg::X(n, off) => {
                let v = getx(b, n, 1, false);
                match off {
                    Some(o) => {
                        let k = b.c64(o);
                        b.bin(BinOp::Add, v, k)
                    }
                    None => v,
                }
            }
            ValueArg::S(n) => vread(b, n, Ty::F32),
        });
    }
    let (ty, n) = match p.result {
        Reg::W(n) => (I32, n),
        Reg::X(n) => (I64, n),
        r => unreachable!("value result {r:?}"),
    };
    let r = b.call(Call::new(Callee::Host(p.func)), ty, &vals);
    setx(b, n, r, false);
}

/// probes.At.post: fn(args) at its place, its result in a register, io registers through a struct
fn at_post(b: &mut Builder, row: &AtRow) {
    let mut vals = Vec::with_capacity(row.args.len() + row.io.len());
    for &a in &row.args {
        vals.push(match a {
            AtArg::Imm(k) => b.c(k, I32),
            AtArg::Sp => b.get(SP),
            AtArg::Load(base, off) => {
                let v = match base {
                    None => b.get(SP),
                    Some(n) => getx(b, n, 1, false),
                };
                let k = b.c64(off);
                let ad = b.bin(BinOp::Add, v, k);
                b.op(Op::Load, I64, &[ad])
            }
            AtArg::Reg(Reg::X(n)) => getx(b, n, 1, false),
            AtArg::Reg(Reg::W(n)) => getx(b, n, 0, false),
            AtArg::Reg(Reg::Q(n)) => vread(b, n, V2I64),
            AtArg::Reg(Reg::S(n)) => vread(b, n, Ty::F32),
        });
    }
    let ty = match row.result {
        None => Ty::Void,
        Some(Reg::X(_)) => I64,
        Some(Reg::W(_)) => I32,
        Some(Reg::S(_)) => Ty::F32,
        Some(Reg::Q(_)) => V2I64,
    };
    let guest = row.guest;
    let r = if !row.io.is_empty() {
        let nargs = vals.len() as u32;
        for &r in &row.io {
            vals.push(match r {
                Reg::Q(n) => vread(b, n, V2I64),
                Reg::X(n) | Reg::W(n) | Reg::S(n) => getx(b, n, 1, false),
            });
        }
        let k = b.f.calls.len() as u32;
        b.f.calls.push(Call { ctx: guest, ..Call::new(Callee::Host(row.func)) });
        let call = b.op(Op::RowIo { call: k, nargs }, ty, &vals);
        for (k, &r) in row.io.iter().enumerate() {
            let q = matches!(r, Reg::Q(_));
            let v = b.op(Op::RowOut(k as u32), if q { V2I64 } else { I64 }, &[call]);
            match r {
                Reg::Q(n) => vwrite(b, n, v),
                Reg::W(n) => {
                    let v = b.trunc(v, I32);
                    setx(b, n, v, false);
                }
                Reg::X(n) | Reg::S(n) => setx(b, n, v, false),
            }
        }
        call
    } else {
        b.call(Call { effects: true, ctx: guest, ..Call::new(Callee::Host(row.func)) }, ty, &vals)
    };
    match row.result {
        None => {}
        Some(Reg::S(n) | Reg::Q(n)) => vwrite(b, n, r),
        Some(Reg::X(n) | Reg::W(n)) => setx(b, n, r, false),
    }
}
