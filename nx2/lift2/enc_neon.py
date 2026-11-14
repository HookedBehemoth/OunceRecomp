"""Advanced SIMD (NEON) encodings, vector and scalar: the table (enc.py's format), its validity names and printers.

Each group of the Arm ARM's "Data processing -- scalar FP and Advanced SIMD" table is one entry (two with its scalar
twin); a group's info function names the operation of a word (a mnemonic and a kind), or None for the encodings this
table leaves out: unallocated ones, half-precision arithmetic (FEAT_FP16, not in the game) and the crypto, dot
product and complex-number groups (not in the game's census). The printers and sem_neon.py's templates use the same
info functions.
"""
from .asm import r as xr
from .enc_fp import ARR, fp_imm_value

UNLIFTED = {"3same": set(), "3diff": set(), "2misc": set()}   # (allocated forms not lifted: none left)


def arr(size, Q):
    return ARR[(size, Q)]


def lowest(imm5):
    for k in range(4):
        if imm5 >> k & 1:
            return k
    return None


# ---- copy (DUP, INS, SMOV, UMOV) and the scalar DUP (element)
def copy_info(Q, op, imm5, imm4):
    size = lowest(imm5)
    if size is None:
        return None
    if op:
        return ("ins_elem", size) if Q else None
    if imm4 in (0, 1):
        return None if size == 3 and not Q else ("dup_elem" if imm4 == 0 else "dup_gen", size)
    if imm4 == 5:
        return ("smov", size) if size < (3 if Q else 2) else None
    if imm4 == 7:
        return ("umov", size) if (size == 3 if Q else size < 3) else None
    if imm4 == 3:
        return ("ins_gen", size) if Q else None
    return None


# ---- three registers of the same type
INT3 = {0: ("shadd", "uhadd", 2), 1: ("sqadd", "uqadd", 3), 2: ("srhadd", "urhadd", 2), 4: ("shsub", "uhsub", 2),
        5: ("sqsub", "uqsub", 3), 6: ("cmgt", "cmhi", 3), 7: ("cmge", "cmhs", 3), 8: ("sshl", "ushl", 3),
        9: ("sqshl", "uqshl", 3), 10: ("srshl", "urshl", 3), 11: ("sqrshl", "uqrshl", 3), 12: ("smax", "umax", 2),
        13: ("smin", "umin", 2), 14: ("sabd", "uabd", 2), 15: ("saba", "uaba", 2), 16: ("add", "sub", 3),
        17: ("cmtst", "cmeq", 3), 18: ("mla", "mls", 2), 19: ("mul", "pmul", 2), 20: ("smaxp", "umaxp", 2),
        21: ("sminp", "uminp", 2), 22: ("sqdmulh", "sqrdmulh", 2), 23: ("addp", None, 3)}
LOGIC = [["and", "bic", "orr", "orn"], ["eor", "bsl", "bit", "bif"]]
FP3 = {(0, 0, 24): "fmaxnm", (0, 0, 25): "fmla", (0, 0, 26): "fadd", (0, 0, 27): "fmulx", (0, 0, 28): "fcmeq",
       (0, 0, 30): "fmax", (0, 0, 31): "frecps", (0, 1, 24): "fminnm", (0, 1, 25): "fmls", (0, 1, 26): "fsub",
       (0, 1, 30): "fmin", (0, 1, 31): "frsqrts", (1, 0, 24): "fmaxnmp", (1, 0, 26): "faddp", (1, 0, 27): "fmul",
       (1, 0, 28): "fcmge", (1, 0, 29): "facge", (1, 0, 30): "fmaxp", (1, 0, 31): "fdiv", (1, 1, 24): "fminnmp",
       (1, 1, 26): "fabd", (1, 1, 28): "fcmgt", (1, 1, 29): "facgt", (1, 1, 30): "fminp"}
SCALAR3_INT = {"sqadd", "uqadd", "sqsub", "uqsub", "cmgt", "cmhi", "cmge", "cmhs", "sshl", "ushl", "sqshl", "uqshl",
               "srshl", "urshl", "sqrshl", "uqrshl", "add", "sub", "cmtst", "cmeq", "sqdmulh", "sqrdmulh"}
SCALAR3_FP = {"fmulx", "fcmeq", "frecps", "frsqrts", "fcmge", "facge", "fabd", "fcmgt", "facgt"}
D_ONLY = {"cmgt", "cmhi", "cmge", "cmhs", "sshl", "ushl", "srshl", "urshl", "add", "sub", "cmtst", "cmeq"}


def same3_info(Q, U, size, opcode, scalar=False):
    """(mnemonic, "i" integer / "l" bitwise / "f" FP, element size) or None"""
    if opcode >= 24:
        mn = FP3.get((U, size >> 1, opcode))
        sz = size & 1
        if mn is None or (scalar and mn not in SCALAR3_FP) or (not scalar and sz and not Q):
            return None
        return mn, "f", 2 + sz
    if opcode == 3:
        return None if scalar else (LOGIC[U][size], "l", 0)
    mn_s, mn_u, top = INT3[opcode]
    mn = mn_u if U else mn_s
    if mn is None or size > top:
        return None
    if mn == "pmul" and size:
        return None
    if mn in ("sqdmulh", "sqrdmulh") and size == 0:
        return None
    if scalar:
        if mn not in SCALAR3_INT or (mn in D_ONLY and size != 3):
            return None
    elif size == 3 and not Q:
        return None
    if mn in UNLIFTED["3same"]:
        return None
    return mn, "i", size


# ---- three registers of different types
DIFF3 = {0: ("saddl", "uaddl", "l"), 1: ("saddw", "uaddw", "w"), 2: ("ssubl", "usubl", "l"), 3: ("ssubw", "usubw", "w"),
         4: ("addhn", "raddhn", "n"), 5: ("sabal", "uabal", "l"), 6: ("subhn", "rsubhn", "n"), 7: ("sabdl", "uabdl", "l"),
         8: ("smlal", "umlal", "l"), 9: ("sqdmlal", None, "l"), 10: ("smlsl", "umlsl", "l"), 11: ("sqdmlsl", None, "l"),
         12: ("smull", "umull", "l"), 13: ("sqdmull", None, "l"), 14: ("pmull", None, "l")}


def diff3_info(U, size, opcode):
    if opcode not in DIFF3 or size == 3:
        return None
    mn = DIFF3[opcode][U]
    if mn is None or (mn.startswith("sqd") and size == 0) or mn in UNLIFTED["3diff"] or (mn == "pmull" and size):
        return None
    return mn, DIFF3[opcode][2], size


# ---- two-register miscellaneous
INT2 = {(0, 0): ("rev64", 2, ""), (0, 1): ("rev16", 0, ""), (0, 2): ("saddlp", 2, "p"), (0, 3): ("suqadd", 3, ""),
        (0, 4): ("cls", 2, ""), (0, 5): ("cnt", 0, ""), (0, 6): ("sadalp", 2, "p"), (0, 7): ("sqabs", 3, ""),
        (0, 8): ("cmgt", 3, "z"), (0, 9): ("cmeq", 3, "z"), (0, 10): ("cmlt", 3, "z"), (0, 11): ("abs", 3, ""),
        (0, 18): ("xtn", 2, "n"), (0, 20): ("sqxtn", 2, "n"),
        (1, 0): ("rev32", 1, ""), (1, 2): ("uaddlp", 2, "p"), (1, 3): ("usqadd", 3, ""), (1, 4): ("clz", 2, ""),
        (1, 5): ("not", 1, ""), (1, 6): ("uadalp", 2, "p"), (1, 7): ("sqneg", 3, ""), (1, 8): ("cmge", 3, "z"),
        (1, 9): ("cmle", 3, "z"), (1, 11): ("neg", 3, ""), (1, 18): ("sqxtun", 2, "n"), (1, 19): ("shll", 2, "s"),
        (1, 20): ("uqxtn", 2, "n")}
FP2 = {(0, 0, 22): "fcvtn", (0, 0, 23): "fcvtl", (0, 0, 24): "frintn", (0, 0, 25): "frintm", (0, 0, 26): "fcvtns",
       (0, 0, 27): "fcvtms", (0, 0, 28): "fcvtas", (0, 0, 29): "scvtf", (0, 1, 12): "fcmgt", (0, 1, 13): "fcmeq",
       (0, 1, 14): "fcmlt", (0, 1, 15): "fabs", (0, 1, 24): "frintp", (0, 1, 25): "frintz", (0, 1, 26): "fcvtps",
       (0, 1, 27): "fcvtzs", (0, 1, 29): "frecpe", (1, 0, 22): "fcvtxn", (1, 0, 24): "frinta", (1, 0, 25): "frintx",
       (1, 0, 26): "fcvtnu", (1, 0, 27): "fcvtmu", (1, 0, 28): "fcvtau", (1, 0, 29): "ucvtf", (1, 1, 12): "fcmge",
       (1, 1, 13): "fcmle", (1, 1, 15): "fneg", (1, 1, 25): "frinti", (1, 1, 26): "fcvtpu", (1, 1, 27): "fcvtzu",
       (1, 1, 29): "frsqrte", (1, 1, 31): "fsqrt", (0, 1, 31): "frecpx"}
SCALAR2_INT = {"suqadd", "sqabs", "cmgt", "cmeq", "cmlt", "abs", "sqxtn", "usqadd", "sqneg", "cmge", "cmle", "neg",
               "sqxtun", "uqxtn"}
SCALAR2_FP = {"fcvtns", "fcvtms", "fcvtas", "scvtf", "fcmgt", "fcmeq", "fcmlt", "fcvtps", "fcvtzs", "frecpe",
              "fcvtxn", "fcvtnu", "fcvtmu", "fcvtau", "ucvtf", "fcmge", "fcmle", "fcvtpu", "fcvtzu", "frsqrte",
              "frecpx"}


def misc2_info(Q, U, size, opcode, scalar=False):
    """(mnemonic, kind, element size); kinds: "" lane op, z compare with zero, n narrow, p pairwise long, s shll,
    f FP lane op, fz FP compare with zero, fn/fl/fx FCVTN/FCVTL/FCVTXN"""
    if opcode >= 12 and (opcode >= 22 or opcode <= 15):
        mn = FP2.get((U, size >> 1, opcode))
        if mn is None or mn in UNLIFTED["2misc"]:
            return None
        sz = size & 1
        if mn in ("fcvtn", "fcvtl", "fcvtxn"):
            if scalar:
                return ("fcvtxn", "fx", 3) if mn == "fcvtxn" and sz else None
            if mn == "fcvtxn" and not sz:
                return None
            return mn, {"fcvtn": "fn", "fcvtl": "fl", "fcvtxn": "fx"}[mn], 2 + sz
        if scalar and mn not in SCALAR2_FP or (not scalar and (mn == "frecpx" or (sz and not Q))):
            return None
        return mn, "fz" if opcode in (12, 13, 14) else "f", 2 + sz
    got = INT2.get((U, opcode))
    if got is None or got[0] in UNLIFTED["2misc"]:
        return None
    mn, top, kind = got
    if mn == "not" and size == 1:
        mn = "rbit"
    if size > top:
        return None
    if scalar:
        if mn not in SCALAR2_INT:
            return None
        if kind == "z" or mn in ("abs", "neg"):
            if size != 3:
                return None
        return mn, kind, size
    if size == 3 and not Q:
        return None
    return mn, kind, size


# ---- across lanes, scalar pairwise
def across_info(Q, U, size, opcode):
    if opcode in (12, 15):
        if not U or size & 1 or not Q:
            return None
        return {(12, 0): "fmaxnmv", (12, 1): "fminnmv", (15, 0): "fmaxv", (15, 1): "fminv"}[(opcode, size >> 1)], "f", 2
    mn = {(0, 3): "saddlv", (1, 3): "uaddlv", (0, 10): "smaxv", (1, 10): "umaxv", (0, 26): "sminv", (1, 26): "uminv",
          (0, 27): "addv"}.get((U, opcode))
    if mn is None or size == 3 or (size == 2 and not Q):
        return None
    return mn, "i", size


def pair_info(U, size, opcode):
    if not U:
        return ("addp", "i", 3) if opcode == 27 and size == 3 else None
    mn = {(12, 0): "fmaxnmp", (12, 1): "fminnmp", (13, 0): "faddp", (15, 0): "fmaxp", (15, 1): "fminp"}.get(
        (opcode, size >> 1))
    return (mn, "f", 2 + (size & 1)) if mn else None


# ---- modified immediate
def modimm_info(Q, op, cmode, o2):
    """(mnemonic, arrangement, shift text kind)"""
    if o2:
        return None
    if cmode < 8:
        return (["movi", "orr", "mvni", "bic"][op * 2 + (cmode & 1)], "4s" if Q else "2s", "lsl32")
    if cmode < 12:
        return (["movi", "orr", "mvni", "bic"][op * 2 + (cmode & 1)], "8h" if Q else "4h", "lsl16")
    if cmode < 14:
        return ("mvni" if op else "movi", "4s" if Q else "2s", "msl")
    if cmode == 14:
        if op:
            return ("movi", "2d" if Q else "d", "b64")
        return ("movi", "16b" if Q else "8b", "b8")
    if op:
        return ("fmov", "2d", "f64") if Q else None
    return ("fmov", "4s" if Q else "2s", "f32")


def modimm_value(op, cmode, imm8):
    """AdvSIMDExpandImm: the 64-bit pattern"""
    if cmode >> 1 in (0, 1, 2, 3):
        return (imm8 << 8 * (cmode >> 1 & 3)) * 0x100000001
    if cmode >> 1 in (4, 5):
        return (imm8 << 8 * (cmode >> 1 & 1)) * 0x0001000100010001
    if cmode >> 1 == 6:
        v = (imm8 << 8 | 0xFF) if cmode & 1 == 0 else (imm8 << 16 | 0xFFFF)
        return v * 0x100000001
    if cmode == 14:
        if op == 0:
            return imm8 * 0x0101010101010101
        return sum((0xFF << 8 * k) for k in range(8) if imm8 >> k & 1)
    if op == 0:   # FMOV single
        from .enc_fp import fp_imm_bits
        return fp_imm_bits(imm8, 0) * 0x100000001
    from .enc_fp import fp_imm_bits
    return fp_imm_bits(imm8, 1)


# ---- shift by immediate
SHI = {0: ("sshr", "ushr", "r"), 2: ("ssra", "usra", "r"), 4: ("srshr", "urshr", "r"), 6: ("srsra", "ursra", "r"),
       8: (None, "sri", "r"), 10: ("shl", "sli", "l"), 12: (None, "sqshlu", "l"), 14: ("sqshl", "uqshl", "l"),
       16: ("shrn", "sqshrun", "n"), 17: ("rshrn", "sqrshrun", "n"), 18: ("sqshrn", "uqshrn", "n"),
       19: ("sqrshrn", "uqrshrn", "n"), 20: ("sshll", "ushll", "w"), 28: ("scvtf", "ucvtf", "f"),
       31: ("fcvtzs", "fcvtzu", "f")}


def shift_info(Q, U, immh, opcode, scalar=False):
    """(mnemonic, kind r/l/n/w/f, element size)"""
    if immh == 0 or opcode not in SHI:
        return None
    mn = SHI[opcode][U]
    kind = SHI[opcode][2]
    if mn is None:
        return None
    esz = 3 if immh >= 8 else 2 if immh >= 4 else 1 if immh >= 2 else 0
    if kind in ("n", "w") and esz == 3:
        return None
    if kind == "f" and esz < 2:
        return None
    if scalar:
        if kind == "w" or mn in ("shrn", "rshrn"):
            return None
        if kind == "r" or mn in ("shl", "sli"):
            if esz != 3:
                return None
    elif esz == 3 and not Q and kind not in ("n", "w"):
        return None
    return mn, kind, esz


# ---- by element
ELEM_INT = {(0, 2): ("smlal", "l"), (0, 3): ("sqdmlal", "l"), (0, 6): ("smlsl", "l"), (0, 7): ("sqdmlsl", "l"),
            (0, 8): ("mul", "s"), (0, 10): ("smull", "l"), (0, 11): ("sqdmull", "l"), (0, 12): ("sqdmulh", "s"),
            (0, 13): ("sqrdmulh", "s"), (1, 0): ("mla", "s"), (1, 2): ("umlal", "l"), (1, 4): ("mls", "s"),
            (1, 6): ("umlsl", "l"), (1, 10): ("umull", "l")}
ELEM_FP = {(0, 1): "fmla", (0, 5): "fmls", (0, 9): "fmul", (1, 9): "fmulx"}


def elem_info(Q, U, size, L, opcode, scalar=False):
    """(mnemonic, kind s same/l long/f FP, element size)"""
    if (U, opcode) in ELEM_FP:
        if size < 2 or (size == 3 and (L or (not Q and not scalar))):
            return None
        return ELEM_FP[(U, opcode)], "f", size
    got = ELEM_INT.get((U, opcode))
    if got is None or size not in (1, 2):
        return None
    if scalar and got[0] not in ("sqdmulh", "sqrdmulh"):
        return None
    return got[0], got[1], size


def elem_index(size, L, M, Rm, H):
    """(register, lane index) of the element operand"""
    if size == 1:
        return Rm, H << 2 | L << 1 | M
    if size == 2:
        return M << 4 | Rm, H << 1 | L
    return M << 4 | Rm, H


ENCODINGS = [
    ("SIMD_COPY", "0 Q op 01110000 imm5:5 0 imm4:4 1 Rn:5 Rd:5", "simd_copy", {},
     "copy_info(Q, op, imm5, imm4) is not None"),
    ("SIMD_DUPS", "01 0 11110000 imm5:5 0 0000 1 Rn:5 Rd:5", "simd_dups", {}, "imm5 & 15 != 0"),
    ("SIMD_3SAME", "0 Q U 01110 size:2 1 Rm:5 opcode:5 1 Rn:5 Rd:5", "simd_3same", {"scalar": False},
     "same3_info(Q, U, size, opcode) is not None"),
    ("SIMD_S3SAME", "01 U 11110 size:2 1 Rm:5 opcode:5 1 Rn:5 Rd:5", "simd_3same", {"scalar": True},
     "same3_info(1, U, size, opcode, True) is not None"),
    ("SIMD_DOT", "0 Q U 01110 size:2 0 Rm:5 1 opcode:4 1 Rn:5 Rd:5", "simd_dot", {},
     "dot_info(Q, U, size, opcode) is not None"),
    ("SIMD_2MISC16", "0 Q U 01110 a 1111 00 opcode:5 10 Rn:5 Rd:5", "simd_2misc16", {},
     "fp16_misc_info(U, a, opcode) is not None"),
    ("SIMD_3DIFF", "0 Q U 01110 size:2 1 Rm:5 opcode:4 00 Rn:5 Rd:5", "simd_3diff", {},
     "diff3_info(U, size, opcode) is not None"),
    ("SIMD_2MISC", "0 Q U 01110 size:2 10000 opcode:5 10 Rn:5 Rd:5", "simd_2misc", {"scalar": False},
     "misc2_info(Q, U, size, opcode) is not None"),
    ("SIMD_S2MISC", "01 U 11110 size:2 10000 opcode:5 10 Rn:5 Rd:5", "simd_2misc", {"scalar": True},
     "misc2_info(1, U, size, opcode, True) is not None"),
    ("SIMD_ACROSS", "0 Q U 01110 size:2 11000 opcode:5 10 Rn:5 Rd:5", "simd_across", {},
     "across_info(Q, U, size, opcode) is not None"),
    ("SIMD_SPAIR", "01 U 11110 size:2 11000 opcode:5 10 Rn:5 Rd:5", "simd_spair", {},
     "pair_info(U, size, opcode) is not None"),
    ("SIMD_PERM", "0 Q 001110 size:2 0 Rm:5 0 opcode:3 10 Rn:5 Rd:5", "simd_perm", {},
     "opcode not in (0, 4) and (Q or size != 3)"),
    ("SIMD_EXT", "0 Q 101110 00 0 Rm:5 0 imm4:4 0 Rn:5 Rd:5", "simd_ext", {}, "Q or imm4 < 8"),
    ("SIMD_TBL", "0 Q 001110 00 0 Rm:5 0 len:2 op 00 Rn:5 Rd:5", "simd_tbl", {}, "True"),
    ("SIMD_MODIMM", "0 Q op 0111100000 abc:3 cmode:4 o2 1 defgh:5 Rd:5", "simd_modimm", {},
     "modimm_info(Q, op, cmode, o2) is not None"),
    ("SIMD_SHIFT", "0 Q U 011110 immh:4 immb:3 opcode:5 1 Rn:5 Rd:5", "simd_shift", {"scalar": False},
     "shift_info(Q, U, immh, opcode) is not None"),
    ("SIMD_SSHIFT", "01 U 111110 immh:4 immb:3 opcode:5 1 Rn:5 Rd:5", "simd_shift", {"scalar": True},
     "shift_info(1, U, immh, opcode, True) is not None"),
    ("SIMD_ELEM", "0 Q U 01111 size:2 L M Rm:4 opcode:4 H 0 Rn:5 Rd:5", "simd_elem", {"scalar": False},
     "elem_info(Q, U, size, L, opcode) is not None"),
    ("SIMD_SELEM", "01 U 11111 size:2 L M Rm:4 opcode:4 H 0 Rn:5 Rd:5", "simd_elem", {"scalar": True},
     "elem_info(1, U, size, L, opcode, True) is not None"),
]

def dot_info(Q, U, size, opcode):
    """three same (extra): SDOT/UDOT (FEAT_DotProd)"""
    return ("udot" if U else "sdot", "d", 2) if opcode == 2 and size == 2 else None


def fp16_misc_info(U, a, opcode):
    """two-register misc (FP16): FNEG and FABS, the only ones in the game (sign-bit operations)"""
    return {(1, 1, 15): "fneg", (0, 1, 15): "fabs"}.get((U, a, opcode))


ENV = {"dot_info": dot_info, "fp16_misc_info": fp16_misc_info, "copy_info": copy_info, "same3_info": same3_info, "diff3_info": diff3_info, "misc2_info": misc2_info,
       "across_info": across_info, "pair_info": pair_info, "modimm_info": modimm_info, "shift_info": shift_info,
       "elem_info": elem_info}


# ---- printers
def v(n, t):
    return "v%d.%s" % (n, t)


def sreg(n, size):
    return "%s%d" % ("bhsd"[size], n)


def _SIMD_COPY(f, pc):
    kind, size = copy_info(f["Q"], f["op"], f["imm5"], f["imm4"])
    idx = f["imm5"] >> (size + 1)
    L = "bhsd"[size]
    if kind == "dup_elem":
        return "dup %s, v%d.%s[%d]" % (v(f["Rd"], arr(size, f["Q"])), f["Rn"], L, idx)
    if kind == "dup_gen":
        return "dup %s, %s" % (v(f["Rd"], arr(size, f["Q"])), xr(f["Rn"], size == 3))
    if kind == "smov":
        return "smov %s, v%d.%s[%d]" % (xr(f["Rd"], f["Q"]), f["Rn"], L, idx)
    if kind == "umov":
        return "umov %s, v%d.%s[%d]" % (xr(f["Rd"], size == 3), f["Rn"], L, idx)
    if kind == "ins_gen":
        return "ins v%d.%s[%d], %s" % (f["Rd"], L, idx, xr(f["Rn"], size == 3))
    return "ins v%d.%s[%d], v%d.%s[%d]" % (f["Rd"], L, idx, f["Rn"], L, f["imm4"] >> size)


def _SIMD_DUPS(f, pc):
    size = lowest(f["imm5"])
    return "mov %s, v%d.%s[%d]" % (sreg(f["Rd"], size), f["Rn"], "bhsd"[size], f["imm5"] >> (size + 1))


def _SIMD_3SAME(f, pc):
    mn, kind, size = same3_info(f["Q"], f["U"], f["size"], f["opcode"])
    t = arr(0 if kind == "l" else size, f["Q"])
    return "%s %s, %s, %s" % (mn, v(f["Rd"], t), v(f["Rn"], t), v(f["Rm"], t))


def _SIMD_S3SAME(f, pc):
    mn, kind, size = same3_info(1, f["U"], f["size"], f["opcode"], True)
    return "%s %s, %s, %s" % (mn, sreg(f["Rd"], size), sreg(f["Rn"], size), sreg(f["Rm"], size))


def _SIMD_DOT(f, pc):
    mn = dot_info(f["Q"], f["U"], f["size"], f["opcode"])[0]
    return "%s %s, %s, %s" % (mn, v(f["Rd"], arr(2, f["Q"])), v(f["Rn"], arr(0, f["Q"])), v(f["Rm"], arr(0, f["Q"])))


def _SIMD_2MISC16(f, pc):
    t = arr(1, f["Q"])
    return "%s %s, %s" % (fp16_misc_info(f["U"], f["a"], f["opcode"]), v(f["Rd"], t), v(f["Rn"], t))


def _SIMD_3DIFF(f, pc):
    mn, kind, size = diff3_info(f["U"], f["size"], f["opcode"])
    ta, tb = arr(size + 1, 1), arr(size, f["Q"])
    mn += "2" if f["Q"] else ""
    if kind == "l":
        return "%s %s, %s, %s" % (mn, v(f["Rd"], ta), v(f["Rn"], tb), v(f["Rm"], tb))
    if kind == "w":
        return "%s %s, %s, %s" % (mn, v(f["Rd"], ta), v(f["Rn"], ta), v(f["Rm"], tb))
    return "%s %s, %s, %s" % (mn, v(f["Rd"], tb), v(f["Rn"], ta), v(f["Rm"], ta))


def _SIMD_2MISC(f, pc):
    Q = f["Q"]
    mn, kind, size = misc2_info(Q, f["U"], f["size"], f["opcode"])
    if kind in ("n", "s", "fn", "fx", "fl"):
        if kind in ("fn", "fl", "fx"):
            narrow, wide = arr(size - 1, Q), arr(size, 1)
        else:
            narrow, wide = arr(size, Q), arr(size + 1, 1)
        mn += "2" if Q else ""
        if kind == "s":
            return "%s %s, %s, #%d" % (mn, v(f["Rd"], wide), v(f["Rn"], narrow), 8 << size)
        if kind == "fl":
            return "%s %s, %s" % (mn, v(f["Rd"], wide), v(f["Rn"], narrow))
        return "%s %s, %s" % (mn, v(f["Rd"], narrow), v(f["Rn"], wide))
    if kind == "p":
        return "%s %s, %s" % (mn, v(f["Rd"], arr(size + 1, Q)), v(f["Rn"], arr(size, Q)))
    t = arr(0 if mn == "rbit" else size, Q)
    s = "%s %s, %s" % (mn, v(f["Rd"], t), v(f["Rn"], t))
    if kind == "z":
        s += ", #0"
    elif kind == "fz":
        s += ", #0.0"
    return s


def _SIMD_S2MISC(f, pc):
    mn, kind, size = misc2_info(1, f["U"], f["size"], f["opcode"], True)
    if kind == "n":
        return "%s %s, %s" % (mn, sreg(f["Rd"], size), sreg(f["Rn"], size + 1))
    if kind == "fx":
        return "%s %s, %s" % (mn, sreg(f["Rd"], 2), sreg(f["Rn"], 3))
    s = "%s %s, %s" % (mn, sreg(f["Rd"], size), sreg(f["Rn"], size))
    if kind == "z":
        s += ", #0"
    elif kind == "fz":
        s += ", #0.0"
    return s


def _SIMD_ACROSS(f, pc):
    mn, kind, size = across_info(f["Q"], f["U"], f["size"], f["opcode"])
    rs = size + 1 if mn.endswith("lv") else size
    return "%s %s, %s" % (mn, sreg(f["Rd"], rs), v(f["Rn"], arr(size, f["Q"])))


def _SIMD_SPAIR(f, pc):
    mn, kind, size = pair_info(f["U"], f["size"], f["opcode"])
    return "%s %s, %s" % (mn, sreg(f["Rd"], size), v(f["Rn"], "2s" if size == 2 else "2d"))


PERM = {1: "uzp1", 2: "trn1", 3: "zip1", 5: "uzp2", 6: "trn2", 7: "zip2"}


def _SIMD_PERM(f, pc):
    t = arr(f["size"], f["Q"])
    return "%s %s, %s, %s" % (PERM[f["opcode"]], v(f["Rd"], t), v(f["Rn"], t), v(f["Rm"], t))


def _SIMD_EXT(f, pc):
    t = "16b" if f["Q"] else "8b"
    return "ext %s, %s, %s, #%d" % (v(f["Rd"], t), v(f["Rn"], t), v(f["Rm"], t), f["imm4"])


def _SIMD_TBL(f, pc):
    t = "16b" if f["Q"] else "8b"
    regs = "{ %s }" % ", ".join("v%d.16b" % ((f["Rn"] + k) % 32) for k in range(f["len"] + 1))
    return "%s %s, %s, %s" % ("tbx" if f["op"] else "tbl", v(f["Rd"], t), regs, v(f["Rm"], t))


def _SIMD_MODIMM(f, pc):
    mn, t, sk = modimm_info(f["Q"], f["op"], f["cmode"], f["o2"])
    imm8 = f["abc"] << 5 | f["defgh"]
    rd = "d%d" % f["Rd"] if t == "d" else v(f["Rd"], t)
    cm = f["cmode"]
    if sk == "lsl32" or sk == "lsl16":
        sh = 8 * (cm >> 1 & (3 if sk == "lsl32" else 1))
        return "%s %s, #%d%s" % (mn, rd, imm8, ", lsl #%d" % sh if sh else "")
    if sk == "msl":
        return "%s %s, #%d, msl #%d" % (mn, rd, imm8, 16 if cm & 1 else 8)
    if sk == "b8":
        return "%s %s, #%d" % (mn, rd, imm8)
    if sk == "b64":
        val = modimm_value(1, 14, imm8)
        return "%s %s, #%s" % (mn, rd, format(val, "#016x") if val else "0" * 16)   # (C's %#016llx)
    return "%s %s, #%.8f" % (mn, rd, fp_imm_value(imm8, 0 if sk == "f32" else 1))


def shift_amount(kind, esz, immh, immb):
    x = immh << 3 | immb
    return x - (8 << esz) if kind in ("l", "w") else (16 << esz) - x


def _SIMD_SHIFT(f, pc):
    Q = f["Q"]
    mn, kind, esz = shift_info(Q, f["U"], f["immh"], f["opcode"])
    sh = shift_amount(kind, esz, f["immh"], f["immb"])
    if kind == "n":
        return "%s%s %s, %s, #%d" % (mn, "2" if Q else "", v(f["Rd"], arr(esz, Q)), v(f["Rn"], arr(esz + 1, 1)), sh)
    if kind == "w":
        return "%s%s %s, %s, #%d" % (mn, "2" if Q else "", v(f["Rd"], arr(esz + 1, 1)), v(f["Rn"], arr(esz, Q)), sh)
    t = arr(esz, Q)
    return "%s %s, %s, #%d" % (mn, v(f["Rd"], t), v(f["Rn"], t), sh)


def _SIMD_SSHIFT(f, pc):
    mn, kind, esz = shift_info(1, f["U"], f["immh"], f["opcode"], True)
    sh = shift_amount(kind, esz, f["immh"], f["immb"])
    src = esz + 1 if kind == "n" else esz
    return "%s %s, %s, #%d" % (mn, sreg(f["Rd"], esz), sreg(f["Rn"], src), sh)


def _SIMD_ELEM(f, pc):
    Q = f["Q"]
    mn, kind, size = elem_info(Q, f["U"], f["size"], f["L"], f["opcode"])
    rm, idx = elem_index(size, f["L"], f["M"], f["Rm"], f["H"])
    el = "v%d.%s[%d]" % (rm, "bhsd"[size], idx)
    if kind == "l":
        return "%s%s %s, %s, %s" % (mn, "2" if Q else "", v(f["Rd"], arr(size + 1, 1)), v(f["Rn"], arr(size, Q)), el)
    t = arr(size, Q)
    return "%s %s, %s, %s" % (mn, v(f["Rd"], t), v(f["Rn"], t), el)


def _SIMD_SELEM(f, pc):
    mn, kind, size = elem_info(1, f["U"], f["size"], f["L"], f["opcode"], True)
    rm, idx = elem_index(size, f["L"], f["M"], f["Rm"], f["H"])
    return "%s %s, %s, v%d.%s[%d]" % (mn, sreg(f["Rd"], size), sreg(f["Rn"], size), rm, "bhsd"[size], idx)


RENDER = {k[1:]: fn for k, fn in globals().items() if k.startswith("_SIMD_")}
