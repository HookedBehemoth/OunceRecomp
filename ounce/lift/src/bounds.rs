//! Call-boundary records (nx2/lift2/bounds.py call_bounds, verify off): one per callee, from the summaries and the
//! host's declarations, read by the lift (what stays in SSA across a BL, the trap after a call that never returns,
//! the call's symbol).

use crate::export::{Bound, BoundKind};
use crate::ir::{Sym, ALL};
use crate::summary::{Program, Summary, ORIG_ARGS, X0, X30, ABI_RESULTS};

/// x19-x29, SP: the registers a BL may keep in SSA
pub const KEEP_ALL: u128 = 0xbff8_0000;

pub struct Records<'a> {
    pub prog: &'a Program<'a>,
    pub sums: &'a [Summary],
    /// the setjmp owners, sorted
    sj: Vec<u64>,
}

fn unknown(kind: BoundKind, addr: u64, symbol: Sym, tail: Sym) -> Bound {
    Bound { kind, addr, reads: ALL, clobbers: ALL, keeps: 0, must: 0, results: ALL, noreturn: false, stop: false, symbol, tail, answer: None }
}

impl<'a> Records<'a> {
    pub fn new(prog: &'a Program<'a>, sums: &'a [Summary], sj_rets: &[[u64; 5]]) -> Records<'a> {
        let mut sj: Vec<u64> = sj_rets.iter().map(|r| r[0]).collect();
        sj.sort_unstable();
        Records { prog, sums, sj }
    }

    /// callee(t): the PLT stubs followed
    pub fn callee(&self, t: u64) -> u64 {
        self.prog.callee(t)
    }

    /// the packed summary's facts of a lifted function, or None
    fn summ(&self, c: u64) -> Option<&Summary> {
        self.prog.fn_id(c).map(|i| &self.sums[i as usize])
    }

    /// bound(c): the record of a callee
    pub fn bound(&self, c: u64) -> Bound {
        self.make(c, false)
    }

    /// the record of c's lifted body itself (a PATCH_FUNC's body: what its orig call reads and keeps)
    pub fn own(&self, c: u64) -> Bound {
        self.make(c, true)
    }

    fn make(&self, c: u64, body: bool) -> Bound {
        let sym = Sym::Entry(c);
        let tail = sym;
        if self.sj.binary_search(&c).is_ok() {
            // (hos_sj_call through a per-owner stub, host/hos_setjmp.c)
            return unknown(BoundKind::SetjmpOwner, c, Sym::SjCall(c), tail);
        }
        if let Some(h) = self.prog.hook(c) {
            let h = &self.prog.hooks[h];
            if h.stops {
                return Bound {
                    kind: BoundKind::Stop,
                    addr: c,
                    reads: ALL,
                    clobbers: ALL,
                    keeps: 0,
                    must: 0,
                    results: ALL,
                    noreturn: true,
                    stop: true,
                    symbol: sym,
                    tail,
                    answer: None,
                };
            }
            if let Some(v) = h.answer {
                return Bound {
                    kind: BoundKind::Answer,
                    addr: c,
                    reads: 0,
                    clobbers: X0 | X30,
                    keeps: KEEP_ALL,
                    must: X0 | X30,
                    results: X0,
                    noreturn: false,
                    stop: false,
                    symbol: sym,
                    tail,
                    answer: Some(v),
                };
            }
            if h.clobbering {
                return unknown(BoundKind::HleClobbering, c, sym, tail);
            }
            let s = &h.summary;
            return Bound {
                kind: BoundKind::Hle,
                addr: c,
                reads: s.inputs,
                clobbers: s.clobbers,
                keeps: KEEP_ALL,
                must: 0,
                results: ABI_RESULTS,
                noreturn: false,
                stop: false,
                symbol: sym,
                tail,
                answer: None,
            };
        }
        if !body {
            if let Some(p) = self.prog.patch(c) {
                return self.func_bound(p, c, &mut Vec::new());
            }
        }
        match self.summ(c) {
            None => unknown(BoundKind::NoSummary, c, sym, tail),
            Some(s) if !s.returns => Bound {
                kind: BoundKind::Noreturn,
                addr: c,
                reads: ALL,
                clobbers: ALL,
                keeps: 0,
                must: 0,
                results: ALL,
                noreturn: true,
                stop: false,
                symbol: sym,
                tail,
                answer: None,
            },
            Some(s) if s.demote != 0 || s.unknown => unknown(BoundKind::Demoted, c, sym, tail),
            Some(s) => Bound {
                kind: BoundKind::Lifted,
                addr: c,
                reads: s.inputs,
                clobbers: s.clobbers,
                keeps: s.preserved & KEEP_ALL,
                must: s.mustdef,
                results: s.results,
                noreturn: false,
                stop: false,
                symbol: sym,
                tail,
                answer: None,
            },
        }
    }

    /// the record of a function replaced by a PATCH_FUNC, from its declaration (bounds.func_bound); c: the address
    /// the record is of (its symbols)
    fn func_bound(&self, p: usize, c: u64, seen: &mut Vec<usize>) -> Bound {
        let d = &self.prog.patches[p];
        let sym = Sym::Entry(c);
        let mut reads = d.reads;
        for &g in &d.guest {
            if let Some(gp) = self.prog.patch(g) {
                // (a replaced function: what its record reads; a cycle of them once)
                if !seen.contains(&gp) && gp != p {
                    seen.push(p);
                    reads |= self.func_bound(gp, c, seen).reads;
                    seen.pop();
                }
                continue;
            }
            reads |= match self.summ(g) {
                Some(s) if !s.unknown => s.inputs,
                _ => ALL,
            };
        }
        let (mut clobbers, mut keeps) = (X30 | d.result, KEEP_ALL);
        if d.orig {
            let s = match self.summ(d.entry) {
                Some(s) if s.demote == 0 && !s.unknown && s.returns => s,
                _ => return unknown(BoundKind::PatchFuncUnproven, c, sym, sym),
            };
            reads |= s.inputs;
            clobbers |= s.clobbers | ORIG_ARGS;
            keeps &= s.preserved;
        }
        Bound {
            kind: BoundKind::PatchFunc,
            addr: c,
            reads,
            clobbers,
            keeps,
            must: X30 | d.result,
            results: d.result,
            noreturn: false,
            stop: false,
            symbol: sym,
            tail: sym,
            answer: None,
        }
    }
}
