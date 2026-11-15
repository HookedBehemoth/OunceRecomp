"""Which guest functions break the AAPCS64 assumptions lifter 2's fast convention makes (docs/lifter2-abi-census.md).

    python -I nx2/lift2/abi_census.py [--out work/lift2/abi_census.txt] [--jobs 4] [--only 0xADDR,...]

The functions are the lift's (the standalone image, its HLE set, nx2/lift.py's discovery with the ADR
entries); HLE'd functions are host code and not analysed. Each body is decoded with lift2's decoder and run through a
small abstract interpretation over its CFG. A register's value is one of
  ('E', r)   the value register r had at entry (r: 0-30 GPR, 'v8'..'v15' for d8-d15),
  ('S', k)   entry SP + k (a frame address),
  ('L',)     loaded from memory outside the frame,
  ('O',)     anything else;
frame slots (entry SP + k) keep the value stored there. Iterated to a fixed point, then walked once more to record:

  sp_dyn          SP set by a register operand (alloca, VLA, realignment): benign when SP is S(0) at every exit
  sp_from_value   SP set from an argument, a loaded value or another register that is not a frame address
  sp_exit         SP is not entry SP at a return or tail call
  lr_ret_reg      RET xN with N != 30, or BR x30
  lr_load         x30 loaded from memory outside the frame
  lr_exit         x30 at a RET / tail call is not the entry x30 (written from a computed or foreign value)
  lr_data         the entry x30 used as data (__builtin_return_address, compared, stored outside the frame)
  cs_save_arg     entry values of callee-saved registers (x19-x30, d8-d15) or SP stored outside the frame: listed
                  when 4 or more (the jmp_buf / context-save signature)
  cs_restore_arg  4 or more callee-saved registers loaded from outside the frame
  cs_exit         a callee-saved register (x19-x29, d8-d15) not its entry value at a return or tail call
  cs_entry_data   the entry value of x19-x28 used as data (an argument passed in a callee-saved register)
  livein_tmp      x9-x18 read before the function writes them (arguments outside x0-x8: veneers, outlined code)
  livein_nzcv     flags read before the function sets them
  after_call      x9-x18 or the flags read after a BL without being written since: blames the callee (it returns
                  something there: a non-AAPCS callee such as an outlined helper)
  sysreg          MRS/MSR of TPIDR_EL0, TPIDRRO_EL0, FPCR, FPSR, NZCV; x18 used; SVC; BRK/HLT/UDF
  pac_lr          PAC/AUT data-processing forms on x30 (hint forms PACIBSP/AUTIBSP are counted, not listed)
  adr_x30         ADR/ADRP into x30
  noret_call      a BL whose return address is not in the function (another function starts there, or a trap):
                  the callee is treated by the compiler as noreturn
  fall_into       the body runs off its last instruction into another function's entry (not after a BL)
  mid_fde         an entry inside another function's .eh_frame range (a call or table into its middle)
Writes one line per (class, function) and prints the counts.
"""
import argparse
import collections
import multiprocessing
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lift2 import enc  # noqa: E402

CS = tuple(range(19, 30))                 # x19-x29: preserved by the callee
VCS = tuple("v%d" % i for i in range(8, 16))
SP = 31
TRACE = False   # (--trace: the recording pass prints each instruction's SP, x29, x30)
O = ("O",)
L = ("L",)
SYSREG_NAMES = {k: v[0] for k, v in enc.SYSREGS.items()}
FLAG_READERS = {"bcond", "csel", "ccmp", "addsub_carry", "fp_csel", "fp_ccmp"}


def sx(v, bits):
    return v - (1 << bits) if v >> (bits - 1) & 1 else v


class State:
    __slots__ = ("r", "v", "slots", "flags", "defd", "dirty")

    def __init__(self):
        self.r = [("E", i) for i in range(31)] + [("S", 0)]
        self.v = {x: ("E", x) for x in VCS}
        self.slots = {}
        self.flags = False                      # flags written on every path
        self.defd = (1 << 9) - 1 | 0x7FF << 19 | 1 << 30   # x0-x8, x19-x30 count as set at entry
        self.dirty = {}                         # reg ('nzcv' for flags) -> callees whose BL last clobbered it

    def copy(self):
        s = State.__new__(State)
        s.r, s.v, s.slots, s.flags, s.defd = list(self.r), dict(self.v), dict(self.slots), self.flags, self.defd
        s.dirty = dict(self.dirty)
        return s

    def merge(self, o, widen):
        """self := self meet o; True if self changed"""
        ch = False
        for i in range(32):
            if self.r[i] != o.r[i] and self.r[i] != O:
                self.r[i] = O
                ch = True
        if widen and self.r[SP] != O and self.r[SP][0] == "S" and self.r[SP] != o.r[SP]:
            self.r[SP] = O
            ch = True
        for k in VCS:
            if self.v[k] != o.v[k] and self.v[k] != O:
                self.v[k] = O
                ch = True
        for k in list(self.slots):
            if o.slots.get(k) != self.slots[k]:
                del self.slots[k]
                ch = True
        if self.flags and not o.flags:
            self.flags = False
            ch = True
        d = self.defd & o.defd
        if d != self.defd:
            self.defd = d
            ch = True
        for k, v in o.dirty.items():
            u = self.dirty.get(k, frozenset()) | v
            if u != self.dirty.get(k):
                self.dirty[k] = u
                ch = True
        return ch


class Rec:
    """findings of one function: class -> list of (pc, detail)"""

    def __init__(self):
        self.on = False
        self.f = collections.defaultdict(list)
        self.cs_saved = set()
        self.cs_loaded = set()
        self.blame = collections.defaultdict(set)   # callee -> registers read after its call

    def add(self, k, pc, d=""):
        if self.on:
            self.f[k].append((pc, d))


class Fn:
    def __init__(self, entry, body, jt, entries, callee):
        self.entry, self.body, self.jt, self.entries, self.callee = entry, body, jt, entries, callee

    def inside(self, t):
        return t in self.body and (t == self.entry or t not in self.entries)


def gpr(n, sp):
    """register number of an operand: 31 is SP where the encoding means SP, else the zero register (None)"""
    return n if n != 31 else (SP if sp else None)


class Interp:
    def __init__(self, fn, rec):
        self.fn, self.rec = fn, rec

    # -- values
    def get(self, s, n):
        return O if n is None else s.r[n]

    def use(self, s, n, pc, saved=False):
        """a data use of GPR n"""
        if n is None or n == SP:
            return
        rec = self.rec
        if not (s.defd >> n & 1):
            if 9 <= n <= 18:
                rec.add("livein_tmp", pc, "x%d" % n)
        dv = s.dirty.get(n)
        if dv:
            for c in dv:
                rec.blame[c].add("x%d" % n)
            rec.add("after_call", pc, "x%d" % n)
        if n == 18:
            rec.add("sysreg", pc, "x18")
        if saved:
            return
        v = s.r[n]
        if v == ("E", 30):
            rec.add("lr_data", pc)
        elif v[0] == "E" and v[1] in CS[:-1]:
            rec.add("cs_entry_data", pc, "x%d" % v[1])

    def define(self, s, n, val=O):
        if n is None:
            return
        s.r[n] = val
        if n != SP:
            s.defd |= 1 << n
            s.dirty.pop(n, None)

    def set_sp(self, s, val, pc, how):
        if val[0] != "S":
            if how == "dyn":
                self.rec.add("sp_dyn", pc)
            else:
                self.rec.add("sp_from_value", pc, how)
        s.r[SP] = val

    def flags_read(self, s, pc):
        if not s.flags:
            if s.dirty.get("nzcv"):
                for c in s.dirty["nzcv"]:
                    self.rec.blame[c].add("nzcv")
                self.rec.add("after_call", pc, "nzcv")
            else:
                self.rec.add("livein_nzcv", pc)

    def flags_write(self, s):
        s.flags = True
        s.dirty.pop("nzcv", None)

    # -- memory
    def addr(self, s, base, off):
        b = self.get(s, base)
        return b[1] + off if b[0] == "S" and off is not None else None, b[0] == "S"

    def store(self, s, pc, base, off, val, reg_name, size):
        a, frame = self.addr(s, base, off)
        if frame:
            if a is not None:
                for k in [k for k in s.slots if a <= k < a + size and k != a]:
                    del s.slots[k]
                s.slots[a] = val
            return
        if val[0] == "E" and (val[1] in VCS or val[1] in CS or val[1] == 30):
            self.rec.cs_saved.add(val[1])
        elif val == ("S", 0):
            self.rec.cs_saved.add("sp")

    def load(self, s, pc, base, off, n, vreg=False):
        a, frame = self.addr(s, base, off)
        val = (s.slots.get(a, O) if a is not None else O) if frame else L
        if not frame:
            if vreg and n in VCS:
                self.rec.cs_loaded.add(n)
            elif not vreg and n is not None and (n in CS or n == 30):
                self.rec.cs_loaded.add(n)
            if not vreg and n == 30:
                self.rec.add("lr_load", pc)
        if vreg:
            if n in VCS:
                s.v[n] = val
        else:
            self.define(s, n, val)

    def wb(self, s, base, delta, pc):
        """base register writeback"""
        b = self.get(s, base)
        nv = ("S", b[1] + delta) if b[0] == "S" else O
        if base == SP:
            self.set_sp(s, nv, pc, "dyn")
        else:
            self.define(s, base, nv)

    # -- one instruction
    def step(self, s, pc, i):
        t, f = i.entry.template, i.f
        rec = self.rec
        if t == "addsub_imm":
            rd, rn = gpr(f["Rd"], not f["S"]), gpr(f["Rn"], True)
            self.use(s, rn, pc)
            imm = f["imm12"] << (12 if f["sh"] else 0)
            b = self.get(s, rn)
            val = O
            if f["sf"] and b[0] == "S":
                val = ("S", b[1] - imm if f["op"] else b[1] + imm)
            elif f["sf"] and imm == 0 and not f["S"]:
                val = b
            if f["S"]:
                self.flags_write(s)
            if rd == SP:
                how = "dyn" if b[0] == "S" or b == O else ("x%d entry value" % b[1] if b[0] == "E" else "a loaded value")
                if rn == SP and b == O:
                    how = "dyn"
                self.set_sp(s, val, pc, how)
            else:
                self.define(s, rd, val)
            return
        if t == "addsub_ext":
            rd, rn, rm = gpr(f["Rd"], not f["S"]), gpr(f["Rn"], True), gpr(f["Rm"], False)
            self.use(s, rn, pc)
            self.use(s, rm, pc)
            if f["S"]:
                self.flags_write(s)
            if rd == SP:
                self.set_sp(s, O, pc, "dyn")
            else:
                self.define(s, rd)
            return
        if t == "logic_imm":
            rd, rn = gpr(f["Rd"], f["opc"] != 3), gpr(f["Rn"], False)
            self.use(s, rn, pc)
            if f["opc"] == 3:
                self.flags_write(s)
            if rd == SP:
                self.set_sp(s, O, pc, "dyn")
            else:
                self.define(s, rd)
            return
        if t == "logic_shift":
            rd, rn, rm = gpr(f["Rd"], False), gpr(f["Rn"], False), gpr(f["Rm"], False)
            self.use(s, rn, pc)
            self.use(s, rm, pc)
            if f["opc"] == 3:
                self.flags_write(s)
            mov = f["opc"] == 1 and not f["N"] and f["imm6"] == 0 and rn is None and f["sf"]
            self.define(s, rd, self.get(s, rm) if mov else O)
            return
        if t in ("addsub_shift", "addsub_carry", "csel", "dp1", "dp2", "dp3", "extr", "crc32", "bitfield", "pacga"):
            for k in ("Rn", "Rm", "Ra"):
                if k in f:
                    self.use(s, gpr(f[k], False), pc)
            if t == "bitfield" and f["opc"] == 1:   # BFM keeps the destination's other bits
                self.use(s, gpr(f["Rd"], False), pc)
            if t in FLAG_READERS:
                self.flags_read(s, pc)
            if f.get("S"):
                self.flags_write(s)
            self.define(s, gpr(f["Rd"], False))
            return
        if t == "ccmp":
            self.use(s, gpr(f["Rn"], False), pc)
            if not i.entry.params["imm"]:
                self.use(s, gpr(f["Rm"], False), pc)
            self.flags_read(s, pc)
            self.flags_write(s)
            return
        if t in ("movewide", "adr"):
            rd = gpr(f["Rd"], False)
            if t == "movewide" and f["opc"] == 3:
                self.use(s, rd, pc)
            if t == "adr" and rd == 30:
                rec.add("adr_x30", pc)
            self.define(s, rd)
            return
        if t == "pac":
            rd, op = f["Rd"], f["opcode"]
            if op < 8:
                self.use(s, gpr(f["Rn"], True), pc)
            if rd == 30:
                rec.add("pac_lr", pc, "opcode %d modifier %s" % (op, "x%d" % f["Rn"] if op < 8 else "zero"))
            return   # (identity: the value is kept)
        if t == "ldst":
            return self.ldst(s, pc, f, i.entry.params["mode"])
        if t == "ldst_pair":
            return self.pair(s, pc, f, False)
        if t == "ldst_v":
            return self.ldst_v(s, pc, f, i.entry.params["mode"])
        if t == "ldst_v_pair":
            return self.pair(s, pc, f, True)
        if t == "ldr_lit":
            if f["opc"] != 3:
                self.define(s, gpr(f["Rt"], False))
            return
        if t in ("lse", "cas", "casp", "ldstex", "ldstexp", "ldstord", "stg"):
            self.use(s, gpr(f["Rn"], True), pc)
            for k in ("Rs", "Rt", "Rt2"):
                if k in f and not (t == "stg"):
                    self.use(s, gpr(f[k], False), pc, saved=True)
            defs = []
            if t == "lse":
                defs = [f["Rt"]]
            elif t == "cas":
                defs = [f["Rs"]]
            elif t == "casp":
                defs = [f["Rs"], f["Rs"] + 1]
            elif t in ("ldstex", "ldstexp"):
                defs = [f["Rt"], f["Rt2"]] if f["L"] else [f["Rs"]]
            elif t == "ldstord":
                defs = [f["Rt"]] if f["L"] else []
            for d in defs:
                self.define(s, gpr(d, False), L)
            return
        if t == "sysreg":
            name = SYSREG_NAMES.get((f["o0"], f["op1"], f["CRn"], f["CRm"], f["op2"]), "other")
            rt = gpr(f["Rt"], False)
            if f["L"]:
                if name in ("TPIDR_EL0", "TPIDRRO_EL0", "FPCR", "FPSR", "NZCV"):
                    rec.add("sysreg", pc, "mrs " + name)
                if name == "NZCV":
                    self.flags_read(s, pc)
                self.define(s, rt)
            else:
                self.use(s, rt, pc)
                rec.add("sysreg", pc, "msr " + name)
                if name == "NZCV":
                    self.flags_write(s)
            return
        if t == "sys":
            self.use(s, gpr(f["Rt"], False), pc)
            return
        if t in ("hint", "barrier", "fence", "udf", "exc"):
            if t == "hint" and f["CRm"] in (1, 3):
                rec.add("pac_hint", pc)
            return
        if t in ("fp_cmp", "fp_ccmp"):
            if t == "fp_ccmp":
                self.flags_read(s, pc)
            self.flags_write(s)
            return
        if t == "fp_csel":
            self.flags_read(s, pc)
        # FP / NEON: GPR operands where the encoding has them; Rd a vector register otherwise
        to_gpr = from_gpr = False
        if t == "fp_int":
            to_gpr = f["opcode"] in (0, 1, 4, 5, 6)
            from_gpr = not to_gpr
        elif t == "fp_fixed":
            to_gpr = f["opcode"] in (0, 1)
            from_gpr = not to_gpr
        elif t == "simd_copy" and f["op"] == 0:
            to_gpr = f["imm4"] in (5, 7)
            from_gpr = f["imm4"] in (1, 3)
        if t in ("ldst_mult", "ldst_one"):
            # registers transferred, and the bytes a post-index by immediate (Rm = 31) adds to the base
            if t == "ldst_mult":
                nregs = {0: 4, 2: 4, 4: 3, 6: 3, 7: 1, 8: 2, 10: 2}.get(f["opcode"], 1)
                nbytes = nregs * (16 if f["Q"] else 8)
            else:
                nregs = ((f["opcode"] & 1) << 1 | f["R"]) + 1
                op = f["opcode"] >> 1
                esize = 1 << f["size"] if op == 3 else (1, 2, 4 if not f["size"] & 1 else 8)[op]
                nbytes = nregs * esize
            base = gpr(f["Rn"], True)
            self.use(s, base, pc)
            if i.entry.params.get("post"):
                if f["Rm"] != 31:
                    self.use(s, gpr(f["Rm"], False), pc)
                    if base == SP:
                        self.set_sp(s, O, pc, "dyn")
                    else:
                        self.define(s, base)
                else:
                    self.wb(s, base, nbytes, pc)
            if f["L"]:
                for k in range(nregs):
                    self.vdef(s, (f["Rt"] + k) % 32)
            return
        if from_gpr:
            self.use(s, gpr(f["Rn"], False), pc)
        if to_gpr:
            self.define(s, gpr(f["Rd"], False))
            return
        if "Rd" in f:
            self.vdef(s, f["Rd"])

    def vdef(self, s, n):
        k = "v%d" % n
        if k in s.v:
            s.v[k] = O

    def ldst(self, s, pc, f, mode):
        size, opc = f["size"], f["opc"]
        base = gpr(f["Rn"], True)
        self.use(s, base, pc)
        prfm = size == 3 and opc == 2
        is_load = opc != 0 and not prfm
        off = 0
        if mode == "uimm":
            off = f["imm12"] << size
        elif mode in ("unscaled",):
            off = sx(f["imm9"], 9)
        elif mode == "reg":
            self.use(s, gpr(f["Rm"], False), pc)
            off = None
        delta = sx(f["imm9"], 9) if mode in ("pre", "post") else 0
        if mode == "pre":
            self.wb(s, base, delta, pc)
            off = 0
        rt = gpr(f["Rt"], False)
        if is_load:
            self.load(s, pc, base, off, rt)
        elif not prfm:
            self.use(s, rt, pc, saved=True)
            self.store_checked(s, pc, base, off, rt, 1 << size)
        if mode == "post":
            self.wb(s, base, delta, pc)

    def store_checked(self, s, pc, base, off, rt, size):
        val = self.get(s, rt) if size == 8 else O
        a, frame = self.addr(s, base, off)
        if not frame and rt is not None:
            v = s.r[rt]
            if v == ("E", 30):
                self.rec.add("lr_data", pc, "stored outside the frame")
        self.store(s, pc, base, off, val, rt, size)

    def pair(self, s, pc, f, vec):
        opc, mode, ld = f["opc"], f["mode"], f["L"]
        scale = (4 << opc) if vec else (8 if opc == 2 else 4)
        base = gpr(f["Rn"], True)
        self.use(s, base, pc)
        imm = sx(f["imm7"], 7) * scale
        off = 0 if mode == 1 else imm
        if mode == 3:
            self.wb(s, base, imm, pc)
            off = 0
        regs = (f["Rt"], f["Rt2"])
        for j, r in enumerate(regs):
            o = off + j * scale
            if vec:
                k = "v%d" % r
                if ld:
                    self.load(s, pc, base, o, k if k in s.v else None, vreg=True)
                else:
                    val = s.v.get(k, O) if scale >= 8 else O
                    self.store(s, pc, base, o, val, k, scale)
            else:
                rr = gpr(r, False)
                if ld:
                    self.load(s, pc, base, o, rr)
                else:
                    self.use(s, rr, pc, saved=True)
                    self.store_checked(s, pc, base, o, rr, scale)
        if mode == 1:
            self.wb(s, base, imm, pc)

    def ldst_v(self, s, pc, f, mode):
        size, opc = f["size"], f["opc"]
        nbytes = 16 if (size == 0 and opc & 2) else 1 << size
        base = gpr(f["Rn"], True)
        self.use(s, base, pc)
        off = 0
        if mode == "uimm":
            off = f["imm12"] * nbytes
        elif mode == "unscaled":
            off = sx(f["imm9"], 9)
        elif mode == "reg":
            self.use(s, gpr(f["Rm"], False), pc)
            off = None
        delta = sx(f["imm9"], 9) if mode in ("pre", "post") else 0
        if mode == "pre":
            self.wb(s, base, delta, pc)
            off = 0
        k = "v%d" % f["Rt"]
        if opc & 1:
            self.load(s, pc, base, off, k if k in s.v else None, vreg=True)
        else:
            val = s.v.get(k, O) if nbytes >= 8 else O
            self.store(s, pc, base, off, val, k, nbytes)
        if mode == "post":
            self.wb(s, base, delta, pc)


def analyse(fn, img_word):
    """the findings of one function (a Rec) plus its exits"""
    pcs = sorted(fn.body)
    # (UDF is a trap: the discovery walks on past it, the hardware does not)
    body = {pc: (("trap",) if (w & 0xFFFF0000) == 0 else k, w) for pc, (k, w) in fn.body.items()}
    leaders = {fn.entry} | {pc for pc in pcs if pc in fn.entries}
    for pc in pcs:
        k = body[pc][0]
        if k[0] in ("b", "bcond", "cbz", "tbz"):
            if fn.inside(k[1]):
                leaders.add(k[1])
        elif k[0] == "br":
            leaders.update(t for t in fn.jt.get(pc, ()) if fn.inside(t))
        if k[0] != "op":
            leaders.add(pc + 4)
    blocks = {}
    cur = None
    for pc in pcs:
        if pc in leaders or cur is None or pc - 4 not in body:
            cur = [pc]
            blocks[pc] = cur
        else:
            cur.append(pc)
    rec = Rec()
    it = Interp(fn, rec)
    dec = {pc: enc.decode(img_word(pc)) for pc in pcs}
    for pc, i in dec.items():
        if i is None:
            rec.on = True
            rec.add("undecoded", pc, "%08x" % img_word(pc))
            rec.on = False

    def run(bpc, s, out):
        """runs block bpc on s (in place); calls out(target_pc, state) for successors"""
        blk = blocks[bpc]
        for pc in blk:
            k = body[pc][0]
            i = dec[pc]
            kind = k[0]
            if TRACE and rec.on:
                print("%x %-6s %-14s sp=%s x29=%s x30=%s" % (pc, kind, i.entry.template if i else "?", s.r[SP], s.r[29],
                                                            s.r[30]))
            if kind == "op" or kind in ("isb", "ic"):
                if i is not None:
                    it.step(s, pc, i)
                if pc == blk[-1]:
                    nxt = pc + 4
                    if fn.inside(nxt):
                        out(nxt, s)
                    else:
                        exit_(s, pc, "fall", nxt)
                continue
            if kind == "bl" or kind == "blr":
                if kind == "blr":
                    it.use(s, k[1], pc)
                callee = fn.callee(k[1]) if kind == "bl" else "blr"
                for r in range(0, 19):
                    if r <= 8:
                        it.define(s, r)
                    else:
                        s.r[r] = O
                        s.defd |= 1 << r
                        s.dirty[r] = frozenset([callee])
                s.r[30] = O
                s.flags = False
                s.dirty["nzcv"] = frozenset([callee])
                for kk in [kk for kk in s.slots if s.r[SP][0] == "S" and kk < s.r[SP][1]]:
                    del s.slots[kk]
                nxt = pc + 4
                if pc == blk[-1]:
                    if fn.inside(nxt):
                        out(nxt, s)
                    elif nxt not in G.get("sjret", ()):
                        rec.add("noret_call", pc, callee)
                continue
            if kind == "svc":
                rec.add("sysreg", pc, "svc %#x" % k[1])
                for r in range(8):
                    it.define(s, r)
                if pc == blk[-1]:
                    nxt = pc + 4
                    if fn.inside(nxt):
                        out(nxt, s)
                    else:
                        exit_(s, pc, "fall", nxt)
                continue
            if kind == "trap":
                w = img_word(pc)
                rec.add("sysreg", pc, "udf" if (w & 0xFFFF0000) == 0 else "brk" if (w & 0xFFE0001F) == 0xD4200000
                        else "hlt")
                return
            if kind == "b":
                t = k[1]
                if fn.inside(t):
                    out(t, s)
                else:
                    exit_(s, pc, "tail", t)
                return
            if kind in ("bcond", "cbz", "tbz"):
                if kind == "bcond":
                    if k[2] < 14:
                        it.flags_read(s, pc)
                else:
                    it.use(s, k[2], pc)
                for t in (k[1], pc + 4):
                    if fn.inside(t):
                        out(t, s)
                    else:
                        exit_(s, pc, "tail" if t == k[1] else "fall", t)
                return
            if kind == "br":
                if pc in fn.jt:
                    it.use(s, k[1], pc)
                    for t in fn.jt[pc]:
                        if fn.inside(t):
                            out(t, s)
                        else:
                            exit_(s, pc, "tail", t)
                    return
                if k[1] == 30:
                    rec.add("lr_ret_reg", pc, "br x30")
                    exit_(s, pc, "ret", None)
                    return
                it.use(s, k[1], pc)
                exit_(s, pc, "tail", "br x%d" % k[1])
                return
            if kind == "ret":
                if k[1] != 30:
                    rec.add("lr_ret_reg", pc, "ret x%d" % k[1])
                    it.use(s, k[1], pc)
                exit_(s, pc, "ret" if k[1] == 30 else "retx", None)
                return
        return

    def exit_(s, pc, how, t):
        if how == "fall":
            if t is not None and t in fn.entries:
                rec.add("fall_into", pc, "%#x" % t)
            else:
                rec.add("fall_into", pc, "out of code")
            return
        if s.r[SP] != ("S", 0):
            if s.r[SP][0] == "S":
                rec.add("split_frame", pc, "%s SP%+d" % (how, s.r[SP][1]))
                return
            rec.add("sp_exit", pc, "%s %s" % (how, s.r[SP]))
        if how in ("ret", "tail") and s.r[30] != ("E", 30):
            rec.add("lr_exit", pc, "%s x30=%s" % (how, s.r[30][0]))
        bad = [("x%d" % r) for r in CS if s.r[r] != ("E", r)] + [k for k in VCS if s.v[k] != ("E", k)]
        if bad:
            rec.add("cs_exit", pc, "%s %s" % (how, ",".join(bad)))

    rec.noret = set()
    # fixed point (no recording), then one recording pass over the fixed-point entry states
    ins = {fn.entry: State()}
    visits = collections.Counter()
    work = [fn.entry]
    while work:
        b = work.pop()
        visits[b] += 1
        s = ins[b].copy()

        def out(t, st, _b=b):
            old = ins.get(t)
            if old is None:
                ins[t] = st.copy()
                work.append(t)
            elif old.merge(st, visits[t] > 8):
                work.append(t)
        run(b, s, out)
        if visits[b] > 200:
            rec.on = True
            rec.add("no_fixpoint", b)
            rec.on = False
            break
    rec.on = True
    for b in sorted(ins):
        run(b, ins[b].copy(), lambda t, st: None)
    if len(rec.cs_saved) >= 4:
        rec.f["cs_save_arg"].append((fn.entry, ",".join(sorted(map(str, rec.cs_saved)))))
    if len(rec.cs_loaded) >= 4 and rec.cs_loaded & {29, 30}:
        rec.f["cs_restore_arg"].append((fn.entry, ",".join(sorted(map(str, rec.cs_loaded)))))
    return rec


RUNTIME = ("__setjmp", "setjmp", "_setjmp", "longjmp", "_longjmp", "__cxa_throw", "__cxa_rethrow", "_Unwind_Resume",
           "_Unwind_RaiseException", "_Unwind_ForcedUnwind", "_Unwind_Backtrace", "__cxa_begin_catch",
           "__cxa_allocate_exception", "_ZSt9terminatev", "abort", "__cxa_pure_virtual",
           "_ZN2nn4diag6detail9AbortImplEv", "_ZN2nn6detail21UnexpectedDefaultImplEv",
           "_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE",
           "_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi",
           "_ZN2nn2os6detail20DispatchFiberContextEPPNS1_22FiberContextForHorizonES4_",
           "_ZN2nn2os6detail17CallFiberFunctionEPvPFPNS0_9FiberTypeES2_E",
           "_ZN2nn2os10ExitThreadEv", "_ZN2nn2os4QuitEv", "_ZN2nn4diag12GetBacktraceEPmi", "exit", "_exit",
           "__cxa_finalize", "pthread_exit")


# ---- the program: discovery as nx2/lift.py --standalone does it -------------------------------------------------
G = {}


def setup():
    import kit
    import nso2  # noqa: F401  (DT_RELR)
    import nso
    import image as eden_image
    import standalone
    import adr_entries
    from lift2 import discover
    exefs = kit.need_exefs()
    hle, stubbed = standalone.hle_set(nso.load_exefs(exefs))
    img = eden_image.EdenImage(exefs, 0, hle=hle | stubbed)
    import tempfile
    tmp = tempfile.mkdtemp(prefix="abi_census_")
    meta = img.write(tmp)
    spans = adr_entries.fde_spans(img.modules)
    refs = adr_entries.code_address_refs(open(os.path.join(tmp, "image.bin"), "rb").read(), meta, spans=spans)
    img.entries |= {e for e in refs if img.in_code(e)} - img.entries
    import shutil
    shutil.rmtree(tmp, ignore_errors=True)
    Lf = discover.Discovery(img)
    Lf.discover()
    return img, Lf, spans


def work(entries):
    img, Lf = G["img"], G["L"]
    out = {}
    for e in entries:
        body = Lf.funcs[e][0]
        if not body or e in Lf.plt:
            continue
        if e in G["sjret"]:   # (a setjmp call's return address: entered by longjmp, not a function)
            out[e] = ({"setjmp_return": [(e, "")]}, {}, set())
            continue
        fn = Fn(e, body, Lf.jt, Lf.entries, Lf.callee)
        try:
            r = analyse(fn, img.word)
        except Exception as ex:   # (a census: one failure is reported, not fatal)
            import traceback
            out[e] = ({"error": [(e, repr(ex) + traceback.format_exc().splitlines()[-3])]}, {}, set())
            continue
        f = {k: v for k, v in r.f.items() if v}
        if f or r.blame:
            out[e] = (f, {c: sorted(v) for c, v in r.blame.items()}, r.noret)
    return out


def module_of(img, a):
    for lo, hi, n in img.code:
        if lo <= a < hi:
            return n
    return "?"


def labels():
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "oot_labels.txt")
    lab = {}
    for line in open(path, encoding="utf-8"):
        p = line.split()
        if p and not line.startswith("#") and len(p) >= 2:
            try:
                lab[int(p[0], 16)] = p[1]
            except ValueError:
                pass
    return lab


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", default=None)
    p.add_argument("--jobs", type=int, default=4)
    p.add_argument("--only", default=None)
    p.add_argument("--trace", action="store_true")
    a = p.parse_args()
    global TRACE
    TRACE = a.trace
    import kit
    import time
    t0 = time.time()
    img, Lf, spans = setup()
    G["img"], G["L"] = img, Lf
    G["sjret"] = {r for rets in Lf.setjmp_owner.values() for r in rets}
    print("discovered %d functions, %d hooked (HLE) (%.0fs)" % (len(Lf.funcs), len(img.hooks), time.time() - t0),
          flush=True)
    ents = sorted(Lf.funcs) if not a.only else [int(x, 16) for x in a.only.split(",")]
    chunks = [ents[k::a.jobs * 8] for k in range(a.jobs * 8)]
    res = {}
    if a.jobs > 1:
        with multiprocessing.get_context("fork").Pool(a.jobs) as pool:
            for r in pool.imap_unordered(work, chunks):
                res.update(r)
    else:
        for c in chunks:
            res.update(work(c))
    print("analysed (%.0fs)" % (time.time() - t0), flush=True)
    # whole-program facts: callees blamed by their callers, noreturn callees, mid-FDE entries
    blame = collections.defaultdict(lambda: collections.Counter())
    noret = collections.Counter()
    for e, (f, b, nr) in res.items():
        for c, regs in b.items():
            for r in regs:
                blame[c][r] += 1
        for pc, c in f.get("noret_call", ()):
            noret[c] += 1
    starts = set(img.eh_starts)
    import bisect
    span_lo = [s for s, _ in spans]
    mid = {}
    for e in Lf.funcs:
        if e in starts:
            continue
        k = bisect.bisect_right(span_lo, e) - 1
        if k >= 0 and spans[k][0] < e < spans[k][1]:
            mid[e] = spans[k][0]
    lab = labels()
    # callers of the runtime functions a lifter cannot treat as ordinary calls (by symbol: the SDK exports them)
    by_name = {n: x for x, n in img.names.items() if n in RUNTIME}
    rt_calls = collections.defaultdict(list)
    for e, (body, _) in Lf.funcs.items():
        for pc, (k, _w) in body.items():
            if k[0] in ("bl", "b") and not (k[0] == "b" and k[1] in body):
                n = img.names.get(Lf.callee(k[1]))
                if n in RUNTIME:
                    rt_calls[n].append((e, pc))
    def name(x):
        if not isinstance(x, int):
            return str(x)
        n = lab.get(x) or img.names.get(x) or ""
        return "%#x %s %s" % (x, module_of(img, x), n)

    counts = collections.Counter()
    lines = []
    for e in sorted(res):
        f = res[e][0]
        for k, items in sorted(f.items()):
            if k == "noret_call":
                continue
            counts[k] += 1
            det = collections.Counter(d for _, d in items)
            lines.append("%-15s %s  [%d] %s  first %#x" % (k, name(e), len(items),
                                                         "; ".join("%s x%d" % kv for kv in det.most_common(4)),
                                                         items[0][0]))
    for c, n in sorted(blame.items(), key=lambda kv: -sum(kv[1].values())):
        counts["blamed_callee"] += 1
        lines.append("%-15s %s  read after its call by callers: %s" % ("blamed_callee", name(c),
                                                                       ", ".join("%s x%d" % kv for kv in n.most_common())))
    for c, n in noret.most_common():
        counts["noret_callee"] += 1
        lines.append("%-15s %s  %d call sites with nothing after them" % ("noret_callee", name(c), n))
    hooked = set(img.hooks)
    for n in RUNTIME:
        sites = rt_calls.get(n, [])
        x = by_name.get(n)
        mods = collections.Counter(module_of(img, e) for e, _ in sites)
        counts["rt_callee"] += 1
        lines.append("%-15s %s %s%s: %d call sites in %d functions (%s)%s" % (
            "rt_callee", n, "%#x" % x if x else "(not defined)", " HLE" if x in hooked else "", len(sites),
            len({e for e, _ in sites}), ", ".join("%s %d" % kv for kv in sorted(mods.items())),
            ": " + " ".join("%#x" % e for e in sorted({e for e, _ in sites})[:12]) if sites else ""))
    for e, s in sorted(mid.items()):
        counts["mid_fde"] += 1
        lines.append("%-15s %s  inside the FDE of %s" % ("mid_fde", name(e), name(s)))
    out = a.out or os.path.join(kit.WORK, "lift2", "abi_census.txt")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as fo:
        fo.write("\n".join(sorted(lines)) + "\n")
    print("%d functions analysed, %d with a finding (%.0fs); list in %s" % (len(ents), len(res), time.time() - t0, out))
    for k, n in sorted(counts.items()):
        print("  %-15s %7d" % (k, n))


if __name__ == "__main__":
    main()
