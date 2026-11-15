"""FP scalar semantics, vector/FP register loads and stores, FPCR/FPSR access: templates for enc_fp.py's table.

Host FP mode (docs/lifter.md section 8): FP arithmetic is LLVM's native FP operations and intrinsics, which
the code generator makes SSE/AVX/FMA3 instructions. Rounding and flush-to-zero come from the host's MXCSR, which the
runtime keeps equal to the guest FPCR (RMode -> RC, FZ -> DAZ|FTZ). No FPSR bookkeeping: MRS/MSR FPSR and FPCR are
calls of runtime helpers. The fix-ups are where x86 values differ from ARM's in ways a program sees: FMIN/FMAX and
FMINNM/FMAXNM (signed zeros, NaNs: LLVM's minimum/maximum and minimumnum/maximumnum), FCVT*S/U (saturating
conversions, NaN to 0), the FRINT* and FCVT[NPMA]* rounding modes (rounding intrinsics), FCMP's unordered flags.
The allowed deviations from ARM are listed in docs/lifter.md section 8.1; LIFT2_DEFAULT_NAN=1 at lift time replaces a
NaN generated from non-NaN operands by ARM's default NaN (positive) instead of x86's (negative).

V registers are the v128 variables "v0".."v31". A scalar write zeroes the rest of the register (ARM's V[d] = ZeroExtend),
as vwrite() does; LLVM drops the zeroing where only lane 0 is read again.
"""
import os

from .ir import WIDTH, lanes, vtype
from . import sem_int
from .sem_int import getx, setx, set_nzcv, cond, extend, sx, store

FTY = {0: "f32", 1: "f64", 3: "f16"}
ITY = {"f16": "i16", "f32": "i32", "f64": "i64"}
DEFAULT_NAN = os.environ.get("LIFT2_DEFAULT_NAN") == "1"
QNAN = {"f16": 0x7E00, "f32": 0x7FC00000, "f64": 0x7FF8000000000000}


# ---- V register access
def vread(b, n, ty):
    """V register n as ty: a 128-bit type is the whole register, a 64-bit vector type the low half, a scalar lane 0"""
    v = b.get("v%d" % n)
    w = WIDTH[ty]
    if w == 128:
        return b.bitcast(v, ty)
    if ty[0] == "v":
        return b.bitcast(b.extract(b.bitcast(v, "v2i64"), 0), ty)
    return b.extract(b.bitcast(v, vtype(128 // w, ty)), 0)


def vwrite(b, n, val):
    """V register n = val zero-extended to 128 bits"""
    w = WIDTH[val.ty]
    if w == 128:
        b.set("v%d" % n, b.bitcast(val, "v128"))
        return
    x = b.bitcast(val, "i%d" % w) if val.ty[0] != "i" else val
    x = b.zext(x, "i64")
    b.set("v%d" % n, b.insert(b.zero("v128"), x, 0))


def fconst(b, value, ty):
    """an FP constant from a Python float (exact for the powers of two and small values used here)"""
    import struct
    if ty == "f32":
        bits = struct.unpack("<I", struct.pack("<f", value))[0]
    elif ty == "f64":
        bits = struct.unpack("<Q", struct.pack("<d", value))[0]
    else:
        bits = struct.unpack("<H", struct.pack("<e", value))[0]
    return b.const(bits, ty)


def suffix(ty):
    """an LLVM intrinsic's type suffix: f32, v4f32"""
    if ty[0] == "v":
        n, lane = lanes(ty)
        return "v%d%s" % (n, lane)
    return ty


def intr(b, name, ty, *args):
    return b.call("llvm.%s.%s" % (name, suffix(ty)), ty, *args)


def nan_fix(b, r, *ins):
    """LIFT2_DEFAULT_NAN: a NaN result from non-NaN operands becomes ARM's default NaN (x86 makes it negative)"""
    if not DEFAULT_NAN:
        return r
    gen = b.op("fcmp", "i1" if r.ty[0] != "v" else vtype(lanes(r.ty)[0], "i1"), r, r, pred="uno")
    for x in ins:
        gen = b.bin("and", gen, b.op("fcmp", gen.ty, x, x, pred="ord"))
    lane = lanes(r.ty)[1] if r.ty[0] == "v" else r.ty
    q = b.vconst(QNAN[lane], vtype(lanes(r.ty)[0], ITY[lane])) if r.ty[0] == "v" else b.const(QNAN[lane], ITY[lane])
    return b.select(gen, b.bitcast(q, r.ty), r)


# ---- arithmetic shared with the vector forms (ty a scalar or vector FP type)
def fbin(b, k, x, y):
    """FMUL FDIV FADD FSUB FMAX FMIN FMAXNM FMINNM FNMUL (enc_fp.DP2's order)"""
    ty = x.ty
    if k <= 3:
        r = b.op(("fmul", "fdiv", "fadd", "fsub")[k], ty, x, y)
        return nan_fix(b, r, x, y)
    if k == 8:
        return b.op("fneg", ty, nan_fix(b, b.op("fmul", ty, x, y), x, y))
    return intr(b, ("maximum", "minimum", "maximumnum", "minimumnum")[k - 4], ty, x, y)


def fma(b, a, x, y):
    """a + x * y, fused"""
    return nan_fix(b, intr(b, "fma", a.ty, x, y, a), a, x, y)


def fsqrt(b, x):
    return nan_fix(b, intr(b, "sqrt", x.ty, x), x)


RINT = {8: "roundeven", 9: "ceil", 10: "floor", 11: "trunc", 12: "round", 14: "rint", 15: "nearbyint"}


def to_int(b, x, ity, signed, rnd=None):
    """FCVT[NPMZA][SU]: rounded by rnd (an intrinsic name, None for toward zero), then the saturating conversion
    (NaN gives 0, out of range the nearest end: ARM's FPToFixed)"""
    if rnd:
        x = intr(b, rnd, x.ty, x)
    sfx = "%s.%s" % (suffix(ity), suffix(x.ty))
    return b.call("llvm.fpto%si.sat.%s" % ("s" if signed else "u", sfx), ity, x)


def flags_cmp(b, x, y):
    """FCMP's NZCV: less 1000, equal 0110, greater 0010, unordered 0011"""
    n = b.op("fcmp", "i1", x, y, pred="olt")
    z = b.op("fcmp", "i1", x, y, pred="oeq")
    c = b.op("fcmp", "i1", x, y, pred="uge")
    v = b.op("fcmp", "i1", x, y, pred="uno")
    return n, z, c, v


# ---- templates
def t_fp_dp1(b, f, pc, p):
    ty, o = FTY[f["ftype"]], f["opcode"]
    if o in (4, 5, 7):
        to = {4: "f32", 5: "f64", 7: "f16"}[o]
        x = vread(b, f["Rn"], ty)
        r = b.cast("fpext" if WIDTH[to] > WIDTH[ty] else "fptrunc", x, to)
    elif o == 0:
        r = vread(b, f["Rn"], ITY[ty])
    else:
        x = vread(b, f["Rn"], ty)
        if o == 1:
            r = intr(b, "fabs", ty, x)
        elif o == 2:
            r = b.op("fneg", ty, x)
        elif o == 3:
            r = fsqrt(b, x)
        else:
            r = intr(b, RINT[o], ty, x)
    vwrite(b, f["Rd"], r)


def t_fp_dp2(b, f, pc, p):
    ty = FTY[f["ftype"]]
    vwrite(b, f["Rd"], fbin(b, f["opcode"], vread(b, f["Rn"], ty), vread(b, f["Rm"], ty)))


def t_fp_dp3(b, f, pc, p):
    ty = FTY[f["ftype"]]
    n, m, a = vread(b, f["Rn"], ty), vread(b, f["Rm"], ty), vread(b, f["Ra"], ty)
    k = f["o1"] * 2 + f["o0"]   # FMADD a+n*m, FMSUB a-n*m, FNMADD -a-n*m, FNMSUB -a+n*m
    if k in (1, 2):
        n = b.op("fneg", ty, n)
    if k in (2, 3):
        a = b.op("fneg", ty, a)
    vwrite(b, f["Rd"], fma(b, a, n, m))


def t_fp_cmp(b, f, pc, p):
    ty = FTY[f["ftype"]]
    x = vread(b, f["Rn"], ty)
    y = b.const(0, ty) if f["opc"] & 1 else vread(b, f["Rm"], ty)
    fl = flags_cmp(b, x, y)
    set_nzcv(b, *fl)
    b.cmp = fl + (x, y, "fcmp")


def t_fp_ccmp(b, f, pc, p):
    ty = FTY[f["ftype"]]
    ok = cond(b, f["cond"])
    fl = flags_cmp(b, vread(b, f["Rn"], ty), vread(b, f["Rm"], ty))
    nzcv = f["nzcv"]
    set_nzcv(b, *[b.select(ok, x, b.const(nzcv >> (3 - k) & 1, "i1")) for k, x in enumerate(fl)])
    b.cmp = None


def t_fp_csel(b, f, pc, p):
    ty = ITY[FTY[f["ftype"]]]   # (a select of the bits: no FP operation)
    vwrite(b, f["Rd"], b.select(cond(b, f["cond"]), vread(b, f["Rn"], ty), vread(b, f["Rm"], ty)))


def t_fp_imm(b, f, pc, p):
    from .enc_fp import fp_imm_bits
    ty = FTY[f["ftype"]]
    vwrite(b, f["Rd"], b.const(fp_imm_bits(f["imm8"], f["ftype"]), ITY[ty]))


CVT_RND = {0: "roundeven", 1: "ceil", 2: "floor", 3: None}


def t_fp_int(b, f, pc, p):
    sf, ft, rm, o = f["sf"], f["ftype"], f["rmode"], f["opcode"]
    ity = "i64" if sf else "i32"
    if ft == 2:   # FMOV Xd, Vn.D[1] / FMOV Vd.D[1], Xn (the low half kept)
        v = b.bitcast(b.get("v%d" % (f["Rn"] if o == 6 else f["Rd"])), "v2i64")
        if o == 6:
            setx(b, f["Rd"], b.extract(v, 1))
        else:
            b.set("v%d" % f["Rd"], b.bitcast(b.insert(v, getx(b, f["Rn"], 1), 1), "v128"))
        return
    ty = FTY[ft]
    if o == 6:
        setx(b, f["Rd"], vread(b, f["Rn"], ity))
    elif o == 7:
        vwrite(b, f["Rd"], getx(b, f["Rn"], sf))
    elif o in (2, 3):
        vwrite(b, f["Rd"], b.cast("sitofp" if o == 2 else "uitofp", getx(b, f["Rn"], sf), ty))
    else:
        rnd = "round" if o >= 4 else CVT_RND[rm]
        setx(b, f["Rd"], to_int(b, vread(b, f["Rn"], ty), ity, o & 1 == 0, rnd))


def t_fp_fixed(b, f, pc, p):
    sf, o = f["sf"], f["opcode"]
    ty, ity = FTY[f["ftype"]], ("i64" if sf else "i32")
    fb = 64 - f["scale"]
    if o in (2, 3):   # (the integer rounds once; the scaling by a power of two is exact: results are >= 2^-64)
        x = b.cast("sitofp" if o == 2 else "uitofp", getx(b, f["Rn"], sf), ty)
        vwrite(b, f["Rd"], b.op("fmul", ty, x, fconst(b, 2.0 ** -fb, ty)))
    else:   # (the scaling is exact or overflows to infinity, which saturates as ARM's does)
        x = b.op("fmul", ty, vread(b, f["Rn"], ty), fconst(b, 2.0 ** fb, ty))
        setx(b, f["Rd"], to_int(b, x, ity, o == 0))


# ---- loads and stores
VTY = {0: "i8", 1: "i16", 2: "i32", 3: "i64", 4: "v128"}


def ldst_reg(b, addr, L, t, sc, base_reg):
    if L:
        vwrite(b, t, b.op("load", VTY[sc], addr, order="plain"))
    else:
        store(b, addr, vread(b, t, VTY[sc]), base_reg)


def t_ldst_v(b, f, pc, p):
    from .enc_fp import v_scale
    sc = v_scale(f["size"], f["opc"])
    mode = p["mode"]
    base = getx(b, f["Rn"], 1, sp=True)
    wb = None
    if mode == "uimm":
        addr = b.bin("add", base, b.const(f["imm12"] << sc))
    elif mode == "unscaled":
        addr = b.bin("add", base, b.const(sx(f["imm9"], 9)))
    elif mode == "post":
        addr, wb = base, b.bin("add", base, b.const(sx(f["imm9"], 9)))
    elif mode == "pre":
        addr = wb = b.bin("add", base, b.const(sx(f["imm9"], 9)))
    else:
        addr = b.bin("add", base, extend(b, getx(b, f["Rm"], 1), f["option"], sc if f["S"] else 0, "i64"))
    ldst_reg(b, addr, f["opc"] & 1, f["Rt"], sc, f["Rn"])
    if wb is not None:
        setx(b, f["Rn"], wb, sp=True)


def t_ldst_v_pair(b, f, pc, p):
    opc, mode, L = f["opc"], f["mode"], f["L"]
    sc = 2 + opc
    base = getx(b, f["Rn"], 1, sp=True)
    off = b.const(sx(f["imm7"], 7) << sc)
    wb = None
    if mode == 1:
        addr, wb = base, b.bin("add", base, off)
    elif mode == 3:
        addr = wb = b.bin("add", base, off)
    else:
        addr = b.bin("add", base, off)
    addr2 = b.bin("add", addr, b.const(1 << sc))
    if L:
        lo = b.op("load", VTY[sc], addr, order="plain")
        hi = b.op("load", VTY[sc], addr2, order="plain")
        vwrite(b, f["Rt"], lo)
        vwrite(b, f["Rt2"], hi)
    else:
        v1, v2 = vread(b, f["Rt"], VTY[sc]), vread(b, f["Rt2"], VTY[sc])
        store(b, addr, v1, f["Rn"])
        store(b, addr2, v2, f["Rn"])
    if wb is not None:
        setx(b, f["Rn"], wb, sp=True)


def t_ldr_v_lit(b, f, pc, p):
    sc = 2 + f["opc"]
    vwrite(b, f["Rt"], b.op("load", VTY[sc], b.const(pc + (sx(f["imm19"], 19) << 2)), order="plain"))


def post_index(b, f, base, nbytes):
    if f["Rm"] == 31:
        inc = b.const(nbytes)
    else:
        inc = getx(b, f["Rm"], 1)
    setx(b, f["Rn"], b.bin("add", base, inc), sp=True)


def t_ldst_mult(b, f, pc, p):
    """LD1-LD4/ST1-ST4 (multiple structures): LD1/ST1 of 1-4 registers are plain consecutive accesses; LDn/STn
    of n structures load the registers' bytes as one vector and de-interleave them with a shuffle (store: the reverse)"""
    from .enc_fp import MULT
    nreg, selem = MULT[f["opcode"]]
    Q, size, t = f["Q"], f["size"], f["Rt"]
    rbytes = 16 if Q else 8
    lane = "i%d" % (8 << size)
    per = rbytes >> size
    rty = vtype(per, lane)
    base = getx(b, f["Rn"], 1, sp=True)
    regs = [(t + k) % 32 for k in range(nreg)]
    if selem == 1:
        for k, rn in enumerate(regs):
            addr = b.bin("add", base, b.const(k * rbytes))
            if f["L"]:
                vwrite(b, rn, b.op("load", rty, addr, order="plain"))
            else:
                store(b, addr, vread(b, rn, rty), f["Rn"])
    else:
        allty = vtype(per * selem, lane)
        if f["L"]:
            whole = b.op("load", allty, base, order="plain")
            for k, rn in enumerate(regs):
                vwrite(b, rn, b.shuffle(whole, whole, [k + selem * e for e in range(per)]))
        else:
            vals = [vread(b, rn, rty) for rn in regs]
            # concatenate the registers, then interleave: element e of register k at e * selem + k
            cat = vals[0]
            for v in vals[1:]:
                n = lanes(cat.ty)[0]
                wide = b.shuffle(v, v, list(range(per)) + [-1] * (n - per)) if n != per else v
                cat = b.shuffle(cat, wide, list(range(n)) + [n + i for i in range(per)])
            store(b, base, b.shuffle(cat, cat, [k * per + e for e in range(per) for k in range(selem)]), f["Rn"])
    if p["post"]:
        post_index(b, f, base, nreg * rbytes)


def t_ldst_one(b, f, pc, p):
    """LD1-LD4/ST1-ST4 (single structure) of one lane, and LD1R-LD4R (one element to every lane)"""
    from .enc_fp import one_shape
    selem, _, sc, idx, rep = one_shape(f)
    lane = "i%d" % (8 << sc)
    base = getx(b, f["Rn"], 1, sp=True)
    for k in range(selem):
        rn = (f["Rt"] + k) % 32
        addr = b.bin("add", base, b.const(k << sc))
        if rep:
            per = (16 if f["Q"] else 8) >> sc
            x = b.op("load", lane, addr, order="plain")
            v = b.insert(b.zero(vtype(per, lane)), x, 0)
            vwrite(b, rn, b.shuffle(v, v, [0] * per))
        elif f["L"]:
            vt = vtype(16 >> sc, lane)
            vwrite(b, rn, b.insert(vread(b, rn, vt), b.op("load", lane, addr, order="plain"), idx))
        else:
            store(b, addr, b.extract(vread(b, rn, vtype(16 >> sc, lane)), idx), f["Rn"])
    if p["post"]:
        post_index(b, f, base, selem << sc)


# ---- FPCR / FPSR in host FP mode: runtime helpers (the runtime keeps MXCSR equal to FPCR and folds MXCSR's sticky
# bits into FPSR). sem_int.t_sysreg (MRS/MSR, the integer side's table) is to call these for FPCR and FPSR.
def fp_sysreg_read(b, name):
    return b.call("a64_fpsr_read" if name == "FPSR" else "a64_fpcr_read", "i64", ctx=True, effects=True)


def fp_sysreg_write(b, name, x):
    if name == "FPCR":   # (the function is emitted strictfp: no FP operation moves across this write)
        b.fn.strict_fp = True
    b.call("a64_fpsr_write" if name == "FPSR" else "a64_fpcr_write", "void", x, ctx=True, effects=True)


TEMPLATES = {k[2:]: v for k, v in globals().items() if k.startswith("t_")}
