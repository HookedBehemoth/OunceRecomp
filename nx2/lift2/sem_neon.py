"""Advanced SIMD (NEON) semantics: templates for enc_neon.py's table, lane-generic over the arrangements.

A vector operand is a lane-typed view of its V register (64-bit arrangements are 64-bit LLVM vectors, which LLVM
widens; the write zeroes the upper half, as ARM's does). Scalar forms use the same operations on scalar types, so
each operation below is written once for "a type": i32 or v4i32, f32 or v4f32. Permutes are shufflevector; TBL/TBX
are pshufb with the out-of-range indices forced to bit 7 by a saturating add; saturating arithmetic is LLVM's .sat
intrinsics; widening/narrowing operations extend to the double-width lane, compute exactly, and truncate. FP lanes
take sem_fp's operations (host FP mode, same fix-ups). FRECPE/FRSQRTE call the out-of-line table routines of
host/hos_fp.c (Arm's estimate algorithm, exact) per lane.
"""
from .ir import WIDTH, lanes, vtype
from .sem_int import getx, setx
from .sem_fp import vread, vwrite, fbin, fma, fsqrt, intr, to_int, fconst, RINT
from . import enc_neon as E


# ---- types
def lane_t(size, fp=False):
    return ("f32" if size == 2 else "f64") if fp else "i%d" % (8 << size)


def VT(size, Q, fp=False):
    return vtype((16 if Q else 8) >> size, lane_t(size, fp))


def shape(ty, lane):
    """ty's shape with another lane type: shape("v4f32", "i32") is v4i32, shape("f32", "i1") is i1"""
    return vtype(lanes(ty)[0], lane) if ty[0] == "v" else lane


def lane_of(ty):
    return lanes(ty)[1] if ty[0] == "v" else ty


def ity(ty):
    return shape(ty, "i%d" % WIDTH[lane_of(ty)])


def widen(ty):
    return shape(ty, "i%d" % (2 * WIDTH[lane_of(ty)]))


def narrow(ty):
    return shape(ty, "i%d" % (WIDTH[lane_of(ty)] // 2))


def K(b, x, ty):
    if ty[0] == "v":
        n, lane = lanes(ty)
        if lane[0] == "f":
            return b.bitcast(b.vconst(x, vtype(n, "i%d" % WIDTH[lane])), ty)
        return b.vconst(x, ty)
    return b.const(x, ty)


def fK(b, value, ty):
    """an FP constant of a scalar or vector FP type"""
    c = fconst(b, value, lane_of(ty))
    return K(b, c.args[0], ty) if ty[0] == "v" else c


def mask(b, pred, x, y, fp=False):
    """a compare as ARM's all-ones/zero lanes"""
    m = b.op("fcmp" if fp else "icmp", shape(x.ty, "i1"), x, y, pred=pred)
    return b.op("sext", ity(x.ty), m)


def call_i(b, name, ty, *args):
    return intr(b, name, ty, *args)


def splat(b, x, ty):
    """scalar x in every lane of ty"""
    n = lanes(ty)[0]
    v = b.insert(K(b, 0, ty), x, 0)
    return b.shuffle(v, v, [0] * n)


def lane_extract(b, n, size, idx, fp=False):
    return b.extract(vread(b, n, vtype(16 >> size, lane_t(size, fp))), idx)


def half(b, n, size, Q):
    """the low (Q=0) or high (Q=1) 64 bits of Vn as a 64-bit vector of size lanes"""
    if not Q:
        return vread(b, n, VT(size, 0))
    return b.bitcast(b.extract(vread(b, n, "v2i64"), 1), VT(size, 0))


def put_half(b, d, r, Q):
    """Vd = r (a 64-bit vector): the low half with the upper zeroed, or (Q, the "2" forms) the upper half with the
    lower kept"""
    if not Q:
        vwrite(b, d, r)
        return
    old = vread(b, d, "v2i64")
    vwrite(b, d, b.insert(old, b.bitcast(r, "i64"), 1))


def ext(b, x, ty, signed):
    return b.op("sext" if signed else "zext", ty, x)


def trunc(b, x, ty):
    return b.op("trunc", ty, x)


def smax_of(w):
    return (1 << (w - 1)) - 1


# ---- integer operations, any integer type
def abd(b, x, y, signed):
    gt = b.op("icmp", shape(x.ty, "i1"), x, y, pred="sgt" if signed else "ugt")
    return b.select(gt, b.bin("sub", x, y), b.bin("sub", y, x))


def halving(b, x, y, signed, sub=False, rnd=False):
    wt = widen(x.ty)
    s = b.bin("sub" if sub else "add", ext(b, x, wt, signed), ext(b, y, wt, signed))
    if rnd:
        s = b.bin("add", s, K(b, 1, wt))
    return trunc(b, b.bin("ashr" if signed else "lshr", s, K(b, 1, wt)), x.ty)


def shl_reg(b, x, m, signed, rnd=False):
    """SSHL/USHL/SRSHL/URSHL: x shifted by the signed low byte of each lane of m (left if positive)"""
    t = x.ty
    w = WIDTH[lane_of(t)]
    mt = shape(t, "i1")
    sh = b.op("sext", t, trunc(b, m, shape(t, "i8"))) if w > 8 else m
    pos = b.op("icmp", mt, sh, K(b, 0, t), pred="sge")
    inr = b.op("icmp", mt, sh, K(b, w, t), pred="slt")
    left = b.select(inr, b.bin("shl", x, sh), K(b, 0, t))
    nsh = b.bin("sub", K(b, 0, t), sh)
    if signed:
        cl = b.call("llvm.umin.%s" % sfx(t), t, nsh, K(b, w - 1, t))
        right = b.bin("ashr", x, cl)
        if rnd:   # (+ the last bit shifted out: (x >> (n-1)) & 1, 0 when n > w)
            n1 = b.bin("sub", nsh, K(b, 1, t))
            bit = b.bin("and", b.bin("lshr", x, b.call("llvm.umin.%s" % sfx(t), t, n1, K(b, w - 1, t))), K(b, 1, t))
            ok = b.op("icmp", mt, n1, K(b, w, t), pred="ult")
            right = b.bin("add", right, b.select(ok, bit, K(b, 0, t)))
            right = b.select(b.op("icmp", mt, nsh, K(b, w, t), pred="ugt"), K(b, 0, t), right)
    else:
        ok = b.op("icmp", mt, nsh, K(b, w, t), pred="ult")
        right = b.select(ok, b.bin("lshr", x, nsh), K(b, 0, t))
        if rnd:
            n1 = b.bin("sub", nsh, K(b, 1, t))
            ok1 = b.op("icmp", mt, n1, K(b, w, t), pred="ult")
            bit = b.select(ok1, b.bin("and", b.bin("lshr", x, n1), K(b, 1, t)), K(b, 0, t))
            right = b.bin("add", right, bit)
    return b.select(pos, left, right)


def sfx(t):
    if t[0] == "v":
        n, lane = lanes(t)
        return "v%d%s" % (n, lane)
    return t


def sat_narrow(b, x, nt, signed_in, signed_out):
    """x (a wide integer type) clamped to the range of the narrow type nt and truncated"""
    w = WIDTH[lane_of(nt)]
    t = x.ty
    if signed_out:
        lo, hi = -(1 << (w - 1)), smax_of(w)
    else:
        lo, hi = 0, (1 << w) - 1
    if signed_in:
        if lo or signed_out:
            x = b.call("llvm.smax.%s" % sfx(t), t, x, K(b, lo, t))
        else:
            x = b.call("llvm.smax.%s" % sfx(t), t, x, K(b, 0, t))
        x = b.call("llvm.smin.%s" % sfx(t), t, x, K(b, hi, t))
    else:
        x = b.call("llvm.umin.%s" % sfx(t), t, x, K(b, hi, t))
    return trunc(b, x, nt)


def sqdmulh(b, x, y, rnd):
    """SQDMULH/SQRDMULH: the high half of 2*x*y (rounded), saturating (only MIN*MIN overflows)"""
    t = x.ty
    w = WIDTH[lane_of(t)]
    wt = widen(t)
    p = b.bin("mul", ext(b, x, wt, True), ext(b, y, wt, True))
    if rnd:
        p = b.bin("add", p, K(b, 1 << (w - 2), wt))
    r = b.bin("ashr", p, K(b, w - 1, wt))
    return sat_narrow(b, r, t, True, True)


def int3(b, mn, x, y, d):
    """the integer three-same operations; d (the destination's old value) for the accumulating ones"""
    t = x.ty
    signed = mn[0] == "s"
    if mn in ("add", "sub", "mul"):
        return b.bin(mn, x, y)
    if mn in ("sqadd", "uqadd", "sqsub", "uqsub"):
        return b.call("llvm.%s%s.sat.%s" % ("s" if signed else "u", mn[2:], sfx(t)), t, x, y)
    if mn in ("cmgt", "cmhi", "cmge", "cmhs", "cmeq"):
        return mask(b, {"cmgt": "sgt", "cmhi": "ugt", "cmge": "sge", "cmhs": "uge", "cmeq": "eq"}[mn], x, y)
    if mn == "cmtst":
        return mask(b, "ne", b.bin("and", x, y), K(b, 0, t))
    if mn in ("smax", "umax", "smin", "umin"):
        return b.call("llvm.%s.%s" % (mn, sfx(t)), t, x, y)
    if mn in ("shadd", "uhadd", "srhadd", "urhadd", "shsub", "uhsub"):
        return halving(b, x, y, signed, "sub" in mn, "rh" in mn)
    if mn in ("sabd", "uabd"):
        return abd(b, x, y, signed)
    if mn in ("saba", "uaba"):
        return b.bin("add", d, abd(b, x, y, signed))
    if mn == "mla":
        return b.bin("add", d, b.bin("mul", x, y))
    if mn == "mls":
        return b.bin("sub", d, b.bin("mul", x, y))
    if mn in ("sshl", "ushl", "srshl", "urshl"):
        return shl_reg(b, x, y, signed, "r" in mn[1:3])
    if mn in ("sqdmulh", "sqrdmulh"):
        return sqdmulh(b, x, y, mn == "sqrdmulh")
    return None


def pairwise(b, x, y, fn):
    """ARM's pairwise operations: the lane pairs of x then y"""
    n = lanes(x.ty)[0]
    ev = b.shuffle(x, y, list(range(0, 2 * n, 2)))
    od = b.shuffle(x, y, list(range(1, 2 * n, 2)))
    return fn(ev, od)


# ---- FP operations, any FP type
def fneg(b, x):
    return b.op("fneg", x.ty, x)


def fabs(b, x):
    return intr(b, "fabs", x.ty, x)


def zero_inf(b, x, y):
    """lanes where one operand is a zero and the other an infinity (FRECPS, FRSQRTS, FMULX's special case); the
    compares honour DAZ, as ARM's FZ flushes the inputs first"""
    mt = shape(x.ty, "i1")
    inf = fK(b, float("inf"), x.ty)
    z = fK(b, 0.0, x.ty)
    ax, ay = fabs(b, x), fabs(b, y)
    xz = b.op("fcmp", mt, x, z, pred="oeq")
    yz = b.op("fcmp", mt, y, z, pred="oeq")
    xi = b.op("fcmp", mt, ax, inf, pred="oeq")
    yi = b.op("fcmp", mt, ay, inf, pred="oeq")
    return b.bin("or", b.bin("and", xz, yi), b.bin("and", xi, yz))


def fp3(b, mn, x, y, d):
    t = x.ty
    if mn in ("fmul", "fdiv", "fadd", "fsub", "fmax", "fmin", "fmaxnm", "fminnm"):
        return fbin(b, ("fmul", "fdiv", "fadd", "fsub", "fmax", "fmin", "fmaxnm", "fminnm").index(mn), x, y)
    if mn == "fmla":
        return fma(b, d, x, y)
    if mn == "fmls":
        return fma(b, d, fneg(b, x), y)
    if mn == "fabd":
        return fabs(b, fbin(b, 3, x, y))
    if mn in ("fcmeq", "fcmge", "fcmgt"):
        return mask(b, {"fcmeq": "oeq", "fcmge": "oge", "fcmgt": "ogt"}[mn], x, y, True)
    if mn in ("facge", "facgt"):
        return mask(b, "oge" if mn == "facge" else "ogt", fabs(b, x), fabs(b, y), True)
    if mn == "frecps":   # 2 - x*y, fused; 0 * inf gives 2
        r = fma(b, fK(b, 2.0, t), fneg(b, x), y)
        return b.select(zero_inf(b, x, y), fK(b, 2.0, t), r)
    if mn == "frsqrts":   # (3 - x*y) / 2, fused; 0 * inf gives 1.5
        r = b.op("fmul", t, fma(b, fK(b, 3.0, t), fneg(b, x), y), fK(b, 0.5, t))
        return b.select(zero_inf(b, x, y), fK(b, 1.5, t), r)
    if mn == "fmulx":   # 0 * inf gives 2 with the sign of the product
        r = b.op("fmul", t, x, y)
        sgn = b.bin("and", b.bin("xor", b.bitcast(x, ity(t)), b.bitcast(y, ity(t))),
                    K(b, 1 << (WIDTH[lane_of(t)] - 1), ity(t)))
        two = b.bitcast(b.bin("or", b.bitcast(fK(b, 2.0, t), ity(t)), sgn), t)
        return b.select(zero_inf(b, x, y), two, r)
    return None


def fp_pair(b, mn, x, y):
    k = {"faddp": "fadd", "fmaxp": "fmax", "fminp": "fmin", "fmaxnmp": "fmaxnm", "fminnmp": "fminnm"}[mn]
    return pairwise(b, x, y, lambda e, o: fp3(b, k, e, o, None))


def int_pair(b, mn, x, y):
    k = {"addp": "add", "smaxp": "smax", "umaxp": "umax", "sminp": "smin", "uminp": "umin"}[mn]
    return pairwise(b, x, y, lambda e, o: int3(b, k, e, o, None))


def estimate(b, x, fn):
    """FRECPE/FRSQRTE: host/hos_fp.c's routine for each lane (a64_frecpe_f32 ...)"""
    lt = lane_of(x.ty)
    name = "a64_%s_%s" % (fn, lt)
    if x.ty[0] != "v":
        return b.call(name, lt, x)
    r = x
    for k in range(lanes(x.ty)[0]):
        r = b.insert(r, b.call(name, lt, b.extract(x, k)), k)
    return r


CVT2 = {"fcvtns": ("roundeven", True), "fcvtms": ("floor", True), "fcvtas": ("round", True),
        "fcvtps": ("ceil", True), "fcvtzs": (None, True), "fcvtnu": ("roundeven", False), "fcvtmu": ("floor", False),
        "fcvtau": ("round", False), "fcvtpu": ("ceil", False), "fcvtzu": (None, False)}
FRINT2 = {"frintn": "roundeven", "frintm": "floor", "frintp": "ceil", "frintz": "trunc", "frinta": "round",
          "frintx": "rint", "frinti": "nearbyint"}


def fp2(b, mn, x):
    t = x.ty
    if mn in FRINT2:
        return intr(b, FRINT2[mn], t, x)
    if mn in CVT2:
        rnd, signed = CVT2[mn]
        return to_int(b, x, ity(t), signed, rnd)
    if mn == "fabs":
        return fabs(b, x)
    if mn == "fneg":
        return fneg(b, x)
    if mn == "fsqrt":
        return fsqrt(b, x)
    if mn in ("frecpe", "frsqrte"):
        return estimate(b, x, mn)
    if mn in ("fcmgt", "fcmeq", "fcmlt", "fcmge", "fcmle"):
        return mask(b, {"fcmgt": "ogt", "fcmeq": "oeq", "fcmlt": "olt", "fcmge": "oge", "fcmle": "ole"}[mn], x,
                    fK(b, 0.0, t), True)
    return None


# ---- templates
def t_simd_copy(b, f, pc, p):
    kind, size = E.copy_info(f["Q"], f["op"], f["imm5"], f["imm4"])
    idx = f["imm5"] >> (size + 1)
    lt = lane_t(size)
    if kind == "dup_elem":
        vwrite(b, f["Rd"], splat(b, lane_extract(b, f["Rn"], size, idx), VT(size, f["Q"])))
    elif kind == "dup_gen":
        x = getx(b, f["Rn"], 1)
        vwrite(b, f["Rd"], splat(b, trunc(b, x, lt) if size < 3 else x, VT(size, f["Q"])))
    elif kind in ("smov", "umov"):
        x = lane_extract(b, f["Rn"], size, idx)
        if kind == "smov":
            x = ext(b, x, "i64" if f["Q"] else "i32", True)
        setx(b, f["Rd"], x)
    else:
        vt = vtype(16 >> size, lt)
        if kind == "ins_gen":
            x = getx(b, f["Rn"], 1)
            x = trunc(b, x, lt) if size < 3 else x
        else:
            x = lane_extract(b, f["Rn"], size, f["imm4"] >> size)
        vwrite(b, f["Rd"], b.insert(vread(b, f["Rd"], vt), x, idx))


def t_simd_dups(b, f, pc, p):
    size = E.lowest(f["imm5"])
    vwrite(b, f["Rd"], lane_extract(b, f["Rn"], size, f["imm5"] >> (size + 1)))


def operand_types(f, size, fp, scalar):
    return lane_t(size, fp) if scalar else VT(size, f["Q"], fp)


def t_simd_3same(b, f, pc, p):
    sc = p["scalar"]
    mn, kind, size = E.same3_info(1 if sc else f["Q"], f["U"], f["size"], f["opcode"], sc)
    if kind == "l":
        t = VT(0, f["Q"])
        n, m, d = vread(b, f["Rn"], t), vread(b, f["Rm"], t), vread(b, f["Rd"], t)
        if mn in ("and", "orr", "eor"):
            r = b.bin({"and": "and", "orr": "or", "eor": "xor"}[mn], n, m)
        elif mn in ("bic", "orn"):
            r = b.bin("and" if mn == "bic" else "or", n, b.not_(m))
        elif mn == "bsl":
            r = b.bin("or", b.bin("and", d, n), b.bin("and", b.not_(d), m))
        elif mn == "bit":
            r = b.bin("or", b.bin("and", n, m), b.bin("and", d, b.not_(m)))
        else:
            r = b.bin("or", b.bin("and", d, m), b.bin("and", n, b.not_(m)))
        vwrite(b, f["Rd"], r)
        return
    fp = kind == "f"
    t = operand_types(f, size, fp, sc)
    n, m = vread(b, f["Rn"], t), vread(b, f["Rm"], t)
    d = vread(b, f["Rd"], t) if mn in ("saba", "uaba", "mla", "mls", "fmla", "fmls") else None
    if mn in ("faddp", "fmaxp", "fminp", "fmaxnmp", "fminnmp"):
        r = fp_pair(b, mn, n, m)
    elif mn in ("addp", "smaxp", "umaxp", "sminp", "uminp"):
        r = int_pair(b, mn, n, m)
    elif fp:
        r = fp3(b, mn, n, m, d)
    else:
        r = int3(b, mn, n, m, d)
    vwrite(b, f["Rd"], r)


def t_simd_3diff(b, f, pc, p):
    mn, kind, size = E.diff3_info(f["U"], f["size"], f["opcode"])
    Q, U = f["Q"], f["U"]
    signed = not U
    wt = VT(size + 1, 1)
    if kind == "n":   # ADDHN/SUBHN/RADDHN/RSUBHN: the high halves of the wide sums
        x, y = vread(b, f["Rn"], wt), vread(b, f["Rm"], wt)
        s = b.bin("sub" if "sub" in mn else "add", x, y)
        w = 8 << size
        if mn[0] == "r":
            s = b.bin("add", s, K(b, 1 << (w - 1), wt))
        put_half(b, f["Rd"], trunc(b, b.bin("lshr", s, K(b, w, wt)), VT(size, 0)), Q)
        return
    y = ext(b, half(b, f["Rm"], size, Q), wt, signed)
    if kind == "w":
        x = vread(b, f["Rn"], wt)
        vwrite(b, f["Rd"], b.bin("sub" if "sub" in mn else "add", x, y))
        return
    xn, yn = half(b, f["Rn"], size, Q), half(b, f["Rm"], size, Q)
    x = ext(b, xn, wt, signed)
    d = vread(b, f["Rd"], wt)
    if mn in ("saddl", "uaddl"):
        r = b.bin("add", x, y)
    elif mn in ("ssubl", "usubl"):
        r = b.bin("sub", x, y)
    elif mn in ("sabdl", "uabdl"):
        r = b.op("zext", wt, abd(b, xn, yn, signed))
    elif mn in ("sabal", "uabal"):
        r = b.bin("add", d, b.op("zext", wt, abd(b, xn, yn, signed)))
    elif mn in ("smull", "umull"):
        r = b.bin("mul", x, y)
    elif mn in ("smlal", "umlal"):
        r = b.bin("add", d, b.bin("mul", x, y))
    elif mn in ("smlsl", "umlsl"):
        r = b.bin("sub", d, b.bin("mul", x, y))
    elif mn in ("sqdmull", "sqdmlal", "sqdmlsl"):
        pr = b.bin("mul", x, y)
        r = b.call("llvm.sadd.sat.%s" % sfx(wt), wt, pr, pr)
        if mn != "sqdmull":
            r = b.call("llvm.s%s.sat.%s" % ("add" if mn == "sqdmlal" else "sub", sfx(wt)), wt, d, r)
    else:
        raise NotImplementedError(mn)
    vwrite(b, f["Rd"], r)


def rev(b, x, group):
    """REV16/32/64: the lanes of x reversed in groups of group lanes"""
    n = lanes(x.ty)[0]
    return b.shuffle(x, x, [g + group - 1 - k for g in range(0, n, group) for k in range(group)])


def t_simd_2misc(b, f, pc, p):
    sc = p["scalar"]
    Q = 1 if sc else f["Q"]
    mn, kind, size = E.misc2_info(Q, f["U"], f["size"], f["opcode"], sc)
    if kind in ("f", "fz"):
        t = operand_types(f, size, True, sc)
        x = vread(b, f["Rn"], t)
        if mn in ("scvtf", "ucvtf"):
            x = vread(b, f["Rn"], ity(t))
            r = b.cast("sitofp" if mn == "scvtf" else "uitofp", x, t)
        else:
            r = fp2(b, mn, x)
        vwrite(b, f["Rd"], r)
        return
    if kind == "fn":   # FCVTN{2}: double to single, single to half, into the low or upper half
        x = vread(b, f["Rn"], "v2f64" if size == 3 else "v4f32")
        put_half(b, f["Rd"], b.cast("fptrunc", x, "v2f32" if size == 3 else "v4f16"), Q)
        return
    if kind == "fl":   # FCVTL{2}
        x = b.bitcast(half(b, f["Rn"], 0, Q), "v2f32" if size == 3 else "v4f16")
        vwrite(b, f["Rd"], b.cast("fpext", x, "v2f64" if size == 3 else "v4f32"))
        return
    if kind == "fx":
        raise NotImplementedError(mn)
    if kind == "n":
        if sc:
            x = vread(b, f["Rn"], lane_t(size + 1))
            nt = lane_t(size)
        else:
            x = vread(b, f["Rn"], VT(size + 1, 1))
            nt = VT(size, 0)
        if mn == "xtn":
            r = trunc(b, x, nt)
        else:
            r = sat_narrow(b, x, nt, mn != "uqxtn", mn == "sqxtn")
        if sc:
            vwrite(b, f["Rd"], r)
        else:
            put_half(b, f["Rd"], r, Q)
        return
    if kind == "s":   # SHLL{2}
        wt = VT(size + 1, 1)
        vwrite(b, f["Rd"], b.bin("shl", b.op("zext", wt, half(b, f["Rn"], size, Q)), K(b, 8 << size, wt)))
        return
    if kind == "p":   # [SU]ADDLP, [SU]ADALP
        t = VT(size, Q)
        wt = vtype(lanes(t)[0] // 2, lane_t(size + 1))
        x = vread(b, f["Rn"], t)
        signed = mn[0] == "s"
        n = lanes(t)[0]
        ev = ext(b, b.shuffle(x, x, list(range(0, n, 2))), wt, signed)
        od = ext(b, b.shuffle(x, x, list(range(1, n, 2))), wt, signed)
        r = b.bin("add", ev, od)
        if "adal" in mn:
            r = b.bin("add", vread(b, f["Rd"], wt), r)
        vwrite(b, f["Rd"], r)
        return
    t = lane_t(size) if sc else VT(size, Q)
    if mn == "rbit":
        t = VT(0, Q)
    x = vread(b, f["Rn"], t)
    w = WIDTH[lane_of(t)]
    if mn in ("rev64", "rev32", "rev16"):
        r = rev(b, x, int(mn[3:]) // w)
    elif mn == "cnt":
        r = call_i(b, "ctpop", t, x)
    elif mn == "clz":
        r = b.call("llvm.ctlz.%s" % sfx(t), t, x, b.const(0, "i1"))
    elif mn == "cls":
        y = b.bin("xor", x, b.bin("ashr", x, K(b, w - 1, t)))
        r = b.bin("sub", b.call("llvm.ctlz.%s" % sfx(t), t, y, b.const(0, "i1")), K(b, 1, t))
    elif mn == "not":
        r = b.not_(x)
    elif mn == "rbit":
        r = call_i(b, "bitreverse", t, x)
    elif mn == "abs":
        r = b.call("llvm.abs.%s" % sfx(t), t, x, b.const(0, "i1"))
    elif mn == "neg":
        r = b.bin("sub", K(b, 0, t), x)
    elif mn in ("sqabs", "sqneg"):
        mn_ = K(b, 1 << (w - 1), t)
        a = b.call("llvm.abs.%s" % sfx(t), t, x, b.const(0, "i1")) if mn == "sqabs" else b.bin("sub", K(b, 0, t), x)
        r = b.select(b.op("icmp", shape(t, "i1"), x, mn_, pred="eq"), K(b, smax_of(w), t), a)
    elif kind == "z":
        r = mask(b, {"cmgt": "sgt", "cmeq": "eq", "cmlt": "slt", "cmge": "sge", "cmle": "sle"}[mn], x, K(b, 0, t))
    else:
        raise NotImplementedError(mn)
    vwrite(b, f["Rd"], r)


def tree(b, xs, fn):
    while len(xs) > 1:
        xs = [fn(xs[k], xs[k + 1]) for k in range(0, len(xs), 2)]
    return xs[0]


def t_simd_across(b, f, pc, p):
    mn, kind, size = E.across_info(f["Q"], f["U"], f["size"], f["opcode"])
    if kind == "f":   # (ARM's order: pairs of pairs)
        x = vread(b, f["Rn"], "v4f32")
        k = mn[:-1]
        vwrite(b, f["Rd"], tree(b, [b.extract(x, i) for i in range(4)], lambda u, w: fp3(b, k, u, w, None)))
        return
    t = VT(size, f["Q"])
    x = vread(b, f["Rn"], t)
    if mn in ("saddlv", "uaddlv"):
        wt = shape(t, lane_t(size + 1))
        r = b.call("llvm.vector.reduce.add.%s" % sfx(wt), lane_t(size + 1), ext(b, x, wt, mn[0] == "s"))
    else:
        op = {"addv": "add", "smaxv": "smax", "umaxv": "umax", "sminv": "smin", "uminv": "umin"}[mn]
        r = b.call("llvm.vector.reduce.%s.%s" % (op, sfx(t)), lane_t(size), x)
    vwrite(b, f["Rd"], r)


def t_simd_spair(b, f, pc, p):
    mn, kind, size = E.pair_info(f["U"], f["size"], f["opcode"])
    t = "v2f32" if (kind == "f" and size == 2) else "v2f64" if kind == "f" else "v2i64"
    x = vread(b, f["Rn"], t)
    e0, e1 = b.extract(x, 0), b.extract(x, 1)
    if kind == "f":
        r = fp3(b, {"faddp": "fadd", "fmaxp": "fmax", "fminp": "fmin", "fmaxnmp": "fmaxnm", "fminnmp": "fminnm"}[mn],
                e0, e1, None)
    else:
        r = b.bin("add", e0, e1)
    vwrite(b, f["Rd"], r)


def t_simd_perm(b, f, pc, p):
    t = VT(f["size"], f["Q"])
    x, y = vread(b, f["Rn"], t), vread(b, f["Rm"], t)
    n = lanes(t)[0]
    o = f["opcode"]
    part = o >> 2   # (1 for the "2" forms)
    if o & 3 == 1:     # UZP
        mask_ = [2 * i + part for i in range(n)]
    elif o & 3 == 2:   # TRN
        mask_ = [k for i in range(0, n, 2) for k in (i + part, n + i + part)]
    else:              # ZIP
        base = part * n // 2
        mask_ = [k for i in range(n // 2) for k in (base + i, n + base + i)]
    vwrite(b, f["Rd"], b.shuffle(x, y, mask_))


def t_simd_ext(b, f, pc, p):
    t = VT(0, f["Q"])
    x, y = vread(b, f["Rn"], t), vread(b, f["Rm"], t)
    n = lanes(t)[0]
    vwrite(b, f["Rd"], b.shuffle(x, y, [f["imm4"] + k for k in range(n)]))


def t_simd_tbl(b, f, pc, p):
    """TBL/TBX: pshufb per table register, indices made relative to it; a saturating add of 0x70 sets bit 7 of
    every index past 15 (pshufb's zeroing bit) and keeps the low four"""
    Q = f["Q"]
    idx = vread(b, f["Rm"], "v16i8") if Q else b.shuffle(vread(b, f["Rm"], "v8i8"), vread(b, f["Rm"], "v8i8"),
                                                          list(range(8)) + [-1] * 8)
    r = None
    for k in range(f["len"] + 1):
        tab = vread(b, (f["Rn"] + k) % 32, "v16i8")
        i = b.bin("sub", idx, K(b, 16 * k, "v16i8")) if k else idx
        i = b.call("llvm.uadd.sat.v16i8", "v16i8", i, K(b, 0x70, "v16i8"))
        s = b.call("llvm.x86.ssse3.pshuf.b.128", "v16i8", tab, i)
        r = s if r is None else b.bin("or", r, s)
    if f["op"]:   # TBX: lanes whose index is past the table keep the destination's
        d = vread(b, f["Rd"], "v16i8")
        out = b.op("icmp", "v16i1", idx, K(b, 16 * (f["len"] + 1), "v16i8"), pred="uge")
        r = b.select(out, d, r)
    if not Q:
        r = b.shuffle(r, r, list(range(8)))
    vwrite(b, f["Rd"], r)


def t_simd_modimm(b, f, pc, p):
    mn, t, sk = E.modimm_info(f["Q"], f["op"], f["cmode"], f["o2"])
    imm8 = f["abc"] << 5 | f["defgh"]
    val = E.modimm_value(f["op"], f["cmode"], imm8)
    if mn == "mvni":
        val = ~val & ((1 << 64) - 1)
    c = b.vconst((val, val) if f["Q"] else (val, 0), "v2i64")
    if mn in ("orr", "bic"):
        d = vread(b, f["Rd"], "v2i64")
        if mn == "bic":
            c = b.vconst(((~val) & ((1 << 64) - 1),) * 2, "v2i64") if f["Q"] else \
                b.vconst(((~val) & ((1 << 64) - 1), 0), "v2i64")
            r = b.bin("and", d, c)
        else:
            r = b.bin("or", d, c)
        if not f["Q"]:
            r = b.insert(b.zero("v2i64"), b.extract(r, 0), 0)
        vwrite(b, f["Rd"], r)
        return
    vwrite(b, f["Rd"], c)


def rshift(b, x, sh, signed, rnd):
    """x >> sh for 1 <= sh <= the lane width (ARM's shift right: ASR fills with the sign, LSR of the full width
    is 0), rounded when rnd: + bit sh-1"""
    t = x.ty
    w = WIDTH[lane_of(t)]
    if signed:
        r = b.bin("ashr", x, K(b, min(sh, w - 1), t))
    else:
        r = b.bin("lshr", x, K(b, sh, t)) if sh < w else K(b, 0, t)
    if rnd:
        r = b.bin("add", r, b.bin("and", b.bin("lshr", x, K(b, sh - 1, t)), K(b, 1, t)))
    return r


def t_simd_shift(b, f, pc, p):
    sc = p["scalar"]
    Q = 1 if sc else f["Q"]
    mn, kind, esz = E.shift_info(Q, f["U"], f["immh"], f["opcode"], sc)
    sh = E.shift_amount(kind, esz, f["immh"], f["immb"])
    U = f["U"]
    if kind == "f":
        ft = lane_t(esz, True) if sc else VT(esz, Q, True)
        if mn in ("scvtf", "ucvtf"):
            x = b.cast("sitofp" if mn == "scvtf" else "uitofp", vread(b, f["Rn"], ity(ft)), ft)
            r = b.op("fmul", ft, x, fK(b, 2.0 ** -sh, ft))
        else:
            r = to_int(b, b.op("fmul", ft, vread(b, f["Rn"], ft), fK(b, 2.0 ** sh, ft)), ity(ft), mn == "fcvtzs")
        vwrite(b, f["Rd"], r)
        return
    if kind == "w":
        wt = VT(esz + 1, 1)
        x = ext(b, half(b, f["Rn"], esz, Q), wt, not U)
        vwrite(b, f["Rd"], b.bin("shl", x, K(b, sh, wt)) if sh else x)
        return
    if kind == "n":
        wt = lane_t(esz + 1) if sc else VT(esz + 1, 1)
        nt = lane_t(esz) if sc else VT(esz, 0)
        x = vread(b, f["Rn"], wt)
        rnd = mn in ("rshrn", "sqrshrn", "uqrshrn", "sqrshrun")
        if mn in ("shrn", "rshrn"):
            if rnd:
                x = b.bin("add", x, K(b, 1 << (sh - 1), wt))
            r = trunc(b, b.bin("lshr", x, K(b, sh, wt)), nt)
        else:
            signed_in = mn.startswith("sq")
            r = sat_narrow(b, rshift(b, x, sh, signed_in, rnd), nt, signed_in, mn in ("sqshrn", "sqrshrn"))
        if sc:
            vwrite(b, f["Rd"], r)
        else:
            put_half(b, f["Rd"], r, Q)
        return
    t = lane_t(esz) if sc else VT(esz, Q)
    x = vread(b, f["Rn"], t)
    w = 8 << esz
    if kind == "r":
        if mn == "sri":
            d = vread(b, f["Rd"], t)
            keep = ~(((1 << w) - 1) >> sh) & ((1 << w) - 1)
            r = b.bin("or", b.bin("and", d, K(b, keep, t)), rshift(b, x, sh, False, False))
        else:
            r = rshift(b, x, sh, not U, mn[1] == "r")
            if mn.endswith("sra"):
                r = b.bin("add", vread(b, f["Rd"], t), r)
    else:
        if mn == "shl":
            r = b.bin("shl", x, K(b, sh, t))
        elif mn == "sli":
            d = vread(b, f["Rd"], t)
            r = b.bin("or", b.bin("and", d, K(b, (1 << sh) - 1, t)), b.bin("shl", x, K(b, sh, t)))
        else:   # SQSHL/UQSHL/SQSHLU by immediate: saturate where the shift loses bits
            s = b.bin("shl", x, K(b, sh, t))
            mt = shape(t, "i1")
            if mn == "uqshl":
                lost = b.op("icmp", mt, b.bin("lshr", s, K(b, sh, t)), x, pred="ne")
                r = b.select(lost, K(b, (1 << w) - 1, t), s)
            elif mn == "sqshl":
                lost = b.op("icmp", mt, b.bin("ashr", s, K(b, sh, t)), x, pred="ne")
                neg = b.op("icmp", mt, x, K(b, 0, t), pred="slt")
                r = b.select(lost, b.select(neg, K(b, 1 << (w - 1), t), K(b, smax_of(w), t)), s)
            else:
                lost = b.op("icmp", mt, b.bin("lshr", s, K(b, sh, t)), x, pred="ne")
                neg = b.op("icmp", mt, x, K(b, 0, t), pred="slt")
                r = b.select(neg, K(b, 0, t), b.select(lost, K(b, (1 << w) - 1, t), s))
    vwrite(b, f["Rd"], r)


def t_simd_elem(b, f, pc, p):
    sc = p["scalar"]
    Q = 1 if sc else f["Q"]
    mn, kind, size = E.elem_info(Q, f["U"], f["size"], f["L"], f["opcode"], sc)
    rm, idx = E.elem_index(size, f["L"], f["M"], f["Rm"], f["H"])
    fp = kind == "f"
    e = lane_extract(b, rm, size, idx, fp)
    if kind == "l":
        wt = VT(size + 1, 1)
        signed = not f["U"]
        xn = half(b, f["Rn"], size, Q)
        x = ext(b, xn, wt, signed)
        y = ext(b, splat(b, e, VT(size, 0)), wt, signed)
        d = vread(b, f["Rd"], wt)
        pr = b.bin("mul", x, y)
        if mn.startswith("sqd"):
            pr = b.call("llvm.sadd.sat.%s" % sfx(wt), wt, pr, pr)
            if mn != "sqdmull":
                pr = b.call("llvm.s%s.sat.%s" % ("add" if mn == "sqdmlal" else "sub", sfx(wt)), wt, d, pr)
            r = pr
        elif mn.endswith("mull"):
            r = pr
        elif mn.endswith("mlal"):
            r = b.bin("add", d, pr)
        else:
            r = b.bin("sub", d, pr)
        vwrite(b, f["Rd"], r)
        return
    t = lane_t(size, fp) if sc else VT(size, Q, fp)
    x = vread(b, f["Rn"], t)
    y = e if sc else splat(b, e, t)
    d = vread(b, f["Rd"], t) if mn in ("mla", "mls", "fmla", "fmls") else None
    vwrite(b, f["Rd"], fp3(b, mn, x, y, d) if fp else int3(b, mn, x, y, d))


TEMPLATES = {k[2:]: v for k, v in globals().items() if k.startswith("t_")}
