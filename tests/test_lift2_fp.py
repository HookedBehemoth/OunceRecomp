"""Lifter 2's FP and NEON lifting (host FP mode) without the game: single instructions assembled by llvm-mc, lifted
to IR and LLVM IR, compiled, run from edge-value and random states (signed zeros, infinities, NaNs, subnormals,
rounding ties, saturation edges; FPCR 0 and FZ), and compared with values computed here from the Arm ARM's
definitions: exact rational arithmetic rounded as FPRound does, ARM's NaN and min/max rules, saturation.

The comparison allows exactly the deviations of docs/lifter.md ("FP and NEON"): a NaN where ARM has a NaN (any
sign or payload), and under FZ a result of the smallest normal magnitude where ARM flushes to zero (or the reverse).
Everything else must be equal bit for bit. FPSR is not compared (its cumulative flags are not kept per operation).

python -I -B tests/test_lift2_fp.py      (needs Linux and clang/llvm-mc of nx2/lift2/llvm.py's pinned major; skipped
                                          without them)
"""
import math
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from fractions import Fraction

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
from lift2 import build_ir, emit_llvm, enc, llvm  # noqa: E402


def have(tool):
    try:
        llvm.check(tool)
        return True
    except SystemExit:
        return False


# ---- the reference: FP formats, FPUnpack, FPRound (round to nearest even), the NaN rules
FMT = {32: (8, 23), 64: (11, 52)}
M = {32: (1 << 32) - 1, 64: (1 << 64) - 1}


def is_nan(x, n):
    e, f = FMT[n]
    return (x >> f) & ((1 << e) - 1) == (1 << e) - 1 and x & ((1 << f) - 1) != 0


def is_snan(x, n):
    return is_nan(x, n) and not x >> (FMT[n][1] - 1) & 1


def quiet(x, n):
    return x | 1 << (FMT[n][1] - 1)


def default_nan(n):
    return 0x7FC00000 if n == 32 else 0x7FF8000000000000


def unpack(x, n, fz):
    """(kind, sign, value): kind 'zero' 'num' 'inf' 'nan'; value a Fraction (FZ flushes subnormal inputs)"""
    e, f = FMT[n]
    s = x >> (n - 1) & 1
    ex = (x >> f) & ((1 << e) - 1)
    fr = x & ((1 << f) - 1)
    bias = (1 << (e - 1)) - 1
    if ex == (1 << e) - 1:
        return ("nan" if fr else "inf"), s, None
    if ex == 0:
        if fr == 0 or fz:
            return "zero", s, Fraction(0)
        v = Fraction(fr, 1 << f) * Fraction(2) ** (1 - bias)
    else:
        v = (1 + Fraction(fr, 1 << f)) * Fraction(2) ** (ex - bias)
    return "num", s, -v if s else v


def zero(s, n):
    return s << (n - 1)


def inf(s, n):
    e, f = FMT[n]
    return s << (n - 1) | ((1 << e) - 1) << f


def fround(v, n, fz, mode="rn"):
    """FPRound of an exact nonzero Fraction (mode rn rz rp rm)"""
    e, f = FMT[n]
    bias = (1 << (e - 1)) - 1
    s = 1 if v < 0 else 0
    a = -v if s else v
    ex = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** ex > a:
        ex -= 1
    elif Fraction(2) ** (ex + 1) <= a:
        ex += 1
    emin = 1 - bias
    if fz and ex < emin:
        return zero(s, n)
    ex = max(ex, emin)
    q = a / Fraction(2) ** (ex - f)
    m = q.numerator // q.denominator
    rem = q - m
    up = {"rn": rem > Fraction(1, 2) or (rem == Fraction(1, 2) and m & 1), "rz": False,
          "rp": rem > 0 and not s, "rm": rem > 0 and s}[mode]
    m += up
    if m >> (f + 1):
        m >>= 1
        ex += 1
    if m >> f == 0:
        biased = 0
    else:
        biased = ex + bias
    if biased >= (1 << e) - 1:
        over = {"rn": True, "rz": False, "rp": not s, "rm": bool(s)}[mode]
        return inf(s, n) if over else (s << (n - 1) | ((1 << e) - 2) << f | ((1 << f) - 1))
    return s << (n - 1) | biased << f | (m & ((1 << f) - 1))


def nan_of(xs, n):
    """FPProcessNaNs: the first signalling NaN quieted, else the first quiet NaN; None if no NaN"""
    for x in xs:
        if is_snan(x, n):
            return quiet(x, n)
    for x in xs:
        if is_nan(x, n):
            return x
    return None


def from_value(v, s_if_zero, n, fz, mode="rn"):
    return zero(s_if_zero, n) if v == 0 else fround(v, n, fz, mode)


def f_add(x, y, n, fz, neg_y=False):
    if neg_y and not is_nan(y, n):
        y ^= 1 << (n - 1)
    r = nan_of([x, y], n)
    if r is not None:
        return r
    (kx, sx, vx), (ky, sy, vy) = unpack(x, n, fz), unpack(y, n, fz)
    if kx == "inf" and ky == "inf":
        return inf(sx, n) if sx == sy else default_nan(n)
    if kx == "inf" or ky == "inf":
        return inf(sx if kx == "inf" else sy, n)
    v = vx + vy
    if v == 0:
        return zero(sx & sy, n)
    return fround(v, n, fz)


def f_mul(x, y, n, fz):
    r = nan_of([x, y], n)
    if r is not None:
        return r
    (kx, sx, vx), (ky, sy, vy) = unpack(x, n, fz), unpack(y, n, fz)
    s = sx ^ sy
    if (kx == "inf" and ky == "zero") or (kx == "zero" and ky == "inf"):
        return default_nan(n)
    if kx == "inf" or ky == "inf":
        return inf(s, n)
    if kx == "zero" or ky == "zero":
        return zero(s, n)
    return fround(vx * vy, n, fz)


def f_div(x, y, n, fz):
    r = nan_of([x, y], n)
    if r is not None:
        return r
    (kx, sx, vx), (ky, sy, vy) = unpack(x, n, fz), unpack(y, n, fz)
    s = sx ^ sy
    if (kx == ky == "inf") or (kx == ky == "zero"):
        return default_nan(n)
    if kx == "inf" or ky == "zero":
        return inf(s, n)
    if kx == "zero" or ky == "inf":
        return zero(s, n)
    return fround(vx / vy, n, fz)


def f_fma(a, x, y, n, fz):
    """FPMulAdd(a, x, y) = a + x*y, one rounding"""
    r = nan_of([a, x, y], n)
    (ka, sa, va), (kx, sx, vx), (ky, sy, vy) = unpack(a, n, fz), unpack(x, n, fz), unpack(y, n, fz)
    inf_zero = (kx == "inf" and ky == "zero") or (kx == "zero" and ky == "inf")
    if r is not None:
        return r
    if inf_zero:
        return default_nan(n)
    sp = sx ^ sy
    pinf = kx == "inf" or ky == "inf"
    if ka == "inf" and pinf and sa != sp:
        return default_nan(n)
    if ka == "inf" or pinf:
        return inf(sa if ka == "inf" else sp, n)
    pz = kx == "zero" or ky == "zero"
    v = va + vx * vy
    if v == 0:
        return zero(sa & sp, n) if (ka == "zero" and pz) else zero(0, n)
    return fround(v, n, fz)


def isqrt_frac(v, n, fz):
    """sqrt of a positive Fraction, rounded (exact through integer square roots at 2f+4 bits)"""
    e, f = FMT[n]
    k = f + 8 + max(0, (v.denominator.bit_length() - v.numerator.bit_length()) // 2 + 1)
    scaled = v * Fraction(4) ** k
    i = scaled.numerator // scaled.denominator
    r = math.isqrt(i)
    exact = r * r == i and scaled.denominator == 1
    val = Fraction(r, 2 ** k) + (0 if exact else Fraction(1, 2 ** (k + 2)))   # (sticky bit below the root)
    return fround(val, n, fz)


def f_sqrt(x, n, fz):
    if is_nan(x, n):
        return nan_of([x], n)
    k, s, v = unpack(x, n, fz)
    if k == "zero":
        return zero(s, n)
    if s:
        return default_nan(n)
    if k == "inf":
        return x
    return isqrt_frac(v, n, fz)


def f_minmax(x, y, n, fz, is_max, nm):
    if nm:   # FMINNM/FMAXNM: a quiet NaN against a number gives the number
        qx, qy = is_nan(x, n) and not is_snan(x, n), is_nan(y, n) and not is_snan(y, n)
        if qx and not is_nan(y, n):
            x = y
        elif qy and not is_nan(x, n):
            y = x
    r = nan_of([x, y], n)
    if r is not None:
        return r
    (kx, sx, vx), (ky, sy, vy) = unpack(x, n, fz), unpack(y, n, fz)

    def key(k, s, v):
        return (float("inf") if not s else -float("inf")) if k == "inf" else v

    a, b = key(kx, sx, vx), key(ky, sy, vy)
    if a == b:   # (equal values: zeros of either sign; -0 < +0)
        if kx == "zero" and ky == "zero":
            return zero(sx & sy if is_max else sx | sy, n)
        return flushed(x, n, fz)
    pick = x if (a > b) == is_max else y
    return flushed(pick, n, fz)


def flushed(x, n, fz):
    k, s, v = unpack(x, n, fz)
    return zero(s, n) if k == "zero" else x


def f_cmp(x, y, n, fz):
    """FCMP's NZCV"""
    if is_nan(x, n) or is_nan(y, n):
        return (0, 0, 1, 1)
    (kx, sx, vx), (ky, sy, vy) = unpack(x, n, fz), unpack(y, n, fz)
    a = (float("-inf") if sx else float("inf")) if kx == "inf" else vx
    b = (float("-inf") if sy else float("inf")) if ky == "inf" else vy
    if a == b:
        return (0, 1, 1, 0)
    return (1, 0, 0, 0) if a < b else (0, 0, 1, 0)


def f_to_int(x, n, w, signed, mode, fz, fbits=0):
    """FPToFixed: rounded by mode (rn ties-even, ra ties-away, rp, rm, rz), saturated, NaN gives 0"""
    if is_nan(x, n):
        return 0
    k, s, v = unpack(x, n, fz)
    lo, hi = (-(1 << (w - 1)), (1 << (w - 1)) - 1) if signed else (0, (1 << w) - 1)
    if k == "inf":
        return (lo if s else hi) & ((1 << w) - 1)
    v = v * 2 ** fbits
    fl = v.numerator // v.denominator
    rem = v - fl
    if mode == "rn":
        r = fl + (rem > Fraction(1, 2) or (rem == Fraction(1, 2) and fl & 1))
    elif mode == "ra":
        r = fl + (rem > Fraction(1, 2) or (rem == Fraction(1, 2) and v > 0))
    elif mode == "rp":
        r = fl + (rem > 0)
    elif mode == "rm":
        r = fl
    else:
        r = fl + (rem > 0 and v < 0)
    return min(max(r, lo), hi) & ((1 << w) - 1)


def f_from_int(i, w, signed, n, fz, fbits=0):
    if signed and i >> (w - 1):
        i -= 1 << w
    return from_value(Fraction(i, 2 ** fbits), 0, n, fz)


def f_rint(x, n, fz, mode):
    if is_nan(x, n):
        return nan_of([x], n)
    k, s, v = unpack(x, n, fz)
    if k != "num":
        return zero(s, n) if k == "zero" else x
    r = Fraction(f_to_int_val(v, mode))
    return zero(s, n) if r == 0 else fround(r, n, fz)


def f_to_int_val(v, mode):
    fl = v.numerator // v.denominator
    rem = v - fl
    if mode == "rn":
        return fl + (rem > Fraction(1, 2) or (rem == Fraction(1, 2) and fl & 1))
    if mode == "ra":
        return fl + (rem > Fraction(1, 2) or (rem == Fraction(1, 2) and v > 0))
    if mode == "rp":
        return fl + (rem > 0)
    if mode == "rm":
        return fl
    return fl + (rem > 0 and v < 0)


def f_cvt(x, n_from, n_to, fz):
    """FCVT between single and double"""
    if is_nan(x, n_from):
        e, f = FMT[n_from]
        e2, f2 = FMT[n_to]
        q = quiet(x, n_from)
        pay = q & ((1 << f) - 1)
        pay = pay << (f2 - f) if f2 > f else pay >> (f - f2)
        return (q >> (n_from - 1)) << (n_to - 1) | ((1 << e2) - 1) << f2 | pay
    k, s, v = unpack(x, n_from, fz)
    if k == "inf":
        return inf(s, n_to)
    if k == "zero":
        return zero(s, n_to)
    return fround(v, n_to, fz)


def recip_est(a):
    a = a * 2 + 1
    b = (1 << 19) // a
    return (b + 1) // 2


def frecpe32(x, fz):
    """FPRecipEstimate, single, rounding mode RN"""
    if is_nan(x, 32):
        return quiet(x, 32)
    s = x >> 31
    ex, fr = x >> 23 & 255, x & 0x7FFFFF
    if ex == 255:
        return zero(s, 32)
    if ex == 0 and (fr == 0 or fz):
        return inf(s, 32)
    if ex == 0 and fr < (1 << 21):
        return inf(s, 32)
    if fz and ex >= 253:
        return zero(s, 32)
    f52 = fr << 29
    if ex == 0:
        if not f52 >> 51 & 1:
            ex, f52 = -1, (f52 << 2) & ((1 << 52) - 1)
        else:
            f52 = (f52 << 1) & ((1 << 52) - 1)
    rexp = 253 - ex
    rf = (recip_est(256 | (f52 >> 44 & 255)) & 255) << 44
    if rexp == 0:
        rf = 1 << 51 | rf >> 1
    elif rexp == -1:
        rf, rexp = 1 << 50 | rf >> 2, 0
    return s << 31 | (rexp & 255) << 23 | rf >> 29


def frsqrte32(x, fz):
    if is_nan(x, 32):
        return quiet(x, 32)
    s = x >> 31
    ex, fr = x >> 23 & 255, x & 0x7FFFFF
    if ex == 0 and (fr == 0 or fz):
        return inf(s, 32)
    if s:
        return default_nan(32)
    if ex == 255:
        return 0
    f52 = fr << 29
    if ex == 0:
        while not f52 >> 51 & 1:
            f52 = (f52 << 1) & ((1 << 52) - 1)
            ex -= 1
        f52 = (f52 << 1) & ((1 << 52) - 1)
    a = (256 | (f52 >> 44 & 255)) if ex % 2 == 0 else (128 | (f52 >> 45 & 127))
    if a < 256:
        a = a * 2 + 1
    else:
        a = ((a >> 1) << 1)
        a = (a + 1) * 2
    b = 512
    while a * (b + 1) * (b + 1) < (1 << 28):
        b += 1
    est = (b + 1) // 2
    rexp = (380 - ex) // 2
    return (rexp & 255) << 23 | (est & 255) << 15


# ---- lanes
def lanes_of(v, w, n=None):
    n = n or 128 // w
    return [(v >> (w * k)) & ((1 << w) - 1) for k in range(n)]


def pack(ls, w):
    return sum((x & ((1 << w) - 1)) << (w * k) for k, x in enumerate(ls))


def sx(x, w):
    return x - (1 << w) if x >> (w - 1) & 1 else x


def sat(x, w, signed):
    lo, hi = (-(1 << (w - 1)), (1 << (w - 1)) - 1) if signed else (0, (1 << w) - 1)
    return min(max(x, lo), hi) & ((1 << w) - 1)


def vec(Q, w, fn):
    """a lane-wise vector reference: fn(k) per lane, the upper half zero for 64-bit arrangements"""
    n = (128 if Q else 64) // w
    return pack([fn(k) for k in range(n)], w)


# ---- the cases: (assembly, reference(state) -> {output: value}); state: v (list of 32 ints), x, fz, nzcv
def L(st, r, w):
    return lanes_of(st["v"][r], w)


def fp3_vec(op, Q, w):
    def ref(st):
        a, b, d = L(st, 1, w), L(st, 2, w), L(st, 0, w)
        return {"v0": vec(Q, w, lambda k: op(d[k], a[k], b[k], w, st["fz"]))}
    return ref


def scalar_fp(op, w):
    def ref(st):
        return {"v0": op(L(st, 1, w)[0], L(st, 2, w)[0], L(st, 3, w)[0], w, st["fz"])}
    return ref


def mask_(c, w):
    return (1 << w) - 1 if c else 0


def minmax(is_max, nm):
    return lambda d, a, b, w, fz: f_minmax(a, b, w, fz, is_max, nm)


def frecps(d, a, b, w, fz):
    ka, kb = unpack(a, w, fz)[0], unpack(b, w, fz)[0]
    if not (is_nan(a, w) or is_nan(b, w)) and {ka, kb} == {"inf", "zero"}:
        return 0x40000000 if w == 32 else 0x4000000000000000
    return f_fma(0x40000000 if w == 32 else 0x4000000000000000, a ^ (1 << (w - 1)) if not is_nan(a, w) else a, b, w, fz)


def frsqrts(d, a, b, w, fz):
    ka, kb = unpack(a, w, fz)[0], unpack(b, w, fz)[0]
    if not (is_nan(a, w) or is_nan(b, w)) and {ka, kb} == {"inf", "zero"}:
        return 0x3FC00000
    r = f_fma(0x40400000, a ^ (1 << 31) if not is_nan(a, w) else a, b, w, fz)
    return f_mul(r, 0x3F000000, w, fz)


def cmp_mask(pred):
    def op(d, a, b, w, fz):
        n, z, c, v = f_cmp(a, b, w, fz)
        return mask_({"eq": z, "ge": c and not v, "gt": c and not v and not z}[pred], w)
    return op


def int_vec(fn, Q, w, acc=False):
    def ref(st):
        a, b, d = L(st, 1, w), L(st, 2, w), L(st, 0, w)
        return {"v0": vec(Q, w, lambda k: fn(d[k], a[k], b[k], w))}
    return ref


def shl_reg_ref(signed, rnd):
    def fn(d, a, b, w):
        s = sx(b & 0xFF, 8)
        x = sx(a, w) if signed else a
        if s >= 0:
            return (x << s) & ((1 << w) - 1)
        s = -s
        r = (x + (1 << (s - 1) if rnd else 0)) >> s
        return r & ((1 << w) - 1)
    return fn


def sqdmulh_ref(rnd):
    def fn(d, a, b, w):
        p = 2 * sx(a, w) * sx(b, w) + ((1 << (w - 1)) if rnd else 0)
        return sat(p >> w, w, True)
    return fn


def two_reg(fn, Q, w, src=1):
    def ref(st):
        a = L(st, src, w)
        return {"v0": vec(Q, w, lambda k: fn(a[k], w, st))}
    return ref


def perm(kind, part, Q, w):
    def ref(st):
        a, b = L(st, 1, w), L(st, 2, w)
        n = (128 if Q else 64) // w
        a, b = a[:n], b[:n]
        if kind == "zip":
            base = part * n // 2
            out = [x for i in range(n // 2) for x in (a[base + i], b[base + i])]
        elif kind == "uzp":
            out = [(a + b)[2 * i + part] for i in range(n)]
        else:
            out = [x for i in range(0, n, 2) for x in (a[i + part], b[i + part])]
        return {"v0": pack(out, w)}
    return ref


def tbl_ref(nregs, tbx, Q):
    def ref(st):
        tab = b"".join(st["v"][1 + k].to_bytes(16, "little") for k in range(nregs))
        idx = L(st, 5, 8)
        d = L(st, 0, 8)
        n = 16 if Q else 8
        return {"v0": pack([tab[idx[k]] if idx[k] < 16 * nregs else (d[k] if tbx else 0) for k in range(n)], 8)}
    return ref


def narrow_ref(fn, w):
    """a narrowing op of V1 (2w lanes) into the low half of V0"""
    def ref(st):
        a = L(st, 1, 2 * w)
        return {"v0": pack([fn(a[k], 2 * w) for k in range(64 // w)], w)}
    return ref


def cases():
    C = []
    rn = "rn"
    for w, t in ((32, "s"), (64, "d")):
        C.append(("fadd %s0, %s1, %s2" % (t, t, t), scalar_fp(lambda a, b, c, w, fz: f_add(a, b, w, fz), w)))
        C.append(("fsub %s0, %s1, %s2" % (t, t, t), scalar_fp(lambda a, b, c, w, fz: f_add(a, b, w, fz, True), w)))
        C.append(("fmul %s0, %s1, %s2" % (t, t, t), scalar_fp(lambda a, b, c, w, fz: f_mul(a, b, w, fz), w)))
        C.append(("fdiv %s0, %s1, %s2" % (t, t, t), scalar_fp(lambda a, b, c, w, fz: f_div(a, b, w, fz), w)))
        C.append(("fmadd %s0, %s1, %s2, %s3" % (t, t, t, t), scalar_fp(lambda a, b, c, w, fz: f_fma(c, a, b, w, fz), w)))
        C.append(("fnmsub %s0, %s1, %s2, %s3" % (t, t, t, t), scalar_fp(
            lambda a, b, c, w, fz: f_fma(c ^ (1 << (w - 1)) if not is_nan(c, w) else c, a, b, w, fz), w)))
        C.append(("fsqrt %s0, %s1" % (t, t), scalar_fp(lambda a, b, c, w, fz: f_sqrt(a, w, fz), w)))
        for mn, mx, nm in (("fmax", True, False), ("fmin", False, False), ("fmaxnm", True, True),
                           ("fminnm", False, True)):
            C.append(("%s %s0, %s1, %s2" % (mn, t, t, t),
                      scalar_fp(lambda a, b, c, w, fz, mx=mx, nm=nm: f_minmax(a, b, w, fz, mx, nm), w)))
        C.append(("fcmp %s1, %s2" % (t, t), lambda st, w=w: {"nzcv": f_cmp(L(st, 1, w)[0], L(st, 2, w)[0], w, st["fz"])}))
        C.append(("fcmp %s1, %s2; cset x0, gt; cset x1, ls; cset x2, lt; cset x3, ge" % (t, t),
                  lambda st, w=w: cond_ref(f_cmp(L(st, 1, w)[0], L(st, 2, w)[0], w, st["fz"]))))
        C.append(("fccmp %s1, %s2, #5, ne" % (t, t), lambda st, w=w: {"nzcv": f_cmp(
            L(st, 1, w)[0], L(st, 2, w)[0], w, st["fz"]) if not st["nzcv"][1] else (0, 1, 0, 1)}))
        for mn, mode in (("frintn", "rn"), ("frinta", "ra"), ("frintp", "rp"), ("frintm", "rm"), ("frintz", "rz")):
            C.append(("%s %s0, %s1" % (mn, t, t), scalar_fp(lambda a, b, c, w, fz, m=mode: f_rint(a, w, fz, m), w)))
        for rw, r in ((32, "w"), (64, "x")):
            for mn, signed, mode in (("fcvtzs", True, "rz"), ("fcvtzu", False, "rz"), ("fcvtns", True, "rn"),
                                     ("fcvtas", True, "ra"), ("fcvtmu", False, "rm"), ("fcvtps", True, "rp")):
                C.append(("%s %s0, %s1" % (mn, r, t), lambda st, w=w, rw=rw, s=signed, m=mode: {
                    "x0": f_to_int(L(st, 1, w)[0], w, rw, s, m, st["fz"])}))
            C.append(("scvtf %s0, %s1" % (t, r), lambda st, w=w, rw=rw: {
                "v0": f_from_int(st["x"][1] & M[rw], rw, True, w, st["fz"])}))
            C.append(("ucvtf %s0, %s1" % (t, r), lambda st, w=w, rw=rw: {
                "v0": f_from_int(st["x"][1] & M[rw], rw, False, w, st["fz"])}))
            C.append(("fcvtzs %s0, %s1, #7" % (r, t), lambda st, w=w, rw=rw: {
                "x0": f_to_int(L(st, 1, w)[0], w, rw, True, "rz", st["fz"], 7)}))
            C.append(("scvtf %s0, %s1, #12" % (t, r), lambda st, w=w, rw=rw: {
                "v0": f_from_int(st["x"][1] & M[rw], rw, True, w, st["fz"], 12)}))
    C.append(("fcvt d0, s1", lambda st: {"v0": f_cvt(L(st, 1, 32)[0], 32, 64, st["fz"])}))
    C.append(("fcvt s0, d1", lambda st: {"v0": f_cvt(L(st, 1, 64)[0], 64, 32, st["fz"])}))
    C.append(("fneg s0, s1", lambda st: {"v0": L(st, 1, 32)[0] ^ (1 << 31)}))
    C.append(("fabs d0, d1", lambda st: {"v0": L(st, 1, 64)[0] & ~(1 << 63)}))
    C.append(("fmov s0, #-1.25000000", lambda st: {"v0": 0xBFA00000}))
    C.append(("fmov x0, d1", lambda st: {"x0": L(st, 1, 64)[0]}))
    C.append(("fmov v0.d[1], x1", lambda st: {"v0": L(st, 0, 64)[0] | (st["x"][1] << 64)}))
    C.append(("fcsel s0, s1, s2, lt", lambda st: {"v0": L(st, 1 if st["nzcv"][0] != st["nzcv"][3] else 2, 32)[0]}))
    # NEON FP
    for Q, w, t in ((1, 32, "4s"), (0, 32, "2s"), (1, 64, "2d")):
        v3 = "v0.%s, v1.%s, v2.%s" % (t, t, t)
        C.append(("fadd " + v3, fp3_vec(lambda d, a, b, w, fz: f_add(a, b, w, fz), Q, w)))
        C.append(("fmul " + v3, fp3_vec(lambda d, a, b, w, fz: f_mul(a, b, w, fz), Q, w)))
        C.append(("fmla " + v3, fp3_vec(lambda d, a, b, w, fz: f_fma(d, a, b, w, fz), Q, w)))
        C.append(("fmls " + v3, fp3_vec(lambda d, a, b, w, fz: f_fma(d, a ^ (1 << (w - 1)) if not is_nan(a, w)
                                                                       else a, b, w, fz), Q, w)))
        C.append(("fmax " + v3, fp3_vec(minmax(True, False), Q, w)))
        C.append(("fminnm " + v3, fp3_vec(minmax(False, True), Q, w)))
        C.append(("fabd " + v3, fp3_vec(lambda d, a, b, w, fz: (lambda r: r if is_nan(r, w) else r & ~(1 << (w - 1)))(
            f_add(a, b, w, fz, True)), Q, w)))
        C.append(("fcmge " + v3, fp3_vec(cmp_mask("ge"), Q, w)))
        C.append(("fcmeq " + v3, fp3_vec(cmp_mask("eq"), Q, w)))
        C.append(("frecps " + v3, fp3_vec(frecps, Q, w)))
        C.append(("fdiv " + v3, fp3_vec(lambda d, a, b, w, fz: f_div(a, b, w, fz), Q, w)))
        C.append(("fcvtzs v0.%s, v1.%s" % (t, t), two_reg(lambda a, w, st: f_to_int(a, w, w, True, "rz", st["fz"]), Q, w)))
        C.append(("scvtf v0.%s, v1.%s, #3" % (t, t), two_reg(lambda a, w, st: f_from_int(a, w, True, w, st["fz"], 3), Q, w)))
        C.append(("fcmlt v0.%s, v1.%s, #0.0" % (t, t), two_reg(
            lambda a, w, st: mask_(f_cmp(a, 0, w, st["fz"])[0], w), Q, w)))
        C.append(("frintm v0.%s, v1.%s" % (t, t), two_reg(lambda a, w, st: f_rint(a, w, st["fz"], "rm"), Q, w)))
        C.append(("fsqrt v0.%s, v1.%s" % (t, t), two_reg(lambda a, w, st: f_sqrt(a, w, st["fz"]), Q, w)))
    C.append(("frsqrts v0.4s, v1.4s, v2.4s", fp3_vec(frsqrts, 1, 32)))
    C.append(("frecpe v0.4s, v1.4s", two_reg(lambda a, w, st: frecpe32(a, st["fz"]), 1, 32)))
    C.append(("frsqrte v0.2s, v1.2s", two_reg(lambda a, w, st: frsqrte32(a, st["fz"]), 0, 32)))
    C.append(("fmul v0.4s, v1.4s, v2.s[3]", lambda st: {"v0": vec(1, 32, lambda k: f_mul(
        L(st, 1, 32)[k], L(st, 2, 32)[3], 32, st["fz"]))}))
    C.append(("fmla s0, s1, v2.s[1]", lambda st: {"v0": f_fma(L(st, 0, 32)[0], L(st, 1, 32)[0], L(st, 2, 32)[1], 32,
                                                              st["fz"])}))
    C.append(("faddp v0.4s, v1.4s, v2.4s", lambda st: {"v0": pack([f_add(*p, 32, st["fz"]) for p in (
        L(st, 1, 32)[0:2], L(st, 1, 32)[2:4], L(st, 2, 32)[0:2], L(st, 2, 32)[2:4])], 32)}))
    C.append(("faddp s0, v1.2s", lambda st: {"v0": f_add(L(st, 1, 32)[0], L(st, 1, 32)[1], 32, st["fz"])}))
    C.append(("fmaxv s0, v1.4s", lambda st: {"v0": (lambda l, fz: f_minmax(f_minmax(l[0], l[1], 32, fz, True, False),
                                                                      f_minmax(l[2], l[3], 32, fz, True, False),
                                                                      32, fz, True, False))(L(st, 1, 32), st["fz"])}))
    C.append(("fcvtl v0.2d, v1.2s", lambda st: {"v0": pack([f_cvt(x, 32, 64, st["fz"]) for x in L(st, 1, 32)[:2]], 64)}))
    C.append(("fcvtn2 v0.4s, v1.2d", lambda st: {"v0": L(st, 0, 64)[0] | pack(
        [f_cvt(x, 64, 32, st["fz"]) for x in L(st, 1, 64)], 32) << 64}))
    # NEON integer
    for Q, w, t in ((1, 8, "16b"), (0, 16, "4h"), (1, 32, "4s"), (1, 64, "2d")):
        v3 = "v0.%s, v1.%s, v2.%s" % (t, t, t)
        C.append(("add " + v3, int_vec(lambda d, a, b, w: a + b, Q, w)))
        C.append(("sqadd " + v3, int_vec(lambda d, a, b, w: sat(sx(a, w) + sx(b, w), w, True), Q, w)))
        C.append(("uqsub " + v3, int_vec(lambda d, a, b, w: sat(a - b, w, False), Q, w)))
        C.append(("cmgt " + v3, int_vec(lambda d, a, b, w: mask_(sx(a, w) > sx(b, w), w), Q, w)))
        C.append(("cmhi " + v3, int_vec(lambda d, a, b, w: mask_(a > b, w), Q, w)))
        C.append(("sshl " + v3, int_vec(shl_reg_ref(True, False), Q, w)))
        C.append(("ushl " + v3, int_vec(shl_reg_ref(False, False), Q, w)))
        C.append(("cmtst " + v3, int_vec(lambda d, a, b, w: mask_(a & b, w), Q, w)))
        C.append(("neg v0.%s, v1.%s" % (t, t), two_reg(lambda a, w, st: -a, Q, w)))
        C.append(("cmlt v0.%s, v1.%s, #0" % (t, t), two_reg(lambda a, w, st: mask_(sx(a, w) < 0, w), Q, w)))
        if w < 64:
            C.append(("mla " + v3, int_vec(lambda d, a, b, w: d + a * b, Q, w)))
            C.append(("umin " + v3, int_vec(lambda d, a, b, w: min(a, b), Q, w)))
            C.append(("smax " + v3, int_vec(lambda d, a, b, w: max(sx(a, w), sx(b, w)), Q, w)))
            C.append(("shadd " + v3, int_vec(lambda d, a, b, w: (sx(a, w) + sx(b, w)) >> 1, Q, w)))
            C.append(("urhadd " + v3, int_vec(lambda d, a, b, w: (a + b + 1) >> 1, Q, w)))
            C.append(("sabd " + v3, int_vec(lambda d, a, b, w: abs(sx(a, w) - sx(b, w)), Q, w)))
            C.append(("abs v0.%s, v1.%s" % (t, t), two_reg(lambda a, w, st: abs(sx(a, w)), Q, w)))
            C.append(("cls v0.%s, v1.%s" % (t, t), two_reg(
                lambda a, w, st: w - 1 - (a ^ (((1 << w) - 1) if a >> (w - 1) else 0)).bit_length() + 0, Q, w)))
            C.append(("clz v0.%s, v1.%s" % (t, t), two_reg(lambda a, w, st: w - a.bit_length(), Q, w)))
            C.append(("addp " + v3, lambda st, Q=Q, w=w: {"v0": pack([sum(p) for p in zip(
                *[iter(L(st, 1, w)[:(128 if Q else 64) // w] + L(st, 2, w)[:(128 if Q else 64) // w])] * 2)], w)}))
            for kind in ("zip", "uzp", "trn"):
                for part in (0, 1):
                    C.append(("%s%d %s" % (kind, part + 1, v3), perm(kind, part, Q, w)))
        if w in (16, 32):
            C.append(("sqdmulh " + v3, int_vec(sqdmulh_ref(False), Q, w)))
            C.append(("sqrdmulh " + v3, int_vec(sqdmulh_ref(True), Q, w)))
    C.append(("cnt v0.16b, v1.16b", two_reg(lambda a, w, st: bin(a).count("1"), 1, 8)))
    C.append(("rev64 v0.4s, v1.4s", lambda st: {"v0": pack([L(st, 1, 32)[k ^ 1] for k in range(4)], 32)}))
    C.append(("rev16 v0.8b, v1.8b", lambda st: {"v0": pack([L(st, 1, 8)[k ^ 1] for k in range(8)], 8)}))
    C.append(("ext v0.16b, v1.16b, v2.16b, #5", lambda st: {"v0": pack((L(st, 1, 8) + L(st, 2, 8))[5:21], 8)}))
    C.append(("ext v0.8b, v1.8b, v2.8b, #3", lambda st: {"v0": pack((L(st, 1, 8)[:8] + L(st, 2, 8)[:8])[3:11], 8)}))
    for n in (1, 2, 4):
        regs = ", ".join("v%d.16b" % (1 + k) for k in range(n))
        C.append(("tbl v0.16b, { %s }, v5.16b" % regs, tbl_ref(n, False, 1)))
        C.append(("tbx v0.8b, { %s }, v5.8b" % regs, tbl_ref(n, True, 0)))
    C.append(("dup v0.4s, v1.s[2]", lambda st: {"v0": pack([L(st, 1, 32)[2]] * 4, 32)}))
    C.append(("dup v0.8h, w1", lambda st: {"v0": pack([st["x"][1] & 0xFFFF] * 8, 16)}))
    C.append(("ins v0.s[3], v1.s[1]", lambda st: {"v0": pack(L(st, 0, 32)[:3] + [L(st, 1, 32)[1]], 32)}))
    C.append(("ins v0.b[9], w1", lambda st: {"v0": pack(L(st, 0, 8)[:9] + [st["x"][1] & 0xFF] + L(st, 0, 8)[10:], 8)}))
    C.append(("umov w0, v1.h[5]", lambda st: {"x0": L(st, 1, 16)[5]}))
    C.append(("smov x0, v1.b[3]", lambda st: {"x0": sx(L(st, 1, 8)[3], 8) & M[64]}))
    C.append(("mov s0, v1.s[3]", lambda st: {"v0": L(st, 1, 32)[3]}))
    C.append(("movi v0.4s, #7, lsl #8", lambda st: {"v0": pack([0x700] * 4, 32)}))
    C.append(("mvni v0.2s, #3, msl #8", lambda st: {"v0": pack([~0x3FF & M[32]] * 2, 32)}))
    C.append(("movi d0, #0xff00ff0000ffff00", lambda st: {"v0": 0xff00ff0000ffff00}))
    C.append(("bic v0.8h, #255", lambda st: {"v0": pack([x & 0xFF00 for x in L(st, 0, 16)], 16)}))
    C.append(("fmov v0.4s, #0.50000000", lambda st: {"v0": pack([0x3F000000] * 4, 32)}))
    C.append(("bsl v0.16b, v1.16b, v2.16b", lambda st: {"v0": (st["v"][0] & st["v"][1]) | (~st["v"][0] & st["v"][2])
                                                         & ((1 << 128) - 1)}))
    C.append(("bif v0.8b, v1.8b, v2.8b", lambda st: {"v0": ((st["v"][0] & st["v"][2]) | (st["v"][1] & ~st["v"][2]))
                                                     & M[64]}))
    C.append(("addv s0, v1.4s", lambda st: {"v0": sum(L(st, 1, 32)) & M[32]}))
    C.append(("umaxv b0, v1.16b", lambda st: {"v0": max(L(st, 1, 8))}))
    C.append(("sminv h0, v1.8h", lambda st: {"v0": min(sx(x, 16) for x in L(st, 1, 16)) & 0xFFFF}))
    C.append(("uaddlv s0, v1.8h", lambda st: {"v0": sum(L(st, 1, 16)) & M[32]}))
    C.append(("addp d0, v1.2d", lambda st: {"v0": sum(L(st, 1, 64)) & M[64]}))
    for t, w in (("8h", 16), ("4s", 32), ("2d", 64)):
        C.append(("sshr v0.%s, v1.%s, #%d" % (t, t, w), two_reg(lambda a, w, st: sx(a, w) >> w, 1, w)))
        C.append(("ushr v0.%s, v1.%s, #3" % (t, t), two_reg(lambda a, w, st: a >> 3, 1, w)))
        C.append(("srshr v0.%s, v1.%s, #%d" % (t, t, w), two_reg(lambda a, w, st: (sx(a, w) + (1 << (w - 1))) >> w, 1, w)))
        C.append(("urshr v0.%s, v1.%s, #5" % (t, t), two_reg(lambda a, w, st: (a + 16) >> 5, 1, w)))
        C.append(("usra v0.%s, v1.%s, #%d" % (t, t, w - 1), lambda st, w=w: {"v0": vec(1, w, lambda k: L(st, 0, w)[k] + (
            L(st, 1, w)[k] >> (w - 1)))}))
        C.append(("shl v0.%s, v1.%s, #%d" % (t, t, w - 1), two_reg(lambda a, w, st: a << (w - 1), 1, w)))
        C.append(("sli v0.%s, v1.%s, #4" % (t, t), lambda st, w=w: {"v0": vec(1, w, lambda k: (L(st, 1, w)[k] << 4) | (
            L(st, 0, w)[k] & 15))}))
        C.append(("sri v0.%s, v1.%s, #%d" % (t, t, w), lambda st: {"v0": st["v"][0]}))
        C.append(("sri v0.%s, v1.%s, #3" % (t, t), lambda st, w=w: {"v0": vec(1, w, lambda k: (L(st, 1, w)[k] >> 3) | (
            L(st, 0, w)[k] & ~(((1 << w) - 1) >> 3)))}))
        C.append(("uqshl v0.%s, v1.%s, #5" % (t, t), two_reg(lambda a, w, st: sat(a << 5, w, False), 1, w)))
        C.append(("sqshl v0.%s, v1.%s, #5" % (t, t), two_reg(lambda a, w, st: sat(sx(a, w) << 5, w, True), 1, w)))
    for nt, t, w in (("8b", "8h", 8), ("4h", "4s", 16), ("2s", "2d", 32)):
        C.append(("xtn v0.%s, v1.%s" % (nt, t), narrow_ref(lambda a, w2: a, w)))
        C.append(("sqxtn v0.%s, v1.%s" % (nt, t), narrow_ref(lambda a, w2, w=w: sat(sx(a, w2), w, True), w)))
        C.append(("sqxtun v0.%s, v1.%s" % (nt, t), narrow_ref(lambda a, w2, w=w: sat(sx(a, w2), w, False), w)))
        C.append(("uqxtn v0.%s, v1.%s" % (nt, t), narrow_ref(lambda a, w2, w=w: sat(a, w, False), w)))
        C.append(("shrn v0.%s, v1.%s, #%d" % (nt, t, w), narrow_ref(lambda a, w2, w=w: a >> w, w)))
        C.append(("rshrn v0.%s, v1.%s, #3" % (nt, t), narrow_ref(lambda a, w2: (a + 4) >> 3, w)))
        C.append(("sqrshrn v0.%s, v1.%s, #%d" % (nt, t, w), narrow_ref(
            lambda a, w2, w=w: sat((sx(a, w2) + (1 << (w - 1))) >> w, w, True), w)))
        C.append(("sqrshrun v0.%s, v1.%s, #2" % (nt, t), narrow_ref(
            lambda a, w2, w=w: sat((sx(a, w2) + 2) >> 2, w, False), w)))
        C.append(("uqshrn v0.%s, v1.%s, #1" % (nt, t), narrow_ref(lambda a, w2, w=w: sat(a >> 1, w, False), w)))
        C.append(("sshll v0.%s, v1.%s, #2" % (t, nt), lambda st, w=w: {"v0": pack(
            [sx(x, w) << 2 for x in L(st, 1, w)[:64 // w]], 2 * w)}))
        C.append(("ushll2 v0.%s, v1.%s, #0" % (t, {8: "16b", 16: "8h", 32: "4s"}[w]), lambda st, w=w: {"v0": pack(
            L(st, 1, w)[64 // w:], 2 * w)}))
        C.append(("smull v0.%s, v1.%s, v2.%s" % (t, nt, nt), lambda st, w=w: {"v0": pack(
            [sx(a, w) * sx(b, w) for a, b in zip(L(st, 1, w)[:64 // w], L(st, 2, w)[:64 // w])], 2 * w)}))
        C.append(("umlal2 v0.%s, v1.%s, v2.%s" % (t, {8: "16b", 16: "8h", 32: "4s"}[w], {8: "16b", 16: "8h", 32: "4s"}[w]),
                  lambda st, w=w: {"v0": pack([d + a * b for d, a, b in zip(
                      L(st, 0, 2 * w), L(st, 1, w)[64 // w:], L(st, 2, w)[64 // w:])], 2 * w)}))
        C.append(("uaddw v0.%s, v1.%s, v2.%s" % (t, t, nt), lambda st, w=w: {"v0": pack(
            [a + b for a, b in zip(L(st, 1, 2 * w), L(st, 2, w)[:64 // w])], 2 * w)}))
        C.append(("raddhn v0.%s, v1.%s, v2.%s" % (nt, t, t), lambda st, w=w: {"v0": pack(
            [(a + b + (1 << (w - 1))) >> w for a, b in zip(L(st, 1, 2 * w), L(st, 2, 2 * w))], w)}))
        if w > 8:
            C.append(("sqdmull v0.%s, v1.%s, v2.%s" % (t, nt, nt), lambda st, w=w: {"v0": pack(
                [sat(2 * sx(a, w) * sx(b, w), 2 * w, True) for a, b in zip(L(st, 1, w)[:64 // w], L(st, 2, w)[:64 // w])],
                2 * w)}))
            C.append(("uaddlp v0.%s, v1.%s" % (t, {16: "8h", 32: "4s"}[w]), lambda st, w=w: {"v0": pack(
                [L(st, 1, w)[2 * k] + L(st, 1, w)[2 * k + 1] for k in range(64 // w)], 2 * w)}))
    C.append(("mul v0.8h, v1.8h, v2.h[5]", lambda st: {"v0": pack([a * L(st, 2, 16)[5] for a in L(st, 1, 16)], 16)}))
    C.append(("smlal2 v0.4s, v1.8h, v2.h[2]", lambda st: {"v0": pack(
        [d + sx(a, 16) * sx(L(st, 2, 16)[2], 16) for d, a in zip(L(st, 0, 32), L(st, 1, 16)[4:])], 32)}))
    C.append(("sqrdmulh v0.4s, v1.4s, v2.s[1]", lambda st: {"v0": pack(
        [sqdmulh_ref(True)(0, a, L(st, 2, 32)[1], 32) for a in L(st, 1, 32)], 32)}))
    C.append(("sqadd s0, s1, s2", lambda st: {"v0": sat(sx(L(st, 1, 32)[0], 32) + sx(L(st, 2, 32)[0], 32), 32, True)}))
    C.append(("cmeq d0, d1, d2", lambda st: {"v0": mask_(L(st, 1, 64)[0] == L(st, 2, 64)[0], 64)}))
    C.append(("sqrshrn h0, s1, #15", lambda st: {"v0": sat((sx(L(st, 1, 32)[0], 32) + (1 << 14)) >> 15, 16, True)}))
    C.append(("fcvtzs s0, s1", lambda st: {"v0": f_to_int(L(st, 1, 32)[0], 32, 32, True, "rz", st["fz"])}))
    C.append(("fabd s0, s1, s2", scalar_fp(lambda a, b, c, w, fz: (lambda r: r if is_nan(r, w) else r & ~(1 << 31))(
        f_add(a, b, w, fz, True)), 32)))
    # the forms lifted last (SQRSHL family by register, PMUL/PMULL, SUQADD/USQADD, FCVTXN, FRECPX, SDOT/UDOT, FP16
    # FNEG/FABS)
    for Q, w, t in ((1, 8, "16b"), (0, 16, "4h"), (1, 32, "4s"), (1, 64, "2d")):
        v3 = "v0.%s, v1.%s, v2.%s" % (t, t, t)
        for mn, signed, rnd in (("sqshl", True, False), ("uqshl", False, False), ("sqrshl", True, True),
                                ("uqrshl", False, True)):
            C.append(("%s %s" % (mn, v3), int_vec(qshl_ref(signed, rnd), Q, w)))
        C.append(("suqadd v0.%s, v1.%s" % (t, t), lambda st, Q=Q, w=w: {"v0": vec(Q, w, lambda k: sat(
            sx(L(st, 0, w)[k], w) + L(st, 1, w)[k], w, True))}))
        C.append(("usqadd v0.%s, v1.%s" % (t, t), lambda st, Q=Q, w=w: {"v0": vec(Q, w, lambda k: sat(
            L(st, 0, w)[k] + sx(L(st, 1, w)[k], w), w, False))}))
    C.append(("sqrshl s0, s1, s2", lambda st: {"v0": qshl_ref(True, True)(0, L(st, 1, 32)[0], L(st, 2, 32)[0], 32)}))
    C.append(("pmul v0.16b, v1.16b, v2.16b", int_vec(lambda d, a, b, w: clmul(a, b) & 0xFF, 1, 8)))
    C.append(("pmull v0.8h, v1.8b, v2.8b", lambda st: {"v0": pack([clmul(a, b) for a, b in zip(
        L(st, 1, 8)[:8], L(st, 2, 8)[:8])], 16)}))
    C.append(("fcvtxn s0, d1", lambda st: {"v0": cvt_odd(L(st, 1, 64)[0], st["fz"])}))
    C.append(("fcvtxn2 v0.4s, v1.2d", lambda st: {"v0": L(st, 0, 64)[0] | pack(
        [cvt_odd(x, st["fz"]) for x in L(st, 1, 64)], 32) << 64}))
    C.append(("frecpx s0, s1", lambda st: {"v0": frecpx_ref(L(st, 1, 32)[0], 32)}))
    C.append(("frecpx d0, d1", lambda st: {"v0": frecpx_ref(L(st, 1, 64)[0], 64)}))
    for mn, signed in (("udot", False), ("sdot", True)):
        for Q, t, bt in ((1, "4s", "16b"), (0, "2s", "8b")):
            C.append(("%s v0.%s, v1.%s, v2.%s" % (mn, t, bt, bt), lambda st, Q=Q, signed=signed: {"v0": vec(
                Q, 32, lambda k: L(st, 0, 32)[k] + sum(
                    (sx(a, 8) if signed else a) * (sx(c, 8) if signed else c)
                    for a, c in zip(L(st, 1, 8)[4 * k:4 * k + 4], L(st, 2, 8)[4 * k:4 * k + 4])))}))
    C.append(("fneg v0.8h, v1.8h", two_reg(lambda a, w, st: a ^ 0x8000, 1, 16)))
    C.append(("fabs v0.4h, v1.4h", two_reg(lambda a, w, st: a & 0x7FFF, 0, 16)))
    # the cryptographic extension (AES-NI and PCLMUL underneath, SHA1/SHA256 from the pseudocode)
    C.append(("aese v0.16b, v1.16b", lambda st: {"v0": aes_ref(st["v"][0] ^ st["v"][1], "e")}))
    C.append(("aesd v0.16b, v1.16b", lambda st: {"v0": aes_ref(st["v"][0] ^ st["v"][1], "d")}))
    C.append(("aesmc v0.16b, v1.16b", lambda st: {"v0": aes_ref(st["v"][1], "mc")}))
    C.append(("aesimc v0.16b, v1.16b", lambda st: {"v0": aes_ref(st["v"][1], "imc")}))
    C.append(("pmull v0.1q, v1.1d, v2.1d", lambda st: {"v0": clmul64(L(st, 1, 64)[0], L(st, 2, 64)[0])}))
    C.append(("pmull2 v0.1q, v1.2d, v2.2d", lambda st: {"v0": clmul64(L(st, 1, 64)[1], L(st, 2, 64)[1])}))
    for k, mn in enumerate(("sha1c", "sha1p", "sha1m")):
        C.append(("%s q0, s1, v2.4s" % mn, lambda st, k=k: {"v0": sha1_ref(L(st, 0, 32), L(st, 1, 32)[0], L(st, 2, 32), k)}))
    C.append(("sha1h s0, s1", lambda st: {"v0": rol32(L(st, 1, 32)[0], 30)}))
    C.append(("sha1su0 v0.4s, v1.4s, v2.4s", lambda st: {"v0": ((L(st, 1, 64)[0] << 64 | L(st, 0, 64)[1]) ^ st["v"][0] ^
                                                               st["v"][2])}))
    C.append(("sha1su1 v0.4s, v1.4s", lambda st: {"v0": sha1su1_ref(st["v"][0], st["v"][1])}))
    C.append(("sha256h q0, q1, v2.4s", lambda st: {"v0": pack(sha256_ref(L(st, 0, 32), L(st, 1, 32), L(st, 2, 32))[0], 32)}))
    C.append(("sha256h2 q0, q1, v2.4s", lambda st: {"v0": pack(sha256_ref(L(st, 1, 32), L(st, 0, 32), L(st, 2, 32))[1], 32)}))
    C.append(("sha256su0 v0.4s, v1.4s", lambda st: {"v0": pack([(ror32(t, 7) ^ ror32(t, 18) ^ (t >> 3)) + d for t, d in zip(
        L(st, 0, 32)[1:] + L(st, 1, 32)[:1], L(st, 0, 32))], 32)}))
    C.append(("sha256su1 v0.4s, v1.4s, v2.4s", lambda st: {"v0": sha256su1_ref(L(st, 0, 32), L(st, 1, 32), L(st, 2, 32))}))
    # MSR FPCR inside a function: the function is strictfp, the second multiply runs in round-toward-zero
    C.append(("fmul s0, s1, s2; mov x0, #12582912; msr fpcr, x0; fmul s3, s1, s2; mrs x1, fpcr",
              lambda st: {"v0": f_mul(L(st, 1, 32)[0], L(st, 2, 32)[0], 32, st["fz"]),
                          "v3": mul_rz(L(st, 1, 32)[0], L(st, 2, 32)[0], 32), "x1": 3 << 22}))
    return C


def qshl_ref(signed, rnd):
    def fn(d, a, b, w):
        s = sx(b & 0xFF, 8)
        x = sx(a, w) if signed else a
        if s >= 0:
            r = x << s
        else:
            r = (x + ((1 << (-s - 1)) if rnd else 0)) >> -s
        return sat(r, w, signed)
    return fn


def clmul(a, b):
    r = 0
    for i in range(8):
        if b >> i & 1:
            r ^= a << i
    return r


def cvt_odd(x, fz):
    """FCVTXN: FPRound to single with round-to-odd (truncate, then set the last bit if inexact)"""
    if is_nan(x, 64):
        return f_cvt(x, 64, 32, fz)
    k, s, v = unpack(x, 64, fz)
    if k == "inf":
        return inf(s, 32)
    if k == "zero":
        return zero(s, 32)
    t = fround(v, 32, fz, "rz")
    _, _, tv = unpack(t, 32, 0)
    if (t & 0x7F800000) == 0x7F800000:
        tv = None
    if tv != v:
        t |= 1
    return t


def frecpx_ref(x, n):
    e, f = FMT[n]
    if is_nan(x, n):
        return quiet(x, n)
    s = x >> (n - 1)
    ex = (x >> f) & ((1 << e) - 1)
    ne = ((1 << e) - 2) if ex == 0 else (~ex & ((1 << e) - 1))
    return s << (n - 1) | ne << f


def _aes_tables():
    def gm(a, b):
        r = 0
        for _ in range(8):
            if b & 1:
                r ^= a
            a = ((a << 1) ^ (0x11B if a & 0x80 else 0)) & 0xFF
            b >>= 1
        return r
    inv = [0] * 256
    for a in range(1, 256):
        for b in range(1, 256):
            if gm(a, b) == 1:
                inv[a] = b
                break
    sbox = []
    for a in range(256):
        x = inv[a]
        y = x
        for k in range(1, 5):
            y ^= ((x << k) | (x >> (8 - k))) & 0xFF
        sbox.append(y ^ 0x63)
    isbox = [0] * 256
    for a, v in enumerate(sbox):
        isbox[v] = a
    return gm, sbox, isbox


GM, SBOX, ISBOX = _aes_tables()


def aes_ref(x, kind):
    """ARM's AESE (after the key xor), AESD, AESMC, AESIMC on the 16 state bytes (byte 4c+r: column c, row r)"""
    s = list(x.to_bytes(16, "little"))
    if kind == "e":   # ShiftRows, SubBytes
        s = [SBOX[s[(4 * (c + r) + r) % 16]] for c in range(4) for r in range(4)]
    elif kind == "d":   # InvShiftRows, InvSubBytes
        s = [ISBOX[s[(4 * (c - r) + r) % 16]] for c in range(4) for r in range(4)]
    else:
        m = [2, 3, 1, 1] if kind == "mc" else [14, 11, 13, 9]
        s = [GM(s[4 * c], m[(0 - r) % 4]) ^ GM(s[4 * c + 1], m[(1 - r) % 4]) ^ GM(s[4 * c + 2], m[(2 - r) % 4]) ^
             GM(s[4 * c + 3], m[(3 - r) % 4]) for c in range(4) for r in range(4)]
    return int.from_bytes(bytes(s), "little")


def clmul64(a, b):
    r = 0
    for i in range(64):
        if b >> i & 1:
            r ^= a << i
    return r


def rol32(x, n):
    return ((x << n) | (x >> (32 - n))) & M[32]


def ror32(x, n):
    return ((x >> n) | (x << (32 - n))) & M[32]


def sha1_ref(X, y, W, kind):
    X = list(X)
    for e in range(4):
        a, b_, c = X[1], X[2], X[3]
        t = [((b_ ^ c) & a) ^ c, a ^ b_ ^ c, (a & b_) | ((a | b_) & c)][kind]
        y = (y + rol32(X[0], 5) + t + W[e]) & M[32]
        X[1] = rol32(X[1], 30)
        X, y = [y] + X[:3], X[3]
    return pack(X, 32)


def sha1su1_ref(d, n):
    t = d ^ (n >> 32)
    tl = lanes_of(t, 32)
    r = [rol32(v, 1) for v in tl]
    r[3] ^= rol32(tl[0], 2)
    return pack(r, 32)


def sha256_ref(X, Y, W):
    """(X, Y) after the four rounds of SHA256hash (part 1 returns X, part 2 Y)"""
    X, Y = list(X), list(Y)
    for e in range(4):
        chs = ((Y[1] ^ Y[2]) & Y[0]) ^ Y[2]
        maj = (X[0] & X[1]) | ((X[0] | X[1]) & X[2])
        s1 = ror32(Y[0], 6) ^ ror32(Y[0], 11) ^ ror32(Y[0], 25)
        s0 = ror32(X[0], 2) ^ ror32(X[0], 13) ^ ror32(X[0], 22)
        t = (Y[3] + s1 + chs + W[e]) & M[32]
        X[3] = (t + X[3]) & M[32]
        Y[3] = (t + s0 + maj) & M[32]
        X, Y = [Y[3]] + X[:3], [X[3]] + Y[:3]
    return X, Y


def sha256su1_ref(d, n, m):
    T0 = n[1:] + m[:1]
    T1 = m[2:]
    r = []
    for e in range(4):
        elt = T1[e] if e < 2 else r[e - 2]
        elt = ror32(elt, 17) ^ ror32(elt, 19) ^ (elt >> 10)
        r.append((elt + d[e] + T0[e]) & M[32])
    return pack(r, 32)


def mul_rz(x, y, n):
    """FMUL with FPCR RMode RZ, FZ clear"""
    (kx, sx_, vx), (ky, sy, vy) = unpack(x, n, 0), unpack(y, n, 0)
    if kx == "num" and ky == "num":
        return fround(vx * vy, n, 0, "rz")
    return f_mul(x, y, n, 0)


def cond_ref(f):
    n, z, c, v = f
    return {"x0": int(not z and n == v), "x1": int(not (c and not z)), "x2": int(n != v), "x3": int(n == v)}


# ---- states
F32 = [0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7FC12345, 0x7FA00000, 0xFF900001,
       0x00000001, 0x80000001, 0x007FFFFF, 0x00800000, 0x80800000, 0x00400000, 0x3F800000, 0xBF800000, 0x3F000000,
       0x3FC00000, 0x40200000, 0x4B000000, 0x4AFFFFFF, 0x4B7FFFFF, 0x3F7FFFFF, 0x4F000000, 0xCF000000, 0x4EFFFFFF,
       0xCF000001, 0x5F000000, 0xDF000000, 0x4F800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x00FFFFFF, 0x3E800000, 0x7E800000,
       0x40400000, 0x00C00000, 0x01000001]
F64 = [0, 1 << 63, 0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0xFFF8000000000000,
       0x7FF4000000000000, 1, 0x800FFFFFFFFFFFFF, 0x0010000000000000, 0x3FF0000000000000, 0xBFF0000000000000,
       0x3FE0000000000000, 0x4330000000000000, 0x432FFFFFFFFFFFFF, 0x41E0000000000000, 0xC1E0000000000000,
       0x41DFFFFFFFC00000, 0xC1E0000000200000, 0x43E0000000000000, 0xC3E0000000000000, 0x43F0000000000000,
       0x7FEFFFFFFFFFFFFF, 0x3FF8000000000000, 0x36A0000000000000, 0x47EFFFFFE0000000, 0x3810000000000000]
INTS = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, (1 << 63) - 1,
        1 << 63, M[64], 3, 5, 15, 16, 17, 31, 32, 33, 63, 64, 0xF0, 0xFFFFFFFE]


def rand_lane(rng, w):
    r = rng.random()
    if w == 32 and r < 0.45:
        return rng.choice(F32)
    if w == 64 and r < 0.45:
        return rng.choice(F64)
    if r < 0.6:
        x = rng.choice([rng.uniform(-4, 4), rng.uniform(-1e5, 1e5), rng.uniform(-1e-3, 1e-3), float(rng.randrange(-9, 9)) + 0.5])
        return struct.unpack("<I", struct.pack("<f", x))[0] if w == 32 else struct.unpack("<Q", struct.pack("<d", x))[0]
    if r < 0.8:
        return rng.choice(INTS) & ((1 << w) - 1)
    return rng.getrandbits(w)


def rand_state(rng):
    v = []
    for _ in range(8):
        w = rng.choice([32, 64, 32, 8, 16])
        if w in (8, 16):
            v.append(rng.getrandbits(128) if rng.random() < 0.5 else pack([rng.choice(INTS) & ((1 << w) - 1)
                                                                          for _ in range(128 // w)], w))
        else:
            v.append(pack([rand_lane(rng, w) for _ in range(128 // w)], w))
    x = [rng.choice(INTS) if rng.random() < 0.5 else rng.getrandbits(64) for _ in range(4)]
    return {"v": v, "x": x, "fz": int(rng.random() < 0.3), "nzcv": [rng.getrandbits(1) for _ in range(4)]}


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <immintrin.h>
typedef struct { uint64_t x[32]; uint64_t pc; uint8_t n, z, c, v; int halted; uint64_t vreg[32][2];
                 uint64_t tpidr, tpidrro, fpcr, fpsr; } Ctx;
#define HOS_FP_TEST
#define A64_NOESC
#include "hos_fp.c"
void a64_trap(Ctx *c, uint64_t pc, uint32_t w) { (void)c; (void)pc; (void)w; }
typedef void (*Fn)(Ctx *);
%(decls)s
static const Fn F[] = {%(fns)s};
static const uint64_t S[][2 * 8 + 4 + 2] = {%(states)s};
int main(void) {
    for (unsigned f = 0; f < sizeof F / sizeof F[0]; f++)
        for (unsigned s = 0; s < sizeof S / sizeof S[0]; s++) {
            Ctx c; memset(&c, 0, sizeof c);
            memcpy(c.vreg, S[s], 128); memcpy(c.x, S[s] + 16, 32);
            c.n = S[s][21] >> 3 & 1; c.z = S[s][21] >> 2 & 1; c.c = S[s][21] >> 1 & 1; c.v = S[s][21] & 1;
            c.fpcr = S[s][20] ? 1u << 24 : 0;
            _mm_setcsr(S[s][20] ? 0x9FC0 : 0x1F80);
            F[f](&c);
            _mm_setcsr(0x1F80);
            printf("%%u %%u", f, s);
            for (int i = 0; i < 4; i++) printf(" %%llx", (unsigned long long)c.x[i]);
            for (int i = 0; i < 8; i++) printf(" %%llx %%llx", (unsigned long long)c.vreg[i][0], (unsigned long long)c.vreg[i][1]);
            printf(" %%d%%d%%d%%d\n", c.n, c.z, c.c, c.v);
        }
    return 0;
}
"""


def subnormal(x, n):
    return (x >> FMT[n][1]) & ((1 << FMT[n][0]) - 1) == 0 and x & ((1 << FMT[n][1]) - 1) != 0


def classify(got, ref, fz, minmax=False, ins=()):
    """None if equal, the allowed-deviation class if one explains every differing lane, else 'bug'. minmax: the
    operation is FMIN/FMAX/FMINNM/FMAXNM (or a reduction or pairwise form of them); ins: its input registers"""
    if got == ref:
        return None
    for lw in (64, 32):
        ok = True
        cls = set()
        for k in range(128 // lw):
            a, b = got >> (lw * k) & M[lw], ref >> (lw * k) & M[lw]
            if a == b:
                continue
            sign = 1 << (lw - 1)
            lanes_in = [x >> (lw * j) & M[lw] for x in ins for j in range(128 // lw)]
            if is_nan(a, lw) and is_nan(b, lw):
                cls.add("nan")
            elif fz and a & sign == b & sign and {a & ~sign, b & ~sign} == {0, 1 << FMT[lw][1]}:
                cls.add("fz-boundary")
            elif minmax and any(is_snan(x, lw) for x in lanes_in):
                cls.add("snan-minmax")
            elif minmax and fz and subnormal(a, lw) and b & ~sign == 0 and a & sign == b & sign:
                cls.add("fz-minmax")
            else:
                ok = False
                break
        if ok:
            return "/".join(sorted(cls))
    return "bug"


@unittest.skipUnless(sys.platform.startswith("linux") and have("clang") and have("llvm-mc"),
                     "Linux, clang and llvm-mc of the pinned LLVM major")
class FPNeon(unittest.TestCase):
    def test_instructions(self):
        C = cases()
        lines = []
        for text, _ in C:
            lines += [s.strip() for s in text.split(";")] + ["ret"]
        r = subprocess.run([llvm.check("llvm-mc"), "-triple=aarch64", "-mattr=" + llvm.MC_ATTRS, "-show-encoding"],
                           input="\n".join(lines), capture_output=True, text=True)
        words = [int.from_bytes(bytes(int(x, 16) for x in m.group(1).split(",")), "little")
                 for m in re.finditer(r"encoding: \[([^\]]*)\]", r.stdout)]
        self.assertEqual(len(words), len(lines), r.stderr[:2000])
        m = emit_llvm.Module(flags_out=True)
        names, k = [], 0
        base = 0x80001000
        for i, (text, _) in enumerate(C):
            n = text.count(";") + 2
            ws = words[k:k + n]
            k += n
            fb = base + 0x1000 * i

            def word(pc, ws=ws, fb=fb):
                return ws[(pc - fb) // 4]
            pcs = [fb + 4 * j for j in range(n)]
            self.assertIsNone(build_ir.scope(fb, pcs, word), text)
            fn = build_ir.build(fb, pcs, word)
            m.add(fn)
            names.append(fn.name)
        rng = random.Random(11)
        states = [rand_state(rng) for _ in range(96)]
        srows = []
        for st in states:
            vv = []
            for q in st["v"]:
                vv += [q & M[64], q >> 64]
            nz = st["nzcv"][0] << 3 | st["nzcv"][1] << 2 | st["nzcv"][2] << 1 | st["nzcv"][3]
            srows.append("{%s}" % ",".join("%#xULL" % x for x in vv + st["x"] + [st["fz"], nz]))
        with tempfile.TemporaryDirectory() as d:
            ll, c, exe = (os.path.join(d, x) for x in ("f.ll", "h.c", "h"))
            with open(ll, "w") as fo:
                fo.write(m.text())
            with open(c, "w") as fo:
                fo.write(HARNESS % {"decls": "\n".join("void %s(Ctx *);" % n for n in names),
                                    "fns": ",".join(names), "states": ",".join(srows)})
            subprocess.run([llvm.check("clang"), "-O1", "-march=x86-64-v3", "-w", "-I" + os.path.join(ROOT, "host"), c, ll, "-o", exe], check=True)
            out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split("\n")
        bugs, devs = [], {}
        for line in out:
            if not line:
                continue
            p = line.split()
            fi, si = int(p[0]), int(p[1])
            x = [int(t, 16) for t in p[2:6]]
            v = [int(p[6 + 2 * j], 16) | int(p[7 + 2 * j], 16) << 64 for j in range(8)]
            nzcv = tuple(int(ch) for ch in p[22])
            text, ref = C[fi]
            st = states[si]
            for key, want in ref(st).items():
                if key == "nzcv":
                    if nzcv != tuple(int(bool(t)) for t in want):
                        bugs.append("%s state %d: NZCV %s, expected %s" % (text, si, nzcv, want))
                    continue
                got = x[int(key[1:])] if key[0] == "x" else v[int(key[1:])]
                want &= M[64] if key[0] == "x" else (1 << 128) - 1
                mm = bool(re.match(r"f(min|max)", text))
                c = classify(got, want, st["fz"], mm, (st["v"][1], st["v"][2]))
                if c == "bug":
                    bugs.append("%s state %d (fz %d): %s = %#x, expected %#x; inputs v1 %#x v2 %#x v3 %#x x1 %#x" % (
                        text, si, st["fz"], key, got, want, st["v"][1], st["v"][2], st["v"][3], st["x"][1]))
                elif c:
                    devs[c] = devs.get(c, 0) + 1
        print("\n%d instructions x %d states; allowed deviations by class (lanes): %s" % (len(C), len(states), devs),
              file=sys.stderr)
        self.assertFalse(bugs, "%d differences outside the allowed deviations (%s); first:\n%s" % (
            len(bugs), devs, "\n".join(bugs[:25])))


HOOK_HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint64_t x[32]; uint64_t pc; uint8_t n, z, c, v; int halted; uint64_t vreg[32][2];
                 uint64_t tpidr, tpidrro, fpcr, fpsr; } Ctx;
static int calls;
/* the host's hook: does the call's work when x1 is 7 (x0 = 99), else lets the lifted body run */
int entry_0x80001000(Ctx *c) { calls++; if (c->x[1] == 7) { c->x[0] = 99; return 1; } return 0; }
void g_80001000(Ctx *);
int main(void) {
    Ctx c; memset(&c, 0, sizeof c); c.x[1] = 7; g_80001000(&c);
    Ctx d; memset(&d, 0, sizeof d); d.x[1] = 3; g_80001000(&d);
    printf("%llu %llu %d\n", (unsigned long long)c.x[0], (unsigned long long)d.x[0], calls);
    return 0;
}
"""


@unittest.skipUnless(sys.platform.startswith("linux") and have("clang"), "Linux and clang of the pinned LLVM major")
class EntryHook(unittest.TestCase):
    def test_entry_hook(self):
        """ENTRY_HOOK at the function's entry: the hook first; 1 returns at once, 0 runs the body (whose loop does not
        call it again: 2 calls for 2 runs)"""
        words = [0xd2800000, 0x91000400, 0xf100241f, 0x54ffffc3, 0xd65f03c0]   # mov x0,#0; add x0,x0,#1; cmp x0,#9; b.lo; ret
        base = 0x80001000
        fn = build_ir.build(base, [base + 4 * k for k in range(len(words))], lambda pc: words[(pc - base) // 4])
        fn.entry_hook = "entry_0x80001000"
        m = emit_llvm.Module()
        m.add(fn)
        with tempfile.TemporaryDirectory() as d:
            ll, c, exe = (os.path.join(d, x) for x in ("f.ll", "h.c", "h"))
            with open(ll, "w") as fo:
                fo.write(m.text())
            with open(c, "w") as fo:
                fo.write(HOOK_HARNESS)
            subprocess.run([llvm.check("clang"), "-O1", "-w", c, ll, "-o", exe], check=True)
            out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual(out, ["99", "9", "2"])


PROBE_HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint64_t x[32]; uint64_t pc; uint8_t n, z, c, v; int halted; uint64_t vreg[32][2];
                 uint64_t tpidr, tpidrro, fpcr, fpsr; } Ctx;
static uint64_t seen_at; static uint32_t seen_bits;
uint32_t hos_frame_whole(uint64_t at, float amount) { seen_at = at; memcpy(&seen_bits, &amount, 4); return 1234; }
void g_80002000(Ctx *);
int main(void) {
    Ctx c; memset(&c, 0, sizeof c); c.x[19] = 0x1000; c.vreg[0][0] = 0x40400000; c.x[12] = 77;
    g_80002000(&c);
    printf("%llu %llx %x %llu\n", (unsigned long long)c.x[12], (unsigned long long)seen_at, seen_bits,
           (unsigned long long)c.x[13]);
    return 0;
}
"""


@unittest.skipUnless(sys.platform.startswith("linux") and have("clang"), "Linux and clang of the pinned LLVM major")
class Probe(unittest.TestCase):
    def test_value_probe(self):
        """a Value probe (nx2/lift2/probes.py): the FCVTZS's result register gets the host function of x19+864 and
        s0 (a float); the other instructions are lifted as usual"""
        from lift2 import probes
        words = [0x1e38000c, 0xd280054d, 0xd65f03c0]   # fcvtzs w12, s0; mov x13, #42; ret
        base = 0x80002000
        word = lambda pc: words[(pc - base) // 4]
        pcs = [base + 4 * k for k in range(len(words))]
        p = probes.Value(base, base, words[0], "hos_frame_whole", ("x19+864", "s0"))
        probes.BY_FUNC[base] = [p]
        try:
            prog = build_ir.Program()
            prog.probes = probes.find(base, pcs, word)
        finally:
            del probes.BY_FUNC[base]
        self.assertEqual(list(prog.probes), [base])
        fn = build_ir.build(base, pcs, word, prog)
        m = emit_llvm.Module()
        m.add(fn)
        with tempfile.TemporaryDirectory() as d:
            ll, c, exe = (os.path.join(d, x) for x in ("f.ll", "h.c", "h"))
            with open(ll, "w") as fo:
                fo.write(m.text())
            with open(c, "w") as fo:
                fo.write(PROBE_HARNESS)
            subprocess.run([llvm.check("clang"), "-O1", "-w", c, ll, "-o", exe], check=True)
            out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual(out, ["1234", "1360", "40400000", "42"])


CTX_H = """#include <stdint.h>
#include <string.h>
typedef struct { uint64_t x[32]; uint64_t pc; uint8_t n, z, c, v; int halted; uint64_t vreg[32][2];
                 uint64_t tpidr, tpidrro, fpcr, fpsr; } Ctx;
"""


@unittest.skipUnless(sys.platform.startswith("linux") and have("clang"), "Linux and clang of the pinned LLVM major")
class BeforeRow(unittest.TestCase):
    def test_before_a_branch(self):
        """an At row before an instruction (f_8007fe60's form): it runs on a TBZ's fall-through, before the CBNZ there,
        and the CBNZ reads the register the row wrote (io x8 += 1, x1 = 0: falls through to the add)"""
        from lift2 import probes
        # tbz w0, #0, #12; cbnz x1, #8; add x8, x8, #100; ret
        words = [0x36000060, 0xb5000041, 0x91019108, 0xd65f03c0]
        base = 0x80003000
        word = lambda pc: words[(pc - base) // 4]
        pcs = [base + 4 * k for k in range(len(words))]
        prog = build_ir.Program()
        real = probes.AT
        probes.AT = [probes.At(base, base + 4, words[1], "t_before", (), before=True, io=("x8", "x1"))]
        try:
            prog.before = probes.at(base, pcs, word, before=True)
        finally:
            probes.AT = real
        self.assertEqual(list(prog.before), [base + 4])
        fn = build_ir.build(base, pcs, word, prog)
        m = emit_llvm.Module()
        m.add(fn)
        harness = r"""
#include "a64.h"
#include <stdio.h>
void g_80003000(Ctx *);
static int calls;
void t_before(uint64_t *io) { calls++; io[0] += 1; io[1] = 0; }
int main(void) {
    for (int k = 0; k < 3; k++) {
        Ctx c; memset(&c, 0, sizeof c); c.x[0] = k != 0; c.x[1] = k == 2 ? 5 : 0; calls = 0;
        g_80003000(&c);
        printf("%llu/%d ", (unsigned long long)c.x[8], calls);
    }
    printf("\n");
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "a64.h"), "w") as fo:
                fo.write(CTX_H)
            ll, c, exe = (os.path.join(d, x) for x in ("f.ll", "h.c", "h"))
            with open(ll, "w") as fo:
                fo.write(m.text())
            with open(c, "w") as fo:
                fo.write(harness)
            subprocess.run([llvm.check("clang"), "-O1", "-w", "-I" + d, c, ll, "-o", exe], check=True)
            out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual(out, ["0/0", "101/1", "101/1"])


if __name__ == "__main__":
    unittest.main()
