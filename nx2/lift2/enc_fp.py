"""FP scalar encodings, vector/FP register loads and stores, and MRS/MSR of FPCR/FPSR: the table (enc.py's format),
its validity names and its printers (llvm-mc's text with `-M no-aliases`).

ftype: 0 single (S), 1 double (D), 3 half (H). Half-precision arithmetic (FEAT_FP16) is not in the game; only the
conversions FCVT to and from H are decoded, the rest of ftype 3 stays undecoded.
"""
from .asm import COND, EXTEND, r as xr, sx

FT = {0: "s", 1: "d", 3: "h"}

ENCODINGS = [
    # ---- FP data processing (scalar)
    ("FP_DP1", "000 11110 ftype:2 1 opcode:6 10000 Rn:5 Rd:5", "fp_dp1", {}, "fp_dp1_ok(ftype, opcode)"),
    ("FP_DP2", "000 11110 ftype:2 1 Rm:5 opcode:4 10 Rn:5 Rd:5", "fp_dp2", {}, "ftype in (0, 1) and opcode <= 8"),
    ("FP_DP3", "000 11111 ftype:2 o1 Rm:5 o0 Ra:5 Rn:5 Rd:5", "fp_dp3", {}, "ftype in (0, 1)"),
    ("FP_CMP", "000 11110 ftype:2 1 Rm:5 00 1000 Rn:5 opc:2 000", "fp_cmp", {}, "ftype in (0, 1)"),
    ("FP_CCMP", "000 11110 ftype:2 1 Rm:5 cond:4 01 Rn:5 op nzcv:4", "fp_ccmp", {}, "ftype in (0, 1)"),
    ("FP_CSEL", "000 11110 ftype:2 1 Rm:5 cond:4 11 Rn:5 Rd:5", "fp_csel", {}, "ftype in (0, 1)"),
    ("FP_IMM", "000 11110 ftype:2 1 imm8:8 100 00000 Rd:5", "fp_imm", {}, "ftype in (0, 1)"),
    ("FP_INT", "sf 0 0 11110 ftype:2 1 rmode:2 opcode:3 000000 Rn:5 Rd:5", "fp_int", {},
     "fp_int_ok(sf, ftype, rmode, opcode)"),
    ("FP_FIXED", "sf 0 0 11110 ftype:2 0 rmode:2 opcode:3 scale:6 Rn:5 Rd:5", "fp_fixed", {},
     "ftype in (0, 1) and (sf or scale >= 32) and ((rmode == 0 and opcode in (2, 3)) or (rmode == 3 and opcode < 2))"),
    # ---- loads and stores of B/H/S/D/Q registers (size:opc<1> gives the width: 0 B, 1 H, 2 S, 3 D, 4 Q)
    ("LDSTV_UIMM", "size:2 111 1 01 opc:2 imm12:12 Rn:5 Rt:5", "ldst_v", {"mode": "uimm"}, "opc < 2 or size == 0"),
    ("LDSTV_UNSCALED", "size:2 111 1 00 opc:2 0 imm9:9 00 Rn:5 Rt:5", "ldst_v", {"mode": "unscaled"},
     "opc < 2 or size == 0"),
    ("LDSTV_POST", "size:2 111 1 00 opc:2 0 imm9:9 01 Rn:5 Rt:5", "ldst_v", {"mode": "post"}, "opc < 2 or size == 0"),
    ("LDSTV_PRE", "size:2 111 1 00 opc:2 0 imm9:9 11 Rn:5 Rt:5", "ldst_v", {"mode": "pre"}, "opc < 2 or size == 0"),
    ("LDSTV_REG", "size:2 111 1 00 opc:2 1 Rm:5 option:3 S 10 Rn:5 Rt:5", "ldst_v", {"mode": "reg"},
     "(opc < 2 or size == 0) and option in (2, 3, 6, 7)"),
    ("LDSTV_PAIR", "opc:2 101 1 0 mode:2 L imm7:7 Rt2:5 Rn:5 Rt:5", "ldst_v_pair", {}, "opc != 3"),
    ("LDRV_LIT", "opc:2 011 1 00 imm19:19 Rt:5", "ldr_v_lit", {}, "opc != 3"),
    # ---- structure loads and stores: multiple structures, single structure (lane or replicate)
    ("LDST_MULT", "0 Q 0011000 L 000000 opcode:4 size:2 Rn:5 Rt:5", "ldst_mult", {"post": False},
     "opcode in (0, 2, 4, 6, 7, 8, 10) and not (size == 3 and not Q and opcode in (0, 4, 8))"),
    ("LDST_MULT_POST", "0 Q 0011001 L 0 Rm:5 opcode:4 size:2 Rn:5 Rt:5", "ldst_mult", {"post": True},
     "opcode in (0, 2, 4, 6, 7, 8, 10) and not (size == 3 and not Q and opcode in (0, 4, 8))"),
    ("LDST_ONE", "0 Q 0011010 L R 00000 opcode:3 S size:2 Rn:5 Rt:5", "ldst_one", {"post": False},
     "ldst_one_ok(L, opcode, S, size)"),
    ("LDST_ONE_POST", "0 Q 0011011 L R Rm:5 opcode:3 S size:2 Rn:5 Rt:5", "ldst_one", {"post": True},
     "ldst_one_ok(L, opcode, S, size)"),
    # ---- the FP status and control registers
    ("MRS_FP", "1101010100 1 1 1 011 0100 0100 00 op2 Rt:5", "mrs_fp", {}, "True"),
    ("MSR_FP", "1101010100 0 1 1 011 0100 0100 00 op2 Rt:5", "msr_fp", {}, "True"),
]


def fp_dp1_ok(ftype, opcode):
    if opcode in (4, 5, 7):   # FCVT to S, D, H
        return ftype != 2 and opcode - 4 != ftype
    return ftype in (0, 1) and opcode in (0, 1, 2, 3, 8, 9, 10, 11, 12, 14, 15)


def fp_int_ok(sf, ftype, rmode, opcode):
    if ftype == 2:
        return sf == 1 and rmode == 1 and opcode in (6, 7)   # FMOV Xd, Vn.D[1] and back
    if ftype == 3:
        return False
    if opcode in (6, 7):
        return rmode == 0 and sf == ftype
    if opcode in (2, 3, 4, 5):
        return rmode == 0
    return True


def ldst_one_ok(L, opcode, S, size):
    if opcode >= 6:
        return L == 1 and S == 0
    k = opcode >> 1
    if k == 0:
        return True
    if k == 1:
        return size & 1 == 0
    return size in (0, 1) and not (size == 1 and S)


ENV = {"fp_dp1_ok": fp_dp1_ok, "fp_int_ok": fp_int_ok, "ldst_one_ok": ldst_one_ok}


# ---- printers
def vr(n, k):
    """a scalar FP/vector register by width letter: b h s d q"""
    return "%s%d" % (k, n)


def fp_imm_value(imm8, ftype):
    """VFPExpandImm as a Python float"""
    s = -1.0 if imm8 >> 7 else 1.0
    b6 = imm8 >> 6 & 1
    e = (imm8 >> 4 & 3) + (-3 if b6 else 1)
    m = 1 + (imm8 & 15) / 16.0
    return s * m * 2.0 ** e


def fp_imm_bits(imm8, ftype):
    """VFPExpandImm as the bit pattern of the type"""
    E, F, W = {0: (8, 23, 32), 1: (11, 52, 64), 3: (5, 10, 16)}[ftype]
    b6 = imm8 >> 6 & 1
    exp = ((b6 ^ 1) << (E - 1)) | ((((1 << (E - 3)) - 1) if b6 else 0) << 2) | (imm8 >> 4 & 3)
    return (imm8 >> 7) << (W - 1) | exp << F | (imm8 & 15) << (F - 4)


DP1 = {0: "fmov", 1: "fabs", 2: "fneg", 3: "fsqrt", 8: "frintn", 9: "frintp", 10: "frintm", 11: "frintz",
       12: "frinta", 14: "frintx", 15: "frinti"}


def _FP_DP1(f, pc):
    t, o = FT[f["ftype"]], f["opcode"]
    if o in (4, 5, 7):
        return "fcvt %s, %s" % (vr(f["Rd"], FT[o - 4]), vr(f["Rn"], t))
    return "%s %s, %s" % (DP1[o], vr(f["Rd"], t), vr(f["Rn"], t))


DP2 = ["fmul", "fdiv", "fadd", "fsub", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul"]


def _FP_DP2(f, pc):
    t = FT[f["ftype"]]
    return "%s %s, %s, %s" % (DP2[f["opcode"]], vr(f["Rd"], t), vr(f["Rn"], t), vr(f["Rm"], t))


def _FP_DP3(f, pc):
    t = FT[f["ftype"]]
    m = ["fmadd", "fmsub", "fnmadd", "fnmsub"][f["o1"] * 2 + f["o0"]]
    return "%s %s, %s, %s, %s" % (m, vr(f["Rd"], t), vr(f["Rn"], t), vr(f["Rm"], t), vr(f["Ra"], t))


def _FP_CMP(f, pc):
    t = FT[f["ftype"]]
    m = "fcmpe" if f["opc"] & 2 else "fcmp"
    if f["opc"] & 1:
        return "%s %s, #0.0" % (m, vr(f["Rn"], t))
    return "%s %s, %s" % (m, vr(f["Rn"], t), vr(f["Rm"], t))


def _FP_CCMP(f, pc):
    t = FT[f["ftype"]]
    return "%s %s, %s, #%d, %s" % ("fccmpe" if f["op"] else "fccmp", vr(f["Rn"], t), vr(f["Rm"], t), f["nzcv"],
                                   COND[f["cond"]])


def _FP_CSEL(f, pc):
    t = FT[f["ftype"]]
    return "fcsel %s, %s, %s, %s" % (vr(f["Rd"], t), vr(f["Rn"], t), vr(f["Rm"], t), COND[f["cond"]])


def _FP_IMM(f, pc):
    return "fmov %s, #%.8f" % (vr(f["Rd"], FT[f["ftype"]]), fp_imm_value(f["imm8"], f["ftype"]))


INT_CONV = {(0, 0): "fcvtns", (0, 1): "fcvtnu", (1, 0): "fcvtps", (1, 1): "fcvtpu", (2, 0): "fcvtms",
            (2, 1): "fcvtmu", (3, 0): "fcvtzs", (3, 1): "fcvtzu", (0, 4): "fcvtas", (0, 5): "fcvtau"}


def _FP_INT(f, pc):
    sf, ft, rm, o = f["sf"], f["ftype"], f["rmode"], f["opcode"]
    x = xr(f["Rd"] if o in (0, 1, 4, 5, 6) else f["Rn"], sf)
    if ft == 2:
        return "fmov %s, v%d.d[1]" % (xr(f["Rd"], 1), f["Rn"]) if o == 6 else "fmov v%d.d[1], %s" % (f["Rd"], xr(f["Rn"], 1))
    v = vr(f["Rn"] if o in (0, 1, 4, 5, 6) else f["Rd"], FT[ft])
    if o in (2, 3):
        return "%s %s, %s" % ("scvtf" if o == 2 else "ucvtf", v, x)
    if o == 7:
        return "fmov %s, %s" % (v, x)
    if o == 6:
        return "fmov %s, %s" % (x, v)
    return "%s %s, %s" % (INT_CONV[(rm, o)], x, v)


def _FP_FIXED(f, pc):
    sf, o = f["sf"], f["opcode"]
    fb = 64 - f["scale"]
    if o in (2, 3):
        return "%s %s, %s, #%d" % ("scvtf" if o == 2 else "ucvtf", vr(f["Rd"], FT[f["ftype"]]), xr(f["Rn"], sf), fb)
    return "%s %s, %s, #%d" % ("fcvtzs" if o == 0 else "fcvtzu", xr(f["Rd"], sf), vr(f["Rn"], FT[f["ftype"]]), fb)


def v_scale(size, opc):
    """log2 of the access size of a B/H/S/D/Q load or store"""
    return 4 if opc >= 2 else size


def _ldstv(f, mode):
    sc = v_scale(f["size"], f["opc"])
    m = "ldr" if f["opc"] & 1 else "str"
    rt = vr(f["Rt"], "bhsdq"[sc])
    base = xr(f["Rn"], 1, True)
    if mode == "uimm":
        return "%s %s, [%s, #%d]" % (m, rt, base, f["imm12"] << sc)
    if mode == "reg":
        opt = f["option"]
        rm = xr(f["Rm"], 1 if opt & 1 else 0)
        amt = sc if f["S"] else 0
        if opt == 3:   # (LLVM prints the scale whether S is set or not, as for the integer forms)
            ext = ", lsl #%d" % sc
        else:
            ext = ", %s%s" % (EXTEND[opt], " #%d" % amt if f["S"] else "")
        return "%s %s, [%s, %s%s]" % (m, rt, base, rm, ext)
    off = sx(f["imm9"], 9)
    if mode == "unscaled":
        return "%s %s, [%s, #%d]" % (m.replace("ldr", "ldur").replace("str", "stur"), rt, base, off)
    if mode == "post":
        return "%s %s, [%s], #%d" % (m, rt, base, off)
    return "%s %s, [%s, #%d]!" % (m, rt, base, off)


def _LDSTV_PAIR(f, pc):
    opc, mode, L = f["opc"], f["mode"], f["L"]
    k = "sdq"[opc]
    m = ("ldp" if L else "stp") if mode else ("ldnp" if L else "stnp")
    off = sx(f["imm7"], 7) << (2 + opc)
    regs = "%s, %s" % (vr(f["Rt"], k), vr(f["Rt2"], k))
    base = xr(f["Rn"], 1, True)
    if mode == 1:
        return "%s %s, [%s], #%d" % (m, regs, base, off)
    if mode == 3:
        return "%s %s, [%s, #%d]!" % (m, regs, base, off)
    return "%s %s, [%s, #%d]" % (m, regs, base, off)


def _LDRV_LIT(f, pc):
    return "ldr %s, #%d" % (vr(f["Rt"], "sdq"[f["opc"]]), sx(f["imm19"], 19) << 2)


ARR = {(0, 0): "8b", (0, 1): "16b", (1, 0): "4h", (1, 1): "8h", (2, 0): "2s", (2, 1): "4s", (3, 0): "1d", (3, 1): "2d"}
# opcode -> (registers, structure elements)
MULT = {0: (4, 4), 2: (4, 1), 4: (3, 3), 6: (3, 1), 7: (1, 1), 8: (2, 2), 10: (2, 1)}


def reglist(t, n, suffix):
    return "{ %s }" % ", ".join("v%d.%s" % ((t + k) % 32, suffix) for k in range(n))


def _LDST_MULT(f, pc, post=False):
    nreg, selem = MULT[f["opcode"]]
    m = ("ld%d" if f["L"] else "st%d") % selem
    s = "%s %s, [%s]" % (m, reglist(f["Rt"], nreg, ARR[(f["size"], f["Q"])]), xr(f["Rn"], 1, True))
    if post:
        s += ", #%d" % (nreg * (16 if f["Q"] else 8)) if f["Rm"] == 31 else ", %s" % xr(f["Rm"], 1)
    return s


def one_shape(f):
    """(structure elements, lane letter, log2 lane bytes, index, replicate) of a single-structure load or store"""
    o, S, size, Q = f["opcode"], f["S"], f["size"], f["Q"]
    selem = ((o & 1) << 1 | f["R"]) + 1
    if o >= 6:
        return selem, None, size, None, True
    k = o >> 1
    if k == 0:
        return selem, "b", 0, Q << 3 | S << 2 | size, False
    if k == 1:
        return selem, "h", 1, Q << 2 | S << 1 | size >> 1, False
    if size == 0:
        return selem, "s", 2, Q << 1 | S, False
    return selem, "d", 3, Q, False


def _LDST_ONE(f, pc, post=False):
    selem, lane, sc, idx, rep = one_shape(f)
    if rep:
        s = "ld%dr %s, [%s]" % (selem, reglist(f["Rt"], selem, ARR[(f["size"], f["Q"])]), xr(f["Rn"], 1, True))
    else:
        s = "%s%d %s[%d], [%s]" % ("ld" if f["L"] else "st", selem, reglist(f["Rt"], selem, lane), idx,
                                  xr(f["Rn"], 1, True))
    if post:
        s += ", #%d" % (selem << sc) if f["Rm"] == 31 else ", %s" % xr(f["Rm"], 1)
    return s


def _MRS_FP(f, pc):
    return "mrs %s, %s" % (xr(f["Rt"], 1), "FPSR" if f["op2"] else "FPCR")


def _MSR_FP(f, pc):
    return "msr %s, %s" % ("FPSR" if f["op2"] else "FPCR", xr(f["Rt"], 1))


RENDER = {
    "FP_DP1": _FP_DP1, "FP_DP2": _FP_DP2, "FP_DP3": _FP_DP3, "FP_CMP": _FP_CMP, "FP_CCMP": _FP_CCMP,
    "FP_CSEL": _FP_CSEL, "FP_IMM": _FP_IMM, "FP_INT": _FP_INT, "FP_FIXED": _FP_FIXED,
    "LDSTV_UIMM": lambda f, pc: _ldstv(f, "uimm"), "LDSTV_UNSCALED": lambda f, pc: _ldstv(f, "unscaled"),
    "LDSTV_POST": lambda f, pc: _ldstv(f, "post"), "LDSTV_PRE": lambda f, pc: _ldstv(f, "pre"),
    "LDSTV_REG": lambda f, pc: _ldstv(f, "reg"), "LDSTV_PAIR": _LDSTV_PAIR, "LDRV_LIT": _LDRV_LIT,
    "LDST_MULT": _LDST_MULT, "LDST_MULT_POST": lambda f, pc: _LDST_MULT(f, pc, True),
    "LDST_ONE": _LDST_ONE, "LDST_ONE_POST": lambda f, pc: _LDST_ONE(f, pc, True),
    "MRS_FP": _MRS_FP, "MSR_FP": _MSR_FP,
}
