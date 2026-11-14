"""Decoded instructions printed as llvm-mc prints them with `-M no-aliases`, so the decoder can be checked against
LLVM's disassembler word by word (selfcheck.py, tests/test_lift2.py). llvm-mc keeps a few preferred aliases even with
no-aliases (MOVZ/MOVN/ORR immediate as `mov`, the sign/zero extensions of SBFM/UBFM, ...); the printer follows them,
so a difference is always a decoder question, never a spelling one."""
from .enc import bitmask

COND = ["eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"]
SHIFT = ["lsl", "lsr", "asr", "ror"]
EXTEND = ["uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx"]
PRFOP = {0: "pldl1keep", 1: "pldl1strm", 2: "pldl2keep", 3: "pldl2strm", 4: "pldl3keep", 5: "pldl3strm",
         6: "pldslckeep", 7: "pldslcstrm",
         8: "plil1keep", 9: "plil1strm", 10: "plil2keep", 11: "plil2strm", 12: "plil3keep", 13: "plil3strm",
         14: "plislckeep", 15: "plislcstrm", 24: "ir",
         16: "pstl1keep", 17: "pstl1strm", 18: "pstl2keep", 19: "pstl2strm", 20: "pstl3keep", 21: "pstl3strm",
         22: "pstslckeep", 23: "pstslcstrm"}


def r(n, sf, sp=False):
    if n == 31:
        return ("sp" if sf else "wsp") if sp else ("xzr" if sf else "wzr")
    return ("x%d" if sf else "w%d") % n


def sx(v, bits):
    return v - (1 << bits) if v >> (bits - 1) & 1 else v


def imm(v):
    return "#%d" % v


def render(insn, pc=0):
    """llvm-mc's text for a decoded instruction (mnemonic and operands, one space after the mnemonic)"""
    f, n = insn.f, insn.entry.name
    return globals()["_" + n](f, pc)


def _ADR(f, pc):
    v = sx(f["immhi"] << 2 | f["immlo"], 21)
    return "%s %s, #%d" % ("adrp" if f["op"] else "adr", r(f["Rd"], 1), v << 12 if f["op"] else v)


def _ADDSUB_IMM(f, pc):
    sf, op, S = f["sf"], f["op"], f["S"]
    m = ["add", "adds", "sub", "subs"][op * 2 + S]
    s = "%s %s, %s, #%d" % (m, r(f["Rd"], sf, not S), r(f["Rn"], sf, True), f["imm12"])
    return s + (", lsl #12" if f["sh"] else "")


def _LOGIC_IMM(f, pc):
    sf, width = f["sf"], 64 if f["sf"] else 32
    v = bitmask(f["N"], f["imms"], f["immr"], width)
    m = ["and", "orr", "eor", "ands"][f["opc"]]
    if f["opc"] == 1 and f["Rn"] == 31 and not movz_able(v, width):
        return "mov %s, #%d" % (r(f["Rd"], sf, True), sx(v, width) if sf else sx(v, 32))
    return "%s %s, %s, #0x%x" % (m, r(f["Rd"], sf, f["opc"] != 3), r(f["Rn"], sf), v)


def movz_able(v, width):
    """a value MOVZ or MOVN can make (LLVM then prints an ORR immediate as orr, not mov)"""
    for hw in range(width // 16):
        if v & ~(0xFFFF << (16 * hw)) & ((1 << width) - 1) == 0:
            return True
        nv = ~v & ((1 << width) - 1)
        if nv & ~(0xFFFF << (16 * hw)) & ((1 << width) - 1) == 0:
            return True
    return False


def _MOVEWIDE(f, pc):
    sf, opc, hw, i = f["sf"], f["opc"], f["hw"], f["imm16"]
    width = 64 if sf else 32
    if opc == 3:
        return "movk %s, #%d%s" % (r(f["Rd"], sf), i, ", lsl #%d" % (16 * hw) if hw else "")
    v = i << (16 * hw)
    if opc == 2 and not (i == 0 and hw):
        return "mov %s, #%d" % (r(f["Rd"], sf), sx(v, width))
    if opc == 0:
        nv = ~v & ((1 << width) - 1)
        if not (i == 0 and hw) and not (not sf and i == 0xFFFF):
            return "mov %s, #%d" % (r(f["Rd"], sf), sx(nv, width))
    return "%s %s, #%d%s" % ("movn" if opc == 0 else "movz", r(f["Rd"], sf), i, ", lsl #%d" % (16 * hw) if hw else "")


def _BITFIELD(f, pc):
    """LLVM's printer picks the bitfield aliases in code, not as aliases, so it prints them even under no-aliases"""
    sf, opc, immr, imms = f["sf"], f["opc"], f["immr"], f["imms"]
    d, nn, width = r(f["Rd"], sf), r(f["Rn"], sf), 64 if sf else 32
    if opc == 1:
        if f["Rn"] == 31 and (immr == 0 or imms < immr):
            return "bfc %s, #%d, #%d" % (d, (width - immr) % width, imms + 1)
        if imms < immr:
            return "bfi %s, %s, #%d, #%d" % (d, nn, (width - immr) % width, imms + 1)
        return "bfxil %s, %s, #%d, #%d" % (d, nn, immr, imms - immr + 1)
    signed = opc == 0
    if immr == 0:
        m = None
        if imms == 7:
            m = "sxtb" if signed else (None if sf else "uxtb")
        elif imms == 15:
            m = "sxth" if signed else (None if sf else "uxth")
        elif imms == 31 and sf and signed:
            m = "sxtw"
        if m:
            return "%s %s, %s" % (m, d, r(f["Rn"], 0))
    if not signed and imms != width - 1 and imms + 1 == immr:
        return "lsl %s, %s, #%d" % (d, nn, width - 1 - imms)
    if imms == width - 1:
        return "%s %s, %s, #%d" % ("asr" if signed else "lsr", d, nn, immr)
    if immr > imms:
        return "%s %s, %s, #%d, #%d" % ("sbfiz" if signed else "ubfiz", d, nn, width - immr, imms + 1)
    return "%s %s, %s, #%d, #%d" % ("sbfx" if signed else "ubfx", d, nn, immr, imms - immr + 1)


def _EXTR(f, pc):
    sf = f["sf"]
    return "extr %s, %s, %s, #%d" % (r(f["Rd"], sf), r(f["Rn"], sf), r(f["Rm"], sf), f["imms"])


def _LOGIC_SHIFT(f, pc):
    sf = f["sf"]
    m = ["and", "bic", "orr", "orn", "eor", "eon", "ands", "bics"][f["opc"] * 2 + f["N"]]
    s = "%s %s, %s, %s" % (m, r(f["Rd"], sf), r(f["Rn"], sf), r(f["Rm"], sf))
    if f["shift"] or f["imm6"]:
        s += ", %s #%d" % (SHIFT[f["shift"]], f["imm6"])
    return s


def _ADDSUB_SHIFT(f, pc):
    sf = f["sf"]
    m = ["add", "adds", "sub", "subs"][f["op"] * 2 + f["S"]]
    s = "%s %s, %s, %s" % (m, r(f["Rd"], sf), r(f["Rn"], sf), r(f["Rm"], sf))
    if f["shift"] or f["imm6"]:
        s += ", %s #%d" % (SHIFT[f["shift"]], f["imm6"])
    return s


def _ADDSUB_EXT(f, pc):
    sf, opt, amt = f["sf"], f["option"], f["imm3"]
    m = ["add", "adds", "sub", "subs"][f["op"] * 2 + f["S"]]
    rm = r(f["Rm"], 1 if sf and (opt & 3) == 3 else 0)
    s = "%s %s, %s, %s" % (m, r(f["Rd"], sf, not f["S"]), r(f["Rn"], sf, True), rm)
    lsl = (f["Rd"] == 31 and not f["S"] or f["Rn"] == 31) and opt == (3 if sf else 2)
    if lsl:
        return s + (", lsl #%d" % amt if amt else "")
    return s + ", %s%s" % (EXTEND[opt], " #%d" % amt if amt else "")


def _ADDSUB_CARRY(f, pc):
    sf = f["sf"]
    m = ["adc", "adcs", "sbc", "sbcs"][f["op"] * 2 + f["S"]]
    return "%s %s, %s, %s" % (m, r(f["Rd"], sf), r(f["Rn"], sf), r(f["Rm"], sf))


def _CCMP_REG(f, pc):
    sf = f["sf"]
    return "%s %s, %s, #%d, %s" % ("ccmp" if f["op"] else "ccmn", r(f["Rn"], sf), r(f["Rm"], sf), f["nzcv"], COND[f["cond"]])


def _CCMP_IMM(f, pc):
    sf = f["sf"]
    return "%s %s, #%d, #%d, %s" % ("ccmp" if f["op"] else "ccmn", r(f["Rn"], sf), f["imm5"], f["nzcv"], COND[f["cond"]])


def _CSEL(f, pc):
    sf = f["sf"]
    m = ["csel", "csinc", "csinv", "csneg"][f["op"] * 2 + f["o2"]]
    return "%s %s, %s, %s, %s" % (m, r(f["Rd"], sf), r(f["Rn"], sf), r(f["Rm"], sf), COND[f["cond"]])


def _DP2(f, pc):
    sf = f["sf"]
    m = {2: "udiv", 3: "sdiv", 8: "lsl", 9: "lsr", 10: "asr", 11: "ror"}[f["opcode"]]
    return "%s %s, %s, %s" % (m, r(f["Rd"], sf), r(f["Rn"], sf), r(f["Rm"], sf))


def _DP1(f, pc):
    sf, o = f["sf"], f["opcode"]
    m = {0: "rbit", 1: "rev16", 2: "rev32" if sf else "rev", 3: "rev", 4: "clz", 5: "cls"}[o]
    return "%s %s, %s" % (m, r(f["Rd"], sf), r(f["Rn"], sf))


def _DP3(f, pc):
    sf, op31, o0 = f["sf"], f["op31"], f["o0"]
    if op31 == 0:
        return "%s %s, %s, %s, %s" % ("msub" if o0 else "madd", r(f["Rd"], sf), r(f["Rn"], sf), r(f["Rm"], sf), r(f["Ra"], sf))
    if op31 in (2, 6):
        return "%s %s, %s, %s" % ("smulh" if op31 == 2 else "umulh", r(f["Rd"], 1), r(f["Rn"], 1), r(f["Rm"], 1))
    m = ("s" if op31 == 1 else "u") + ("msubl" if o0 else "maddl")
    return "%s %s, %s, %s, %s" % (m, r(f["Rd"], 1), r(f["Rn"], 0), r(f["Rm"], 0), r(f["Ra"], 1))


LDST_NAMES = {(0, 0): "strb", (0, 1): "ldrb", (0, 2): "ldrsb", (0, 3): "ldrsb",
              (1, 0): "strh", (1, 1): "ldrh", (1, 2): "ldrsh", (1, 3): "ldrsh",
              (2, 0): "str", (2, 1): "ldr", (2, 2): "ldrsw",
              (3, 0): "str", (3, 1): "ldr", (3, 2): "prfm"}


def ldst_reg(size, opc):
    """the register width of Rt: 64-bit for size 3 and for sign extension to 64 (opc 2)"""
    return 1 if size == 3 or opc == 2 else 0


def _ldst(f, pc, mode, unpriv=False):
    size, opc = f["size"], f["opc"]
    m = LDST_NAMES[(size, opc)]
    rt = PRFOP.get(f["Rt"], "#%d" % f["Rt"]) if m == "prfm" else r(f["Rt"], ldst_reg(size, opc))
    base = r(f["Rn"], 1, True)
    if mode == "uimm":
        return "%s %s, [%s, #%d]" % (m, rt, base, f["imm12"] << size)
    if mode == "reg":
        opt, S = f["option"], f["S"]
        rm = r(f["Rm"], 1 if opt & 1 else 0)
        if opt == 3:   # (LLVM prints the shift whether S is set or not)
            ext = ", lsl #%d" % size
        else:
            ext = ", %s%s" % (EXTEND[opt], " #%d" % size if S else "")
        return "%s %s, [%s, %s%s]" % (m, rt, base, rm, ext)
    off = sx(f["imm9"], 9)
    if mode == "unscaled":
        if unpriv:
            m = m.replace("ldr", "ldtr").replace("str", "sttr")
        else:
            m = {"prfm": "prfum"}.get(m, m.replace("ldr", "ldur").replace("str", "stur"))
        return "%s %s, [%s, #%d]" % (m, rt, base, off)
    if mode == "post":
        return "%s %s, [%s], #%d" % (m, rt, base, off)
    return "%s %s, [%s, #%d]!" % (m, rt, base, off)


def _LDST_UIMM(f, pc):
    return _ldst(f, pc, "uimm")


def _LDST_UNSCALED(f, pc):
    return _ldst(f, pc, "unscaled")


def _LDST_UNPRIV(f, pc):
    return _ldst(f, pc, "unscaled", True)


def _LDST_POST(f, pc):
    return _ldst(f, pc, "post")


def _LDST_PRE(f, pc):
    return _ldst(f, pc, "pre")


def _LDST_REG(f, pc):
    return _ldst(f, pc, "reg")


def _LDST_PAIR(f, pc):
    opc, mode, L = f["opc"], f["mode"], f["L"]
    sf = 1 if opc >= 1 else 0
    scale = 3 if opc == 2 else 2
    m = ("ldp" if L else "stp") if mode else ("ldnp" if L else "stnp")
    if opc == 1:
        m = "ldpsw"
    off = sx(f["imm7"], 7) << scale
    regs = "%s, %s" % (r(f["Rt"], sf), r(f["Rt2"], sf))
    base = r(f["Rn"], 1, True)
    if mode == 1:
        return "%s %s, [%s], #%d" % (m, regs, base, off)
    if mode == 3:
        return "%s %s, [%s, #%d]!" % (m, regs, base, off)
    return "%s %s, [%s, #%d]" % (m, regs, base, off)


def _LDR_LIT(f, pc):
    opc = f["opc"]
    off = sx(f["imm19"], 19) << 2
    if opc == 3:
        return "prfm %s, #%d" % (PRFOP.get(f["Rt"], "#%d" % f["Rt"]), off)
    m = "ldrsw" if opc == 2 else "ldr"
    return "%s %s, #%d" % (m, r(f["Rt"], 1 if opc else 0), off)


def _B(f, pc):
    return "%s #%d" % ("bl" if f["op"] else "b", sx(f["imm26"], 26) << 2)


def _BCOND(f, pc):
    return "b.%s #%d" % (COND[f["cond"]], sx(f["imm19"], 19) << 2)


def _CBZ(f, pc):
    return "%s %s, #%d" % ("cbnz" if f["op"] else "cbz", r(f["Rt"], f["sf"]), sx(f["imm19"], 19) << 2)


def _TBZ(f, pc):
    return "%s %s, #%d, #%d" % ("tbnz" if f["op"] else "tbz", r(f["Rt"], f["b5"]), f["b5"] << 5 | f["b40"],
                                sx(f["imm14"], 14) << 2)


def _BR(f, pc):
    return "br %s" % r(f["Rn"], 1)


def _BLR(f, pc):
    return "blr %s" % r(f["Rn"], 1)


def _RET(f, pc):
    return "ret %s" % r(f["Rn"], 1)


def _RETA(f, pc):
    return "retab" if f["M"] else "retaa"


def _HINT(f, pc):
    return "hint #%d" % (f["CRm"] << 3 | f["op2"])


def _UDF(f, pc):
    return "udf #%d" % f["imm16"]
