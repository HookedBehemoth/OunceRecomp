//! FRECPE and FRSQRTE inline (nx2/lift2/fp_estimate.py): the Arm ARM's FPRecipEstimate and FPRSqrtEstimate
//! (FEAT_RPRES off), bit-exact, as LLVM IR helpers the lifted code calls per lane (sem/neon.rs estimate). The tables
//! are computed from the pseudocode's RecipEstimate and RecipSqrtEstimate. The text is the Python's, line for line
//! (its nested calls are written out in their evaluation order).

/// offsetof(GuestContext, fpcr), third_party/a64-kit/rt/a64.h
const FPCR_OFF: u32 = 0x320;

/// RecipEstimate: a in 256..511 -> 256..511
fn recip_estimate(a: u64) -> u64 {
    let a = a * 2 + 1;
    let b = (1u64 << 19) / a;
    (b + 1) / 2
}

/// RecipSqrtEstimate: a in 128..511 -> 256..511
fn rsqrt_estimate(a: u64) -> u64 {
    let a = if a < 256 { a * 2 + 1 } else { (((a >> 1) << 1) + 1) * 2 };
    let mut b = 512u64;
    while a * (b + 1) * (b + 1) < (1 << 28) {
        b += 1;
    }
    (b + 1) / 2
}

fn recip() -> Vec<u64> {
    (0..256).map(|i| recip_estimate(256 + i) & 255).collect()
}

fn rsqrt() -> Vec<u64> {
    (0..512).map(|i| if i >= 128 { rsqrt_estimate(i) & 255 } else { 0 }).collect()
}

fn table(name: &str, vals: &[u64]) -> String {
    let v: Vec<String> = vals.iter().map(|&v| format!("i8 {}", if v > 127 { v as i64 - 256 } else { v as i64 })).collect();
    format!("@{name} = private unnamed_addr constant [{} x i8] [{}], align 1", vals.len(), v.join(", "))
}

/// a few lines of LLVM IR, fresh value names
struct T {
    lines: Vec<String>,
    k: u32,
}

impl T {
    fn t(&mut self, text: String) -> String {
        self.k += 1;
        let n = format!("%t{}", self.k);
        self.lines.push(format!("  {n} = {text}"));
        n
    }
}

fn helper(kind: &str, n: u32) -> String {
    let (e, f): (u32, u32) = if n == 32 { (8, 23) } else { (11, 52) };
    let (ft, it) = if n == 32 { ("float", "i32") } else { ("double", "i64") };
    let emask: u128 = (1 << e) - 1;
    let fmask: u128 = (1 << f) - 1;
    let bias: i128 = (1 << (e - 1)) - 1;
    let m52: u128 = (1 << 52) - 1;
    let mut t = T { lines: Vec::new(), k: 0 };
    macro_rules! t {
        ($($a:tt)*) => { t.t(format!($($a)*)) };
    }
    let pf = t!("getelementptr i8, ptr %ctx, i64 {}", FPCR_OFF);
    let fpcr = t!("load i64, ptr {pf}, align 8");
    let a = t!("and i64 {fpcr}, {}", 1u64 << 24);
    let fz = t!("icmp ne i64 {a}, 0");
    let l = t!("lshr i64 {fpcr}, 22");
    let rm = t!("and i64 {l}, 3");
    let raw = t!("bitcast {ft} %x to {it}");
    let op = if n == 64 { raw.clone() } else { t!("zext i32 {raw} to i64") };
    let sbit = t!("and i64 {op}, {}", 1u128 << (n - 1));
    let neg = t!("icmp ne i64 {sbit}, 0");
    let l = t!("lshr i64 {op}, {f}");
    let exp = t!("and i64 {l}, {emask}");
    let frac = t!("and i64 {op}, {fmask}");
    let e_max = t!("icmp eq i64 {exp}, {emask}");
    let e_zero = t!("icmp eq i64 {exp}, 0");
    let f_zero = t!("icmp eq i64 {frac}, 0");
    let x = t!("xor i1 {f_zero}, true");
    let is_nan = t!("and i1 {e_max}, {x}");
    let qnan = t!("or i64 {op}, {}", 1u128 << (f - 1));
    let inf = t!("or i64 {sbit}, {}", emask << f);
    let o = t!("or i1 {f_zero}, {fz}");
    let zero_in = t!("and i1 {e_zero}, {o}");
    let f52 = if f < 52 { t!("shl i64 {frac}, {}", 52 - f) } else { frac.clone() };
    let mut r;
    if kind == "frecpe" {
        let a = t!("and i64 {f52}, {}", 1u128 << 51);
        let top = t!("icmp ne i64 {a}, 0");
        let d_exp = t!("select i1 {top}, i64 0, i64 -1");
        let s1 = t!("shl i64 {f52}, 1");
        let a1 = t!("and i64 {s1}, {m52}");
        let s2 = t!("shl i64 {f52}, 2");
        let a2 = t!("and i64 {s2}, {m52}");
        let d_f52 = t!("select i1 {top}, i64 {a1}, i64 {a2}");
        let ex = t!("select i1 {e_zero}, i64 {d_exp}, i64 {exp}");
        let fr = t!("select i1 {e_zero}, i64 {d_f52}, i64 {f52}");
        let l = t!("lshr i64 {fr}, 44");
        let idx = t!("and i64 {l}, 255");
        let g = t!("getelementptr [256 x i8], ptr @lift2_recip_est_f{n}, i64 0, i64 {idx}");
        let ld = t!("load i8, ptr {g}, align 1");
        let est = t!("zext i8 {ld} to i64");
        let mut rf = t!("shl i64 {est}, 44");
        let mut rexp = t!("sub i64 {}, {ex}", 2 * bias - 1);
        let r0 = t!("icmp eq i64 {rexp}, 0");
        let rm1 = t!("icmp eq i64 {rexp}, -1");
        let l = t!("lshr i64 {rf}, 1");
        let o = t!("or i64 {}, {l}", 1u128 << 51);
        rf = t!("select i1 {r0}, i64 {o}, i64 {rf}");
        let l = t!("lshr i64 {rf}, 2");
        let o = t!("or i64 {}, {l}", 1u128 << 50);
        rf = t!("select i1 {rm1}, i64 {o}, i64 {rf}");
        rexp = t!("select i1 {rm1}, i64 0, i64 {rexp}");
        let a = t!("and i64 {rexp}, {emask}");
        let s = t!("shl i64 {a}, {f}");
        let o1 = t!("or i64 {sbit}, {s}");
        let lo = if f < 52 { t!("lshr i64 {rf}, {}", 52 - f) } else { rf.clone() };
        r = t!("or i64 {o1}, {lo}");
        let c = t!("icmp sge i64 {exp}, {}", 2 * bias - 1);
        let big = t!("and i1 {fz}, {c}");
        r = t!("select i1 {big}, i64 {sbit}, i64 {r}");
        let l = t!("lshr i64 {frac}, {}", f - 2);
        let c = t!("icmp eq i64 {l}, 0");
        let tiny = t!("and i1 {e_zero}, {c}");
        let e0 = t!("icmp eq i64 {rm}, 0");
        let e1 = t!("icmp eq i64 {rm}, 1");
        let nn = t!("xor i1 {neg}, true");
        let a1 = t!("and i1 {e1}, {nn}");
        let e2 = t!("icmp eq i64 {rm}, 2");
        let a2 = t!("and i1 {e2}, {neg}");
        let o = t!("or i1 {a1}, {a2}");
        let up = t!("or i1 {e0}, {o}");
        let o = t!("or i64 {sbit}, {}", ((emask - 1) << f) | fmask);
        let ovf = t!("select i1 {up}, i64 {inf}, i64 {o}");
        r = t!("select i1 {tiny}, i64 {ovf}, i64 {r}");
        r = t!("select i1 {zero_in}, i64 {inf}, i64 {r}");
        let s = t!("select i1 {is_nan}, i64 {qnan}, i64 {sbit}");
        r = t!("select i1 {e_max}, i64 {s}, i64 {r}");
    } else {
        let c = t!("call i64 @llvm.ctlz.i64(i64 {f52}, i1 false)");
        let lz = t!("sub i64 {c}, 12");
        let a = t!("add i64 {lz}, 1");
        let s = t!("shl i64 {f52}, {a}");
        let d_f52 = t!("and i64 {s}, {m52}");
        let s = t!("sub i64 0, {lz}");
        let ex = t!("select i1 {e_zero}, i64 {s}, i64 {exp}");
        let fr = t!("select i1 {e_zero}, i64 {d_f52}, i64 {f52}");
        let a = t!("and i64 {ex}, 1");
        let odd = t!("icmp ne i64 {a}, 0");
        let l = t!("lshr i64 {fr}, 45");
        let a = t!("and i64 {l}, 127");
        let o1 = t!("or i64 128, {a}");
        let l = t!("lshr i64 {fr}, 44");
        let a = t!("and i64 {l}, 255");
        let o2 = t!("or i64 256, {a}");
        let idx = t!("select i1 {odd}, i64 {o1}, i64 {o2}");
        let g = t!("getelementptr [512 x i8], ptr @lift2_rsqrt_est_f{n}, i64 0, i64 {idx}");
        let ld = t!("load i8, ptr {g}, align 1");
        let est = t!("zext i8 {ld} to i64");
        let s = t!("sub i64 {}, {ex}", 3 * bias - 1);
        let rexp = t!("lshr i64 {s}, 1");
        let a = t!("and i64 {rexp}, {emask}");
        let s = t!("shl i64 {a}, {f}");
        let e44 = t!("shl i64 {est}, 44");
        let lo = if f < 52 { t!("lshr i64 {e44}, {}", 52 - f) } else { e44 };
        r = t!("or i64 {s}, {lo}");
        r = t!("select i1 {e_max}, i64 0, i64 {r}");
        r = t!("select i1 {neg}, i64 {}, i64 {r}", (emask << f) | (1u128 << (f - 1)));
        r = t!("select i1 {zero_in}, i64 {inf}, i64 {r}");
        r = t!("select i1 {is_nan}, i64 {qnan}, i64 {r}");
    }
    let res = if n == 64 { r } else { t!("trunc i64 {r} to i32") };
    let out = t!("bitcast {it} {res} to {ft}");
    format!(
        "define internal {ft} @lift2_{kind}_f{n}(ptr %ctx, {ft} %x) alwaysinline {{\n{}\n  ret {ft} {out}\n}}",
        t.lines.join("\n")
    )
}

/// (helper name, its IR text with its table, the declarations it needs), in the Python's order
pub fn helpers() -> Vec<(&'static str, String, Vec<String>)> {
    let mut out = Vec::new();
    let (rc, rs) = (recip(), rsqrt());
    for (kind, tab, vals) in [("frecpe", "recip", &rc), ("frsqrte", "rsqrt", &rs)] {
        for n in [32u32, 64] {
            let text = format!("{}\n{}", table(&format!("lift2_{tab}_est_f{n}"), vals), helper(kind, n));
            let name: &'static str = Box::leak(format!("lift2_{kind}_f{n}").into_boxed_str());
            let decls =
                if kind == "frsqrte" { vec!["declare i64 @llvm.ctlz.i64(i64, i1)".to_string()] } else { vec![] };
            out.push((name, text, decls));
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn tables() {
        // (RecipEstimate(256) is 511: 1/0.5 in 9 bits; RecipSqrtEstimate(128) is 511)
        assert_eq!(recip_estimate(256), 511);
        assert_eq!(rsqrt_estimate(128), 511);
        assert_eq!(rsqrt_estimate(511), 256);
    }
}
