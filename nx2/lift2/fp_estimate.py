"""FRECPE and FRSQRTE inline: the Arm ARM's FPRecipEstimate and FPRSqrtEstimate (FEAT_RPRES off), bit-exact, as
LLVM IR helpers the lifted code calls per lane (sem_neon.estimate) and LLVM inlines.

The estimate tables are computed here from the pseudocode's RecipEstimate and RecipSqrtEstimate (the integer
arithmetic below), not pasted: 256 entries for the reciprocal (index: the fraction's top 8 bits), 512 for the square
root (index 128-511: the top 8 fraction bits with an even exponent, the top 7 with an odd one), each the estimate's low
8 bits (bit 8 is always set). Everything else is integer arithmetic and selects, no branches; FPCR.FZ and RMode come
from the context's FPCR (the host keeps MXCSR equal to it, host/hos_fp.c). A NaN operand comes back quieted (FPCR.DN
is not modelled, docs/lifter.md "FP and NEON"); a negative FRSQRTE operand gives the default NaN. No FPSR flag is
set (the lifted code keeps no cumulative flags per operation)."""

FPCR_OFF = 0x320   # offsetof(GuestContext, fpcr), third_party/a64-kit/rt/a64.h (checked by tests/test_lift2_fpest.py)


def recip_estimate(a):
    """RecipEstimate: a in 256..511 ([0.5, 1) in 1/512 steps) -> 256..511"""
    a = a * 2 + 1
    b = (1 << 19) // a
    return (b + 1) // 2


def rsqrt_estimate(a):
    """RecipSqrtEstimate: a in 128..511 ([0.25, 1) in 1/512 steps) -> 256..511"""
    if a < 256:
        a = a * 2 + 1
    else:
        a = (a >> 1) << 1
        a = (a + 1) * 2
    b = 512
    while a * (b + 1) * (b + 1) < (1 << 28):
        b += 1
    return (b + 1) // 2


RECIP = [recip_estimate(256 + i) & 255 for i in range(256)]
RSQRT = [rsqrt_estimate(i) & 255 if i >= 128 else 0 for i in range(512)]


def _table(name, vals):
    return "@%s = private unnamed_addr constant [%d x i8] [%s], align 1" % (
        name, len(vals), ", ".join("i8 %d" % (v - 256 if v > 127 else v) for v in vals))


class _T:
    """a few lines of LLVM IR, fresh value names"""

    def __init__(self):
        self.lines, self.k = [], 0

    def __call__(self, text):
        self.k += 1
        n = "%%t%d" % self.k
        self.lines.append("  %s = %s" % (n, text))
        return n


def _helper(kind, n):
    """define internal <ft> @lift2_<kind>_f<n>(ptr %ctx, <ft> %x): one lane"""
    E, F = {32: (8, 23), 64: (11, 52)}[n]
    ft, it = {32: ("float", "i32"), 64: ("double", "i64")}[n]
    emask, fmask, bias = (1 << E) - 1, (1 << F) - 1, (1 << (E - 1)) - 1
    m52 = (1 << 52) - 1
    t = _T()
    pf = t("getelementptr i8, ptr %%ctx, i64 %d" % FPCR_OFF)
    fpcr = t("load i64, ptr %s, align 8" % pf)
    fz = t("icmp ne i64 %s, 0" % t("and i64 %s, %d" % (fpcr, 1 << 24)))
    rm = t("and i64 %s, 3" % t("lshr i64 %s, 22" % fpcr))
    raw = t("bitcast %s %%x to %s" % (ft, it))
    op = raw if n == 64 else t("zext i32 %s to i64" % raw)
    sbit = t("and i64 %s, %d" % (op, 1 << (n - 1)))
    neg = t("icmp ne i64 %s, 0" % sbit)
    exp = t("and i64 %s, %d" % (t("lshr i64 %s, %d" % (op, F)), emask))
    frac = t("and i64 %s, %d" % (op, fmask))
    e_max = t("icmp eq i64 %s, %d" % (exp, emask))
    e_zero = t("icmp eq i64 %s, 0" % exp)
    f_zero = t("icmp eq i64 %s, 0" % frac)
    is_nan = t("and i1 %s, %s" % (e_max, t("xor i1 %s, true" % f_zero)))
    qnan = t("or i64 %s, %d" % (op, 1 << (F - 1)))
    inf = t("or i64 %s, %d" % (sbit, emask << F))
    zero_in = t("and i1 %s, %s" % (e_zero, t("or i1 %s, %s" % (f_zero, fz))))   # (zero, or a denormal under FZ)
    f52 = t("shl i64 %s, %d" % (frac, 52 - F)) if F < 52 else frac
    if kind == "frecpe":
        # a denormal: one or two places up (the pseudocode's normalisation by at most 2; smaller ones overflowed)
        top = t("icmp ne i64 %s, 0" % t("and i64 %s, %d" % (f52, 1 << 51)))
        d_exp = t("select i1 %s, i64 0, i64 -1" % top)
        d_f52 = t("select i1 %s, i64 %s, i64 %s" % (top, t("and i64 %s, %d" % (t("shl i64 %s, 1" % f52), m52)),
                                                    t("and i64 %s, %d" % (t("shl i64 %s, 2" % f52), m52))))
        ex = t("select i1 %s, i64 %s, i64 %s" % (e_zero, d_exp, exp))
        fr = t("select i1 %s, i64 %s, i64 %s" % (e_zero, d_f52, f52))
        idx = t("and i64 %s, 255" % t("lshr i64 %s, 44" % fr))
        est = t("zext i8 %s to i64" % t("load i8, ptr %s, align 1" % t(
            "getelementptr [256 x i8], ptr @lift2_recip_est_f%d, i64 0, i64 %s" % (n, idx))))
        rf = t("shl i64 %s, 44" % est)
        rexp = t("sub i64 %d, %s" % (2 * bias - 1, ex))
        r0 = t("icmp eq i64 %s, 0" % rexp)
        rm1 = t("icmp eq i64 %s, -1" % rexp)
        rf = t("select i1 %s, i64 %s, i64 %s" % (r0, t("or i64 %d, %s" % (1 << 51, t("lshr i64 %s, 1" % rf))), rf))
        rf = t("select i1 %s, i64 %s, i64 %s" % (rm1, t("or i64 %d, %s" % (1 << 50, t("lshr i64 %s, 2" % rf))), rf))
        rexp = t("select i1 %s, i64 0, i64 %s" % (rm1, rexp))
        r = t("or i64 %s, %s" % (t("or i64 %s, %s" % (sbit, t("shl i64 %s, %d" % (
            t("and i64 %s, %d" % (rexp, emask)), F)))), t("lshr i64 %s, %d" % (rf, 52 - F)) if F < 52 else rf))
        # (the checks of the pseudocode, last first: the first that holds decides)
        big = t("and i1 %s, %s" % (fz, t("icmp sge i64 %s, %d" % (exp, 2 * bias - 1))))   # the result would be denormal
        r = t("select i1 %s, i64 %s, i64 %s" % (big, sbit, r))
        tiny = t("and i1 %s, %s" % (e_zero, t("icmp eq i64 %s, 0" % t("lshr i64 %s, %d" % (frac, F - 2)))))
        # (|x| below 2^-(bias+1): the result overflows: infinity, or the largest normal where rounding goes the other way)
        up = t("or i1 %s, %s" % (t("icmp eq i64 %s, 0" % rm), t("or i1 %s, %s" % (
            t("and i1 %s, %s" % (t("icmp eq i64 %s, 1" % rm), t("xor i1 %s, true" % neg))),
            t("and i1 %s, %s" % (t("icmp eq i64 %s, 2" % rm), neg))))))
        ovf = t("select i1 %s, i64 %s, i64 %s" % (up, inf, t("or i64 %s, %d" % (sbit, ((emask - 1) << F) | fmask))))
        r = t("select i1 %s, i64 %s, i64 %s" % (tiny, ovf, r))
        r = t("select i1 %s, i64 %s, i64 %s" % (zero_in, inf, r))
        r = t("select i1 %s, i64 %s, i64 %s" % (e_max, t("select i1 %s, i64 %s, i64 %s" % (is_nan, qnan, sbit)), r))
    else:
        # a denormal: normalised (the leading one shifted out, the exponent down by the zeros above it)
        lz = t("sub i64 %s, 12" % t("call i64 @llvm.ctlz.i64(i64 %s, i1 false)" % f52))
        d_f52 = t("and i64 %s, %d" % (t("shl i64 %s, %s" % (f52, t("add i64 %s, 1" % lz))), m52))
        ex = t("select i1 %s, i64 %s, i64 %s" % (e_zero, t("sub i64 0, %s" % lz), exp))
        fr = t("select i1 %s, i64 %s, i64 %s" % (e_zero, d_f52, f52))
        odd = t("icmp ne i64 %s, 0" % t("and i64 %s, 1" % ex))
        idx = t("select i1 %s, i64 %s, i64 %s" % (
            odd, t("or i64 128, %s" % t("and i64 %s, 127" % t("lshr i64 %s, 45" % fr))),
            t("or i64 256, %s" % t("and i64 %s, 255" % t("lshr i64 %s, 44" % fr)))))
        est = t("zext i8 %s to i64" % t("load i8, ptr %s, align 1" % t(
            "getelementptr [512 x i8], ptr @lift2_rsqrt_est_f%d, i64 0, i64 %s" % (n, idx))))
        rexp = t("lshr i64 %s, 1" % t("sub i64 %d, %s" % (3 * bias - 1, ex)))   # (positive for every exponent)
        r = t("or i64 %s, %s" % (t("shl i64 %s, %d" % (t("and i64 %s, %d" % (rexp, emask)), F)),
                                 t("lshr i64 %s, %d" % (t("shl i64 %s, 44" % est), 52 - F)) if F < 52 else
                                 t("shl i64 %s, 44" % est)))
        r = t("select i1 %s, i64 0, i64 %s" % (e_max, r))                                   # +infinity: +0
        r = t("select i1 %s, i64 %d, i64 %s" % (neg, (emask << F) | (1 << (F - 1)), r))     # negative: default NaN
        r = t("select i1 %s, i64 %s, i64 %s" % (zero_in, inf, r))                           # zero: infinity
        r = t("select i1 %s, i64 %s, i64 %s" % (is_nan, qnan, r))
    res = r if n == 64 else t("trunc i64 %s to i32" % r)
    out = t("bitcast %s %s to %s" % (it, res, ft))
    return ("define internal %s @lift2_%s_f%d(ptr %%ctx, %s %%x) alwaysinline {\n%s\n  ret %s %s\n}"
            % (ft, kind, n, ft, "\n".join(t.lines), ft, out))


def helpers():
    """{helper name: (its IR text with its table, the declarations it needs)} for emit_llvm.HELPERS"""
    out = {}
    for kind, tab, vals in (("frecpe", "recip", RECIP), ("frsqrte", "rsqrt", RSQRT)):
        for n in (32, 64):
            text = _table("lift2_%s_est_f%d" % (tab, n), vals) + "\n" + _helper(kind, n)
            out["lift2_%s_f%d" % (kind, n)] = (text, ["declare i64 @llvm.ctlz.i64(i64, i1)"] if kind == "frsqrte" else [])
    return out
