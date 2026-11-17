//! src/info.rs equals Python's helpers (generate/encoding/enc*.py) over each one's whole domain: the lines built here are the
//! ones generate/info_vectors.py hashes into tests/info_digests.rs. On a mismatch, OUNCE_INFO_DUMP=DIR writes this
//! side's lines to compare with gen_tables.py --dump-info DIR2.
use ounce_decode::info::*;

#[path = "data/info_digests.rs"]
mod info_digests;

fn b(x: bool) -> &'static str {
    if x { "1" } else { "0" }
}

fn op(x: Option<Op>) -> String {
    x.map_or("None".into(), |(m, k, s)| format!("{m},{k},{s}"))
}

fn opt<T: std::fmt::Display>(x: Option<T>) -> String {
    x.map_or("None".into(), |v| v.to_string())
}

/// every point of a grid of ranges (the last one fastest, as itertools.product)
fn grid(ranges: &[std::ops::Range<u32>]) -> Vec<Vec<u32>> {
    let mut out = vec![vec![]];
    for r in ranges {
        out = out.into_iter().flat_map(|p| r.clone().map(move |v| { let mut q = p.clone(); q.push(v); q })).collect();
    }
    out
}

fn lines(points: Vec<Vec<u32>>, f: impl Fn(&[u32]) -> String) -> String {
    points.iter().map(|p| {
        let a: Vec<String> = p.iter().map(|v| v.to_string()).collect();
        format!("{}:{}\n", a.join(","), f(p))
    }).collect()
}

fn fnv(s: &str) -> u64 {
    s.bytes().fold(0xcbf29ce484222325u64, |h, c| (h ^ c as u64).wrapping_mul(0x100000001b3))
}

fn all() -> Vec<(&'static str, String)> {
    let r = |n| 0..n;
    let mut v = vec![];
    // (bitmask's width is 32 or 64: index 0/1 here, printed as the width)
    v.push(("bitmask", grid(&[r(2), r(64), r(64), r(2)]).iter().map(|p| {
        let w = if p[3] == 0 { 32 } else { 64 };
        format!("{},{},{},{}:{}\n", p[0], p[1], p[2], w, opt(bitmask(p[0], p[1], p[2], w)))
    }).collect()));
    v.push(("ldst_ok", lines(grid(&[r(4), r(4)]), |p| b(ldst_ok(p[0], p[1])).into())));
    v.push(("sysreg", lines(grid(&[r(2), r(8), r(16), r(16), r(8)]), |p| {
        sysreg(p[0], p[1], p[2], p[3], p[4]).map_or("None".into(), |(n, w)| format!("{n},{}", b(w)))
    })));
    v.push(("sys_op", lines(grid(&[r(16), r(8)]), |p| opt(sys_op(p[0], p[1])))));
    v.push(("fp_dp1_ok", lines(grid(&[r(4), r(64)]), |p| b(fp_dp1_ok(p[0], p[1])).into())));
    v.push(("fp_int_ok", lines(grid(&[r(2), r(4), r(4), r(8)]), |p| b(fp_int_ok(p[0], p[1], p[2], p[3])).into())));
    v.push(("ldst_one_ok", lines(grid(&[r(2), r(8), r(2), r(4)]), |p| b(ldst_one_ok(p[0], p[1], p[2], p[3])).into())));
    v.push(("fp_imm_value", lines(grid(&[r(256)]), |p| format!("{:016x}", fp_imm_value(p[0]).to_bits()))));
    v.push(("fp_imm_bits", grid(&[r(256), r(3)]).iter().map(|p| {
        let t = [0, 1, 3][p[1] as usize];
        format!("{},{}:{}\n", p[0], t, fp_imm_bits(p[0], t))
    }).collect()));
    v.push(("v_scale", lines(grid(&[r(4), r(4)]), |p| v_scale(p[0], p[1]).to_string())));
    v.push(("mult", lines(grid(&[r(16)]), |p| mult(p[0]).map_or("None".into(), |(a, c)| format!("{a},{c}")))));
    v.push(("arr", lines(grid(&[r(4), r(2)]), |p| arr(p[0], p[1]).into())));
    v.push(("one_shape", lines(grid(&[r(8), r(2), r(4), r(2), r(2)]), |p| {
        let s = one_shape(p[0], p[1], p[2], p[3], p[4]);
        format!("{},{},{},{},{}", s.selem, s.lane.unwrap_or("None"), s.scale, opt(s.index), b(s.replicate))
    })));
    v.push(("lowest", lines(grid(&[r(32)]), |p| opt(lowest(p[0])))));
    v.push(("copy_info", lines(grid(&[r(2), r(2), r(32), r(16)]), |p| {
        copy_info(p[0], p[1], p[2], p[3]).map_or("None".into(), |(k, s)| format!("{k},{s}"))
    })));
    v.push(("same3_info", lines(grid(&[r(2), r(2), r(4), r(32), r(2)]), |p| op(same3_info(p[0], p[1], p[2], p[3], p[4] != 0)))));
    v.push(("diff3_info", lines(grid(&[r(2), r(4), r(16)]), |p| op(diff3_info(p[0], p[1], p[2])))));
    v.push(("misc2_info", lines(grid(&[r(2), r(2), r(4), r(32), r(2)]), |p| op(misc2_info(p[0], p[1], p[2], p[3], p[4] != 0)))));
    v.push(("across_info", lines(grid(&[r(2), r(2), r(4), r(32)]), |p| op(across_info(p[0], p[1], p[2], p[3])))));
    v.push(("pair_info", lines(grid(&[r(2), r(4), r(32)]), |p| op(pair_info(p[0], p[1], p[2])))));
    v.push(("modimm_info", lines(grid(&[r(2), r(2), r(16), r(2)]), |p| {
        modimm_info(p[0], p[1], p[2], p[3]).map_or("None".into(), |(m, a, k)| format!("{m},{a},{k}"))
    })));
    v.push(("modimm_value", lines(grid(&[r(2), r(16), r(256)]), |p| modimm_value(p[0], p[1], p[2]).to_string())));
    v.push(("shift_info", lines(grid(&[r(2), r(2), r(16), r(32), r(2)]), |p| op(shift_info(p[0], p[1], p[2], p[3], p[4] != 0)))));
    v.push(("shift_amount", lines(grid(&[r(5), r(4), r(16), r(8)]), |p| {
        shift_amount(&"rlnwf"[p[0] as usize..p[0] as usize + 1], p[1], p[2], p[3]).to_string()
    })));
    v.push(("elem_info", lines(grid(&[r(2), r(2), r(4), r(2), r(16), r(2)]), |p| {
        op(elem_info(p[0], p[1], p[2], p[3], p[4], p[5] != 0))
    })));
    v.push(("elem_index", lines(grid(&[1..4, r(2), r(2), r(16), r(2)]), |p| {
        let (a, c) = elem_index(p[0], p[1], p[2], p[3], p[4]);
        format!("{a},{c}")
    })));
    v.push(("dot_info", lines(grid(&[r(2), r(2), r(4), r(16)]), |p| op(dot_info(p[0], p[1], p[2], p[3])))));
    v.push(("fp16_misc_info", lines(grid(&[r(2), r(2), r(32)]), |p| opt(fp16_misc_info(p[0], p[1], p[2])))));
    v.push(("perm", lines(grid(&[r(8)]), |p| opt(perm(p[0])))));
    v
}

#[test]
fn info_equals_python() {
    let dump = std::env::var("OUNCE_INFO_DUMP").ok();
    let got = all();
    let mut bad = vec![];
    for (name, want) in info_digests::DIGESTS {
        let text = &got.iter().find(|(n, _)| n == name).unwrap_or_else(|| panic!("{name}: not built here")).1;
        if let Some(d) = &dump {
            std::fs::create_dir_all(d).unwrap();
            std::fs::write(format!("{d}/{name}.txt"), text).unwrap();
        }
        if fnv(text) != *want {
            bad.push(*name);
        }
    }
    assert_eq!(got.len(), info_digests::DIGESTS.len());
    assert!(bad.is_empty(), "differ from Python: {bad:?}");
}
