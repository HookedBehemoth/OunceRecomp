//! Hand-ported helpers of nx2/lift2/enc*.py that the semantic templates use: the operation of a NEON word
//! (enc_neon's *_info), DecodeBitMasks, the FP and SIMD immediates, the system registers and SYS operations, the
//! shapes of vector loads and stores. tests/info.rs proves each equal to Python over its whole domain.
//!
//! An info function gives (mnemonic, kind, element size) as Python's tuple, or None where Python gives None.

/// (mnemonic, kind, element size)
pub type Op = (&'static str, &'static str, u32);

// ---- enc.py

/// a system register: (op0 - 2, op1, CRn, CRm, op2), its name, writable at EL0
pub type SysReg = ((u32, u32, u32, u32, u32), &'static str, bool);

/// MRS/MSR's system registers, (op0 - 2, op1, CRn, CRm, op2) -> (name, writable at EL0): enc.SYSREGS
pub const SYSREGS: &[SysReg] = &[
    ((1, 3, 4, 2, 0), "NZCV", true),
    ((1, 3, 4, 4, 0), "FPCR", true),
    ((1, 3, 4, 4, 1), "FPSR", true),
    ((1, 3, 13, 0, 2), "TPIDR_EL0", true),
    ((1, 3, 13, 0, 3), "TPIDRRO_EL0", false),
    ((1, 3, 14, 0, 1), "CNTPCT_EL0", false),
    ((1, 3, 14, 0, 2), "CNTVCT_EL0", false),
    ((1, 3, 14, 0, 0), "CNTFRQ_EL0", false),
    ((1, 3, 0, 0, 1), "CTR_EL0", false),
    ((1, 3, 0, 0, 7), "DCZID_EL0", false),
];

/// (name, writable at EL0) of a system register
pub fn sysreg(o0: u32, op1: u32, crn: u32, crm: u32, op2: u32) -> Option<(&'static str, bool)> {
    SYSREGS.iter().find(|r| r.0 == (o0, op1, crn, crm, op2)).map(|r| (r.1, r.2))
}

/// SYS operations at EL0 (op1 3, CRn 7), (CRm, op2) -> name: enc.SYS_OPS
pub const SYS_OPS: &[((u32, u32), &str)] = &[
    ((4, 1), "dc zva"), ((4, 3), "dc gva"), ((4, 4), "dc gzva"), ((5, 1), "ic ivau"), ((10, 1), "dc cvac"),
    ((11, 1), "dc cvau"), ((12, 1), "dc cvap"), ((13, 1), "dc cvadp"), ((14, 1), "dc civac"),
    ((10, 3), "dc cgvac"), ((10, 5), "dc cgdvac"), ((12, 3), "dc cgvap"), ((12, 5), "dc cgdvap"),
    ((13, 3), "dc cgvadp"), ((13, 5), "dc cgdvadp"), ((14, 3), "dc cigvac"), ((14, 5), "dc cigdvac"),
];

pub fn sys_op(crm: u32, op2: u32) -> Option<&'static str> {
    SYS_OPS.iter().find(|o| o.0 == (crm, op2)).map(|o| o.1)
}

/// allocated integer load/store (enc.ldst_ok)
pub fn ldst_ok(size: u32, opc: u32) -> bool {
    opc != 3 || size < 2
}

/// DecodeBitMasks: the logical immediate of width 32 or 64, or None for a reserved encoding (enc.bitmask)
pub fn bitmask(n: u32, imms: u32, immr: u32, width: u32) -> Option<u64> {
    let combined = (n << 6) | (!imms & 0x3f);
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
    let emask = if esize == 64 { u64::MAX } else { (1u64 << esize) - 1 };
    let welem = (1u64 << (s + 1)) - 1;
    let elem = if r == 0 { welem } else { ((welem >> r) | (welem << (esize - r))) & emask };
    let mut v = 0u64;
    for i in 0..width / esize {
        v |= elem << (i * esize);
    }
    Some(v)
}

// ---- enc_fp.py

/// FP data processing, one source: allocated (enc_fp.fp_dp1_ok)
pub fn fp_dp1_ok(ftype: u32, opcode: u32) -> bool {
    if matches!(opcode, 4 | 5 | 7) {
        return ftype != 2 && opcode - 4 != ftype;
    }
    ftype <= 1 && matches!(opcode, 0..=3 | 8..=12 | 14 | 15)
}

/// FP and integer conversions: allocated (enc_fp.fp_int_ok)
pub fn fp_int_ok(sf: u32, ftype: u32, rmode: u32, opcode: u32) -> bool {
    if ftype == 2 {
        return sf == 1 && rmode == 1 && matches!(opcode, 6 | 7);
    }
    if ftype == 3 {
        return false;
    }
    if matches!(opcode, 6 | 7) {
        return rmode == 0 && sf == ftype;
    }
    if matches!(opcode, 2..=5) {
        return rmode == 0;
    }
    true
}

/// single-structure load/store: allocated (enc_fp.ldst_one_ok)
pub fn ldst_one_ok(l: u32, opcode: u32, s: u32, size: u32) -> bool {
    if opcode >= 6 {
        return l == 1 && s == 0;
    }
    match opcode >> 1 {
        0 => true,
        1 => size & 1 == 0,
        _ => size <= 1 && !(size == 1 && s != 0),
    }
}

/// VFPExpandImm as a float (enc_fp.fp_imm_value)
pub fn fp_imm_value(imm8: u32) -> f64 {
    let s = if imm8 >> 7 != 0 { -1.0 } else { 1.0 };
    let b6 = imm8 >> 6 & 1;
    let e = (imm8 >> 4 & 3) as i32 + if b6 != 0 { -3 } else { 1 };
    let m = 1.0 + (imm8 & 15) as f64 / 16.0;
    s * m * 2f64.powi(e)
}

/// VFPExpandImm as the bit pattern of ftype 0 (single), 1 (double) or 3 (half) (enc_fp.fp_imm_bits)
pub fn fp_imm_bits(imm8: u32, ftype: u32) -> u64 {
    let (e, f, w): (u32, u32, u32) = match ftype {
        0 => (8, 23, 32),
        1 => (11, 52, 64),
        3 => (5, 10, 16),
        _ => panic!("fp_imm_bits: ftype {ftype}"),
    };
    let imm8 = imm8 as u64;
    let b6 = imm8 >> 6 & 1;
    let exp = ((b6 ^ 1) << (e - 1)) | ((if b6 != 0 { (1u64 << (e - 3)) - 1 } else { 0 }) << 2) | (imm8 >> 4 & 3);
    (imm8 >> 7) << (w - 1) | exp << f | (imm8 & 15) << (f - 4)
}

/// log2 of the access size of a B/H/S/D/Q load or store (enc_fp.v_scale)
pub fn v_scale(size: u32, opc: u32) -> u32 {
    if opc >= 2 { 4 } else { size }
}

/// multiple-structure opcode -> (registers, structure elements) (enc_fp.MULT)
pub fn mult(opcode: u32) -> Option<(u32, u32)> {
    match opcode {
        0 => Some((4, 4)),
        2 => Some((4, 1)),
        4 => Some((3, 3)),
        6 => Some((3, 1)),
        7 => Some((1, 1)),
        8 => Some((2, 2)),
        10 => Some((2, 1)),
        _ => None,
    }
}

/// the arrangement of (size, Q) (enc_fp.ARR)
pub fn arr(size: u32, q: u32) -> &'static str {
    ["8b", "16b", "4h", "8h", "2s", "4s", "1d", "2d"][(size * 2 + (q & 1)) as usize]
}

/// A single-structure load or store's shape (enc_fp.one_shape)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct OneShape {
    pub selem: u32,
    /// lane letter ("b", "h", "s", "d"), None when replicating
    pub lane: Option<&'static str>,
    /// log2 lane bytes
    pub scale: u32,
    /// lane index, None when replicating
    pub index: Option<u32>,
    pub replicate: bool,
}

pub fn one_shape(opcode: u32, s: u32, size: u32, q: u32, r: u32) -> OneShape {
    let selem = ((opcode & 1) << 1 | r) + 1;
    if opcode >= 6 {
        return OneShape { selem, lane: None, scale: size, index: None, replicate: true };
    }
    let (lane, scale, index) = match opcode >> 1 {
        0 => ("b", 0, q << 3 | s << 2 | size),
        1 => ("h", 1, q << 2 | s << 1 | size >> 1),
        _ if size == 0 => ("s", 2, q << 1 | s),
        _ => ("d", 3, q),
    };
    OneShape { selem, lane: Some(lane), scale, index: Some(index), replicate: false }
}

// ---- enc_neon.py

/// the lowest set bit of imm5's low four (enc_neon.lowest)
pub fn lowest(imm5: u32) -> Option<u32> {
    (0..4).find(|k| imm5 >> k & 1 != 0)
}

/// copy (DUP, INS, SMOV, UMOV): (kind, size)
pub fn copy_info(q: u32, op: u32, imm5: u32, imm4: u32) -> Option<(&'static str, u32)> {
    let size = lowest(imm5)?;
    if op != 0 {
        return if q != 0 { Some(("ins_elem", size)) } else { None };
    }
    match imm4 {
        0 | 1 => {
            if size == 3 && q == 0 {
                None
            } else {
                Some((if imm4 == 0 { "dup_elem" } else { "dup_gen" }, size))
            }
        }
        5 => (size < if q != 0 { 3 } else { 2 }).then_some(("smov", size)),
        7 => (if q != 0 { size == 3 } else { size < 3 }).then_some(("umov", size)),
        3 => (q != 0).then_some(("ins_gen", size)),
        _ => None,
    }
}

const INT3: [(&str, Option<&str>, u32); 24] = [
    ("shadd", Some("uhadd"), 2), ("sqadd", Some("uqadd"), 3), ("srhadd", Some("urhadd"), 2), ("", None, 0),
    ("shsub", Some("uhsub"), 2), ("sqsub", Some("uqsub"), 3), ("cmgt", Some("cmhi"), 3), ("cmge", Some("cmhs"), 3),
    ("sshl", Some("ushl"), 3), ("sqshl", Some("uqshl"), 3), ("srshl", Some("urshl"), 3),
    ("sqrshl", Some("uqrshl"), 3), ("smax", Some("umax"), 2), ("smin", Some("umin"), 2), ("sabd", Some("uabd"), 2),
    ("saba", Some("uaba"), 2), ("add", Some("sub"), 3), ("cmtst", Some("cmeq"), 3), ("mla", Some("mls"), 2),
    ("mul", Some("pmul"), 2), ("smaxp", Some("umaxp"), 2), ("sminp", Some("uminp"), 2),
    ("sqdmulh", Some("sqrdmulh"), 2), ("addp", None, 3),
];
const LOGIC: [[&str; 4]; 2] = [["and", "bic", "orr", "orn"], ["eor", "bsl", "bit", "bif"]];

fn fp3(u: u32, hi: u32, opcode: u32) -> Option<&'static str> {
    Some(match (u, hi, opcode) {
        (0, 0, 24) => "fmaxnm", (0, 0, 25) => "fmla", (0, 0, 26) => "fadd", (0, 0, 27) => "fmulx",
        (0, 0, 28) => "fcmeq", (0, 0, 30) => "fmax", (0, 0, 31) => "frecps", (0, 1, 24) => "fminnm",
        (0, 1, 25) => "fmls", (0, 1, 26) => "fsub", (0, 1, 30) => "fmin", (0, 1, 31) => "frsqrts",
        (1, 0, 24) => "fmaxnmp", (1, 0, 26) => "faddp", (1, 0, 27) => "fmul", (1, 0, 28) => "fcmge",
        (1, 0, 29) => "facge", (1, 0, 30) => "fmaxp", (1, 0, 31) => "fdiv", (1, 1, 24) => "fminnmp",
        (1, 1, 26) => "fabd", (1, 1, 28) => "fcmgt", (1, 1, 29) => "facgt", (1, 1, 30) => "fminp",
        _ => return None,
    })
}
const SCALAR3_INT: &[&str] = &["sqadd", "uqadd", "sqsub", "uqsub", "cmgt", "cmhi", "cmge", "cmhs", "sshl", "ushl",
    "sqshl", "uqshl", "srshl", "urshl", "sqrshl", "uqrshl", "add", "sub", "cmtst", "cmeq", "sqdmulh", "sqrdmulh"];
const SCALAR3_FP: &[&str] = &["fmulx", "fcmeq", "frecps", "frsqrts", "fcmge", "facge", "fabd", "fcmgt", "facgt"];
const D_ONLY: &[&str] = &["cmgt", "cmhi", "cmge", "cmhs", "sshl", "ushl", "srshl", "urshl", "add", "sub", "cmtst",
    "cmeq"];

/// three registers of the same type: kind "i" integer, "l" bitwise, "f" FP
pub fn same3_info(q: u32, u: u32, size: u32, opcode: u32, scalar: bool) -> Option<Op> {
    if opcode >= 24 {
        let mn = fp3(u, size >> 1, opcode)?;
        let sz = size & 1;
        if (scalar && !SCALAR3_FP.contains(&mn)) || (!scalar && sz != 0 && q == 0) {
            return None;
        }
        return Some((mn, "f", 2 + sz));
    }
    if opcode == 3 {
        return if scalar { None } else { Some((LOGIC[u as usize][size as usize], "l", 0)) };
    }
    let (mn_s, mn_u, top) = INT3[opcode as usize];
    let mn = if u != 0 { mn_u? } else { mn_s };
    if size > top {
        return None;
    }
    if mn == "pmul" && size != 0 {
        return None;
    }
    if (mn == "sqdmulh" || mn == "sqrdmulh") && size == 0 {
        return None;
    }
    if scalar {
        if !SCALAR3_INT.contains(&mn) || (D_ONLY.contains(&mn) && size != 3) {
            return None;
        }
    } else if size == 3 && q == 0 {
        return None;
    }
    Some((mn, "i", size))
}

const DIFF3: [(&str, Option<&str>, &str); 15] = [
    ("saddl", Some("uaddl"), "l"), ("saddw", Some("uaddw"), "w"), ("ssubl", Some("usubl"), "l"),
    ("ssubw", Some("usubw"), "w"), ("addhn", Some("raddhn"), "n"), ("sabal", Some("uabal"), "l"),
    ("subhn", Some("rsubhn"), "n"), ("sabdl", Some("uabdl"), "l"), ("smlal", Some("umlal"), "l"),
    ("sqdmlal", None, "l"), ("smlsl", Some("umlsl"), "l"), ("sqdmlsl", None, "l"), ("smull", Some("umull"), "l"),
    ("sqdmull", None, "l"), ("pmull", None, "l"),
];

/// three registers of different types: kind "l" long, "w" wide, "n" narrow
pub fn diff3_info(u: u32, size: u32, opcode: u32) -> Option<Op> {
    if opcode as usize >= DIFF3.len() || (size == 3 && !(opcode == 14 && u == 0)) {
        return None;
    }
    let (s, un, kind) = DIFF3[opcode as usize];
    let mn = if u != 0 { un? } else { s };
    if (mn.starts_with("sqd") && size == 0) || (mn == "pmull" && size != 0 && size != 3) {
        return None;
    }
    Some((mn, kind, size))
}

fn int2(u: u32, opcode: u32) -> Option<(&'static str, u32, &'static str)> {
    Some(match (u, opcode) {
        (0, 0) => ("rev64", 2, ""), (0, 1) => ("rev16", 0, ""), (0, 2) => ("saddlp", 2, "p"),
        (0, 3) => ("suqadd", 3, ""), (0, 4) => ("cls", 2, ""), (0, 5) => ("cnt", 0, ""),
        (0, 6) => ("sadalp", 2, "p"), (0, 7) => ("sqabs", 3, ""), (0, 8) => ("cmgt", 3, "z"),
        (0, 9) => ("cmeq", 3, "z"), (0, 10) => ("cmlt", 3, "z"), (0, 11) => ("abs", 3, ""),
        (0, 18) => ("xtn", 2, "n"), (0, 20) => ("sqxtn", 2, "n"),
        (1, 0) => ("rev32", 1, ""), (1, 2) => ("uaddlp", 2, "p"), (1, 3) => ("usqadd", 3, ""),
        (1, 4) => ("clz", 2, ""), (1, 5) => ("not", 1, ""), (1, 6) => ("uadalp", 2, "p"),
        (1, 7) => ("sqneg", 3, ""), (1, 8) => ("cmge", 3, "z"), (1, 9) => ("cmle", 3, "z"),
        (1, 11) => ("neg", 3, ""), (1, 18) => ("sqxtun", 2, "n"), (1, 19) => ("shll", 2, "s"),
        (1, 20) => ("uqxtn", 2, "n"),
        _ => return None,
    })
}

fn fp2(u: u32, hi: u32, opcode: u32) -> Option<&'static str> {
    Some(match (u, hi, opcode) {
        (0, 0, 22) => "fcvtn", (0, 0, 23) => "fcvtl", (0, 0, 24) => "frintn", (0, 0, 25) => "frintm",
        (0, 0, 26) => "fcvtns", (0, 0, 27) => "fcvtms", (0, 0, 28) => "fcvtas", (0, 0, 29) => "scvtf",
        (0, 1, 12) => "fcmgt", (0, 1, 13) => "fcmeq", (0, 1, 14) => "fcmlt", (0, 1, 15) => "fabs",
        (0, 1, 24) => "frintp", (0, 1, 25) => "frintz", (0, 1, 26) => "fcvtps", (0, 1, 27) => "fcvtzs",
        (0, 1, 29) => "frecpe", (1, 0, 22) => "fcvtxn", (1, 0, 24) => "frinta", (1, 0, 25) => "frintx",
        (1, 0, 26) => "fcvtnu", (1, 0, 27) => "fcvtmu", (1, 0, 28) => "fcvtau", (1, 0, 29) => "ucvtf",
        (1, 1, 12) => "fcmge", (1, 1, 13) => "fcmle", (1, 1, 15) => "fneg", (1, 1, 25) => "frinti",
        (1, 1, 26) => "fcvtpu", (1, 1, 27) => "fcvtzu", (1, 1, 29) => "frsqrte", (1, 1, 31) => "fsqrt",
        (0, 1, 31) => "frecpx",
        _ => return None,
    })
}
const SCALAR2_INT: &[&str] = &["suqadd", "sqabs", "cmgt", "cmeq", "cmlt", "abs", "sqxtn", "usqadd", "sqneg", "cmge",
    "cmle", "neg", "sqxtun", "uqxtn"];
const SCALAR2_FP: &[&str] = &["fcvtns", "fcvtms", "fcvtas", "scvtf", "fcmgt", "fcmeq", "fcmlt", "fcvtps", "fcvtzs",
    "frecpe", "fcvtxn", "fcvtnu", "fcvtmu", "fcvtau", "ucvtf", "fcmge", "fcmle", "fcvtpu", "fcvtzu", "frsqrte",
    "frecpx"];

/// two-register miscellaneous; kinds: "" lane op, "z" compare with zero, "n" narrow, "p" pairwise long, "s" shll,
/// "f" FP lane op, "fz" FP compare with zero, "fn"/"fl"/"fx" FCVTN/FCVTL/FCVTXN
pub fn misc2_info(q: u32, u: u32, size: u32, opcode: u32, scalar: bool) -> Option<Op> {
    if opcode >= 12 && (opcode >= 22 || opcode <= 15) {
        let mn = fp2(u, size >> 1, opcode)?;
        let sz = size & 1;
        if matches!(mn, "fcvtn" | "fcvtl" | "fcvtxn") {
            if scalar {
                return if mn == "fcvtxn" && sz != 0 { Some(("fcvtxn", "fx", 3)) } else { None };
            }
            if mn == "fcvtxn" && sz == 0 {
                return None;
            }
            let kind = match mn { "fcvtn" => "fn", "fcvtl" => "fl", _ => "fx" };
            return Some((mn, kind, 2 + sz));
        }
        if (scalar && !SCALAR2_FP.contains(&mn)) || (!scalar && (mn == "frecpx" || (sz != 0 && q == 0))) {
            return None;
        }
        return Some((mn, if matches!(opcode, 12..=14) { "fz" } else { "f" }, 2 + sz));
    }
    let (mut mn, top, kind) = int2(u, opcode)?;
    if mn == "not" && size == 1 {
        mn = "rbit";
    }
    if size > top {
        return None;
    }
    if scalar {
        if !SCALAR2_INT.contains(&mn) {
            return None;
        }
        if (kind == "z" || mn == "abs" || mn == "neg") && size != 3 {
            return None;
        }
        return Some((mn, kind, size));
    }
    if size == 3 && q == 0 {
        return None;
    }
    Some((mn, kind, size))
}

/// across lanes: kind "i" or "f"
pub fn across_info(q: u32, u: u32, size: u32, opcode: u32) -> Option<Op> {
    if opcode == 12 || opcode == 15 {
        if u == 0 || size & 1 != 0 || q == 0 {
            return None;
        }
        let mn = match (opcode, size >> 1) {
            (12, 0) => "fmaxnmv", (12, _) => "fminnmv", (15, 0) => "fmaxv", _ => "fminv",
        };
        return Some((mn, "f", 2));
    }
    let mn = match (u, opcode) {
        (0, 3) => "saddlv", (1, 3) => "uaddlv", (0, 10) => "smaxv", (1, 10) => "umaxv", (0, 26) => "sminv",
        (1, 26) => "uminv", (0, 27) => "addv",
        _ => return None,
    };
    if size == 3 || (size == 2 && q == 0) {
        return None;
    }
    Some((mn, "i", size))
}

/// scalar pairwise
pub fn pair_info(u: u32, size: u32, opcode: u32) -> Option<Op> {
    if u == 0 {
        return if opcode == 27 && size == 3 { Some(("addp", "i", 3)) } else { None };
    }
    let mn = match (opcode, size >> 1) {
        (12, 0) => "fmaxnmp", (12, 1) => "fminnmp", (13, 0) => "faddp", (15, 0) => "fmaxp", (15, 1) => "fminp",
        _ => return None,
    };
    Some((mn, "f", 2 + (size & 1)))
}

/// modified immediate: (mnemonic, arrangement, shift text kind)
pub fn modimm_info(q: u32, op: u32, cmode: u32, o2: u32) -> Option<(&'static str, &'static str, &'static str)> {
    if o2 != 0 {
        return None;
    }
    let mvo = ["movi", "orr", "mvni", "bic"];
    Some(if cmode < 8 {
        (mvo[(op * 2 + (cmode & 1)) as usize], if q != 0 { "4s" } else { "2s" }, "lsl32")
    } else if cmode < 12 {
        (mvo[(op * 2 + (cmode & 1)) as usize], if q != 0 { "8h" } else { "4h" }, "lsl16")
    } else if cmode < 14 {
        (if op != 0 { "mvni" } else { "movi" }, if q != 0 { "4s" } else { "2s" }, "msl")
    } else if cmode == 14 {
        if op != 0 {
            ("movi", if q != 0 { "2d" } else { "d" }, "b64")
        } else {
            ("movi", if q != 0 { "16b" } else { "8b" }, "b8")
        }
    } else if op != 0 {
        if q != 0 { ("fmov", "2d", "f64") } else { return None }
    } else {
        ("fmov", if q != 0 { "4s" } else { "2s" }, "f32")
    })
}

/// AdvSIMDExpandImm: the 64-bit pattern (enc_neon.modimm_value)
pub fn modimm_value(op: u32, cmode: u32, imm8: u32) -> u64 {
    let imm8 = imm8 as u64;
    match cmode >> 1 {
        0..=3 => (imm8 << (8 * (cmode >> 1 & 3))).wrapping_mul(0x100000001),
        4 | 5 => (imm8 << (8 * (cmode >> 1 & 1))).wrapping_mul(0x0001000100010001),
        6 => {
            let v = if cmode & 1 == 0 { imm8 << 8 | 0xff } else { imm8 << 16 | 0xffff };
            v.wrapping_mul(0x100000001)
        }
        _ if cmode == 14 => {
            if op == 0 {
                imm8.wrapping_mul(0x0101010101010101)
            } else {
                (0..8).filter(|k| imm8 >> k & 1 != 0).fold(0, |v, k| v | 0xffu64 << (8 * k))
            }
        }
        _ if op == 0 => fp_imm_bits(imm8 as u32, 0).wrapping_mul(0x100000001),
        _ => fp_imm_bits(imm8 as u32, 1),
    }
}

fn shi(opcode: u32) -> Option<(Option<&'static str>, &'static str, &'static str)> {
    Some(match opcode {
        0 => (Some("sshr"), "ushr", "r"), 2 => (Some("ssra"), "usra", "r"), 4 => (Some("srshr"), "urshr", "r"),
        6 => (Some("srsra"), "ursra", "r"), 8 => (None, "sri", "r"), 10 => (Some("shl"), "sli", "l"),
        12 => (None, "sqshlu", "l"), 14 => (Some("sqshl"), "uqshl", "l"), 16 => (Some("shrn"), "sqshrun", "n"),
        17 => (Some("rshrn"), "sqrshrun", "n"), 18 => (Some("sqshrn"), "uqshrn", "n"),
        19 => (Some("sqrshrn"), "uqrshrn", "n"), 20 => (Some("sshll"), "ushll", "w"),
        28 => (Some("scvtf"), "ucvtf", "f"), 31 => (Some("fcvtzs"), "fcvtzu", "f"),
        _ => return None,
    })
}

/// shift by immediate: kind "r" right, "l" left, "n" narrow, "w" widen, "f" fixed-point conversion
pub fn shift_info(q: u32, u: u32, immh: u32, opcode: u32, scalar: bool) -> Option<Op> {
    if immh == 0 {
        return None;
    }
    let (s, un, kind) = shi(opcode)?;
    let mn = if u != 0 { un } else { s? };
    let esz = if immh >= 8 { 3 } else if immh >= 4 { 2 } else if immh >= 2 { 1 } else { 0 };
    if (kind == "n" || kind == "w") && esz == 3 {
        return None;
    }
    if kind == "f" && esz < 2 {
        return None;
    }
    if scalar {
        if kind == "w" || mn == "shrn" || mn == "rshrn" {
            return None;
        }
        if (kind == "r" || mn == "shl" || mn == "sli") && esz != 3 {
            return None;
        }
    } else if esz == 3 && q == 0 && kind != "n" && kind != "w" {
        return None;
    }
    Some((mn, kind, esz))
}

/// the shift amount of a shift by immediate (enc_neon.shift_amount)
pub fn shift_amount(kind: &str, esz: u32, immh: u32, immb: u32) -> i64 {
    let x = (immh << 3 | immb) as i64;
    if kind == "l" || kind == "w" { x - (8i64 << esz) } else { (16i64 << esz) - x }
}

/// by element: kind "s" same, "l" long, "f" FP
pub fn elem_info(q: u32, u: u32, size: u32, l: u32, opcode: u32, scalar: bool) -> Option<Op> {
    let fp = match (u, opcode) {
        (0, 1) => Some("fmla"), (0, 5) => Some("fmls"), (0, 9) => Some("fmul"), (1, 9) => Some("fmulx"),
        _ => None,
    };
    if let Some(mn) = fp {
        if size < 2 || (size == 3 && (l != 0 || (q == 0 && !scalar))) {
            return None;
        }
        return Some((mn, "f", size));
    }
    let (mn, kind) = match (u, opcode) {
        (0, 2) => ("smlal", "l"), (0, 3) => ("sqdmlal", "l"), (0, 6) => ("smlsl", "l"), (0, 7) => ("sqdmlsl", "l"),
        (0, 8) => ("mul", "s"), (0, 10) => ("smull", "l"), (0, 11) => ("sqdmull", "l"), (0, 12) => ("sqdmulh", "s"),
        (0, 13) => ("sqrdmulh", "s"), (1, 0) => ("mla", "s"), (1, 2) => ("umlal", "l"), (1, 4) => ("mls", "s"),
        (1, 6) => ("umlsl", "l"), (1, 10) => ("umull", "l"),
        _ => return None,
    };
    if size != 1 && size != 2 {
        return None;
    }
    if scalar && mn != "sqdmulh" && mn != "sqrdmulh" {
        return None;
    }
    Some((mn, kind, size))
}

/// (register, lane index) of a by-element operand
pub fn elem_index(size: u32, l: u32, m: u32, rm: u32, h: u32) -> (u32, u32) {
    match size {
        1 => (rm, h << 2 | l << 1 | m),
        2 => (m << 4 | rm, h << 1 | l),
        _ => (m << 4 | rm, h),
    }
}

/// SDOT/UDOT
pub fn dot_info(_q: u32, u: u32, size: u32, opcode: u32) -> Option<Op> {
    (opcode == 2 && size == 2).then_some((if u != 0 { "udot" } else { "sdot" }, "d", 2))
}

/// two-register misc (FP16): FNEG and FABS
pub fn fp16_misc_info(u: u32, a: u32, opcode: u32) -> Option<&'static str> {
    match (u, a, opcode) {
        (1, 1, 15) => Some("fneg"),
        (0, 1, 15) => Some("fabs"),
        _ => None,
    }
}

/// permute opcode -> mnemonic (enc_neon.PERM)
pub fn perm(opcode: u32) -> Option<&'static str> {
    match opcode {
        1 => Some("uzp1"), 2 => Some("trn1"), 3 => Some("zip1"), 5 => Some("uzp2"), 6 => Some("trn2"),
        7 => Some("zip2"),
        _ => None,
    }
}
