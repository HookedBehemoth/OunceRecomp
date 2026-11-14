"""Integer instruction semantics: one template per family of encodings (enc.py names the template and its params).

A template takes the builder, the decoded fields and the instruction's address, and lowers the instruction into IR
values: guest registers through get()/set(), memory through load/store operations. Branches are terminators and live
in build_ir.py. The Arm ARM's pseudocode is the reference; where LLVM's operations differ from it (division by zero,
shift amounts of the register width or more) the template says so.
"""
from .enc import bitmask
from .ir import WIDTH

MASK27 = (1 << 27) - 1


def ty(sf):
    return "i64" if sf else "i32"


def getx(b, n, sf, sp=False):
    """Xn or Wn (the low 32 bits); register 31 is SP when sp, else the zero register"""
    if n == 31:
        if not sp:
            return b.const(0, ty(sf))
        v = b.get("sp")
    else:
        v = b.get("x%d" % n)
    return v if sf else b.trunc(v, "i32")


def setx(b, n, v, sp=False):
    """Xn = v (an i32 is zero-extended, as every W write is); register 31 is SP when sp, else the write is dropped"""
    if n == 31 and not sp:
        return
    b.set("sp" if n == 31 else "x%d" % n, b.zext(v, "i64"))


def set_nzcv(b, n, z, c, v):
    b.set("n", n)
    b.set("z", z)
    b.set("c", c)
    b.set("v", v)


def nz(b, r):
    zero = b.const(0, r.ty)
    return b.icmp("slt", r, zero), b.icmp("eq", r, zero)


def add_flags(b, x, y):
    r = b.bin("add", x, y)
    n, z = nz(b, r)
    c = b.op("ovf", "i1", x, y, kind="uadd")
    v = b.op("ovf", "i1", x, y, kind="sadd")
    set_nzcv(b, n, z, c, v)
    b.cmp = (n, z, c, v, x, y, "add")
    return r


def sub_flags(b, x, y):
    r = b.bin("sub", x, y)
    n, z = nz(b, r)
    c = b.icmp("uge", x, y)
    v = b.op("ovf", "i1", x, y, kind="ssub")
    set_nzcv(b, n, z, c, v)
    b.cmp = (n, z, c, v, x, y, "sub")
    return r


def logic_flags(b, r):
    n, z = nz(b, r)
    f = b.const(0, "i1")
    set_nzcv(b, n, z, f, f)


def carry_flags(b, x, y, cin):
    """x + y + cin with NZCV (ADCS/SBCS; for SBCS y is already inverted)"""
    w = WIDTH[x.ty]
    wide = "i%d" % (w + 1)
    r = b.bin("add", b.bin("add", x, y), b.zext(cin, x.ty))
    uw = b.bin("add", b.bin("add", b.zext(x, wide), b.zext(y, wide)), b.zext(cin, wide))
    c = b.trunc(b.bin("lshr", uw, b.const(w, wide)), "i1")
    sw = b.bin("add", b.bin("add", b.sext(x, wide), b.sext(y, wide)), b.zext(cin, wide))
    v = b.icmp("ne", b.sext(r, wide), sw)
    n, z = nz(b, r)
    set_nzcv(b, n, z, c, v)
    return r


FOLD = {0: "eq", 1: "ne", 2: "uge", 3: "ult", 8: "ugt", 9: "ule", 10: "sge", 11: "slt", 12: "sgt", 13: "sle"}


def cond(b, code):
    """the condition as an i1; folded into one compare of the last SUBS/CMP's operands when the flags are still its"""
    if code >= 14:
        return b.const(1, "i1")
    n, z, c, v = b.get("n"), b.get("z"), b.get("c"), b.get("v")
    k = b.cmp
    if k is not None and (n, z, c, v) == k[:4]:
        if k[6] == "sub" and code in FOLD:
            return b.icmp(FOLD[code], k[4], k[5])
    base = code >> 1
    if base == 0:
        r = z
    elif base == 1:
        r = c
    elif base == 2:
        r = n
    elif base == 3:
        r = v
    elif base == 4:
        r = b.bin("and", c, b.not_(z))
    elif base == 5:
        r = b.icmp("eq", n, v)
    else:
        r = b.bin("and", b.not_(z), b.icmp("eq", n, v))
    return b.not_(r) if code & 1 else r


def shift(b, v, kind, amt):
    """LSL/LSR/ASR/ROR by a constant below the width"""
    if amt == 0:
        return v
    a = b.const(amt, v.ty)
    if kind == 0:
        return b.bin("shl", v, a)
    if kind == 1:
        return b.bin("lshr", v, a)
    if kind == 2:
        return b.bin("ashr", v, a)
    return b.intr("fshr", v.ty, v, v, a)


def extend(b, v64, option, amt, t):
    """ExtendReg: the low 8/16/32/64 bits of a register, zero- or sign-extended to t, shifted left by amt"""
    src = ["i8", "i16", "i32", "i64"][option & 3]
    x = b.trunc(v64, src)
    if WIDTH[src] > WIDTH[t]:
        x = b.trunc(x, t)
    else:
        x = b.sext(x, t) if option & 4 else b.zext(x, t)
    return b.bin("shl", x, b.const(amt, t)) if amt else x


# ---- data processing, immediate
def t_adr(b, f, pc, p):
    v = f["immhi"] << 2 | f["immlo"]
    v = v - (1 << 21) if v >> 20 else v
    setx(b, f["Rd"], b.const((pc & ~0xFFF) + (v << 12) if f["op"] else pc + v))


def t_addsub_imm(b, f, pc, p):
    sf = f["sf"]
    x = getx(b, f["Rn"], sf, sp=True)
    y = b.const(f["imm12"] << (12 if f["sh"] else 0), ty(sf))
    if f["S"]:
        r = sub_flags(b, x, y) if f["op"] else add_flags(b, x, y)
    else:
        r = b.bin("sub" if f["op"] else "add", x, y)
    setx(b, f["Rd"], r, sp=not f["S"])


def t_logic_imm(b, f, pc, p):
    sf = f["sf"]
    x = getx(b, f["Rn"], sf)
    y = b.const(bitmask(f["N"], f["imms"], f["immr"], 64 if sf else 32), ty(sf))
    r = b.bin(["and", "or", "xor", "and"][f["opc"]], x, y)
    if f["opc"] == 3:
        logic_flags(b, r)
    setx(b, f["Rd"], r, sp=f["opc"] != 3)


def t_movewide(b, f, pc, p):
    sf, opc, hw = f["sf"], f["opc"], f["hw"]
    t = ty(sf)
    v = f["imm16"] << (16 * hw)
    if opc == 2:
        r = b.const(v, t)
    elif opc == 0:
        r = b.const(~v, t)
    else:
        old = getx(b, f["Rd"], sf)
        r = b.bin("or", b.bin("and", old, b.const(~(0xFFFF << (16 * hw)), t)), b.const(v, t))
    setx(b, f["Rd"], r)


def bit_masks(n, imms, immr, width):
    """DecodeBitMasks(immediate = FALSE): (wmask, tmask)"""
    length = ((n << 6) | (~imms & 0x3F)).bit_length() - 1
    levels = (1 << length) - 1
    s, r = imms & levels, immr & levels
    d = (s - r) & levels
    esize = 1 << length
    welem = (1 << (s + 1)) - 1
    telem = (1 << (d + 1)) - 1
    welem = ((welem >> r) | (welem << (esize - r))) & ((1 << esize) - 1) if r else welem
    w = t = 0
    for i in range(width // esize):
        w |= welem << (i * esize)
        t |= telem << (i * esize)
    return w, t


def t_bitfield(b, f, pc, p):
    sf, opc, immr, imms = f["sf"], f["opc"], f["immr"], f["imms"]
    t = ty(sf)
    width = 64 if sf else 32
    wmask, tmask = bit_masks(f["N"], imms, immr, width)
    src = getx(b, f["Rn"], sf)
    dst = getx(b, f["Rd"], sf) if opc == 1 else b.const(0, t)
    rot = b.intr("fshr", t, src, src, b.const(immr, t)) if immr else src
    bot = b.bin("or", b.bin("and", dst, b.const(~wmask, t)), b.bin("and", rot, b.const(wmask, t)))
    if opc == 0:   # SBFM: the bits above the field are copies of bit imms of the source
        top = b.bin("ashr", b.bin("shl", src, b.const(width - 1 - imms, t)), b.const(width - 1, t))
    else:
        top = dst
    r = b.bin("or", b.bin("and", top, b.const(~tmask, t)), b.bin("and", bot, b.const(tmask, t)))
    setx(b, f["Rd"], r)


def t_extr(b, f, pc, p):
    sf = f["sf"]
    hi, lo = getx(b, f["Rn"], sf), getx(b, f["Rm"], sf)
    r = b.intr("fshr", ty(sf), hi, lo, b.const(f["imms"], ty(sf))) if f["imms"] else lo
    setx(b, f["Rd"], r)


# ---- data processing, register
def t_logic_shift(b, f, pc, p):
    sf, opc = f["sf"], f["opc"]
    x = getx(b, f["Rn"], sf)
    y = shift(b, getx(b, f["Rm"], sf), f["shift"], f["imm6"])
    if f["N"]:
        y = b.not_(y)
    r = b.bin(["and", "or", "xor", "and"][opc], x, y)
    if opc == 3:
        logic_flags(b, r)
    setx(b, f["Rd"], r)


def t_addsub_shift(b, f, pc, p):
    sf = f["sf"]
    x = getx(b, f["Rn"], sf)
    y = shift(b, getx(b, f["Rm"], sf), f["shift"], f["imm6"])
    if f["S"]:
        r = sub_flags(b, x, y) if f["op"] else add_flags(b, x, y)
    else:
        r = b.bin("sub" if f["op"] else "add", x, y)
    setx(b, f["Rd"], r)


def t_addsub_ext(b, f, pc, p):
    sf = f["sf"]
    t = ty(sf)
    x = getx(b, f["Rn"], sf, sp=True)
    y = extend(b, getx(b, f["Rm"], 1), f["option"], f["imm3"], t)
    if f["S"]:
        r = sub_flags(b, x, y) if f["op"] else add_flags(b, x, y)
    else:
        r = b.bin("sub" if f["op"] else "add", x, y)
    setx(b, f["Rd"], r, sp=not f["S"])


def t_addsub_carry(b, f, pc, p):
    sf = f["sf"]
    x, y = getx(b, f["Rn"], sf), getx(b, f["Rm"], sf)
    if f["op"]:
        y = b.not_(y)
    cin = b.get("c")
    if f["S"]:
        r = carry_flags(b, x, y, cin)
    else:
        r = b.bin("add", b.bin("add", x, y), b.zext(cin, x.ty))
    setx(b, f["Rd"], r)


def t_ccmp(b, f, pc, p):
    sf = f["sf"]
    x = getx(b, f["Rn"], sf)
    y = b.const(f["imm5"], ty(sf)) if p["imm"] else getx(b, f["Rm"], sf)
    ok = cond(b, f["cond"])
    if f["op"]:
        sub_flags(b, x, y)
    else:
        add_flags(b, x, y)
    nzcv = f["nzcv"]
    flags = [b.select(ok, b.get(name), b.const(nzcv >> bit & 1, "i1")) for name, bit in (("n", 3), ("z", 2), ("c", 1), ("v", 0))]
    set_nzcv(b, *flags)
    b.cmp = None


def t_csel(b, f, pc, p):
    sf = f["sf"]
    x, y = getx(b, f["Rn"], sf), getx(b, f["Rm"], sf)
    k = f["op"] * 2 + f["o2"]
    if k == 1:
        y = b.bin("add", y, b.const(1, y.ty))
    elif k == 2:
        y = b.not_(y)
    elif k == 3:
        y = b.bin("sub", b.const(0, y.ty), y)
    setx(b, f["Rd"], b.select(cond(b, f["cond"]), x, y))


def t_dp2(b, f, pc, p):
    sf, o = f["sf"], f["opcode"]
    t = ty(sf)
    x, y = getx(b, f["Rn"], sf), getx(b, f["Rm"], sf)
    zero = b.const(0, t)
    if o in (2, 3):
        # ARM: division by zero gives 0, and INT_MIN / -1 gives INT_MIN; LLVM leaves both undefined, so the divisor
        # is made 1 for them and the result picked afterwards
        is0 = b.icmp("eq", y, zero)
        if o == 2:
            q = b.bin("udiv", x, b.select(is0, b.const(1, t), y))
        else:
            minv = b.const(1 << (WIDTH[t] - 1), t)
            ovf = b.bin("and", b.icmp("eq", x, minv), b.icmp("eq", y, b.const(-1, t)))
            q = b.bin("sdiv", x, b.select(b.bin("or", is0, ovf), b.const(1, t), y))
            q = b.select(ovf, minv, q)
        r = b.select(is0, zero, q)
    else:
        amt = b.bin("and", y, b.const(WIDTH[t] - 1, t))   # (the shift amount is taken modulo the width)
        if o == 11:
            r = b.intr("fshr", t, x, x, amt)
        else:
            r = b.bin({8: "shl", 9: "lshr", 10: "ashr"}[o], x, amt)
    setx(b, f["Rd"], r)


def t_dp1(b, f, pc, p):
    sf, o = f["sf"], f["opcode"]
    t = ty(sf)
    x = getx(b, f["Rn"], sf)
    if o == 0:
        r = b.intr("bitreverse", t, x)
    elif o == 1:   # REV16: the bytes of each halfword swapped
        m = b.const(0x00FF00FF00FF00FF, t)
        r = b.bin("or", b.bin("and", b.bin("lshr", x, b.const(8, t)), m), b.bin("shl", b.bin("and", x, m), b.const(8, t)))
    elif o == 2 and sf:   # REV32: the bytes of each word reversed
        s = b.intr("bswap", t, x)
        r = b.intr("fshr", t, s, s, b.const(32, t))
    elif o in (2, 3):
        r = b.intr("bswap", t, x)
    elif o == 4:
        r = b.intr("ctlz", t, x)
    else:   # CLS: the leading bits equal to the top bit, the top bit not counted
        y = b.bin("xor", x, b.bin("ashr", x, b.const(WIDTH[t] - 1, t)))
        r = b.bin("sub", b.intr("ctlz", t, y), b.const(1, t))
    setx(b, f["Rd"], r)


def t_dp3(b, f, pc, p):
    sf, op31, o0 = f["sf"], f["op31"], f["o0"]
    if op31 == 0:
        x, y, a = getx(b, f["Rn"], sf), getx(b, f["Rm"], sf), getx(b, f["Ra"], sf)
        m = b.bin("mul", x, y)
        r = b.bin("sub" if o0 else "add", a, m)
    elif op31 in (2, 6):
        ext = b.sext if op31 == 2 else b.zext
        x, y = ext(getx(b, f["Rn"], 1), "i128"), ext(getx(b, f["Rm"], 1), "i128")
        r = b.trunc(b.bin("lshr", b.bin("mul", x, y), b.const(64, "i128")), "i64")
    else:
        ext = b.sext if op31 == 1 else b.zext
        m = b.bin("mul", ext(getx(b, f["Rn"], 0), "i64"), ext(getx(b, f["Rm"], 0), "i64"))
        r = b.bin("sub" if o0 else "add", getx(b, f["Ra"], 1), m)
    setx(b, f["Rd"], r)


# ---- loads and stores
SIZES = ["i8", "i16", "i32", "i64"]


def load(b, addr, t):
    return b.op("load", t, addr, order="plain")


def store(b, addr, v, base_reg):
    """a guest store; it marks its pages (A64_MARK) unless its base register is SP (the guest stack, never read by
    the GPU)"""
    b.op("store", "void", addr, v, mark=base_reg != 31)


def ldst_value(b, f, addr, size, opc, rt, base_reg):
    if opc == 0:
        store(b, addr, b.trunc(getx(b, rt, 1), SIZES[size]), base_reg)
        return
    v = load(b, addr, SIZES[size])
    if opc == 1:
        r = b.zext(v, "i64")
    elif opc == 2:
        r = b.sext(v, "i64")
    else:
        r = b.zext(b.sext(v, "i32"), "i64")
    setx(b, rt, r)


def sx(v, bits):
    return v - (1 << bits) if v >> (bits - 1) & 1 else v


def t_ldst(b, f, pc, p):
    size, opc, mode = f["size"], f["opc"], p["mode"]
    if size == 3 and opc == 2:   # PRFM, PRFUM: a hint
        return
    base = getx(b, f["Rn"], 1, sp=True)
    wb = None
    if mode == "uimm":
        addr = b.bin("add", base, b.const(f["imm12"] << size))
    elif mode == "unscaled":
        addr = b.bin("add", base, b.const(sx(f["imm9"], 9)))
    elif mode == "post":
        addr, wb = base, b.bin("add", base, b.const(sx(f["imm9"], 9)))
    elif mode == "pre":
        addr = wb = b.bin("add", base, b.const(sx(f["imm9"], 9)))
    else:
        addr = b.bin("add", base, extend(b, getx(b, f["Rm"], 1), f["option"], size if f["S"] else 0, "i64"))
    ldst_value(b, f, addr, size, opc, f["Rt"], f["Rn"])
    if wb is not None:
        setx(b, f["Rn"], wb, sp=True)


def t_ldst_pair(b, f, pc, p):
    opc, mode, L = f["opc"], f["mode"], f["L"]
    size = 3 if opc == 2 else 2
    step = 1 << size
    base = getx(b, f["Rn"], 1, sp=True)
    off = b.const(sx(f["imm7"], 7) << size)
    wb = None
    if mode == 1:
        addr, wb = base, b.bin("add", base, off)
    elif mode == 3:
        addr = wb = b.bin("add", base, off)
    else:
        addr = b.bin("add", base, off)
    addr2 = b.bin("add", addr, b.const(step))
    if L:
        lo, hi = load(b, addr, SIZES[size]), load(b, addr2, SIZES[size])
        ext = b.sext if opc == 1 else b.zext
        setx(b, f["Rt"], ext(lo, "i64"))
        setx(b, f["Rt2"], ext(hi, "i64"))
    else:
        v1 = b.trunc(getx(b, f["Rt"], 1), SIZES[size])
        v2 = b.trunc(getx(b, f["Rt2"], 1), SIZES[size])
        store(b, addr, v1, f["Rn"])
        store(b, addr2, v2, f["Rn"])
    if wb is not None:
        setx(b, f["Rn"], wb, sp=True)


def t_ldr_lit(b, f, pc, p):
    opc = f["opc"]
    if opc == 3:
        return
    addr = b.const(pc + (sx(f["imm19"], 19) << 2))
    v = load(b, addr, "i32" if opc != 1 else "i64")
    setx(b, f["Rt"], b.sext(v, "i64") if opc == 2 else b.zext(v, "i64"))


def t_hint(b, f, pc, p):
    """NOP, YIELD, WFE, SEV and the PAC hints (PACIBSP, AUTIBSP, ...): nothing here (the return address is the
    host's, so signing it has no effect)"""


# ---- CRC32, pointer authentication, MTE, system registers and operations
def t_crc32(b, f, pc, p):
    """CRC32/CRC32C over 8 << sz bits of Rm (bit-reflected, as the Arm ARM's CRC32 pseudocode): the internal helper
    lift2_crc32 (emit_llvm.HELPERS), polynomial 0xEDB88320 or, for CRC32C, 0x82F63B78"""
    sz = f["sz"]
    data = b.zext(getx(b, f["Rm"], 1 if sz == 3 else 0), "i64")
    acc = getx(b, f["Rn"], 0)
    poly = b.const(0x82F63B78 if f["C"] else 0xEDB88320, "i32")
    r = b.op("call", "i32", acc, data, b.const(8 << sz, "i32"), poly, fn="lift2_crc32", ctx=False, helper=True)
    setx(b, f["Rd"], r)


def t_pac(b, f, pc, p):
    """PAC*/AUT*/XPAC*: the identity (no pointer is ever signed here; nx2/insn.py's model)"""


def t_pacga(b, f, pc, p):
    """PACGA: 0 (nx2/insn.py's model)"""
    setx(b, f["Rd"], b.const(0))


def t_stg(b, f, pc, p):
    """STG/ST2G: memory tags are not modelled; only the writeback of the base register"""
    if f["op2"] in (1, 3):
        setx(b, f["Rn"], b.bin("add", getx(b, f["Rn"], 1, sp=True), b.const(sx(f["imm9"], 9) << 4)), sp=True)


CTX_TPIDR, CTX_TPIDRRO, CTX_FPCR, CTX_FPSR = 0x310, 0x318, 0x320, 0x328
FPCR_INHIBIT = 1 << 32   # (the host's kill switch for the FP fast paths, kept across guest writes; a64_suyu.h)


def t_sysreg(b, f, pc, p):
    from .enc import SYSREGS
    name = SYSREGS[(f["o0"], f["op1"], f["CRn"], f["CRm"], f["op2"])][0]
    rt = f["Rt"]
    if f["L"]:
        if name == "NZCV":
            v = b.const(0)
            for k, flag in enumerate("nzcv"):
                v = b.bin("or", v, b.bin("shl", b.zext(b.get(flag), "i64"), b.const(31 - k)))
        elif name == "FPCR":
            v = b.bin("and", b.op("ctxload", "i64", off=CTX_FPCR), b.const(0xFFFFFFFF))
        elif name in ("FPSR", "TPIDR_EL0", "TPIDRRO_EL0"):
            v = b.op("ctxload", "i64", off={"FPSR": CTX_FPSR, "TPIDR_EL0": CTX_TPIDR, "TPIDRRO_EL0": CTX_TPIDRRO}[name])
        elif name in ("CNTPCT_EL0", "CNTVCT_EL0"):
            v = b.op("call", "i64", fn="recomp_cntpct", ctx=True)
        elif name == "CNTFRQ_EL0":
            v = b.const(31250000)   # (the Switch 2's counter, host/mkw_glue.c a64_cntpct_host)
        elif name == "CTR_EL0":
            v = b.const(0x8444C004)   # (suyu's dynarmic configuration, as the present lifter)
        else:   # DCZID_EL0: DC ZVA clears 64 bytes
            v = b.const(4)
        setx(b, rt, v)
        return
    x = getx(b, rt, 1)
    if name == "NZCV":
        for k, flag in enumerate("nzcv"):
            b.set(flag, b.trunc(b.bin("lshr", x, b.const(31 - k)), "i1"))
    elif name == "FPCR":
        old = b.op("ctxload", "i64", off=CTX_FPCR)
        b.op("ctxstore", "void", b.bin("or", b.bin("and", x, b.const(0xFFFFFFFF)),
                                       b.bin("and", old, b.const(FPCR_INHIBIT))), off=CTX_FPCR)
    elif name == "FPSR":
        b.op("ctxstore", "void", x, off=CTX_FPSR)
    elif name == "TPIDR_EL0":
        b.op("ctxstore", "void", x, off=CTX_TPIDR)


def t_sys(b, f, pc, p):
    """DC ZVA / DC GZVA clear the 64-byte block holding the address; the cache and tag maintenance operations and IC
    IVAU are nothing here (no self-modifying code, tags not modelled)"""
    if (f["CRm"], f["op2"]) in ((4, 1), (4, 4)):
        base = b.bin("and", getx(b, f["Rt"], 1), b.const(~63))
        for k in range(8):
            store(b, b.bin("add", base, b.const(8 * k)), b.const(0), f["Rt"] if f["Rt"] != 31 else 0)


def t_barrier(b, f, pc, p):
    """ISB: nothing (no self-modifying code)"""


TEMPLATES = {k[2:]: v for k, v in globals().items() if k.startswith("t_")}
# decoded, lifted from milestone 6 (atomics, acquire/release, exclusives, DMB/DSB/CLREX) and 7 (SVC)
TEMPLATES.update({"fence": None, "lse": None, "cas": None, "casp": None, "ldstord": None, "ldstex": None, "ldstexp": None})
BRANCH_TEMPLATES = {"b", "bcond", "cbz", "tbz", "br", "blr", "ret", "udf"}
